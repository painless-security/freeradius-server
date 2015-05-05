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
 * @file rlm_csv.c
 * @brief Read and map CSV files
 *
 * @copyright 2015 The FreeRADIUS server project
 * @copyright 2015 Alan DeKok <aland@freeradius.org>
 */
RCSID("$Id$")

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
typedef struct rlm_csv_t {
	char const	*filename;
	char const	*delimiter;
	char const	*header;
	char const	*key;

	int		num_fields;
	int		used_fields;
	int		key_field;

	char const     	**field_names;
	int		*field_offsets; /* field X from the file maps to array entry Y here */
	rbtree_t	*tree;
} rlm_csv_t;

typedef struct rlm_csv_entry_t {
	struct rlm_csv_entry_t *next;
	char const *key;
	char const *data[];
} rlm_csv_entry_t;

/*
 *	A mapping of configuration file names to internal variables.
 */
static const CONF_PARSER module_config[] = {
	{ "filename", FR_CONF_OFFSET(PW_TYPE_FILE_INPUT | PW_TYPE_REQUIRED | PW_TYPE_NOT_EMPTY, rlm_csv_t, filename), NULL },
	{ "delimiter", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_REQUIRED | PW_TYPE_NOT_EMPTY, rlm_csv_t, delimiter), NULL },
	{ "header", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_REQUIRED | PW_TYPE_NOT_EMPTY, rlm_csv_t, header), NULL },
	{ "key_field", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_REQUIRED | PW_TYPE_NOT_EMPTY, rlm_csv_t, key), NULL },

	{ NULL, -1, 0, NULL, NULL }		/* end the list */
};

static int csv_entry_cmp(void const *one, void const *two)
{
	rlm_csv_entry_t const *a = one;
	rlm_csv_entry_t const *b = two;

	return strcmp(a->key, b->key);
}

/*
 *	Allow for quotation marks.
 */
static bool buf2entry(rlm_csv_t *inst, char *buf, char **out)
{
	char *p, *q;

	if (*buf != '"') {
		*out = strchr(buf + 1, *inst->delimiter);
		return true;
	}

	p = buf + 1;
	q = buf;

	while (*p) {
		/*
		 *	Double quotes to single quotes.
		 */
		if ((*p == '"') && (p[1] == '"')) {
			*(q++) = '"';
			p += 2;
			continue;
		}

		/*
		 *	Double quotes and EOL mean we're done.
		 */
		if ((*p == '"') && (p[1] < ' ')) {
			*(q++) = '\0';

			*out = NULL;
			return true;
		}

		/*
		 *	Double quotes and delimiter: point to the delimiter.
		 */
		if ((*p == '"') && (p[1] == *inst->delimiter)) {
			*(q++) = '\0';

			*out = p + 1;
			return true;
		}

		/*
		 *	Everything else gets copied over verbatim
		 */
		*(q++) = *(p++);
		*q = '\0';
	}

	return false;
}

/*
 *	Convert a buffer to a CSV entry
 */
static rlm_csv_entry_t *file2csv(CONF_SECTION *conf, rlm_csv_t *inst, int lineno, char *buffer)
{
	rlm_csv_entry_t *e;
	int i;
	char *p, *q;

	e = (rlm_csv_entry_t *) talloc_zero_array(inst->tree, uint8_t, sizeof(*e) + inst->used_fields + sizeof(e->data[0]));
	if (!e) {
		cf_log_err_cs(conf, "Out of memory");
		return NULL;
	}

	for (p = buffer, i = 0; p != NULL; p = q, i++) {
		if (!buf2entry(inst, p, &q)) {
			cf_log_err_cs(conf, "Malformed entry in file %s line %d", inst->filename, lineno);
			return NULL;
		}

		if (q) *(q++) = '\0';

		if (i >= inst->num_fields) {
			cf_log_err_cs(conf, "Too many fields at file %s line %d", inst->filename, lineno);
			return NULL;
		}

		/*
		 *	This is the key field.
		 */
		if (i == inst->key_field) {
			e->key = talloc_strdup(e, p);
			continue;
		}

		/*
		 *	This field is unused.  Ignore it.
		 */
		if (inst->field_offsets[i] < 0) continue;

		e->data[inst->field_offsets[i]] = talloc_strdup(e, p);
	}

	if (i < inst->num_fields) {
		cf_log_err_cs(conf, "Too few fields at file %s line %d (%d < %d)", inst->filename, lineno, i, inst->num_fields);
		return NULL;
	}

	DEBUG("###################################################################### LINE %d key %s entry %s",
	      lineno, e->key, e->data[0]);


	/*
	 *	FIXME: Allow duplicate keys later.
	 */
	if (!rbtree_insert(inst->tree, e)) {
		cf_log_err_cs(conf, "Failed inserting entry for filename %s line %d: duplicate entry",
			      inst->filename, lineno);
		return NULL;
	}

	return e;
}

/*
 *	Do any per-module initialization that is separate to each
 *	configured instance of the module.  e.g. set up connections
 *	to external databases, read configuration files, set up
 *	dictionary entries, etc.
 *
 *	If configuration information is given in the config section
 *	that must be referenced in later calls, store a handle to it
 *	in *instance otherwise put a null pointer there.
 */
static int mod_instantiate(CONF_SECTION *conf, void *instance)
{
	rlm_csv_t *inst = instance;
	int i;
	char const *p;
	char *q;
	char *header;
	FILE *fp;
	int lineno;
	char buffer[8192];

	if (inst->delimiter[1]) {
		cf_log_err_cs(conf, "'delimiter' must be one character long");
		return -1;
	}

	for (p = inst->header; p != NULL; p = strchr(p + 1, *inst->delimiter)) {
		inst->num_fields++;
	}

	if (inst->num_fields < 2) {
		cf_log_err_cs(conf, "Must have at least a key field and data field");
		return -1;
	}

	inst->field_names = talloc_array(inst, const char *, inst->num_fields);
	if (!inst->field_names) {
	oom:
		cf_log_err_cs(conf, "Out of memory");
		return -1;
	}

	inst->field_offsets = talloc_array(inst, int, inst->num_fields);
	if (!inst->field_offsets) goto oom;

	for (i = 0; i < inst->num_fields; i++) {
		inst->field_offsets[i] = -1; /* unused */
	}

	/*
	 *	Get a writeable copy of the header
	 */
	header = talloc_strdup(inst, inst->header);
	if (!header) goto oom;

	/*
	 *	Mark up the field names.  Note that they can be empty,
	 *	in which case they don't map to anything.
	 */
	inst->key_field = -1;

	/*
	 *	FIXME: remove whitespace from field names, if we care.
	 */
	for (p = header, i = 0; p != NULL; p = q, i++) {
		q = strchr(p, *inst->delimiter);

		/*
		 *	Fields 0..N-1
		 */
		if (q) {
			*q = '\0';

			if (q > (p + 1)) {
					if (strcmp(p, inst->key) == 0) {
					inst->key_field = i;
				} else {
					inst->field_offsets[i] = inst->used_fields++;
				}
			}
			q++;

		} else {	/* field N */
			if (*p) {
				if (strcmp(p, inst->key) == 0) {
					inst->key_field = i;
				} else {
					inst->field_offsets[i] = inst->used_fields++;
				}
			}
		}

		/*
		 *	Save the field names, even when they're not used.
		 */
		inst->field_names[i] = p;
	}

	if (inst->key_field < 0) {
		cf_log_err_cs(conf, "Key field '%s' does not appear in header", inst->key);
		return -1;
	}

	inst->tree = rbtree_create(inst, csv_entry_cmp, NULL, 0);
	if (!inst->tree) goto oom;

	/*
	 *	Read the file line by line.
	 */
	fp = fopen(inst->filename, "r");
	if (!fp) {
		cf_log_err_cs(conf, "Error opening filename %s: %s", inst->filename, strerror(errno));
		return -1;
	}

	lineno = 1;
	while (fgets(buffer, sizeof(buffer), fp)) {
		rlm_csv_entry_t *e;

		e = file2csv(conf, inst, lineno, buffer);
		if (!e) {
			fclose(fp);
			return -1;
		}

		lineno++;
	}

	fclose(fp);

	/*
	 *	And register the map function.
	 */

	return 0;
}

extern module_t rlm_csv;
module_t rlm_csv = {
	.magic		= RLM_MODULE_INIT,
	.name		= "csv",
	.type		= 0,
	.inst_size	= sizeof(rlm_csv_t),
	.config		= module_config,
	.instantiate	= mod_instantiate,
};
