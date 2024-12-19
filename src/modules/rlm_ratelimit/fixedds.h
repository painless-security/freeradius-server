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
  * @file fixedds.h
  * @brief Fixed length datastore for token bucket storage.
  *
  * @copyright 2024 The FreeRADIUS server project
  * @copyright 2024 your name \<your address\>
  */
RCSIDH(fixedds_h, "$Id$")

#include <freeradius-devel/radiusd.h>

enum IDType { NONE, MACADDR, IPV4, IPV6 };

typedef struct RatelimitID {
	const char *key;
	enum IDType key_type;
} RatelimitID;

typedef struct Bucket {
	uint8_t *ntokens;
	uint64_t *lastaccessed;
	uint64_t *lastlogged;
} Bucket;

typedef struct BucketList {
	uint8_t *tokens;
	uint64_t *accessed;
	uint64_t *lastlogged;
} BucketList;

typedef int32_t BucketRef;

void *datastore_init(uint32_t listlength);
Bucket *insert(Bucket *buffer, void *datastore, Bucket data, RatelimitID id);
Bucket *lookup(Bucket *buffer, void *datastore, RatelimitID id);
