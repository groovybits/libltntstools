#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <libltntstools/ltntstools.h>
#include "klringbuffer.h"
#include "klbitstream_readwriter.h"

#define LOCAL_DEBUG 0
#define ORDERED_LIST_DEPTH 10

struct pes_extractor_s
{
	uint16_t pid;
	KLRingBuffer *rb;
	int appending;
	uint8_t streamId;
	int skipDataExtraction;

	pes_extractor_callback cb;
	void *userContext;

	/* Cache N pes packets and emit them in PTS order,
	 * where the lowest PTS PES from an array is emitted
	 * when a new PES is added.
	 *
	 */
	int orderedOutput;
	struct xorg_list listOrdered;
	pthread_mutex_t listOrderedMutex;
	int64_t orderedBaseTime;
	int64_t lastDeliveredPTS;
};

struct item_s
{
	struct xorg_list list;
	int64_t correctedPTS; /* true 64bit number where when the PTS wrapper we don't truncate, always increasing value. */
	struct ltn_pes_packet_s *pes;
};

int ltntstools_pes_extractor_alloc(void **hdl, uint16_t pid, uint8_t streamId, pes_extractor_callback cb, void *userContext)
{
	struct pes_extractor_s *ctx = calloc(1, sizeof(*ctx));

	ctx->rb = rb_new(4 * 1048576, 32 * 1048576);
	ctx->pid = pid;
	ctx->streamId = streamId;
	ctx->cb = cb;
	ctx->userContext = userContext;
	ctx->skipDataExtraction = 0;
	ctx->orderedOutput = 0;
	ctx->orderedBaseTime = 0;
	xorg_list_init(&ctx->listOrdered);
	pthread_mutex_init(&ctx->listOrderedMutex, NULL);

	/* initialize a 10 item deep list */
	for (int i = 0; i < ORDERED_LIST_DEPTH; i++) {
		struct item_s *item = malloc(sizeof(*item));
		if (item) {
			item->correctedPTS = 0;
			item->pes = NULL;
			xorg_list_append(&item->list, &ctx->listOrdered);
		} 
	}

	*hdl = ctx;
	return 0;
}

#if LOCAL_DEBUG
static void _list_print(struct pes_extractor_s *ctx)
{
	struct item_s *e = NULL;

	int n = 0;
	xorg_list_for_each_entry(e, &ctx->listOrdered, list) {
		printf("item[%2d] %p correctedPTS %" PRIi64 "\n", n++, e, e->correctedPTS);
	}
}
#endif

static void _list_insert(struct pes_extractor_s *ctx, struct item_s *newitem)
{
	struct item_s *e = NULL;

	int didAdd = 0;
	xorg_list_for_each_entry(e, &ctx->listOrdered, list) {
		if (newitem->correctedPTS < e->correctedPTS) {
			__xorg_list_add(&newitem->list, e->list.prev, &e->list);
			didAdd++;
			break;
		}
		if (e->pes == NULL) {
			__xorg_list_add(&newitem->list, e->list.prev, &e->list);
			didAdd++;
			break;
		}
	}
	if (didAdd == 0) {
		xorg_list_append(&newitem->list, &ctx->listOrdered);
	}
}

static struct item_s * _list_find_oldest(struct pes_extractor_s *ctx)
{
	struct item_s *e = NULL;
	struct item_s *oldest = NULL;

	int cnt = 0;
	xorg_list_for_each_entry(e, &ctx->listOrdered, list) {
		cnt++;
		if (oldest == NULL) {
			oldest = e;
		} else {
			if (e->correctedPTS < oldest->correctedPTS) {
				oldest = e;
			}
		}
	}
	return oldest;
}

void ltntstools_pes_extractor_free(void *hdl)
{
	struct pes_extractor_s *ctx = (struct pes_extractor_s *)hdl;
	rb_free(ctx->rb);

	while (!xorg_list_is_empty(&ctx->listOrdered)) {
		struct item_s *item = xorg_list_first_entry(&ctx->listOrdered, struct item_s, list);
		if (item->pes) {
			ltn_pes_packet_free(item->pes);
			item->pes = NULL;
			item->correctedPTS = 0;
		}
		xorg_list_del(&item->list);
	}

	free(ctx);
}

int ltntstools_pes_extractor_set_ordered_output(void *hdl, int tf)
{
	struct pes_extractor_s *ctx = (struct pes_extractor_s *)hdl;
	ctx->orderedOutput = tf;
	return 0; /* Success */
}

int ltntstools_pes_extractor_set_skip_data(void *hdl, int tf)
{
	struct pes_extractor_s *ctx = (struct pes_extractor_s *)hdl;
	ctx->skipDataExtraction = tf;
	return 0; /* Success */
}

/* Remove any bytes leading up to a 00 00 01 pattern, align the ring.  */
static void _trimRing(struct pes_extractor_s *ctx)
{
	unsigned char pattern[4] = { 0x00, 0x00, 0x01, ctx->streamId };

	int rlen = rb_used(ctx->rb);
	if (rlen <= 0)
		return;

	int count = 0;
	uint8_t buf[8];

	while (1) {
		size_t l = rb_peek(ctx->rb, (char *)&buf[0], 4);
		if (l != 4)
			break;

		if (memcmp(pattern, buf, 4) != 0) {
			rb_discard(ctx->rb, 1);
			count++;
		} else
			break;
	}
}

static int searchReverse(const unsigned char *buf, int lengthBytes, uint8_t streamId)
{
	unsigned char pattern[4] = { 0x00, 0x00, 0x01, streamId };

	for (int i = lengthBytes - 4; i >= 0; i--) {
		if (memcmp(pattern, buf + i, 4) == 0)
			return i;
	}

	return -1;
}

static int _processRing(struct pes_extractor_s *ctx)
{
	int rlen = rb_used(ctx->rb);
	if (rlen < 16)
		return -1;

#if LOCAL_DEBUG
	printf("%s() ring size %d\n", __func__, rb_used(ctx->rb));
#endif

	unsigned char *buf = malloc(rlen);
	if (buf) {
		int plen = rb_peek(ctx->rb, (char *)buf, rlen);
		if (plen == rlen) {
			/* Search backwards for the start of the next mpeg signature.
			 * result is the position of the signature as an offset from the beginning of the buffer.
			 * If the value is zero, we only havea  single porbably incomplete PES in the buffer, which is
			 * meaningless, becasue the buffer is expected to contain and ENTIRE PES followed by the header from
			 * a subsequence PES.
			 */
			int offset = searchReverse(buf, rlen, ctx->streamId);
			if (offset < 16) {
				/* We'll come back again in the future */
				free(buf);
				return -1;
			}
#if LOCAL_DEBUG
			if (offset == 423) {
				ltntstools_hexdump(buf, rlen, 32);
			}
			printf("%s() offset %d, rlen %d\n", __func__, offset, rlen);
#endif
			struct klbs_context_s bs;
			klbs_init(&bs);
			klbs_read_set_buffer(&bs, buf, rlen - (rlen - offset)); /* This ensures the entire PES payload is collected */
#if LOCAL_DEBUG
			printf("%s() set bs length to %d bytes\n", __func__, rlen - (rlen - offset));
#endif
			struct ltn_pes_packet_s *pes = ltn_pes_packet_alloc();
			//ssize_t xlen =
			int bitsProcessed = ltn_pes_packet_parse(pes, &bs, ctx->skipDataExtraction);
			if (bitsProcessed && ctx->cb) {
				
				pes->rawBufferLengthBytes = rlen - (rlen - offset);
				pes->rawBuffer = malloc(pes->rawBufferLengthBytes);
				memcpy(pes->rawBuffer, buf, pes->rawBufferLengthBytes);

				if (ctx->orderedOutput) {
					/* Send the PES's to the callback in the correct temporal order,
					 * which compensates for B frames.
					 */
					struct item_s *item = _list_find_oldest(ctx);
					if (item) {
						if (item->pes) {
							/* User owns the lifetime of the object */
							ctx->cb(ctx->userContext, item->pes);
						}
						item->pes = pes;
						if ((pes->PTS + (10 * 90000)) < ctx->lastDeliveredPTS) {
							/* PTS has wrapped. Increment our base so we continue to order the 
							 * list correctly, regardless.
							 */
							ctx->orderedBaseTime += MAX_PTS_VALUE;
						}
						item->correctedPTS = ctx->orderedBaseTime + pes->PTS; /* TODO: handle the wrap */

						/* Now put the current parsed item on the list for future callback */
						xorg_list_del(&item->list);
						_list_insert(ctx, item);
#if LOCAL_DEBUG
						_list_print(ctx);
#endif
						ctx->lastDeliveredPTS = pes->PTS;
					}

				} else {
					ctx->cb(ctx->userContext, pes);
					/* User owns the lifetime of the object */
				}
			} else
			if (bitsProcessed) {
				ltn_pes_packet_dump(pes, "\t");
				ltn_pes_packet_free(pes);
			} else {
#if LOCAL_DEBUG
				printf("skipping, processedbits = %d\n", bitsProcessed);
#endif
			}
		}
		free(buf);
	}

	uint8_t tbuf[16];
	size_t l = rb_read(ctx->rb, (char *)&tbuf[0], sizeof(tbuf));
	if (l == 16) {
		//ltntstools_hexdump(buf, sizeof(tbuf), 16);
	}

#if LOCAL_DEBUG
	printf("%s() ring processing complete, size now %d\n", __func__, rb_used(ctx->rb));
#endif

	return 0; /* Success */
}

ssize_t ltntstools_pes_extractor_write(void *hdl, const uint8_t *pkts, int packetCount)
{
	struct pes_extractor_s *ctx = (struct pes_extractor_s *)hdl;

	int didOverflow;
	for (int i = 0; i < packetCount; i++) {
		const uint8_t *pkt = pkts + (i * 188);
		if (ltntstools_pid(pkt) != ctx->pid)
			continue;

		int offset = 4;
		/* Skip any adaption stuffing */
		if (ltntstools_has_adaption((uint8_t *)pkt)) {
			offset++;
			offset += ltntstools_adaption_field_length(pkt);
		}

		if (ltntstools_payload_unit_start_indicator(pkt) && ctx->appending == 1) {
			ctx->appending = 2;
		}
		if (ltntstools_payload_unit_start_indicator(pkt) && ctx->appending == 0) {
			ctx->appending = 1;
		}

		if (ctx->appending) {
			rb_write_with_state(ctx->rb, (const char *)pkt + offset, 188 - offset, &didOverflow);
		}

		if (ltntstools_payload_unit_start_indicator(pkt) && ctx->appending == 2) {
			/* Process any existing data in the ring. */
			_trimRing(ctx);
			_processRing(ctx);
			ctx->appending = 1;

			/* Now flush the buffer up to the next pes header marker */
			_trimRing(ctx);
		}

	}


	return packetCount;
}
