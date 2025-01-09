/*
 *   This program is free software; you can redistribute it and/or modify
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
 * @file rlm_ratelimit.c
 * @brief Allow FreeRADIUS to rate limit requests.
 *
 * @copyright 2024 The FreeRADIUS server project
 * @copyright 2024 your name <TODO>
 */
RCSID("$Id$")

#include "fixedds.h"
#include "rlm_ratelimit.h"

static Bucket *add_bucket(Bucket *buffer, rlm_ratelimit_t *inst, RatelimitID id);
static uint64_t current_time_in_sec(void);
static Bucket *get_bucket(Bucket *buffer, rlm_ratelimit_t *inst, RatelimitID id);
static int id_from_request(RatelimitID *id, const REQUEST *request);
static void log_ratelimit(Bucket *b, RatelimitID id, uint32_t lograte);
static void *ratelimit_init_datastore(rlm_ratelimit_t *instance);
static bool ratelimit_ok(rlm_ratelimit_t *inst, RatelimitID id);
static uint tokens_to_add(uint64_t elapsed, uint32_t refreshrate);
static bool valid_bucket(Bucket *b);
static void update_bucket_tokens(Bucket *b, uint32_t maxtokens, uint32_t refreshrate);
static void update_used_bucket(Bucket *b);

static uint numbuckets; /* TODO: used for debugging. Remove? */


/*
 * ratelimit_init_datastore calls hashtable_init to create and return a backend datastore.
 */
static void *ratelimit_init_datastore(rlm_ratelimit_t *instance) {
	INFO("ratelimit: using a fix size array");
	return datastore_init(instance->datastoresize);
}

/*
 * add_bucket creates a new CSID token bucket, insert it into the datastore and returns a reference to it.
 */
static Bucket *add_bucket(Bucket *buffer, rlm_ratelimit_t *inst, RatelimitID id) {
	Bucket b;

	b.ntokens = inst->tokenmax;
	b.lastaccessed = current_time_in_sec();
	DEBUG("ratelimit: add_bucket() created bucket for ID %s. Total allocated buckets: %d", id.key, ++numbuckets);
	return insert(buffer, inst->datastore, b, id);
}

/*
 * update_used_bucket decrements the token count and updates last access time
 */
static void update_used_bucket(Bucket *b) {
	if (!valid_bucket(b)) {
		ERROR("ratelimit: update_used_bucket(): bucket index out of range");
	}

	(*(b->ntokens))--;
	*(b->lastaccessed) = current_time_in_sec();
}

/*
 * update_bucket_tokens determines if the bucket's tokens can be replenished. If so,
 * it is replenished up to, but not exceeding, TOKENMAX.
 */
static void update_bucket_tokens(Bucket *b, uint32_t maxtokens, uint32_t refreshrate) {
	uint nTokens;
	uint32_t toks_to_add;

	if (!valid_bucket(b)) {
		ERROR("ratelimit: update_bucket_tokens(): invalid bucket");
		return;
	}

	nTokens = tokens_to_add(current_time_in_sec() - *(b)->lastaccessed, refreshrate);
	toks_to_add = (*(b)->ntokens + nTokens <= (uint)maxtokens) ? *(b)->ntokens + nTokens : maxtokens;
	*(b)->ntokens = toks_to_add;
}

/*
 * tokens_to_add returns the number of tokens to add for the elapse time for the update_period
 */
static uint tokens_to_add(uint64_t elapsed, uint32_t refreshrate) {
	DEBUG("ratelimit: tokens_to_add(): elapsed: %lu refreshrate %d", elapsed, refreshrate);
	return elapsed / refreshrate;
}

/*
 *  valid_bucket return true if the bucketRef is valid
 */
static bool valid_bucket(Bucket *b) {
	if (b != NULL) {
		return true;
	}
	return false;
}

/*
 * current_time_in_sec returns the current time in seconds since UNIX Epoch.
 */
static uint64_t current_time_in_sec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec;
}

/** returns a point to the bucket with the specified id. If a bucket with id doesn't exist
 *  a new bucket is created and a reference to the new bucket is returned.
 *
 * @param[out] buffer	Where the bucket is written.
 * @param[in] inst		This session's rate limit instance data.
 * @param[in] id		The id of the bucket (existing or new).
 * @return
 *		- a pointer to bucket.
 */
static Bucket *get_bucket(Bucket *buffer, rlm_ratelimit_t *inst, const RatelimitID id) {
	Bucket *b = NULL;

	b = lookup(buffer, inst->datastore, id);

	/* bucket for id doesn't exist. Add one. */
	if (b == NULL) {
		DEBUG("ratelimit: get_bucket(): bucket not found. Adding bucket: %s", id.key);
		b = add_bucket(buffer, inst, id);
		INFO("ratelimit: after add_bucket tokens %d", *(b->ntokens));
	}

	DEBUG("ratelimit: get_bucket(): %s %d %lu %lu", id.key, *(b->ntokens), *(b->lastaccessed), *(b->lastlogged));
	return b;
}

/** Check if the rate limit for RatelimitID has been exceeded.
 *
 * @param[in] inst	This session's rate limit instance data.
 * @param[in] id	The RatelimitID of the incoming request to check.
 * @return
 *		- true if the rate limit for the request hasn't been exceeded.
 *		- false if the rate limit for the request has been exceeded.
 */
static bool ratelimit_ok(rlm_ratelimit_t *inst, const RatelimitID id) {
	Bucket b;
	Bucket buffer;

	DEBUG("ratelimit: ratelimit_ok(): checking rate limit for %s", id.key);

	/*
	 * get the bucket for id. Update tokens to account for elapsed time since it
	 * was last accessed. Return false if the bucket has run out of tokens.
	 */
	b = *get_bucket(&buffer, inst, id);
	update_bucket_tokens(&b, inst->tokenmax, inst->refreshrate);
	if (*(b.ntokens) <= 0) {
		log_ratelimit(&b, id, inst->lograte);
		return false;
	}

	/* the request is within limits - update the bucket and return "OK" (true) */
	update_used_bucket(&b);
	return true;
}

/*
 * log_ratelimit logs the ratelimit event for the RatelimitID. A log is written
 * for the id if it is lograte seconds since it was last logged.
 */
static void log_ratelimit(Bucket *b, RatelimitID id, uint32_t lograte) {
	uint64_t now = current_time_in_sec();
	if (*(b->lastlogged) + lograte <= current_time_in_sec()) {
		WARN("ratelimit: request id %s for client %s ratelimited", id.key, id.client_ip_address);
		*(b->lastlogged) = now;
	}
}

/*
 * If configuration information is given in the config section
 * that must be referenced in later calls, store a handle to it
 * in *instance otherwise put a null pointer there.
 */
static int mod_instantiate(UNUSED CONF_SECTION *conf, void *instance) {
	rlm_ratelimit_t *inst = instance;

	/* trivial sanity check on config values passed in */
	rad_assert(inst->datastoresize > 0);
	rad_assert(inst->tokenmax > 0);
	rad_assert(inst->refreshrate > 0);

	inst->datastore = ratelimit_init_datastore(inst);
	rad_assert(inst->datastore != NULL);

	return 0;
}

#ifdef WITH_ACCOUNTING
/*
 * Massage the request before recording it or proxying it
 */
static rlm_rcode_t CC_HINT(nonnull) mod_preacct(UNUSED void *instance, UNUSED REQUEST *request) {
	return RLM_MODULE_OK;
}

/*
 * Write accounting information to this module's database.
 */
static rlm_rcode_t CC_HINT(nonnull) mod_accounting(UNUSED void *instance, UNUSED REQUEST *request) {
	return RLM_MODULE_OK;
}

/*
 * See if a user is already logged in. Sets request->simul_count to the
 * current session count for this user and sets request->simul_mpp to 2
 * if it looks like a multilink attempt based on the requested IP
 * address, otherwise leaves request->simul_mpp alone.
 *
 * Check twice. If on the first pass the user exceeds his
 * max. number of logins, do a second pass and validate all
 * logins by querying the terminal server (using eg. SNMP).
 */
static rlm_rcode_t CC_HINT(nonnull) mod_checksimul(UNUSED void *instance, REQUEST *request) {
	request->simul_count = 0;

	return RLM_MODULE_OK;
}
#endif

/*
 * Only free memory we allocated.  The strings allocated via
 * cf_section_parse() do not need to be freed.
 */
static int mod_detach(void *instance) {
	rlm_ratelimit_t *inst = instance;

	talloc_free(inst->datastore);

	/*
	 * We need to explicitly free all children, so if the driver
	 * parented any memory off the instance, their destructors
	 * run before we unload the bytecode for them.
	 *
	 * If we don't do this, we get a SEGV deep inside the talloc code
	 * when it tries to call a destructor that no longer exists.
	 */
	talloc_free_children(inst);

	return 0;
}

/** Creates a RatelimitID for the request. The ID is created from the calling_station_id attribute. If the
 *  request doesn't contain a calling_station_id the ID is created from the request's source IP address.
 *
 * @param[out] id		Where the RatelimitID is written.
 * @param[in] request	The request to parse.
 * @return
 *		- 0 on success
 *		- -1 on failure
 */
static int CC_HINT(nonnull) id_from_request(RatelimitID *id, const REQUEST *request) {
	const VALUE_PAIR *vp;
	const char *ip;

	/* Set the default key type to NONE to indicate that we don't have one yet */
	id->key_type = NONE;

	/* store the src_ip (ipv4 or ipv6) */
	ip = inet_ntop(request->packet->src_ipaddr.af,
	        &request->packet->src_ipaddr.ipaddr,
	        id->client_ip_address,
	        INET6_ADDRSTRLEN);
	if (ip) {
		if (request->packet->src_ipaddr.af == AF_INET) {
			id->key_type = IPV4;
		} else if (request->packet->src_ipaddr.af == AF_INET6) {
			id->key_type = IPV6;
		}
	}

	/* create the ID from the calling_station_id if present */
	vp = fr_pair_find_by_num(request->packet->vps, PW_CALLING_STATION_ID, 0, TAG_ANY);
	if (vp) {
		strlcpy(id->key, vp->vp_strvalue, sizeof(id->key));
		id->key_type = MACADDR;
		return 0;
	} else if (NONE != id->key_type) {
		strlcpy(id->key, id->client_ip_address, sizeof(id->key));
		return 0;
	}

	return -1;
}

/** Checks if the incoming request should be rate limited.
 *
 * @param[in] instance	This session's instance data.
 * @param[in] request	The incoming request.
 * @return
 *		- RLM_MODULE_OK if the request isn't rate limited or doesn't contain a calling_station_id.
 *		- RLM_MODULE_REJECT if the request for this session exceeds the rate limit.
 */
static rlm_rcode_t CC_HINT(nonnull) mod_pre_proxy(void *instance, REQUEST *request) {
	rlm_ratelimit_t *inst = instance;
	RatelimitID id;

	/* retrieve the calling_station_id from the request */
	if (request->packet->code == PW_CODE_ACCESS_REQUEST) {
		const int ok = id_from_request(&id, request);
		if (ok == 0) {
			DEBUG("ratelimit: id returned from request: %s", id.key);
			if (!ratelimit_ok(inst, id)) {
				return RLM_MODULE_REJECT;
			}
		} else {
			WARN("ratelimit: neither calling_station_id nor client_IP contained in the request");
		}
	}

	/* TODO: should this be a RLM_MODULE_NOP? */
	return RLM_MODULE_OK;
}

/*
 * The module name should be the only globally exported symbol.
 * That is, everything else should be 'static'.
 *
 * If the module needs to temporarily modify it's instantiation
 * data, the type should be changed to RLM_TYPE_THREAD_UNSAFE.
 * The server will then take care of ensuring that the module
 * is single-threaded.
 */
extern module_t rlm_ratelimit;
module_t rlm_ratelimit = {
	.magic = RLM_MODULE_INIT,
	.name = "ratelimit",
	.type = RLM_TYPE_THREAD_SAFE,
	.inst_size = sizeof(rlm_ratelimit_t),
	.config = module_config,
	.instantiate = mod_instantiate,
	.detach = mod_detach,
	.methods = {
		[MOD_PRE_PROXY] = mod_pre_proxy,
#ifdef WITH_ACCOUNTING
		[MOD_PREACCT] = mod_preacct,
		[MOD_ACCOUNTING] = mod_accounting,
		[MOD_SESSION] = mod_checksimul
#endif
	},
};

