
#include <stdio.h>
#include "libltntstools/ltntstools.h"

int ltntstools_descriptor_list_add(struct ltntstools_descriptor_list_s *list,
	uint8_t tag, uint8_t *src, uint8_t lengthBytes)
{
	if (list->count == LTNTSTOOLS_DESCRIPTOR_ENTRIES_MAX)
		return -1;

	if (lengthBytes > 256)
		return -1;

	if (!src)
		return -1;

	if (!list)
		return -1;

	struct ltntstools_descriptor_entry_s *d = &list->array[list->count];

	d->tag = tag;
	d->len = lengthBytes;
	memcpy(&d->data[0], src, d->len);
	
	list->count++;
	return 0;
}

int ltntstools_descriptor_list_contains_scte35_cue_registration(struct ltntstools_descriptor_list_s *list)
{
	int found = 0;

	for (int i = 0; i < list->count; i++) {
		struct ltntstools_descriptor_entry_s *d = &list->array[i];

		if (d->tag == 0x05 && d->len == 0x04) {
			if (d->data[0] == 'C' && d->data[1] == 'U' && d->data[2] == 'E' && d->data[3] == 'I') {
				found = 1;
				break;
			}
		}
	}

	return found;
}

int ltntstools_descriptor_list_contains_ltn_encoder_sw_version(struct ltntstools_descriptor_list_s *list,
	unsigned int *major, unsigned int *minor, unsigned int *patch)
{
	int found = 0;

	for (int i = 0; i < list->count; i++) {
		struct ltntstools_descriptor_entry_s *d = &list->array[i];

		/* Look for tag a2 length 4 and embedded tag 1 (sw version) */

		if (d->tag == 0xa2 && d->len == 0x04 && d->data[0] == 0x01) {
			*major = d->data[1];
			*minor = d->data[2];
			*patch = d->data[3];
			found = 1;
			break;
		}
	}

	return found;
}
