
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
 * @file rlm_ratelimit.h
 * @brief Allow FreeRADIUS to rate limit requests.
 *
 * @copyright 2024 The FreeRADIUS server project
 * @copyright 2024 your name \<your address\>
 */
RCSIDH(ratelimit_h, "$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/rad_assert.h>

/*
 *	Define a structure for our module configuration.
 *
 *	These variables do not need to be in a structure, but it's
 *	a lot cleaner to do so, and a pointer to the structure can
 *	be used as the instance handle.
 */
typedef struct rlm_ratelimit_t {
	uint32_t tokenmax;
	uint32_t refreshrate;
	uint32_t datastoresize;

	void *datastore;
} rlm_ratelimit_t;

/**
 *	A mapping of configuration parameter to internal variables.
 */
static const CONF_PARSER module_config[] = {
	{ "tokenmax", FR_CONF_OFFSET(PW_TYPE_INTEGER, rlm_ratelimit_t, tokenmax), "10" },
	{ "refreshrate", FR_CONF_OFFSET(PW_TYPE_INTEGER, rlm_ratelimit_t, refreshrate), "5" },
	{ "datastoresize", FR_CONF_OFFSET(PW_TYPE_INTEGER, rlm_ratelimit_t, datastoresize), "16777215" },
	CONF_PARSER_TERMINATOR
};
