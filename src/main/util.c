/*
 * util.c	Various utility functions.
 *
 * Version:     $Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000,2006  The FreeRADIUS server project
 */

RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/rad_assert.h>

#include <ctype.h>

#include <sys/stat.h>
#include <fcntl.h>

/*
 *	The signal() function in Solaris 2.5.1 sets SA_NODEFER in
 *	sa_flags, which causes grief if signal() is called in the
 *	handler before the cause of the signal has been cleared.
 *	(Infinite recursion).
 *
 *	The same problem appears on HPUX, so we avoid it, if we can.
 *
 *	Using sigaction() to reset the signal handler fixes the problem,
 *	so where available, we prefer that solution.
 */

void (*reset_signal(int signo, void (*func)(int)))(int)
{
#ifdef HAVE_SIGACTION
	struct sigaction act, oact;

	memset(&act, 0, sizeof(act));
	act.sa_handler = func;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
#ifdef  SA_INTERRUPT		/* SunOS */
	act.sa_flags |= SA_INTERRUPT;
#endif
	if (sigaction(signo, &act, &oact) < 0)
		return SIG_ERR;
	return oact.sa_handler;
#else

	/*
	 *	re-set by calling the 'signal' function, which
	 *	may cause infinite recursion and core dumps due to
	 *	stack growth.
	 *
	 *	However, the system is too dumb to implement sigaction(),
	 *	so we don't have a choice.
	 */
	signal(signo, func);

	return NULL;
#endif
}

/*
 *	Per-request data, added by modules...
 */
struct request_data_t {
	request_data_t	*next;

	void		*unique_ptr;
	int		unique_int;
	void		*opaque;
	bool		free_opaque;
};

/*
 *	Add opaque data (with a "free" function) to a REQUEST.
 *
 *	The unique ptr is meant to be a module configuration,
 *	and the unique integer allows the caller to have multiple
 *	opaque data associated with a REQUEST.
 */
int request_data_add(REQUEST *request,
		     void *unique_ptr, int unique_int,
		     void *opaque, bool free_opaque)
{
	request_data_t *this, **last, *next;

	/*
	 *	Some simple sanity checks.
	 */
	if (!request || !opaque) return -1;

	this = next = NULL;
	for (last = &(request->data); *last != NULL; last = &((*last)->next)) {
		if (((*last)->unique_ptr == unique_ptr) &&
		    ((*last)->unique_int == unique_int)) {
			this = *last;

			next = this->next;

			/*
			 *	If caller requires custom behaviour on free
			 *	they must set a destructor.
			 */
			if (this->opaque && this->free_opaque) {
				talloc_free(this->opaque);
			}
			break;				/* replace the existing entry */
		}
	}

	if (!this) this = talloc_zero(request, request_data_t);

	this->next = next;
	this->unique_ptr = unique_ptr;
	this->unique_int = unique_int;
	this->opaque = opaque;
	if (free_opaque) {
		this->free_opaque = free_opaque;
	}

	*last = this;

	return 0;
}


/*
 *	Get opaque data from a request.
 */
void *request_data_get(REQUEST *request,
		       void *unique_ptr, int unique_int)
{
	request_data_t **last;

	if (!request) return NULL;

	for (last = &(request->data); *last != NULL; last = &((*last)->next)) {
		if (((*last)->unique_ptr == unique_ptr) &&
		    ((*last)->unique_int == unique_int)) {
			request_data_t *this = *last;
			void *ptr = this->opaque;

			/*
			 *	Remove the entry from the list, and free it.
			 */
			*last = this->next;
			talloc_free(this);
			return ptr; 		/* don't free it, the caller does that */
		}
	}

	return NULL;		/* wasn't found, too bad... */
}


/*
 *	Get opaque data from a request without removing it.
 */
void *request_data_reference(REQUEST *request,
		       void *unique_ptr, int unique_int)
{
	request_data_t **last;

	for (last = &(request->data); *last != NULL; last = &((*last)->next)) {
		if (((*last)->unique_ptr == unique_ptr) &&
		    ((*last)->unique_int == unique_int)) {
			request_data_t *this = *last;
			void *ptr = this->opaque;

			return ptr;
		}
	}

	return NULL;		/* wasn't found, too bad... */
}

/*
 *	Create possibly many directories.
 *
 *	Note that the input directory name is NOT a constant!
 *	This is so that IF an error is returned, the 'directory' ptr
 *	points to the name of the file which caused the error.
 */
int rad_mkdir(char *directory, mode_t mode)
{
	int rcode;
	char *p;

	/*
	 *	Try to make the directory.  If it exists, chmod it.
	 *	If a path doesn't exist, that's OK.  Otherwise
	 *	return with an error.
	 */
	rcode = mkdir(directory, mode & 0777);
	if (rcode < 0) {
		if (errno == EEXIST) {
			return chmod(directory, mode);
		}

		if (errno != ENOENT) {
			return rcode;
		}

		/*
		 *	A component in the directory path doesn't
		 *	exist.  Look for the LAST directory name.  Try
		 *	to create that.  If there's an error, we leave
		 *	the directory path as the one at which the
		 *	error occured.
		 */
		p = strrchr(directory, FR_DIR_SEP);
		if (!p || (p == directory)) return -1;

		*p = '\0';
		rcode = rad_mkdir(directory, mode);
		if (rcode < 0) return rcode;

		/*
		 *	Reset the directory path, and try again to
		 *	make the directory.
		 */
		*p = FR_DIR_SEP;
		rcode = mkdir(directory, mode & 0777);
		if (rcode < 0) return rcode;
	} /* else we successfully created the directory */

	return chmod(directory, mode);
}


/*
 *	Allocate memory, or exit.
 *
 *	This call ALWAYS succeeds!
 */
void *rad_malloc(size_t size)
{
	void *ptr = malloc(size);

	if (ptr == NULL) {
		ERROR("no memory");
		fr_exit(1);
	}

	return ptr;
}


void rad_const_free(void const *ptr)
{
	void *tmp;
	if (!ptr) return;

	memcpy(&tmp, &ptr, sizeof(tmp));
	talloc_free(tmp);
}

char *rad_ajoin(TALLOC_CTX *ctx, char const **array, char c)
{
	char const **array_p;
	char *buff, *p;
	size_t len = 0, left, wrote;

	if (!*array) {
		goto null;
	}

	for (array_p = array; *array_p; array_p++) {
		len += (strlen(*array_p) + 1);
	}

	if (!len) {
		null:
		return talloc_zero_array(ctx, char, 1);
	}

	left = len + 1;
	buff = p = talloc_zero_array(ctx, char, left);
	for (array_p = array; *array_p; array_p++) {
		wrote = snprintf(p, left, "%s%c", *array_p, c);
		left -= wrote;
		p += wrote;
	}
	buff[len - 1] = '\0';

	return buff;
}

/*
 *	Logs an error message and aborts the program
 *
 */

void NEVER_RETURNS rad_assert_fail(char const *file, unsigned int line, char const *expr)
{
	ERROR("ASSERT FAILED %s[%u]: %s", file, line, expr);
	fr_fault(SIGABRT);
	fr_exit_now(1);
}

/*
 *	Free a REQUEST struct.
 */
static int _request_free(REQUEST *request)
{
	rad_assert(!request->in_request_hash);
#ifdef WITH_PROXY
	rad_assert(!request->in_proxy_hash);
#endif
	rad_assert(!request->ev);

#ifdef WITH_COA
	if (request->coa) {
		request->coa->parent = NULL;
	}

	if (request->parent && (request->parent->coa == request)) {
		request->parent->coa = NULL;
	}
#endif

#ifndef NDEBUG
	request->magic = 0x01020304;	/* set the request to be nonsense */
#endif
	request->client = NULL;
#ifdef WITH_PROXY
	request->home_server = NULL;
#endif

	return 0;
}

/*
 *	Create a new REQUEST data structure.
 */
REQUEST *request_alloc(TALLOC_CTX *ctx)
{
	REQUEST *request;

	request = talloc_zero(ctx, REQUEST);
	talloc_set_destructor(request, _request_free);
#ifndef NDEBUG
	request->magic = REQUEST_MAGIC;
#endif
#ifdef WITH_PROXY
	request->proxy = NULL;
#endif
	request->reply = NULL;
#ifdef WITH_PROXY
	request->proxy_reply = NULL;
#endif
	request->config_items = NULL;
	request->username = NULL;
	request->password = NULL;
	request->timestamp = time(NULL);
	request->log.lvl = debug_flag; /* Default to global debug level */

	request->module = "";
	request->component = "<core>";
	request->log.func = vradlog_request;

	return request;
}


/*
 *	Create a new REQUEST, based on an old one.
 *
 *	This function allows modules to inject fake requests
 *	into the server, for tunneled protocols like TTLS & PEAP.
 */
REQUEST *request_alloc_fake(REQUEST *request)
{
	REQUEST *fake;

	fake = request_alloc(request);

	fake->number = request->number;
#ifdef HAVE_PTHREAD_H
	fake->child_pid = request->child_pid;
#endif
	fake->parent = request;
	fake->root = request->root;
	fake->client = request->client;

	/*
	 *	For new server support.
	 *
	 *	FIXME: Key instead off of a "virtual server" data structure.
	 *
	 *	FIXME: Permit different servers for inner && outer sessions?
	 */
	fake->server = request->server;

	fake->packet = rad_alloc(fake, true);
	if (!fake->packet) {
		talloc_free(fake);
		return NULL;
	}

	fake->reply = rad_alloc(fake, false);
	if (!fake->reply) {
		talloc_free(fake);
		return NULL;
	}

	fake->master_state = REQUEST_ACTIVE;
	fake->child_state = REQUEST_RUNNING;

	/*
	 *	Fill in the fake request.
	 */
	fake->packet->sockfd = -1;
	fake->packet->src_ipaddr = request->packet->src_ipaddr;
	fake->packet->src_port = request->packet->src_port;
	fake->packet->dst_ipaddr = request->packet->dst_ipaddr;
	fake->packet->dst_port = 0;

	/*
	 *	This isn't STRICTLY required, as the fake request MUST NEVER
	 *	be put into the request list.  However, it's still reasonable
	 *	practice.
	 */
	fake->packet->id = fake->number & 0xff;
	fake->packet->code = request->packet->code;
	fake->timestamp = request->timestamp;
	fake->packet->timestamp = request->packet->timestamp;

	/*
	 *	Required for new identity support
	 */
	fake->listener = request->listener;

	/*
	 *	Fill in the fake reply, based on the fake request.
	 */
	fake->reply->sockfd = fake->packet->sockfd;
	fake->reply->src_ipaddr = fake->packet->dst_ipaddr;
	fake->reply->src_port = fake->packet->dst_port;
	fake->reply->dst_ipaddr = fake->packet->src_ipaddr;
	fake->reply->dst_port = fake->packet->src_port;
	fake->reply->id = fake->packet->id;
	fake->reply->code = 0; /* UNKNOWN code */

	/*
	 *	Copy debug information.
	 */
	memcpy(&(fake->log), &(request->log), sizeof(fake->log));

	return fake;
}

#ifdef WITH_COA
REQUEST *request_alloc_coa(REQUEST *request)
{
	if (!request || request->coa) return NULL;

	/*
	 *	Originate CoA requests only when necessary.
	 */
	if ((request->packet->code != PW_CODE_ACCESS_REQUEST) &&
	    (request->packet->code != PW_CODE_ACCOUNTING_REQUEST)) return NULL;

	request->coa = request_alloc_fake(request);
	if (!request->coa) return NULL;

	request->coa->packet->code = 0; /* unknown, as of yet */
	request->coa->child_state = REQUEST_RUNNING;
	request->coa->proxy = rad_alloc(request->coa, false);
	if (!request->coa->proxy) {
		TALLOC_FREE(request->coa);
		return NULL;
	}

	return request->coa;
}
#endif

/*
 *	Copy a quoted string.
 */
int rad_copy_string(char *to, char const *from)
{
	int length = 0;
	char quote = *from;

	do {
		if (*from == '\\') {
			*(to++) = *(from++);
			length++;
		}
		*(to++) = *(from++);
		length++;
	} while (*from && (*from != quote));

	if (*from != quote) return -1; /* not properly quoted */

	*(to++) = quote;
	length++;
	*to = '\0';

	return length;
}

/*
 *	Copy a quoted string but without the quotes. The length
 *	returned is the number of chars written; the number of
 *	characters consumed is 2 more than this.
 */
int rad_copy_string_bare(char *to, char const *from)
{
	int length = 0;
	char quote = *from;

	from++;
	while (*from && (*from != quote)) {
		if (*from == '\\') {
			*(to++) = *(from++);
			length++;
		}
		*(to++) = *(from++);
		length++;
	}

	if (*from != quote) return -1; /* not properly quoted */

	*to = '\0';

	return length;
}


/*
 *	Copy a %{} string.
 */
int rad_copy_variable(char *to, char const *from)
{
	int length = 0;
	int sublen;

	*(to++) = *(from++);
	length++;

	while (*from) {
		switch (*from) {
		case '"':
		case '\'':
			sublen = rad_copy_string(to, from);
			if (sublen < 0) return sublen;
			from += sublen;
			to += sublen;
			length += sublen;
			break;

		case '}':	/* end of variable expansion */
			*(to++) = *(from++);
			*to = '\0';
			length++;
			return length; /* proper end of variable */

		case '\\':
			*(to++) = *(from++);
			*(to++) = *(from++);
			length += 2;
			break;

		case '%':	/* start of variable expansion */
			if (from[1] == '{') {
				*(to++) = *(from++);
				length++;

				sublen = rad_copy_variable(to, from);
				if (sublen < 0) return sublen;
				from += sublen;
				to += sublen;
				length += sublen;
				break;
			} /* else FIXME: catch %%{ ?*/

			/* FALL-THROUGH */
		default:
			*(to++) = *(from++);
			length++;
			break;
		}
	} /* loop over the input string */

	/*
	 *	We ended the string before a trailing '}'
	 */

	return -1;
}

#ifndef USEC
#define USEC 1000000
#endif

uint32_t rad_pps(uint32_t *past, uint32_t *present, time_t *then, struct timeval *now)
{
	uint32_t pps;

	if (*then != now->tv_sec) {
		*then = now->tv_sec;
		*past = *present;
		*present = 0;
	}

	/*
	 *	Bootstrap PPS by looking at a percentage of
	 *	the previous PPS.  This lets us take a moving
	 *	count, without doing a moving average.  If
	 *	we're a fraction "f" (0..1) into the current
	 *	second, we can get a good guess for PPS by
	 *	doing:
	 *
	 *	PPS = pps_now + pps_old * (1 - f)
	 *
	 *	It's an instantaneous measurement, rather than
	 *	a moving average.  This will hopefully let it
	 *	respond better to sudden spikes.
	 *
	 *	Doing the calculations by thousands allows us
	 *	to not overflow 2^32, AND to not underflow
	 *	when we divide by USEC.
	 */
	pps = USEC - now->tv_usec; /* useconds left in previous second */
	pps /= 1000;		   /* scale to milliseconds */
	pps *= *past;		   /* multiply by past count to get fraction */
	pps /= 1000;		   /* scale to usec again */
	pps += *present;	   /* add in current count */

	return pps;
}

/** Split string into words and expand each one
 *
 * @param request Current request.
 * @param cmd string to split.
 * @param max_argc the maximum number of arguments to split into.
 * @param argv Where to write the pointers into argv_buf.
 * @param can_fail If false, stop processing if any of the xlat expansions fail.
 * @param argv_buflen size of argv_buf.
 * @param argv_buf temporary buffer we used to mangle/expand cmd.
 *	Pointers to offsets of this buffer will be written to argv.
 * @return argc or -1 on failure.
 */

int rad_expand_xlat(REQUEST *request, char const *cmd,
		    int max_argc, char *argv[], bool can_fail,
		    size_t argv_buflen, char *argv_buf)
{
	char const *from;
	char *to;
	int argc = -1;
	int i;
	int left;

	if (strlen(cmd) > (argv_buflen - 1)) {
		ERROR("rad_expand_xlat: Command line is too long");
		return -1;
	}

	/*
	 *	Check for bad escapes.
	 */
	if (cmd[strlen(cmd) - 1] == '\\') {
		ERROR("rad_expand_xlat: Command line has final backslash, without a following character");
		return -1;
	}

	strlcpy(argv_buf, cmd, argv_buflen);

	/*
	 *	Split the string into argv's BEFORE doing radius_xlat...
	 */
	from = cmd;
	to = argv_buf;
	argc = 0;
	while (*from) {
		int length;

		/*
		 *	Skip spaces.
		 */
		if ((*from == ' ') || (*from == '\t')) {
			from++;
			continue;
		}

		argv[argc] = to;
		argc++;

		if (argc >= (max_argc - 1)) break;

		/*
		 *	Copy the argv over to our buffer.
		 */
		while (*from && (*from != ' ') && (*from != '\t')) {
			if (to >= argv_buf + argv_buflen - 1) {
				ERROR("rad_expand_xlat: Ran out of space in command line");
				return -1;
			}

			switch (*from) {
			case '"':
			case '\'':
				length = rad_copy_string_bare(to, from);
				if (length < 0) {
					ERROR("rad_expand_xlat: Invalid string passed as argument");
					return -1;
				}
				from += length+2;
				to += length;
				break;

			case '%':
				if (from[1] == '{') {
					*(to++) = *(from++);

					length = rad_copy_variable(to, from);
					if (length < 0) {
						ERROR("rad_expand_xlat: Invalid variable expansion passed as argument");
						return -1;
					}
					from += length;
					to += length;
				} else { /* FIXME: catch %%{ ? */
					*(to++) = *(from++);
				}
				break;

			case '\\':
				if (from[1] == ' ') from++;
				/* FALL-THROUGH */

			default:
				*(to++) = *(from++);
			}
		} /* end of string, or found a space */

		*(to++) = '\0';	/* terminate the string */
	}

	/*
	 *	We have to have SOMETHING, at least.
	 */
	if (argc <= 0) {
		ERROR("rad_expand_xlat: Empty command line");
		return -1;
	}

	/*
	 *	Expand each string, as appropriate.
	 */
	left = argv_buf + argv_buflen - to;
	for (i = 0; i < argc; i++) {
		int sublen;

		/*
		 *	Don't touch argv's which won't be translated.
		 */
		if (strchr(argv[i], '%') == NULL) continue;

		if (!request) continue;

		sublen = radius_xlat(to, left - 1, request, argv[i], NULL, NULL);
		if (sublen <= 0) {
			if (can_fail) {
				/*
				 *	Fail to be backwards compatible.
				 *
				 *	It's yucky, but it won't break anything,
				 *	and it won't cause security problems.
				 */
				sublen = 0;
			} else {
				ERROR("rad_expand_xlat: xlat failed");
				return -1;
			}
		}

		argv[i] = to;
		to += sublen;
		*(to++) = '\0';
		left -= sublen;
		left--;

		if (left <= 0) {
			ERROR("rad_expand_xlat: Ran out of space while expanding arguments");
			return -1;
		}
	}
	argv[argc] = NULL;

	return argc;
}

const FR_NAME_NUMBER pair_lists[] = {
	{ "request",		PAIR_LIST_REQUEST },
	{ "reply",		PAIR_LIST_REPLY },
	{ "control",		PAIR_LIST_CONTROL },		/* New name should have priority */
	{ "config",		PAIR_LIST_CONTROL },
#ifdef WITH_PROXY
	{ "proxy-request",	PAIR_LIST_PROXY_REQUEST },
	{ "proxy-reply",	PAIR_LIST_PROXY_REPLY },
#endif
#ifdef WITH_COA
	{ "coa",		PAIR_LIST_COA },
	{ "coa-reply",		PAIR_LIST_COA_REPLY },
	{ "disconnect",		PAIR_LIST_DM },
	{ "disconnect-reply",	PAIR_LIST_DM_REPLY },
#endif
	{  NULL , -1 }
};

const FR_NAME_NUMBER request_refs[] = {
	{ "outer",		REQUEST_OUTER },
	{ "current",		REQUEST_CURRENT },
	{ "parent",		REQUEST_PARENT },
	{  NULL , -1 }
};


/** Resolve attribute name to a list.
 *
 * Check the name string for qualifiers that specify a list and return
 * an pair_lists_t value for that list. This value may be passed to
 * radius_list, along with the current request, to get a pointer to the
 * actual list in the request.
 *
 * If qualifiers were consumed, write a new pointer into name to the
 * char after the last qualifier to be consumed.
 *
 * radius_list_name should be called before passing a name string that
 * may contain qualifiers to dict_attrbyname.
 *
 * @see dict_attrbyname
 *
 * @param[in,out] name of attribute.
 * @param[in] default_list the list to return if no qualifiers were found.
 * @return PAIR_LIST_UNKOWN if qualifiers couldn't be resolved to a list.
 */
pair_lists_t radius_list_name(char const **name, pair_lists_t default_list)
{
	char const *p = *name;
	char const *q;
	pair_lists_t output;

	/* This should never be a NULL pointer or zero length string */
	rad_assert(name && *name);

	/*
	 *	Unfortunately, ':' isn't a definitive separator for
	 *	the list name.  We may have numeric tags, too.
	 */
	q = strchr(p, ':');
	if (q) {
		/*
		 *	Check for tagged attributes.  They have
		 *	"name:tag", where tag is a decimal number.
		 *	Valid tags are invalid attributes, so that's
		 *	OK.
		 *
		 *	Also allow "name:tag[#]" as a tag.
		 *
		 *	However, "request:" is allowed, too, and
		 *	shouldn't be interpreted as a tag.
		 *
		 *	We do this check first rather than just
		 *	looking up the request name, because this
		 *	check is cheap, and looking up the request
		 *	name is expensive.
		 */
		if (isdigit((int) q[1])) {
			char const *d = q + 1;

			while (isdigit((int) *d)) {
				d++;
			}

			/*
			 *	Return the DEFAULT list as supplied by
			 *	the caller.  This is usually
			 *	PAIRLIST_REQUEST.
			 */
			if (!*d || (*d == '[')) {
				return default_list;
			}
		}

		/*
		 *	If the first part is a list name, then treat
		 *	it as a list.  This means that we CANNOT have
		 *	an attribute which is named "request",
		 *	"reply", etc.  Allowing a tagged attribute
		 *	"request:3" would just be insane.
		 */
		output = fr_substr2int(pair_lists, p, PAIR_LIST_UNKNOWN, (q - p));
		if (output != PAIR_LIST_UNKNOWN) {
			*name = (q + 1);	/* Consume the list and delimiter */
			return output;
		}

		/*
		 *	It's not a known list, say so.
		 */
		return PAIR_LIST_UNKNOWN;
	}

	/*
	 *	The input string may be just a list name,
	 *	e.g. "request".  Check for that.
	 */
	q = (p + strlen(p));
	output = fr_substr2int(pair_lists, p, PAIR_LIST_UNKNOWN, (q - p));
	if (output != PAIR_LIST_UNKNOWN) {
		*name = q;
		return output;
	}

	/*
	 *	It's just an attribute name.  Return the default list
	 *	as supplied by the caller.
	 */
	return default_list;
}


/** Resolve attribute name to a request.
 *
 * Check the name string for qualifiers that reference a parent request and
 * write the pointer to this request to 'request'.
 *
 * If qualifiers were consumed, write a new pointer into name to the
 * char after the last qualifier to be consumed.
 *
 * radius_ref_request should be called before radius_list_name.
 *
 * @see radius_list_name
 * @param[in,out] name of attribute.
 * @param[in] def default request ref to return if no request qualifier is present.
 * @return one of the REQUEST_* definitions or REQUEST_UNKOWN
 */
request_refs_t radius_request_name(char const **name, request_refs_t def)
{
	char *p;
	int request;

	p = strchr(*name, '.');
	if (!p) {
		return def;
	}

	/*
	 *	We may get passed "127.0.0.1".
	 */
	request = fr_substr2int(request_refs, *name, REQUEST_UNKNOWN,
				p - *name);

	/*
	 *	If we get a valid name, skip it.
	 */
	if (request != REQUEST_UNKNOWN) {
		*name = p + 1;
		return request;
	}

	/*
	 *	Otherwise leave it alone, and return the caller's
	 *	default.
	 */
	return def;
}

/** Resolve request to a request.
 *
 * Resolve name to a current request.
 *
 * @see radius_list
 * @param[in,out] context Base context to use, and to write the result back to.
 * @param[in] name (request) to resolve to.
 * @return 0 if request is valid in this context, else -1.
 */
int radius_request(REQUEST **context, request_refs_t name)
{
	REQUEST *request = *context;

	switch (name) {
	case REQUEST_CURRENT:
		return 0;

	case REQUEST_PARENT:	/* for future use in request chaining */
	case REQUEST_OUTER:
		if (!request->parent) {
			return -1;
		}
		*context = request->parent;
		break;

	case REQUEST_UNKNOWN:
	default:
		rad_assert(0);
		return -1;
	}

	return 0;
}

/** Adds subcapture values to request data
 *
 * Allows use of %{n} expansions.
 *
 * @param request Current request.
 * @param compare Result returned by regexec.
 * @param value The original value.
 * @param rxmatch Pointers into value.
 */
void rad_regcapture(REQUEST *request, int compare, char const *value, regmatch_t rxmatch[])
{
	int i;
	char *p;
	size_t len;

	if (compare == REG_NOMATCH) {
		return;
	}

	/*
	 *	Add new %{0}, %{1}, etc.
	 */
	for (i = 0; i <= REQUEST_MAX_REGEX; i++) {
		/*
		 *	Didn't match: delete old match, if it existed.
		 */
		if (rxmatch[i].rm_so == -1) {
			p = request_data_get(request, request, REQUEST_DATA_REGEX | i);
			if (p) {
				RDEBUG4("%%{%i}: Clearing old value \"%s\"", i, p);
				talloc_free(p);
			} else {
				RDEBUG4("%%{%i}: Was empty", i);
			}

			continue;
		}

		len = rxmatch[i].rm_eo - rxmatch[i].rm_so;
		p = talloc_array(request, char, len + 1);
		if (!p) {
			ERROR("Out of memory");
			return;
		}

		memcpy(p, value + rxmatch[i].rm_so, len);
		p[len] = '\0';

		RDEBUG4("%%{%i}: Inserting new value \"%s\"", i, p);
		/*
		 *	Copy substring, and add it to
		 *	the request.
		 *
		 *	Note that we don't check
		 *	for out of memory, which is
		 *	the only error we can get...
		 */
		request_data_add(request, request, REQUEST_DATA_REGEX | i, p, true);
	}
}

/** Return the default log dir
 *
 * This is set at build time from --prefix
 * @return the value of LOGDIR
 */
char const *rad_default_log_dir(void)
{
	return LOGDIR;
}

/** Return the default lib dir
 *
 * This is set at build time from --prefix
 * @return the value of LIBDIR
 */
char const *rad_default_lib_dir(void)
{
	return LIBDIR;
}

/** Return the default raddb dir
 *
 * This is set at build time from --prefix
 * @return the value of RADDBDIR
 */
char const *rad_default_raddb_dir(void)
{
	return RADDBDIR;
}

/** Return the default run dir
 *
 * This is set at build time from --prefix
 * @return the value of RUNDIR
 */
char const *rad_default_run_dir(void)
{
	return RUNDIR;
}

/** Return the default sbin dir
 *
 * This is set at build time from --prefix
 * @return the value of SBINDIR
 */
char const *rad_default_sbin_dir(void)
{
	return SBINDIR;
}

/** Return the default radacct dir
 *
 * This is set at build time from --prefix
 * @return the value of RADIR
 */
char const *rad_radacct_dir(void)
{
	return RADIR;
}

#ifndef NDEBUG
/*
 *	Verify a packet.
 */
static void verify_packet(char const *file, int line, REQUEST *request, RADIUS_PACKET *packet, char const *type)
{
	TALLOC_CTX *parent;

	if (!packet) {
		fprintf(stderr, "CONSISTENCY CHECK FAILED %s[%u]: RADIUS_PACKET %s pointer was NULL", file, line, type);
		fr_assert(0);
		fr_exit_now(0);
	}

	parent = talloc_parent(packet);
	if (parent != request) {
		ERROR("CONSISTENCY CHECK FAILED %s[%u]: Expected RADIUS_PACKET %s to be parented by %p (%s), "
		      "but parented by %p (%s)", file, line, type, request, talloc_get_name(request),
		      parent, parent ? talloc_get_name(parent) : "NULL");

		fr_log_talloc_report(packet);
		if (parent) fr_log_talloc_report(parent);

		rad_assert(0);
	}

	VERIFY_PACKET(packet);

	if (!packet->vps) return;

#ifdef WITH_VERIFY_PTR
	fr_verify_list(file, line, packet, packet->vps);
#endif
}
/*
 *	Catch horrible talloc errors.
 */
void verify_request(char const *file, int line, REQUEST *request)
{
	if (!request) {
		fprintf(stderr, "CONSISTENCY CHECK FAILED %s[%u]: REQUEST pointer was NULL", file, line);
		fr_assert(0);
		fr_exit_now(0);
	}

	(void) talloc_get_type_abort(request, REQUEST);

#ifdef WITH_VERIFY_PTR
	fr_verify_list(file, line, request, request->config_items);
#endif

	if (request->packet) verify_packet(file, line, request, request->packet, "request");
	if (request->reply) verify_packet(file, line, request, request->reply, "reply");
#ifdef WITH_PROXY
	if (request->proxy) verify_packet(file, line, request, request->proxy, "proxy-request");
	if (request->proxy_reply) verify_packet(file, line, request, request->proxy_reply, "proxy-reply");
#endif

#ifdef WITH_COA
	if (request->coa) {
		void *parent;

		(void) talloc_get_type_abort(request->coa, REQUEST);
		parent = talloc_parent(request->coa);

		rad_assert(parent == request);

		verify_request(file, line, request->coa);
	}
#endif
}
#endif
