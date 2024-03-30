/*
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
 */

/**
 * $Id$
 *
 * @file src/bin/radclient.c
 * @brief General radius client and debug tool.
 *
 * @copyright 2000,2006,2014 The FreeRADIUS server project
 * @copyright 2000 Miquel van Smoorenburg (miquels@cistron.nl)
 * @copyright 2000 Alan DeKok (aland@freeradius.org)
 */

RCSID("$Id$")

#include <freeradius-devel/util/conf.h>
#include <freeradius-devel/util/syserror.h>
#include <freeradius-devel/util/atexit.h>
#include <freeradius-devel/util/pair_legacy.h>
#include <freeradius-devel/util/time.h>
#include <freeradius-devel/util/event.h>
#include <freeradius-devel/server/packet.h>
#include <freeradius-devel/radius/list.h>
#include <freeradius-devel/radius/radius.h>
#include <freeradius-devel/util/chap.h>
#include <freeradius-devel/radius/client.h>

#ifdef HAVE_OPENSSL_SSL_H
#include <openssl/ssl.h>
#endif
#include <ctype.h>

#ifdef HAVE_GETOPT_H
#  include <getopt.h>
#endif

#include <assert.h>

typedef struct request_s request_t;	/* to shut up warnings about mschap.h */

#include "smbdes.h"
#include "mschap.h"

#include "radclient.h"

#define pair_update_request(_attr, _da) do { \
		_attr = fr_pair_find_by_da(&request->request_pairs, NULL, _da); \
		if (!_attr) { \
			_attr = fr_pair_afrom_da(request, _da); \
			fr_assert(_attr != NULL); \
			fr_pair_append(&request->request_pairs, _attr); \
		} \
	} while (0)

static int retries = 3;
static fr_time_delta_t timeout = fr_time_delta_wrap((int64_t)5 * NSEC);	/* 5 seconds */
static char *secret = NULL;
static bool do_output = true;

static rc_stats_t stats;

static int packet_code = FR_RADIUS_CODE_UNDEFINED;
static int resend_count = 1;
static bool print_filename = false;

static fr_bio_fd_config_t fd_config;

static fr_bio_fd_info_t const *fd_info = NULL;

static fr_bio_t *bio = NULL;

static fr_radius_client_config_t client_config;

static fr_bio_packet_t *client_bio = NULL;

static int ipproto = IPPROTO_UDP;

static fr_dlist_head_t rc_request_list;
static rc_request_t	*current = NULL;

static char const *radclient_version = RADIUSD_VERSION_BUILD("radclient");

static fr_dict_t const *dict_freeradius;
static fr_dict_t const *dict_radius;

extern fr_dict_autoload_t radclient_dict[];
fr_dict_autoload_t radclient_dict[] = {
	{ .out = &dict_freeradius, .proto = "freeradius" },
	{ .out = &dict_radius, .proto = "radius" },
	{ NULL }
};

static fr_dict_attr_t const *attr_cleartext_password;

static fr_dict_attr_t const *attr_ms_chap_challenge;
static fr_dict_attr_t const *attr_ms_chap_password;
static fr_dict_attr_t const *attr_ms_chap_response;

static fr_dict_attr_t const *attr_radclient_test_name;
static fr_dict_attr_t const *attr_request_authenticator;

static fr_dict_attr_t const *attr_chap_password;
static fr_dict_attr_t const *attr_chap_challenge;
static fr_dict_attr_t const *attr_packet_type;
static fr_dict_attr_t const *attr_user_name;
static fr_dict_attr_t const *attr_user_password;

extern fr_dict_attr_autoload_t radclient_dict_attr[];
fr_dict_attr_autoload_t radclient_dict_attr[] = {
	{ .out = &attr_cleartext_password, .name = "Password.Cleartext", .type = FR_TYPE_STRING, .dict = &dict_freeradius },
	{ .out = &attr_ms_chap_challenge, .name = "Vendor-Specific.Microsoft.CHAP-Challenge", .type = FR_TYPE_OCTETS, .dict = &dict_radius },
	{ .out = &attr_ms_chap_password, .name = "Password.MS-CHAP", .type = FR_TYPE_STRING, .dict = &dict_freeradius },
	{ .out = &attr_ms_chap_response, .name = "Vendor-Specific.Microsoft.CHAP-Response", .type = FR_TYPE_OCTETS, .dict = &dict_radius },

	{ .out = &attr_radclient_test_name, .name = "Radclient-Test-Name", .type = FR_TYPE_STRING, .dict = &dict_freeradius },
	{ .out = &attr_request_authenticator, .name = "Request-Authenticator", .type = FR_TYPE_OCTETS, .dict = &dict_freeradius },

	{ .out = &attr_chap_password, .name = "CHAP-Password", .type = FR_TYPE_OCTETS, .dict = &dict_radius },
	{ .out = &attr_chap_challenge, .name = "CHAP-Challenge", .type = FR_TYPE_OCTETS, .dict = &dict_radius },
	{ .out = &attr_packet_type, .name = "Packet-Type", .type = FR_TYPE_UINT32, .dict = &dict_radius },
	{ .out = &attr_user_password, .name = "User-Password", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_user_name, .name = "User-Name", .type = FR_TYPE_STRING, .dict = &dict_radius },

	{ NULL }
};

static NEVER_RETURNS void usage(void)
{
	fprintf(stderr, "Usage: radclient [options] server[:port] <command> [<secret>]\n");

	fprintf(stderr, "  <command>                         One of auth, acct, status, coa, disconnect or auto.\n");
	fprintf(stderr, "  -4                                Use IPv4 address of server\n");
	fprintf(stderr, "  -6                                Use IPv6 address of server.\n");
	fprintf(stderr, "  -C [<client_ip>:]<client_port>    Client source port and source IP address.  Port values may be 1..65535\n");
	fprintf(stderr, "  -c <count>			     Send each packet 'count' times.\n");
	fprintf(stderr, "  -d <raddb>                        Set user dictionary directory (defaults to " RADDBDIR ").\n");
	fprintf(stderr, "  -D <dictdir>                      Set main dictionary directory (defaults to " DICTDIR ").\n");
	fprintf(stderr, "  -f <file>[:<file>]                Read packets from file, not stdin.\n");
	fprintf(stderr, "                                    If a second file is provided, it will be used to verify responses\n");
	fprintf(stderr, "  -F                                Print the file name, packet number and reply code.\n");
	fprintf(stderr, "  -h                                Print usage help information.\n");
	fprintf(stderr, "  -P <proto>                        Use proto (tcp or udp) for transport.\n");
	fprintf(stderr, "  -r <retries>                      If timeout, retry sending the packet 'retries' times.\n");
	fprintf(stderr, "  -s                                Print out summary information of auth results.\n");
	fprintf(stderr, "  -S <file>                         read secret from file, not command line.\n");
	fprintf(stderr, "  -t <timeout>                      Wait 'timeout' seconds before retrying (may be a floating point number).\n");
	fprintf(stderr, "  -v                                Show program version information.\n");
	fprintf(stderr, "  -x                                Debugging mode.\n");

	fr_exit_now(EXIT_SUCCESS);
}

/*
 *	Free a radclient struct, which may (or may not)
 *	already be in the list.
 */
static int _rc_request_free(rc_request_t *request)
{
	fr_dlist_remove(&rc_request_list, request);

	return 0;
}

#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
#  include <openssl/provider.h>

static OSSL_PROVIDER *openssl_default_provider = NULL;
static OSSL_PROVIDER *openssl_legacy_provider = NULL;

static int openssl3_init(void)
{
	/*
	 *	Load the default provider for most algorithms
	 */
	openssl_default_provider = OSSL_PROVIDER_load(NULL, "default");
	if (!openssl_default_provider) {
		ERROR("(TLS) Failed loading default provider");
		return -1;
	}

	/*
	 *	Needed for MD4
	 *
	 *	https://www.openssl.org/docs/man3.0/man7/migration_guide.html#Legacy-Algorithms
	 */
	openssl_legacy_provider = OSSL_PROVIDER_load(NULL, "legacy");
	if (!openssl_legacy_provider) {
		ERROR("(TLS) Failed loading legacy provider");
		return -1;
	}

	return 0;
}

static void openssl3_free(void)
{
	if (openssl_default_provider && !OSSL_PROVIDER_unload(openssl_default_provider)) {
		ERROR("Failed unloading default provider");
	}
	openssl_default_provider = NULL;

	if (openssl_legacy_provider && !OSSL_PROVIDER_unload(openssl_legacy_provider)) {
		ERROR("Failed unloading legacy provider");
	}
	openssl_legacy_provider = NULL;
}
#else
#define openssl3_init()
#define openssl3_free()
#endif

static int _loop_status(UNUSED fr_time_t now, fr_time_delta_t wake, UNUSED void *ctx)
{
	// @todo - actually call %pV handler

	if (fr_time_delta_unwrap(wake) > (NSEC / 10)) DEBUG2("Main loop waking up in %pV seconds", fr_box_time_delta(wake));

	return 0;
}

static int mschapv1_encode(fr_packet_t *packet, fr_pair_list_t *list,
			   char const *password)
{
	unsigned int		i;
	uint8_t			*p;
	fr_pair_t		*challenge, *reply;
	uint8_t			nthash[16];

	fr_pair_delete_by_da(list, attr_ms_chap_challenge);
	fr_pair_delete_by_da(list, attr_ms_chap_response);

	MEM(challenge = fr_pair_afrom_da(packet, attr_ms_chap_challenge));

	fr_pair_append(list, challenge);

	MEM(p = talloc_array(challenge, uint8_t, 8));
	fr_pair_value_memdup_buffer_shallow(challenge, p, false);

	for (i = 0; i < challenge->vp_length; i++) {
		p[i] = fr_rand();
	}

	MEM(reply = fr_pair_afrom_da(packet, attr_ms_chap_response));
	fr_pair_append(list, reply);
	p = talloc_zero_array(reply, uint8_t, 50); /* really reply->da->flags.length */
	fr_pair_value_memdup_buffer_shallow(reply, p, false);

	p[1] = 0x01; /* NT hash */

	if (mschap_nt_password_hash(nthash, password) < 0) return 0;

	smbdes_mschap(nthash, challenge->vp_octets, p + 26);
	return 1;
}


static int getport(char const *name)
{
	struct servent *svp;

	svp = getservbyname(name, "udp");
	if (!svp) return 0;

	return ntohs(svp->s_port);
}

/*
 *	Set a port from the request type if we don't already have one
 */
static void radclient_get_port(fr_radius_packet_code_t type, uint16_t *port)
{
	switch (type) {
	default:
	case FR_RADIUS_CODE_ACCESS_REQUEST:
	case FR_RADIUS_CODE_ACCESS_CHALLENGE:
	case FR_RADIUS_CODE_STATUS_SERVER:
		if (*port == 0) *port = getport("radius");
		if (*port == 0) *port = FR_AUTH_UDP_PORT;
		return;

	case FR_RADIUS_CODE_ACCOUNTING_REQUEST:
		if (*port == 0) *port = getport("radacct");
		if (*port == 0) *port = FR_ACCT_UDP_PORT;
		return;

	case FR_RADIUS_CODE_DISCONNECT_REQUEST:
		if (*port == 0) *port = FR_POD_UDP_PORT;
		return;

	case FR_RADIUS_CODE_COA_REQUEST:
		if (*port == 0) *port = FR_COA_UDP_PORT;
		return;

	case FR_RADIUS_CODE_UNDEFINED:
		if (*port == 0) *port = 0;
		return;
	}
}

/*
 *	Resolve a port to a request type
 */
static fr_radius_packet_code_t radclient_get_code(uint16_t port)
{
	/*
	 *	getport returns 0 if the service doesn't exist
	 *	so we need to return early, to avoid incorrect
	 *	codes.
	 */
	if (port == 0) return FR_RADIUS_CODE_UNDEFINED;

	if ((port == getport("radius")) || (port == FR_AUTH_UDP_PORT) || (port == FR_AUTH_UDP_PORT_ALT)) {
		return FR_RADIUS_CODE_ACCESS_REQUEST;
	}
	if ((port == getport("radacct")) || (port == FR_ACCT_UDP_PORT) || (port == FR_ACCT_UDP_PORT_ALT)) {
		return FR_RADIUS_CODE_ACCOUNTING_REQUEST;
	}
	if (port == FR_COA_UDP_PORT) return FR_RADIUS_CODE_COA_REQUEST;

	return FR_RADIUS_CODE_UNDEFINED;
}


static bool already_hex(fr_pair_t *vp)
{
	size_t i;

	if (!vp || (vp->vp_type != FR_TYPE_OCTETS)) return true;

	/*
	 *	If it's 17 octets, it *might* be already encoded.
	 *	Or, it might just be a 17-character password (maybe UTF-8)
	 *	Check it for non-printable characters.  The odds of ALL
	 *	of the characters being 32..255 is (1-7/8)^17, or (1/8)^17,
	 *	or 1/(2^51), which is pretty much zero.
	 */
	for (i = 0; i < vp->vp_length; i++) {
		if (vp->vp_octets[i] < 32) {
			return true;
		}
	}

	return false;
}

/*
 *	Initialize a radclient data structure and add it to
 *	the global linked list.
 */
static int radclient_init(TALLOC_CTX *ctx, rc_file_pair_t *files)
{
	FILE		*packets, *filters = NULL;

	fr_pair_t	*vp;
	rc_request_t	*request = NULL;
	bool		packets_done = false;
	uint64_t	num = 0;

	fr_assert(files->packets != NULL);

	/*
	 *	Determine where to read the VP's from.
	 */
	if (strcmp(files->packets, "-") != 0) {
		packets = fopen(files->packets, "r");
		if (!packets) {
			ERROR("Error opening %s: %s", files->packets, fr_syserror(errno));
			return -1;
		}

		/*
		 *	Read in the pairs representing the expected response.
		 */
		if (files->filters) {
			filters = fopen(files->filters, "r");
			if (!filters) {
				ERROR("Error opening %s: %s", files->filters, fr_syserror(errno));
				goto error;
			}
		}
	} else {
		packets = stdin;
	}

	/*
	 *	Loop until the file is done.
	 */
	do {
		/*
		 *	Allocate it.
		 */
		MEM(request = talloc_zero(ctx, rc_request_t));
		MEM(request->packet = fr_packet_alloc(request, true));
		request->packet->uctx = request;

		request->packet->socket.inet.src_ipaddr = fd_config.src_ipaddr;
		request->packet->socket.inet.src_port = fd_config.src_port;
		request->packet->socket.inet.dst_ipaddr = fd_config.dst_ipaddr;
		request->packet->socket.inet.dst_port = fd_config.dst_port;
		request->packet->socket.type = (ipproto == IPPROTO_TCP) ? SOCK_STREAM : SOCK_DGRAM;

		request->files = files;
		request->packet->id = -1;
		request->num = num++;

		fr_pair_list_init(&request->filter);
		fr_pair_list_init(&request->request_pairs);
		fr_pair_list_init(&request->reply_pairs);

		/*
		 *	Read the request VP's.
		 */
		if (fr_pair_list_afrom_file(request, dict_radius,
					    &request->request_pairs, packets, &packets_done) < 0) {
			char const *input;

			if ((files->packets[0] == '-') && (files->packets[1] == '\0')) {
				input = "stdin";
			} else {
				input = files->packets;
			}

			REDEBUG("Error parsing \"%s\"", input);
			goto error;
		}

		/*
		 *	Skip empty entries
		 */
		if (fr_pair_list_empty(&request->request_pairs)) {
			WARN("Skipping \"%s\": No Attributes", files->packets);
			talloc_free(request);
			continue;
		}

		/*
		 *	Read in filter VP's.
		 */
		if (filters) {
			bool filters_done;

			if (fr_pair_list_afrom_file(request, dict_radius,
						    &request->filter, filters, &filters_done) < 0) {
				REDEBUG("Error parsing \"%s\"", files->filters);
				goto error;
			}

			if (filters_done && !packets_done) {
				REDEBUG("Differing number of packets/filters in %s:%s "
				        "(too many requests))", files->packets, files->filters);
				goto error;
			}

			if (!filters_done && packets_done) {
				REDEBUG("Differing number of packets/filters in %s:%s "
				        "(too many filters))", files->packets, files->filters);
				goto error;
			}

			vp = fr_pair_find_by_da(&request->filter, NULL, attr_packet_type);
			if (vp) {
				request->filter_code = vp->vp_uint32;
				fr_pair_delete(&request->filter, vp);
			}

			/*
			 *	This allows efficient list comparisons later
			 */
			fr_pair_list_sort(&request->filter, fr_pair_cmp_by_da);
		}

		/*
		 *	Process special attributes
		 */
		for (vp = fr_pair_list_head(&request->request_pairs);
		     vp;
		     vp = fr_pair_list_next(&request->request_pairs, vp)) {
			/*
			 *	Allow it to set the packet type in
			 *	the attributes read from the file.
			 */
			if (vp->da == attr_packet_type) {
				request->packet->code = vp->vp_uint32;
			} else if (vp->da == attr_request_authenticator) {
				if (vp->vp_length > sizeof(request->packet->vector)) {
					memcpy(request->packet->vector, vp->vp_octets, sizeof(request->packet->vector));
				} else {
					memset(request->packet->vector, 0, sizeof(request->packet->vector));
					memcpy(request->packet->vector, vp->vp_octets, vp->vp_length);
				}
			} else if (vp->da == attr_cleartext_password) {
				request->password = vp;
			/*
			 *	Keep a copy of the the password attribute.
			 */
			} else if (vp->da == attr_chap_password) {
				/*
				 *	If it's already hex, do nothing.
				 */
				if ((vp->vp_length == 17) && (already_hex(vp))) continue;

				/*
				 *	CHAP-Password is octets, so it may not be zero terminated.
				 */
				pair_update_request(request->password, attr_cleartext_password);
				fr_pair_value_bstrndup(request->password, vp->vp_strvalue, vp->vp_length, true);
			} else if ((vp->da == attr_user_password) ||
				   (vp->da == attr_ms_chap_password)) {
				pair_update_request(request->password, attr_cleartext_password);
				fr_pair_value_bstrndup(request->password, vp->vp_strvalue, vp->vp_length, true);

			} else if (vp->da == attr_radclient_test_name) {
				request->name = vp->vp_strvalue;

			}
		} /* loop over the VP's we read in */

		/*
		 *	Use the default set on the command line
		 */
		if (request->packet->code == FR_RADIUS_CODE_UNDEFINED) request->packet->code = packet_code;

		/*
		 *	Fill in the packet header from attributes, and then
		 *	re-realize the attributes.
		 */
		fr_packet_pairs_to_packet(request->packet, &request->request_pairs);

		/*
		 *	Default to the filename
		 */
		if (!request->name) request->name = request->files->packets;

		/*
		 *	Automatically set the response code from the request code
		 *	(if one wasn't already set).
		 */
		if (request->filter_code == FR_RADIUS_CODE_UNDEFINED) {
			switch (request->packet->code) {
			case FR_RADIUS_CODE_ACCESS_REQUEST:
				request->filter_code = FR_RADIUS_CODE_ACCESS_ACCEPT;
				break;

			case FR_RADIUS_CODE_ACCOUNTING_REQUEST:
				request->filter_code = FR_RADIUS_CODE_ACCOUNTING_RESPONSE;
				break;

			case FR_RADIUS_CODE_COA_REQUEST:
				request->filter_code = FR_RADIUS_CODE_COA_ACK;
				break;

			case FR_RADIUS_CODE_DISCONNECT_REQUEST:
				request->filter_code = FR_RADIUS_CODE_DISCONNECT_ACK;
				break;

			case FR_RADIUS_CODE_STATUS_SERVER:
				switch (radclient_get_code(request->packet->socket.inet.dst_port)) {
				case FR_RADIUS_CODE_ACCESS_REQUEST:
					request->filter_code = FR_RADIUS_CODE_ACCESS_ACCEPT;
					break;

				case FR_RADIUS_CODE_ACCOUNTING_REQUEST:
					request->filter_code = FR_RADIUS_CODE_ACCOUNTING_RESPONSE;
					break;

				default:
					request->filter_code = FR_RADIUS_CODE_UNDEFINED;
					break;
				}
				break;

			case FR_RADIUS_CODE_UNDEFINED:
				REDEBUG("Packet-Type must be defined,"
					"or a well known RADIUS port");
				goto error;

			default:
				REDEBUG("Can't determine expected &reply.Packet-Type for Packet-Type %i",
					request->packet->code);
				goto error;
			}
		/*
		 *	Automatically set the request code from the response code
		 *	(if one wasn't already set).
		 */
		} else if (request->packet->code == FR_RADIUS_CODE_UNDEFINED) {
			switch (request->filter_code) {
			case FR_RADIUS_CODE_ACCESS_ACCEPT:
			case FR_RADIUS_CODE_ACCESS_REJECT:
				request->packet->code = FR_RADIUS_CODE_ACCESS_REQUEST;
				break;

			case FR_RADIUS_CODE_ACCOUNTING_RESPONSE:
				request->packet->code = FR_RADIUS_CODE_ACCOUNTING_REQUEST;
				break;

			case FR_RADIUS_CODE_DISCONNECT_ACK:
			case FR_RADIUS_CODE_DISCONNECT_NAK:
				request->packet->code = FR_RADIUS_CODE_DISCONNECT_REQUEST;
				break;

			case FR_RADIUS_CODE_COA_ACK:
			case FR_RADIUS_CODE_COA_NAK:
				request->packet->code = FR_RADIUS_CODE_COA_REQUEST;
				break;

			default:
				REDEBUG("Can't determine expected Packet-Type for &reply.Packet-Type %i",
					request->filter_code);
				goto error;
			}
		}

		/*
		 *	Automatically set the dst port (if one wasn't already set).
		 */
		if (request->packet->socket.inet.dst_port == 0) {
			radclient_get_port(request->packet->code, &request->packet->socket.inet.dst_port);
			if (request->packet->socket.inet.dst_port == 0) {
				REDEBUG("Can't determine destination port");
				goto error;
			}
		}

		/*
		 *	Add it to the tail of the list.
		 */
		fr_dlist_insert_tail(&rc_request_list, request);

		/*
		 *	Set the destructor so it removes itself from the
		 *	request list when freed. We don't set this until
		 *	the packet is actually in the list, else we trigger
		 *	the asserts in the free callback.
		 */
		talloc_set_destructor(request, _rc_request_free);
	} while (!packets_done); /* loop until the file is done. */

	if (packets != stdin) fclose(packets);
	if (filters) fclose(filters);

	/*
	 *	And we're done.
	 */
	return 0;

error:
	talloc_free(request);

	if (packets != stdin) fclose(packets);
	if (filters) fclose(filters);

	return -1;
}


/*
 *	Sanity check each argument.
 */
static int radclient_sane(rc_request_t *request)
{
	if (request->packet->socket.inet.dst_port == 0) {
		request->packet->socket.inet.dst_port = fd_config.dst_port;
	}
	if (request->packet->socket.inet.dst_ipaddr.af == AF_UNSPEC) {
		if (fd_config.dst_ipaddr.af == AF_UNSPEC) {
			ERROR("No server was given, and request %" PRIu64 " in file %s did not contain "
			      "Packet-Dst-IP-Address", request->num, request->files->packets);
			return -1;
		}
		request->packet->socket.inet.dst_ipaddr = fd_config.dst_ipaddr;
	}
	if (request->packet->code == 0) {
		if (packet_code == -1) {
			ERROR("Request was \"auto\", and request %" PRIu64 " in file %s did not contain Packet-Type",
			      request->num, request->files->packets);
			return -1;
		}
		request->packet->code = packet_code;
	}
	request->packet->socket.fd = -1;

	return 0;
}


/*
 *	Deallocate packet ID, etc.
 */
static void deallocate_id(rc_request_t *request)
{
	if (!request || !request->packet || (request->packet->id < 0)) return;

	(void) fr_radius_client_fd_bio_cancel(client_bio, request->packet);

	request->packet->id = -1;

	/*
	 *	If we've already sent a packet, free up the old one,
	 *	and ensure that the next packet has a unique
	 *	authentication vector.
	 */
	if (request->packet->data) TALLOC_FREE(request->packet->data);
	if (request->reply) fr_packet_free(&request->reply);
}

/*
 *	Send one packet.
 */
static int send_one_packet(fr_bio_packet_t *client, rc_request_t *request)
{
	fr_assert(!request->done);
	fr_assert(request->reply == NULL);

#ifdef STATIC_ANALYZER
	if (!secret) fr_exit_now(1);
#endif

	fr_assert(request->packet->id < 0);
	fr_assert(request->packet->data == NULL);

	/*
	 *	Update the password, so it can be encrypted with the
	 *	new authentication vector.
	 */
	if (request->password) {
		fr_pair_t *vp;

		if ((vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_user_password)) != NULL) {
			fr_pair_value_strdup(vp, request->password->vp_strvalue, false);

		} else if ((vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_chap_password)) != NULL) {
			uint8_t		buffer[17];
			fr_pair_t	*challenge;
			uint8_t	const	*vector;

			/*
			 *	Use Chap-Challenge pair if present,
			 *	Request Authenticator otherwise.
			 */
			challenge = fr_pair_find_by_da(&request->request_pairs, NULL, attr_chap_challenge);
			if (challenge && (challenge->vp_length == RADIUS_AUTH_VECTOR_LENGTH)) {
				vector = challenge->vp_octets;
			} else {
				vector = request->packet->vector;
			}

			fr_chap_encode(buffer,
				       fr_rand() & 0xff, vector, RADIUS_AUTH_VECTOR_LENGTH,
				       request->password->vp_strvalue,
				       request->password->vp_length);
			fr_pair_value_memdup(vp, buffer, sizeof(buffer), false);

		} else if (fr_pair_find_by_da_nested(&request->request_pairs, NULL, attr_ms_chap_password) != NULL) {
			mschapv1_encode(request->packet, &request->request_pairs, request->password->vp_strvalue);

		} else {
			DEBUG("WARNING: No password in the request");
		}
	}

	request->timestamp = fr_time();
	request->tries = 1;
	request->resend++;

	/*
	 *	Send the current packet.
	 */
	if (fr_bio_packet_write(client, request, request->packet, &request->request_pairs) < 0) {
		REDEBUG("Failed writing packet");
		return -1;
	}

	fr_packet_log(&default_log, request->packet, &request->request_pairs, false);

	return 0;
}

#if 0
/*
 *	Receive one packet, maybe.
 */
static int recv_one_packet(fr_time_delta_t wait_time)
{

	// @todo - get a packet

	if (print_filename) {
		RDEBUG("%s response code %d", request->files->packets, reply->code);
	}

	deallocate_id(request);
	request->reply = reply;
	reply = NULL;

	/*
	 *	If this fails, we're out of memory.
	 */
	if (fr_radius_decode_simple(request, &request->reply_pairs,
				    request->reply->data, request->reply->data_len,
				    request->packet->vector, secret) < 0) {
		REDEBUG("Reply decode failed");
		stats.lost++;
		goto packet_done;
	}
	PAIR_LIST_VERIFY(&request->reply_pairs);
	fr_packet_log(&default_log, request->reply, &request->reply_pairs, true);

	/*
	 *	Increment counters...
	 */
	switch (request->reply->code) {
	case FR_RADIUS_CODE_ACCESS_ACCEPT:
	case FR_RADIUS_CODE_ACCOUNTING_RESPONSE:
	case FR_RADIUS_CODE_COA_ACK:
	case FR_RADIUS_CODE_DISCONNECT_ACK:
		stats.accepted++;
		break;

	case FR_RADIUS_CODE_ACCESS_CHALLENGE:
		break;

	default:
		stats.rejected++;
	}

	fr_strerror_clear();	/* Clear strerror buffer */

	/*
	 *	If we had an expected response code, check to see if the
	 *	packet matched that.
	 */
	if ((request->filter_code != FR_RADIUS_CODE_UNDEFINED) && (request->reply->code != request->filter_code)) {
		if (FR_RADIUS_PACKET_CODE_VALID(request->reply->code)) {
			REDEBUG("%s: Expected %s got %s", request->name, fr_radius_packet_name[request->filter_code],
				fr_radius_packet_name[request->reply->code]);
		} else {
			REDEBUG("%s: Expected %u got %i", request->name, request->filter_code,
				request->reply->code);
		}
		stats.failed++;
	/*
	 *	Check if the contents of the packet matched the filter
	 */
	} else if (fr_pair_list_empty(&request->filter)) {
		stats.passed++;
	} else {
		fr_pair_t const *failed[2];

		fr_pair_list_sort(&request->reply_pairs, fr_pair_cmp_by_da);
		if (fr_pair_validate(failed, &request->filter, &request->reply_pairs)) {
			RDEBUG("%s: Response passed filter", request->name);
			stats.passed++;
		} else {
			fr_pair_validate_debug(failed);
			REDEBUG("%s: Response for failed filter", request->name);
			stats.failed++;
		}
	}

	if (request->resend == resend_count) {
		request->done = true;
	}

packet_done:
	fr_packet_free(&request->reply);
	fr_packet_free(&reply);	/* may be NULL */

	return 0;
}
#endif

static void client_error(UNUSED fr_event_list_t *el, UNUSED int fd, UNUSED int flags,
			 UNUSED int fd_errno, UNUSED void *uctx)
{
	fr_assert(0);
}

static void client_read(UNUSED fr_event_list_t *el, UNUSED int fd, UNUSED int flags, void *uctx)
{
	fr_bio_packet_t *client = uctx;
	rc_request_t *request;
	fr_pair_list_t list;
	fr_packet_t *reply;
	int rcode;

	fr_pair_list_init(&list);

	/*
	 *	@todo list_ctx is ignored
	 */
	rcode = fr_bio_packet_read(client, (void **) &request, &reply, client, &list);
	if (rcode < 0) fr_assert(0);

	if (!rcode) {
		ERROR("Failed reading packet - %s", fr_strerror());
		fr_assert(0);
	}

	fr_packet_log(&default_log, reply, &list, true);

	fr_assert(0);
}

static fr_event_update_t const pause_write[] = {
	FR_EVENT_SUSPEND(fr_event_io_func_t, write),
	{ 0 }
};

static void client_write(fr_event_list_t *el, int fd, UNUSED int flags, void *uctx)
{
	fr_bio_packet_t *client = uctx;
	rc_request_t *request;

	request = fr_dlist_next(&rc_request_list, current);

	/*
	 *	Nothing more to send, stop trying to write packets.
	 */
	if (!request) {
		if (fr_event_filter_update(el, fd, FR_EVENT_FILTER_IO, pause_write) < 0) fr_assert(0);

		return;
	}

	current = request;

	if (send_one_packet(client, request) < 0) fr_assert(0);
}


/**
 *
 * @hidecallgraph
 */
int main(int argc, char **argv)
{
	int		ret = EXIT_SUCCESS;
	int		c;
	char		const *raddb_dir = RADDBDIR;
	char		const *dict_dir = DICTDIR;
	char		filesecret[256];
	FILE		*fp;
	int		do_summary = false;
	TALLOC_CTX	*autofree;
	fr_dlist_head_t	filenames;

	/*
	 *	It's easier having two sets of flags to set the
	 *	verbosity of library calls and the verbosity of
	 *	radclient.
	 */
	fr_debug_lvl = 0;
	fr_log_fp = stdout;

	/*
	 *	Must be called first, so the handler is called last
	 */
	fr_atexit_global_setup();

	autofree = talloc_autofree_context();
#ifndef NDEBUG
	if (fr_fault_setup(autofree, getenv("PANIC_ACTION"), argv[0]) < 0) {
		fr_perror("radclient");
		fr_exit_now(EXIT_FAILURE);
	}
#endif

	talloc_set_log_stderr();

	fr_dlist_talloc_init(&rc_request_list, rc_request_t, entry);

	fr_dlist_talloc_init(&filenames, rc_file_pair_t, entry);

	/*
	 *	Always log to stdout
	 */
	default_log.dst = L_DST_STDOUT;
	default_log.fd = STDOUT_FILENO;
	default_log.print_level = false;

	fd_config = (fr_bio_fd_config_t) {
		.type = FR_BIO_FD_CONNECTED,
		.socket_type = SOCK_DGRAM,

		.src_ipaddr = (fr_ipaddr_t) {
			.af = AF_INET,
		},
		.dst_ipaddr = (fr_ipaddr_t) {
			.af = AF_INET,
		},

		.src_port = 0,
		.dst_port = 1812,

		.interface = NULL,

		.path = NULL,
		.filename = NULL,

		.async = false,
	};

	/*
	 *	Initialize our client configuration.
	 */
	client_config = (fr_radius_client_config_t) {
		.log = &default_log,
		.verify = {
			.require_message_authenticator = false,
			.max_attributes = RADIUS_MAX_ATTRIBUTES,
		},
	};

	while ((c = getopt(argc, argv, "46c:C:d:D:f:FhP:r:sS:t:vx")) != -1) switch (c) {
		case '4':
			fd_config.dst_ipaddr.af = AF_INET;
			break;

		case '6':
			fd_config.dst_ipaddr.af = AF_INET6;
			break;

		case 'c':
			if (!isdigit((uint8_t) *optarg)) usage();

			resend_count = atoi(optarg);

			if (resend_count < 1) usage();
			break;

		case 'C':
		{
			int tmp;

			if (strchr(optarg, ':')) {
				if (fr_inet_pton_port(&fd_config.src_ipaddr, &fd_config.src_port,
						      optarg, -1, AF_UNSPEC, true, false) < 0) {
					fr_perror("Failed parsing source address");
					fr_exit_now(1);
				}
				break;
			}

			tmp = atoi(optarg);
			if (tmp < 1 || tmp > 65535) usage();

			fd_config.src_port = (uint16_t)tmp;
		}
			break;

		case 'D':
			dict_dir = optarg;
			break;

		case 'd':
			raddb_dir = optarg;
			break;

			/*
			 *	packet,filter
			 */
		case 'f':
		{
			char const *p;
			rc_file_pair_t *files;

			MEM(files = talloc_zero(talloc_autofree_context(), rc_file_pair_t));

			/*
			 *	Commas are nicer than colons.
			 */
			c = ':';

			p = strchr(optarg, c);
			if (!p) {
				c = ',';
				p = strchr(optarg, c);
			}
			if (!p) {
				files->packets = optarg;
				files->filters = NULL;
			} else {
				MEM(files->packets = talloc_strndup(files, optarg, p - optarg));
				files->filters = p + 1;
			}
			fr_dlist_insert_tail(&filenames, files);
		}
			break;

		case 'F':
			print_filename = true;
			break;

		case 'P':
			if (!strcmp(optarg, "tcp")) {
				fd_config.socket_type = SOCK_STREAM;
				ipproto = IPPROTO_TCP;
			} else if (!strcmp(optarg, "udp")) {
				fd_config.socket_type = SOCK_DGRAM;
				ipproto = IPPROTO_UDP;
			} else {
				usage();
			}
			break;

		case 'r':
			if (!isdigit((uint8_t) *optarg)) usage();
			retries = atoi(optarg);
			if ((retries == 0) || (retries > 1000)) usage();
			break;

		case 's':
			do_summary = true;
			break;

		case 'S':
		{
			char *p;
			fp = fopen(optarg, "r");
			if (!fp) {
			       ERROR("Error opening %s: %s", optarg, fr_syserror(errno));
			       fr_exit_now(1);
			}
			if (fgets(filesecret, sizeof(filesecret), fp) == NULL) {
			       ERROR("Error reading %s: %s", optarg, fr_syserror(errno));
			       fr_exit_now(1);
			}
			fclose(fp);

			/* truncate newline */
			p = filesecret + strlen(filesecret) - 1;
			while ((p >= filesecret) &&
			      (*p < ' ')) {
			       *p = '\0';
			       --p;
			}

			if (strlen(filesecret) < 2) {
			       ERROR("Secret in %s is too short", optarg);
			       fr_exit_now(1);
			}
			secret = talloc_strdup(NULL, filesecret);
			client_config.verify.secret = (uint8_t *) secret;
			client_config.verify.secret_len = talloc_array_length(secret) - 1;
		}
		       break;

		case 't':
			if (fr_time_delta_from_str(&timeout, optarg, strlen(optarg), FR_TIME_RES_SEC) < 0) {
				fr_perror("Failed parsing timeout value");
				fr_exit_now(EXIT_FAILURE);
			}
			break;

		case 'v':
			fr_debug_lvl = 1;
			DEBUG("%s", radclient_version);
			fr_exit_now(0);

		case 'x':
			fr_debug_lvl++;
			if (fr_debug_lvl > 1) default_log.print_level = true;
			break;

		case 'h':
		default:
			usage();
	}
	argc -= (optind - 1);
	argv += (optind - 1);

	if ((argc < 3) || ((secret == NULL) && (argc < 4))) {
		ERROR("Insufficient arguments");
		usage();
	}
	/*
	 *	Mismatch between the binary and the libraries it depends on
	 */
	if (fr_check_lib_magic(RADIUSD_MAGIC_NUMBER) < 0) {
		fr_perror("radclient");
		fr_exit_now(EXIT_FAILURE);
	}

	if (!fr_dict_global_ctx_init(NULL, true, dict_dir)) {
		fr_perror("radclient");
		fr_exit_now(EXIT_FAILURE);
	}

	if (fr_radius_global_init() < 0) {
		fr_perror("radclient");
		fr_exit_now(EXIT_FAILURE);
	}

	if (fr_dict_autoload(radclient_dict) < 0) {
		fr_perror("radclient");
		exit(EXIT_FAILURE);
	}

	if (fr_dict_attr_autoload(radclient_dict_attr) < 0) {
		fr_perror("radclient");
		exit(EXIT_FAILURE);
	}

	if (fr_dict_read(fr_dict_unconst(dict_freeradius), raddb_dir, FR_DICTIONARY_FILE) == -1) {
		fr_log_perror(&default_log, L_ERR, __FILE__, __LINE__, NULL,
			      "Failed to initialize the dictionaries");
		exit(EXIT_FAILURE);
	}

	packet_global_init();

	fr_strerror_clear();	/* Clear the error buffer */

	/*
	 *	Get the request type
	 */
	if (!isdigit((uint8_t) argv[2][0])) {
		packet_code = fr_table_value_by_str(fr_radius_request_name_table, argv[2], -2);
		if (packet_code == -2) {
			ERROR("Unrecognised request type \"%s\"", argv[2]);
			usage();
		}
	} else {
		packet_code = atoi(argv[2]);
	}

	fr_assert(packet_code != 0);
	fr_assert(packet_code < FR_RADIUS_CODE_MAX);
	client_config.allowed[packet_code] = true;
	client_config.retry[packet_code] = (fr_retry_config_t) {
		.irt = fr_time_delta_from_sec(2),
		.mrt = fr_time_delta_from_sec(16),
		.mrd = fr_time_delta_from_sec(30),
		.mrc = 5,
	};
	client_config.retry_cfg.retry_config = client_config.retry[packet_code];

	/*
	 *	Resolve hostname.
	 */
	if (strcmp(argv[1], "-") != 0) {
		if (fr_inet_pton_port(&fd_config.dst_ipaddr, &fd_config.dst_port, argv[1], -1, fd_config.dst_ipaddr.af, true, true) < 0) {
			fr_perror("radclient");
			fr_exit_now(1);
		}

		/*
		 *	Work backwards from the port to determine the packet type
		 */
		if (packet_code == FR_RADIUS_CODE_UNDEFINED) packet_code = radclient_get_code(fd_config.dst_port);
	}
	radclient_get_port(packet_code, &fd_config.dst_port);

	/*
	 *	Add the secret.
	 */
	if (argv[3]) {
		secret = talloc_strdup(NULL, argv[3]);
		client_config.verify.secret = (uint8_t *) secret;
		client_config.verify.secret_len = talloc_array_length(secret) - 1;
	}

	/*
	 *	If no '-f' is specified, we're reading from stdin.
	 */
	if (fr_dlist_num_elements(&filenames) == 0) {
		rc_file_pair_t *files;

		files = talloc_zero(talloc_autofree_context(), rc_file_pair_t);
		files->packets = "-";
		if (radclient_init(files, files) < 0) fr_exit_now(1);
	}

	/*
	 *	Walk over the list of filenames, creating the requests.
	 */
	fr_dlist_foreach(&filenames, rc_file_pair_t, files) {
		if (radclient_init(files, files)) {
			ERROR("Failed parsing input files");
			fr_exit_now(1);
		}
	}

	/*
	 *	No packets read.  Die.
	 */
	if (!fr_dlist_num_elements(&rc_request_list)) {
		ERROR("Nothing to send");
		fr_exit_now(1);
	}

	openssl3_init();

	client_config.retry_cfg.el = fr_event_list_alloc(autofree, _loop_status, NULL);
	if (!client_config.retry_cfg.el) {
		ERROR("Failed opening event list: %s", fr_strerror());
		fr_exit_now(1);
	}

	client_bio = fr_radius_client_bio_alloc(autofree, &client_config, &fd_config);
	if (!client_bio) {
		ERROR("Failed opening socket: %s", fr_strerror());
		fr_exit_now(1);
	}

	/*
	 *	@todo - keep calling connect() when socket is readable?
	 */
	if (fr_radius_client_bio_connect(client_bio) < 0) {
		ERROR("Failed connecting socket: %s", fr_strerror());
		fr_exit_now(1);
	}

	bio = fr_radius_client_bio_get_fd(client_bio);
	fr_assert(bio != NULL);

	fd_info = fr_bio_fd_info(bio);
	fr_assert(fd_info != NULL);

	/*
	 *	Walk over the list of packets, sanity checking
	 *	everything.
	 */
	fr_dlist_foreach(&rc_request_list, rc_request_t, this) {
		this->packet->socket.inet.src_ipaddr = fd_config.src_ipaddr;
		this->packet->socket.inet.src_port = fd_config.src_port;
		if (radclient_sane(this) != 0) {
			fr_exit_now(1);
		}
	}

	if (fr_event_fd_insert(NULL, client_config.retry_cfg.el, fd_info->socket.fd, client_read, client_write, client_error, client_bio) < 0) {
		fr_perror("radclient");
		fr_exit_now(1);
	}

	(void) fr_event_loop(client_config.retry_cfg.el);

	fr_dlist_talloc_free(&rc_request_list);

	talloc_free(secret);

	fr_radius_global_free();

	packet_global_free();

	if (fr_dict_autofree(radclient_dict) < 0) {
		fr_perror("radclient");
		ret = EXIT_FAILURE;
	}

	talloc_free(autofree);

	if (do_summary) {
		fr_perror("Packet summary:\n"
		      "\tAccepted      : %" PRIu64 "\n"
		      "\tRejected      : %" PRIu64 "\n"
		      "\tLost          : %" PRIu64 "\n"
		      "\tPassed filter : %" PRIu64 "\n"
		      "\tFailed filter : %" PRIu64,
		      stats.accepted,
		      stats.rejected,
		      stats.lost,
		      stats.passed,
		      stats.failed
		);
	}

	/*
	 *	Ensure our atexit handlers run before any other
	 *	atexit handlers registered by third party libraries.
	 */
	fr_atexit_global_trigger_all();

	if ((stats.lost > 0) || (stats.failed > 0)) return EXIT_FAILURE;

	openssl3_free();

	return ret;
}
