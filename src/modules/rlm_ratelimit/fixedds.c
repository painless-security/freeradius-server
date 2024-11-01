/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file fixedds.c
 * @brief Fixed length datastore for token bucket storage.
 *
 * @copyright 2024 The FreeRADIUS server project
 * @copyright 2024 your name <TODO>
 */
RCSID("$Id$")

#include "fixedds.h"

static int index_from_id(RatelimitID id, uint32_t *index);

void *datastore_init(uint32_t listlength) {
    BucketList *b;

    INFO("ratelimit: datastore_init(): creating bucket store with capacity %d.", listlength);
	b = talloc(NULL, BucketList);
	b->buckets = (Bucket *) talloc_array_size(b, sizeof(Bucket), listlength);
    INFO("ratelimit: datastore_init(): storage allocated: %ld bytes", talloc_total_size(b));
	for (uint32_t i=0; i<listlength; i++) {
		b->buckets[i].accessed = 0;
	}
	return b;
}

/*
 * insert
 */
Bucket *insert(void *datastore, Bucket data, RatelimitID id) {
    BucketList *list = datastore;
	uint32_t index;

    list = datastore;
    if (index_from_id(id, &index) == -1) {
        return NULL;
    }

    DEBUG("ratelimit: insert(): id %s inserted at index %d", id.key, index);

    list->buckets[index].tokens = data.tokens;
    list->buckets[index].accessed = data.accessed;
    return &(list->buckets[index]);
}

/*
 * lookup returns the entry in the datastore with the given id or NULL if the datastore
 * doesn't contain an entry for id.
 */
Bucket *lookup(void *datastore, RatelimitID id) {
    BucketList *list;
	uint32_t index;

    list = datastore;

	if (index_from_id(id, &index) == -1) {
        WARN("ratelimit: lookup(): bucket not found for id %s", id.key);
        return NULL;
    }
    return &(list->buckets[index]);
}

/*
 * index_from_id return the left most 24 bit from mac address
 * TODO: pass in type to do a switch on.
 */
static int index_from_id(RatelimitID id, uint32_t *index) {
    uint values[8];
    uint64_t int_val;

    switch (id.key_type) {
        case MACADDR:
            /* Contains 6 components - use the last 3 octets (extension ID) as the index */
            if (6 == sscanf(id.key, "%x:%x:%x:%x:%x:%x",
                &values[0], &values[1], &values[2], &values[3], &values[4], &values[5])) {
                int_val = values[3] << 16 | values[4] << 8 | values[5];
                (*index) = int_val;
                return 0;
            }
            break;
        case IPV4:
            /* Contains 4 components - use the last 3 octets for the index. */
            if (4 == sscanf(id.key, "%d.%d.%d.%d", &values[0], &values[1], &values[2], &values[3])) {
                int_val = values[1] << 16 | values[2] << 8 | values[3];
                (*index) = int_val;
                return 0;
            }
            break;
        case IPV6:
            /* Contains 8 octets - use last 3 octets for the index. */
            if (8 == sscanf(id.key, "%x:%x:%x:%x:%x:%x:%x:%x",
                &values[0], &values[1], &values[2], &values[3], &values[4], &values[5], &values[6], &values[7])) {
                int_val = values[5] << 16 | values[6] << 8 | values[7];
                (*index) = int_val;
                return 0;
            }
            break;
        case NONE:
            break;
    }

    INFO("ratelimit: index_from_id(): invalid id %s", id.key);
    return -1;
}
