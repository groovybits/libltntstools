/* Copyright LiveTimeNet, Inc. 2021. All Rights Reserved. */

#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <pthread.h>

#include "libltntstools/ltntstools.h"
#include "xorg-list.h"

#define LOCAL_DEBUG 0

#define MAX_PARTIAL_BUFFER_SIZE (50 * 1024 * 1024) /* 50MB default */

#define CLAMP_FAR_FUTURE_MS (30 * 1000) /* 30 seconds in the future */

struct smoother_pcr_item_s
{
	struct xorg_list list;
	uint64_t       seqno; /* Unique number per item, so we can check for loss/corruption in the lists. */

	unsigned char *buf;
	int            lengthBytes;
	int            maxLengthBytes;

	int            pcrComputed; /* Boolean. Was the PCR in this item computed from a base offset, or read from stream? */
	int64_t        pcrIntervalPerPacketTicks;

	struct ltntstools_pcr_position_s pcrdata; /* PCR value from pid N in the buffer, first PCR only. */
	uint64_t       received_TSuS;  /* Item received timestamp Via makeTimestampFromNow */
	uint64_t       scheduled_TSuS; /* Time this item is schedule for push via thread for smoothing output. */

	int            pcrDidReset; /* Boolean */
};

#if LOCAL_DEBUG
static void itemPrint(struct smoother_pcr_item_s *item)
{
	printf("seqno %" PRIu64, item->seqno);
	printf(" lengthBytes %5d", item->lengthBytes);
	printf(" received_TSuS %" PRIu64, item->received_TSuS);
	printf(" scheduled_TSuS %" PRIu64, item->scheduled_TSuS);
	printf(" pcrComputed %d", item->pcrComputed);
	printf(" pcr %" PRIi64 "  pcrDidReset %d\n", item->pcrdata.pcr, item->pcrDidReset);
}
#endif

/* byte_array.... ---------- */
struct byte_array_s
{
	uint8_t *buf;
	int maxLengthBytes;
	int lengthBytes;
};

static int byte_array_init(struct byte_array_s *ba, int lengthBytes)
{
	ba->buf = malloc(lengthBytes);
	if (!ba->buf)
		return -1;

	ba->maxLengthBytes = lengthBytes;
	ba->lengthBytes = 0;

	return 0;
}

static void byte_array_free(struct byte_array_s *ba)
{
	free(ba->buf);
	ba->lengthBytes = 0;
	ba->maxLengthBytes = 0;
}

static int byte_array_append(struct byte_array_s *ba, const uint8_t *buf, int lengthBytes)
{
	int newLengthBytes = ba->lengthBytes + lengthBytes;
	if (newLengthBytes > ba->maxLengthBytes) {
		/* reallocate in a simple linear fashion */
		ba->buf = realloc(ba->buf, newLengthBytes);
		ba->maxLengthBytes = newLengthBytes;
	}

	if (newLengthBytes > MAX_PARTIAL_BUFFER_SIZE) {
		int over = newLengthBytes - MAX_PARTIAL_BUFFER_SIZE;
		if (over > ba->lengthBytes) {
			/* If over is bigger than everything in the buffer, 
			 * that implies we can't fit the new data at all, so let's 
			 * discard everything and skip appending.
			 */
			ba->lengthBytes = 0;
			return ba->lengthBytes;
		} else {
			/* Discard `over` oldest bytes to make room for new data. */
			memmove(ba->buf, ba->buf + over, ba->lengthBytes - over);
			ba->lengthBytes -= over;
			newLengthBytes = ba->lengthBytes + lengthBytes;
		}
	}

	memcpy(ba->buf + ba->lengthBytes, buf, lengthBytes);
	ba->lengthBytes += lengthBytes;

	return ba->lengthBytes;
}

static void byte_array_trim(struct byte_array_s *ba, int lengthBytes)
{
	if (lengthBytes > ba->lengthBytes)
		return;

	memmove(ba->buf, ba->buf + lengthBytes, ba->lengthBytes - lengthBytes);
	ba->lengthBytes -= lengthBytes;
}

static const uint8_t *byte_array_addr(struct byte_array_s *ba)
{
	return ba->buf;
}

struct smoother_pcr_context_s
{
	struct xorg_list itemsFree;
	struct xorg_list itemsBusy;
	pthread_mutex_t listMutex;

	void *userContext;
	smoother_pcr_output_callback outputCb;

	uint64_t walltimeFirstPCRuS; /* Reset this when the clock significantly leaps backwards */
	int64_t pcrFirst; /* Reset this when the clock significantly leaps backwards */
	int64_t pcrTail; /* PCR on the first list item */
	int64_t pcrHead; /* PCR on the last list item */
	uint16_t pcrPID;

	int latencyuS;
	uint64_t bitsReceivedSinceLastPCR;

	uint64_t seqno;
	uint64_t last_seqno;

	int itemLengthBytes;
	pthread_t threadId;
	int threadRunning, threadTerminate, threadTerminated;

	int64_t totalSizeBytes;

	/* A contigious chunk of ram containing transport packets, in order.
	 * starting with a transport packet containing a PCR on pid ctx->pcrPid
	*/
	struct byte_array_s ba; /* partial buffer for unparsed data */

	/* Handle the case where the PCR goes forward or back in time,
	 * in our case by more than 15 seconds.
	 * Flag an internal PCR reset and let the implementation recompute its clocks.
	*/
	int didPcrReset;
	time_t lastPcrResetTime;
	int64_t pcrIntervalPerPacketTicksLast;
	int64_t pcrIntervalTicksLast;

	/* based on first and last PCRs in the list, how much latency do we have? */
	int64_t measuredLatencyMs;

	struct ltn_histogram_s *histReceive;
	struct ltn_histogram_s *histTransmit;
};

static inline uint64_t makeTimestampFromTimeval(struct timeval *ts)
{
	uint64_t t = ((int64_t)ts->tv_sec * 1000000LL) + ts->tv_usec;
	return t;
}
static inline uint64_t makeTimestampFromNow()
{
	struct timeval now;
	gettimeofday(&now, NULL);
	return makeTimestampFromTimeval(&now);
}

static void itemFree(struct smoother_pcr_item_s *item)
{
	if (item) {
		free(item->buf);
		free(item);
	}
}

static void itemReset(struct smoother_pcr_item_s *item)
{
	item->lengthBytes = 0;
	item->received_TSuS = 0;
	item->scheduled_TSuS = 0;
	item->pcrComputed = 0;
	ltntstools_pcr_position_reset(&item->pcrdata);
}

static struct smoother_pcr_item_s *itemAlloc(int lengthBytes)
{
	struct smoother_pcr_item_s *item = calloc(1, sizeof(*item));
	if (!item)
		return NULL;

	item->buf = calloc(1, lengthBytes);
	if (!item->buf) {
		free(item);
		item = NULL;
	}
	item->maxLengthBytes = lengthBytes;
	itemReset(item);

	return item;
}

#if LOCAL_DEBUG
static void _queuePrintList(struct smoother_pcr_context_s *ctx, struct xorg_list *head, const char *name)
{
        int totalItems = 0;

	printf("Queue %s -->\n", name);
	struct smoother_pcr_item_s *e = NULL, *next = NULL;
	xorg_list_for_each_entry_safe(e, next, head, list) {
		totalItems++;
		itemPrint(e);
	}
	printf("Queue End --> %d items\n", totalItems);
}
#endif

/******************************************************************************
 * We clamp scheduling if it is more than CLAMP_FAR_FUTURE_MS from now.
 * If we detect that the scheduled time is extremely far in the future, 
 * we'll clamp or increment the time slightly, to avoid indefinite deferral.
 ******************************************************************************/
static uint64_t getScheduledOutputuS(struct smoother_pcr_context_s *ctx, int64_t pcr, int64_t pcrIntervalTicks)
{
	int64_t ticks = ltntstools_scr_diff(ctx->pcrFirst, pcr);

	uint64_t scheduledTimeuS = ctx->walltimeFirstPCRuS + (ticks / 27);
	scheduledTimeuS += ctx->latencyuS;

	uint64_t now = makeTimestampFromNow();
	uint64_t far_future_us = now + (CLAMP_FAR_FUTURE_MS * 1000ULL);
	if (scheduledTimeuS > far_future_us) {
		scheduledTimeuS = far_future_us;
	}

	return scheduledTimeuS;
}

/*  Service the busy list. Find any items due for output
 *  and send via the callback.
 *  It's important that we hold the mutex for a short time so we don't block
 *  the _write() method.
 */
static int _queueProcess(struct smoother_pcr_context_s *ctx, int64_t uS)
{
	struct xorg_list loclist;
	xorg_list_init(&loclist);

	int count = 0, totalItems = 0, redundantItems = 0;
	struct smoother_pcr_item_s *e = NULL, *next = NULL;
	xorg_list_for_each_entry_safe(e, next, &ctx->itemsBusy, list) {
		totalItems++;

		if (totalItems == 1) {
			ctx->pcrHead = e->pcrdata.pcr;
		}

		if (e->scheduled_TSuS <= (uint64_t)uS) {
			xorg_list_del(&e->list);
			xorg_list_append(&e->list, &loclist);
			count++;
		} else {
			if (count > 0) {
				/* The list is out of order, 
				 * but in practice we keep them in time ascending order. 
				 */
				redundantItems++;
			}
		}
	}

	/* check contiguous seqno ordering to detect any bug. */
	e = NULL;
	next = NULL;
	int countSeq = 0;
	uint64_t last_seq = 0;
	xorg_list_for_each_entry_safe(e, next, &ctx->itemsBusy, list) {
		countSeq++;
		if (countSeq > 1) {
			if (last_seq + 1 != e->seqno) {
				/* Almost certainly, the schedule US time is out of order, warn. */
				printf("List possibly mangled, seqnos might be bad now, %" PRIu64 ", %" PRIu64 "\n", last_seq, e->seqno);
#if LOCAL_DEBUG
				_queuePrintList(ctx, &ctx->itemsBusy, "Busy");
				_queuePrintList(ctx, &loclist, "loclist");
				fflush(stdout);
				fflush(stderr);
				exit(1);
#endif
			}
		}
		last_seq = e->seqno;
	}

	pthread_mutex_unlock(&ctx->listMutex);

	if (count <= 0) {
		return -1;
	}

	/* Deliver items in loclist. */
	xorg_list_for_each_entry_safe(e, next, &loclist, list) {
		if (ctx->outputCb) {
			/* build PCR array for the entire item. */
			struct ltntstools_pcr_position_s *array = NULL;
			int arrayLength = 0;
			int packetCount = e->lengthBytes / 188;
			for (int i = 0; i < packetCount; i++) {
				struct ltntstools_pcr_position_s p;
				p.offset = i * 188;
				p.pcr = e->pcrdata.pcr + (i * e->pcrIntervalPerPacketTicks);
				p.pid = ltntstools_pid(e->buf + (i * 188));
				ltntstools_pcr_position_append(&array, &arrayLength, &p);
			}

			struct timeval tv;
			gettimeofday(&tv, NULL);
			ltn_histogram_interval_update(ctx->histTransmit, &tv);

			int x = e->lengthBytes;
			uint64_t sn = e->seqno;

			ctx->outputCb(ctx->userContext, e->buf, e->lengthBytes, array, arrayLength);

			if (x != e->lengthBytes) {
				printf("%s() ERROR %d != %d, mangled returned object length\n",
					__func__, x, e->lengthBytes);
			}
			if (sn != e->seqno) {
				printf("%s() ERROR %" PRIu64 " != %" PRIu64 
					", mangled returned object seqno\n", __func__, sn, e->seqno);
			}

			pthread_mutex_lock(&ctx->listMutex);
			ctx->totalSizeBytes -= e->lengthBytes;
			pthread_mutex_unlock(&ctx->listMutex);

			free(array);

			if (ctx->last_seqno && ctx->last_seqno + 1 != e->seqno) {
				printf("%s() seq err %" PRIu64 " vs %" PRIu64 "\n",
					__func__, ctx->last_seqno, e->seqno);
			}
			ctx->last_seqno = e->seqno;
		}
	}

	/* Return items to free list. */
	e = NULL; next = NULL;
	pthread_mutex_lock(&ctx->listMutex);
	xorg_list_for_each_entry_safe(e, next, &loclist, list) {
		itemReset(e);
		xorg_list_del(&e->list);
		xorg_list_append(&e->list, &ctx->itemsFree);
	}
	pthread_mutex_unlock(&ctx->listMutex);

	return 0;
}

extern int ltnpthread_setname_np(pthread_t thread, const char *name);

static void *_threadFunc(void *p)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)p;

	pthread_detach(ctx->threadId);
	ltnpthread_setname_np(ctx->threadId, "thread-brsmooth");

	ctx->threadTerminated = 0;
	ctx->threadRunning = 1;

	while (!ctx->threadTerminate) {
		pthread_mutex_lock(&ctx->listMutex);
		if (xorg_list_is_empty(&ctx->itemsBusy)) {
			pthread_mutex_unlock(&ctx->listMutex);
			usleep(1 * 1000);
			continue;
		}
		int64_t nowUs = makeTimestampFromNow();

		int64_t uS = makeTimestampFromNow();

		/* Service the output schedule queue, output any UDP packets when they're due.
		 * Important to remember that we're calling this func while we're holding the mutex.
		 */
		if (_queueProcess(ctx, nowUs) < 0) {
			/* nothing was scheduled, short sleep */
			usleep(1 * 1000);
		}
	}

	ctx->threadRunning = 1;
	ctx->threadTerminated = 1;
	return NULL;
}

int smoother_pcr_alloc(void **hdl, void *userContext, smoother_pcr_output_callback cb,
	int itemsPerSecond, int itemLengthBytes, uint16_t pcrPID, int latencyMS)
{
	struct smoother_pcr_context_s *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	xorg_list_init(&ctx->itemsFree);
	xorg_list_init(&ctx->itemsBusy);
	pthread_mutex_init(&ctx->listMutex, NULL);

	ctx->userContext = userContext;
	ctx->outputCb = cb;
	ctx->itemLengthBytes = itemLengthBytes;
	ctx->walltimeFirstPCRuS = 0;
	ctx->pcrFirst = -1;
	ctx->pcrTail = -1;
	ctx->pcrHead = -1;
	ctx->pcrPID = pcrPID;
	ctx->latencyuS = latencyMS * 1000;
	ctx->lastPcrResetTime = time(NULL);

	byte_array_init(&ctx->ba, 8000 * 188); /* Initial size of 300mbps with 40ms PCR intervals */

	ltn_histogram_alloc_video_defaults(&ctx->histReceive, "receive arrival times");
	ltn_histogram_alloc_video_defaults(&ctx->histTransmit, "transmit arrival times");

	/* TODO: We probably don't need an itemspersecond fixed value, probably,
	 * calculate the number of items based on input bitrate value and
	 * a (TODO) future latency/smoothing window.
	 */
	pthread_mutex_lock(&ctx->listMutex);
	for (int i = 0; i < itemsPerSecond; i++) {
		struct smoother_pcr_item_s *item = itemAlloc(itemLengthBytes);
		if (!item)
			continue;
		xorg_list_append(&item->list, &ctx->itemsFree);
	}
	pthread_mutex_unlock(&ctx->listMutex);

	/* Spawn a thread that manages the scheduled output queue. */
	pthread_create(&ctx->threadId, NULL, _threadFunc, ctx);

	*hdl = ctx;
	return 0;
}

void smoother_pcr_free(void *hdl)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;

	if (ctx->threadRunning) {
		ctx->threadTerminated = 0;
		ctx->threadTerminate = 1;
		while (!ctx->threadTerminated) {
			usleep(20 * 1000);
		}
	}

	pthread_mutex_lock(&ctx->listMutex);
	while (!xorg_list_is_empty(&ctx->itemsFree)) {
		struct smoother_pcr_item_s *item = 
			xorg_list_first_entry(&ctx->itemsFree, struct smoother_pcr_item_s, list);
		xorg_list_del(&item->list);
		itemFree(item);
	}
	while (!xorg_list_is_empty(&ctx->itemsBusy)) {
		struct smoother_pcr_item_s *item = 
			xorg_list_first_entry(&ctx->itemsBusy, struct smoother_pcr_item_s, list);
		xorg_list_del(&item->list);
		itemFree(item);
	}
	pthread_mutex_unlock(&ctx->listMutex);

	byte_array_free(&ctx->ba);

	ltn_histogram_free(ctx->histReceive);
	ltn_histogram_free(ctx->histTransmit);

	free(ctx);
}

int smoother_pcr_write2(void *hdl, const unsigned char *buf, int lengthBytes,
	int64_t pcrValue, int64_t pcrIntervalPerPacketTicks, int64_t pcrIntervalTicks)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;

	pthread_mutex_lock(&ctx->listMutex);
	if (xorg_list_is_empty(&ctx->itemsFree)) {
		/* Grow free list if needed. */
		for (int i = 0; i < 64; i++) {
			struct smoother_pcr_item_s *item = itemAlloc(ctx->itemLengthBytes);
			if (!item)
				continue;
			xorg_list_append(&item->list, &ctx->itemsFree);
		}
	}


	struct smoother_pcr_item_s *item = NULL;
	if (!xorg_list_is_empty(&ctx->itemsFree)) {
		    item = xorg_list_first_entry(&ctx->itemsFree, struct smoother_pcr_item_s, list);
	}
	if (!item) {
		pthread_mutex_unlock(&ctx->listMutex);
		return -1;
	}
	xorg_list_del(&item->list);
	pthread_mutex_unlock(&ctx->listMutex);

	item->received_TSuS = makeTimestampFromNow();
	item->pcrIntervalPerPacketTicks = pcrIntervalPerPacketTicks;

	if (item->maxLengthBytes < lengthBytes) {
		item->buf = realloc(item->buf, lengthBytes);
		item->maxLengthBytes = lengthBytes;
	}
	memcpy(item->buf, buf, lengthBytes);
	item->lengthBytes = lengthBytes;

	item->pcrdata.pcr = pcrValue;
	if (ctx->pcrFirst == -1) {
		ctx->pcrFirst = item->pcrdata.pcr;
		ctx->walltimeFirstPCRuS = item->received_TSuS;
	}
	ctx->bitsReceivedSinceLastPCR = 0;
	ctx->pcrTail = item->pcrdata.pcr;

	item->scheduled_TSuS = getScheduledOutputuS(ctx, pcrValue, pcrIntervalTicks);
	item->pcrComputed = 0;

	pthread_mutex_lock(&ctx->listMutex);
	item->seqno = ctx->seqno++;
	ctx->totalSizeBytes += item->lengthBytes;
	if (item->lengthBytes <= 0) {
		fprintf(stderr, "%s() bug: item->lengthBytes = %d\n", __func__, item->lengthBytes);
	}

	/* If the last item is scheduled after this, ensure monotonic time ordering. */
	if (!xorg_list_is_empty(&ctx->itemsBusy)) {
		struct smoother_pcr_item_s *last = xorg_list_last_entry(
			&ctx->itemsBusy, struct smoother_pcr_item_s, list);
		if (last->scheduled_TSuS > item->scheduled_TSuS) {
			item->scheduled_TSuS = last->scheduled_TSuS + 1;
		}
	}

	xorg_list_append(&item->list, &ctx->itemsBusy);
	pthread_mutex_unlock(&ctx->listMutex);

	return 0;
}

int smoother_pcr_write(void *hdl, const unsigned char *buf, int lengthBytes, struct timeval *ts)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;

	ltn_histogram_interval_update(ctx->histReceive, ts);

	/* 1) Append new data to partial buffer (with internal limit checks). */
	byte_array_append(&ctx->ba, buf, lengthBytes);

	/* If we STILL have no pcrFirst and the buffer is huge, we forcibly discard. */
	if (ctx->pcrFirst == -1 && ctx->ba.lengthBytes > MAX_PARTIAL_BUFFER_SIZE / 2) {
		fprintf(stderr,
			"[smoother_pcr_write] WARN: No PCR found, partial buffer is large (%d bytes). Discarding.\n",
			ctx->ba.lengthBytes);
		ctx->ba.lengthBytes = 0;
		return 0;
	}

	int pcrCount;
	do {
		/* 2) Attempt to locate PCRs in the partial buffer. */
		struct ltntstools_pcr_position_s *array = NULL;
		int arrayLength = 0;
		int r = ltntstools_queryPCRs(ctx->ba.buf, ctx->ba.lengthBytes, 0, &array, &arrayLength);
		if (r < 0) {
			free(array);
			return 0;
		}

		struct ltntstools_pcr_position_s *pcr[2] = { NULL, NULL };
		pcrCount = 0;
		for (int i = 0; i < arrayLength; i++) {
			struct ltntstools_pcr_position_s *e = &array[i];
			if (e->pid == ctx->pcrPID) {
				pcrCount++;
				if (pcrCount == 1 && pcr[0] == NULL)
					pcr[0] = e;
				else if (pcrCount == 2 && pcr[1] == NULL)
					pcr[1] = e;
			}
			if (pcrCount == 3) /* we only need the first two for scheduling logic */
				break;
		}
		if (pcrCount < 2) {
			free(array);
			return 0; /* not enough PCR to schedule */
		}

		/* 3) We have 2 consecutive PCRs. The distance in bytes is (pcr[1]->offset - pcr[0]->offset). */
		int byteCount = (int)(pcr[1]->offset - pcr[0]->offset);
		if (byteCount <= 0) {
			/* Possibly malformed or out-of-order offsets. Just bail. */
			free(array);
			return 0;
		}

		int pktCount = byteCount / 188;
		int64_t pcrIntervalTicks = ltntstools_scr_diff(pcr[0]->pcr, pcr[1]->pcr);
		int64_t pcrIntervalPerPacketTicks = 0;
		if (pktCount > 0) {
			pcrIntervalPerPacketTicks = pcrIntervalTicks / pktCount;
		}

		/* If there's a huge jump > 15s, we do a partial reset. */
		if (pcrIntervalTicks > (int64_t)(15 * 27000000)) {
			printf("Detected significant pcr jump:\n");
			ctx->didPcrReset = 1;
			/* restore to last known intervals to avoid meltdown */
			printf("Auto-correcting PCR schedule from big jump. "
				"pcrIntervalPerPacketTicks was %" PRIi64 ", we revert to %" PRIi64 "\n",
				pcrIntervalPerPacketTicks, ctx->pcrIntervalPerPacketTicksLast);
			pcrIntervalPerPacketTicks = ctx->pcrIntervalPerPacketTicksLast;
			pcrIntervalTicks = ctx->pcrIntervalTicksLast;
		}

		ctx->measuredLatencyMs = ltntstools_scr_diff(ctx->pcrHead, ctx->pcrTail) / 27000LL;

		int64_t pcrValue = pcr[0]->pcr;
		int idx = 0;
		int rem = byteCount;

		/* 4) We feed data in 7*188 chunks to smoother_pcr_write2() by default. */
		while (rem > 0) {
			int cplen = 7 * 188;
			if (cplen > rem)
				cplen = rem;

			smoother_pcr_write2(ctx, &ctx->ba.buf[pcr[0]->offset + idx], cplen,
				pcrValue, pcrIntervalPerPacketTicks, pcrIntervalTicks);

			pcrValue = ltntstools_scr_add(pcrValue,
				pcrIntervalPerPacketTicks * (cplen / 188));

			rem -= cplen;
			idx += cplen;
		}

		/* 5) Trim out everything up to pcr[1]->offset from partial buffer. */
		byte_array_trim(&ctx->ba, pcr[1]->offset);

		free(array);
		array = NULL;

		/* If its been more than 60 seconds, reset the PCR to avoid slow drift over time.
		 * Also, prevents issues where the pcrFirst value wraps and tick calculations that
		 * drive scheduled packet output time goes back in time.
		 */
		time_t now = time(NULL);
		if (now >= ctx->lastPcrResetTime + 60) {
			ctx->lastPcrResetTime = now;
			ctx->didPcrReset = 1;
		}

		/* If the PCR resets, we need to track some pcr interval state so we can properly
		 * schedule out the remaining packets from a good PCR, without bursting, prior
		 * to restabalishing the timebase for the new PCR, we'll need this value
		 * to schedule, preserve it.
		 */
		ctx->pcrIntervalPerPacketTicksLast = pcrIntervalPerPacketTicks;
		ctx->pcrIntervalTicksLast = pcrIntervalTicks;

		/* And finally, if during this pass we detected a PCR reset, ensure the next
		 * round of writes resets its internal clocks and goes through a full PCR reset.
		 */
		if (ctx->didPcrReset) {
			ctx->pcrFirst = -1;
			ctx->didPcrReset = 0;
		}

	/* We only wrote out until just before the second PCR, so loop if we found more than
	 * two PCRs in our buffer to handle the next PCR->PCR interval as well.
	 */
	} while (pcrCount > 2);

	return 0;
}

int64_t smoother_pcr_get_size(void *hdl)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;
	int64_t sizeBytes = 0;

	pthread_mutex_lock(&ctx->listMutex);
	if (ctx->totalSizeBytes > 0)
		sizeBytes = ctx->totalSizeBytes;
	pthread_mutex_unlock(&ctx->listMutex);

	return sizeBytes;
}

void smoother_pcr_reset(void *hdl)
{
	struct smoother_pcr_context_s *ctx = (struct smoother_pcr_context_s *)hdl;

	pthread_mutex_lock(&ctx->listMutex);
	ctx->walltimeFirstPCRuS = 0;
	ctx->pcrFirst = -1;
	ctx->pcrHead = -1;
	ctx->pcrTail = -1;
	ctx->totalSizeBytes = 0;

	ctx->ba.lengthBytes = 0;

	while (!xorg_list_is_empty(&ctx->itemsBusy)) {
		struct smoother_pcr_item_s *item = 
			xorg_list_first_entry(&ctx->itemsBusy, struct smoother_pcr_item_s, list);
		itemReset(item);
		xorg_list_del(&item->list);
		xorg_list_append(&item->list, &ctx->itemsFree);
	}

	pthread_mutex_unlock(&ctx->listMutex);
}

