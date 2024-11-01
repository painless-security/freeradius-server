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
 * @file rlm_ratelimit.c
 * @brief Allow FreeRADIUS to rate limit requests.
 *
 * @copyright 2024 The FreeRADIUS server project
 * @copyright 2024 your name <TODO>
 */
RCSID("$Id$")

#include "fixedds.h"
#include "rlm_ratelimit.h"

typedef Bucket* bucketRef;

static bucketRef add_bucket(rlm_ratelimit_t *inst, RatelimitID id);
static uint64_t current_time_in_sec(void);
static Bucket* get_bucket(rlm_ratelimit_t *inst, RatelimitID id);
static int id_from_request(RatelimitID *id, REQUEST *request, char* buffer, uint bsize);
static void log_ratelimit(RatelimitID id);
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
static bucketRef add_bucket(rlm_ratelimit_t *inst, RatelimitID id) {
	Bucket b;

	b.tokens = inst->tokenmax;
	b.accessed = current_time_in_sec();
	DEBUG("ratelimit: add_bucket() created bucket for ID %s. Total allocated buckets: %d", id.key, ++numbuckets);
	return insert(inst->datastore, b, id);
}

/*
 * update_used_bucket decrements the token count and updates last access time
 */
static void update_used_bucket(Bucket *b) {
	if (!valid_bucket(b)) {
		ERROR("ratelimit: update_used_bucket(): bucket index out of range");
	}
	b->tokens--;
	b->accessed = current_time_in_sec();
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

	nTokens = tokens_to_add(current_time_in_sec() - b->accessed, refreshrate);
	DEBUG("ratelimit: update_bucket_tokens(): nTokens: %d", nTokens);
	toks_to_add = (b->tokens+nTokens <= (uint) maxtokens) ? b->tokens+nTokens : maxtokens;
	b->tokens = toks_to_add;
}

/*
 * tokens_to_add returns the number of tokens to add for the elapse time for the update_period
 */
static uint tokens_to_add(uint64_t elapsed, uint32_t refreshrate) {
	DEBUG("ratelimit: tokens_to_add(): elapsed: %llu refreshrate %d", elapsed, refreshrate);
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

/*
 * get_bucket returns a reference to the token bucket with the specified id. If the id
 * doesn't exist a new bucket is created and a reference to the new bucket is
 * returned.
 */
static Bucket* get_bucket(rlm_ratelimit_t *inst, RatelimitID id) {
	Bucket *b = NULL;

	b = lookup(inst->datastore, id);

	/* bucket for ID doesn't exist. Add one. */
	if (b == NULL) {
		DEBUG("ratelimit: get_bucket(): bucket not found. Adding bucket: %s", id.key);
		b = add_bucket(inst, id);
	}

	DEBUG("ratelimit: getbucket(): %s %d %llu", id.key, b->tokens, b->accessed);
	return b;
}

/*
 * ratelimit_ok returns true if the rate limit for RatelimitID hasn't been exceeded.
 */
static bool ratelimit_ok(rlm_ratelimit_t *inst, RatelimitID id) {
	Bucket *b;

	DEBUG("ratelimit: ratelimit_ok(): checking rate limit for %s", id.key);

	/*
	 * get the bucket for id. Update tokens to account for elapsed time since it
	 * it was last accessed. Return false if the bucket has run out of tokens.
	 */
	b = get_bucket(inst, id);
	update_bucket_tokens(b, inst->tokenmax, inst->refreshrate);
	if (b->tokens <= 0) {
		return false;
	}

	/* the request is within limits - update the bucket and return "OK" (true) */
	update_used_bucket(b);
	return true;
}

/*
 * log_ratelimit logs the ratelimit event for the RatelimitID.
 */
static void log_ratelimit(RatelimitID id) {
	WARN("ratelimit: request id %s ratelimited", id.key);
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
	if (inst->datastore == NULL) {
		return -1;
	}

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
 * Write accounting information to this modules database.
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
	request->simul_count=0;

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

/*
 * id_from_request creates a RatelimitID for the request. The ID is created from the
 * the calling_station_id attribute. If request doesn't contain a calling_station_id
 * the ReatelimitID is created from the request's source IP address, which require
 * the buffer and bsize arguments.
 *
 * Returns 0 on success or -1 on error.
 */
static int id_from_request(RatelimitID *id, REQUEST *request, char *buffer, uint bsize) {
	VALUE_PAIR *vp;
	const char *ip;

	/* create the ID from the calling_station_id if present */
	vp = fr_pair_find_by_num(request->packet->vps, PW_CALLING_STATION_ID, 0, TAG_ANY);
	if (vp) {
		id->key = vp->vp_strvalue;
		id->key_type = MACADDR;
		return 0;
	}

	/* no calling_station_id attribute so fall back to using the src_ip (ipv4 or ipv6) */
	ip = inet_ntop(request->packet->src_ipaddr.af, &request->packet->src_ipaddr.ipaddr, buffer, bsize);
	if (ip) {
		id->key = ip;
		if (request->packet->src_ipaddr.af == AF_INET) {
			id->key_type = IPV4;
		} else if (request->packet->src_ipaddr.af == AF_INET6) {
			id->key_type = IPV6;
		} else {
			id->key_type = NONE;
		}
		return 0;
	}

	return -1;
}

/*
 * Retrieve the calling_station_id from the request and return a RLM_MODULE_REJECT if the
 * request for this session exceeds the rate limit.
 * Return OK/NOP if the request doesn't contain a calling_station_id.
 */
static rlm_rcode_t CC_HINT(nonnull) mod_pre_proxy(void *instance, REQUEST *request) {
	rlm_ratelimit_t *inst = instance;
	int ok;
	RatelimitID id;

    /* retrieve the calling_station_id from the request */
	if (request->packet->code == PW_CODE_ACCESS_REQUEST) {
		char buffer[128];
		ok = id_from_request(&id, request, buffer, sizeof(buffer));
		if (ok == 0) {
			DEBUG("ratelimit: id returned from request: %s", id.key);
			if (!ratelimit_ok(inst, id)) {
				log_ratelimit(id);
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
	.magic		= RLM_MODULE_INIT,
	.name		= "ratelimit",
	.type		= RLM_TYPE_THREAD_SAFE,
	.inst_size	= sizeof(rlm_ratelimit_t),
	.config		= module_config,
	.instantiate	= mod_instantiate,
	.detach		= mod_detach,
	.methods = {
		// [MOD_AUTHENTICATE]	= mod_authenticate,
		// [MOD_AUTHORIZE]		= mod_authorize,
		[MOD_PRE_PROXY]		= mod_pre_proxy,
#ifdef WITH_ACCOUNTING
		[MOD_PREACCT]		= mod_preacct,
		[MOD_ACCOUNTING]	= mod_accounting,
		[MOD_SESSION]		= mod_checksimul
#endif
	},
};

