/*
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 2001 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef DFILTER_INT_H
#define DFILTER_INT_H

#include "dfilter.h"
#include "syntax-tree.h"

#include <epan/proto.h>
#include <stdio.h>

/* Passed back to user */
struct epan_dfilter {
	GPtrArray	*insns;
	GPtrArray	*consts;
	guint		num_registers;
	guint		max_registers;
	GList		**registers;
	gboolean	*attempted_load;
	gboolean	*owns_memory;
	int		*interesting_fields;
	int		num_interesting_fields;
	GPtrArray	*deprecated;
};

typedef struct {
	/* Syntax Tree stuff */
	stnode_t	*st_root;
	gboolean	syntax_error;
	gchar		*error_message;
	GPtrArray	*insns;
	GPtrArray	*consts;
	GHashTable	*loaded_fields;
	GHashTable	*interesting_fields;
	int		next_insn_id;
	int		next_const_id;
	int		next_register;
	int		first_constant; /* first register used as a constant */
	GPtrArray	*deprecated;
} dfwork_t;

/*
 * State kept by the scanner.
 */
typedef struct {
	dfwork_t *dfw;
	GString* quoted_string;
	gboolean raw_string;
	gboolean in_set;	/* true if parsing set elements for the membership operator */
} df_scanner_state_t;

typedef struct {
	char *value;
} df_lval_t;

static inline df_lval_t *
df_lval_new(void)
{
	return g_new0(df_lval_t, 1);
}

static inline char *
df_lval_value(df_lval_t *lval)
{
	return lval->value;
}

static inline void
df_lval_free(df_lval_t *lval)
{
	if (lval) {
		g_free(lval->value);
		g_free(lval);
	}
}

/* Constructor/Destructor prototypes for Lemon Parser */
void *DfilterAlloc(void* (*)(gsize));

void DfilterFree(void*, void (*)(void *));

void Dfilter(void*, int, df_lval_t*, dfwork_t*);

/* Return value for error in scanner. */
#define SCAN_FAILED	-1	/* not 0, as that means end-of-input */

/* Set dfw->error_message */
void
dfilter_fail(dfwork_t *dfw, const char *format, ...) G_GNUC_PRINTF(2, 3);

void
dfilter_parse_fail(dfwork_t *dfw, const char *format, ...) G_GNUC_PRINTF(2, 3);

void
add_deprecated_token(dfwork_t *dfw, const char *token);

void
free_deprecated(GPtrArray *deprecated);

void
DfilterTrace(FILE *TraceFILE, char *zTracePrompt);

stnode_t *
dfilter_new_function(dfwork_t *dfw, const char *name);

stnode_t *
dfilter_new_regex(dfwork_t *dfw, stnode_t *node);

stnode_t *
dfilter_resolve_unparsed(dfwork_t *dfw, stnode_t *node);

const char *tokenstr(int token);

#endif
