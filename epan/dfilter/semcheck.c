/*
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 2001 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#define WS_LOG_DOMAIN LOG_DOMAIN_DFILTER

#include <string.h>

#include "dfilter-int.h"
#include "semcheck.h"
#include "syntax-tree.h"
#include "sttype-range.h"
#include "sttype-test.h"
#include "sttype-set.h"
#include "sttype-function.h"

#include <epan/exceptions.h>
#include <epan/packet.h>

#include <wsutil/ws_assert.h>
#include <wsutil/wslog.h>

#include <ftypes/ftypes-int.h>


static void
semcheck(dfwork_t *dfw, stnode_t *st_node);

static stnode_t*
check_param_entity(dfwork_t *dfw, stnode_t *st_node);

static void
check_function(dfwork_t *dfw, stnode_t *st_node);

static fvalue_t *
mk_fvalue_from_val_string(dfwork_t *dfw, header_field_info *hfinfo, const char *s);

typedef gboolean (*FtypeCanFunc)(enum ftenum);

/* Compares to ftenum_t's and decides if they're
 * compatible or not (if they're the same basic type) */
static gboolean
compatible_ftypes(ftenum_t a, ftenum_t b)
{
	switch (a) {
		case FT_NONE:
		case FT_PROTOCOL:
		case FT_FLOAT:		/* XXX - should be able to compare with INT */
		case FT_DOUBLE:		/* XXX - should be able to compare with INT */
		case FT_ABSOLUTE_TIME:
		case FT_RELATIVE_TIME:
		case FT_IEEE_11073_SFLOAT:
		case FT_IEEE_11073_FLOAT:
		case FT_IPv4:
		case FT_IPv6:
		case FT_IPXNET:
		case FT_INT40:		/* XXX - should be able to compare with INT */
		case FT_UINT40:		/* XXX - should be able to compare with INT */
		case FT_INT48:		/* XXX - should be able to compare with INT */
		case FT_UINT48:		/* XXX - should be able to compare with INT */
		case FT_INT56:		/* XXX - should be able to compare with INT */
		case FT_UINT56:		/* XXX - should be able to compare with INT */
		case FT_INT64:		/* XXX - should be able to compare with INT */
		case FT_UINT64:		/* XXX - should be able to compare with INT */
		case FT_EUI64:		/* XXX - should be able to compare with INT */
			return a == b;

		case FT_ETHER:
		case FT_BYTES:
		case FT_UINT_BYTES:
		case FT_GUID:
		case FT_OID:
		case FT_AX25:
		case FT_VINES:
		case FT_FCWWN:
		case FT_REL_OID:
		case FT_SYSTEM_ID:

			return (b == FT_ETHER || b == FT_BYTES || b == FT_UINT_BYTES || b == FT_GUID || b == FT_OID || b == FT_AX25 || b == FT_VINES || b == FT_FCWWN || b == FT_REL_OID || b == FT_SYSTEM_ID);

		case FT_BOOLEAN:
		case FT_FRAMENUM:
		case FT_CHAR:
		case FT_UINT8:
		case FT_UINT16:
		case FT_UINT24:
		case FT_UINT32:
		case FT_INT8:
		case FT_INT16:
		case FT_INT24:
		case FT_INT32:
			switch (b) {
				case FT_BOOLEAN:
				case FT_FRAMENUM:
				case FT_CHAR:
				case FT_UINT8:
				case FT_UINT16:
				case FT_UINT24:
				case FT_UINT32:
				case FT_INT8:
				case FT_INT16:
				case FT_INT24:
				case FT_INT32:
					return TRUE;
				default:
					return FALSE;
			}

		case FT_STRING:
		case FT_STRINGZ:
		case FT_UINT_STRING:
		case FT_STRINGZPAD:
		case FT_STRINGZTRUNC:
			switch (b) {
				case FT_STRING:
				case FT_STRINGZ:
				case FT_UINT_STRING:
				case FT_STRINGZPAD:
				case FT_STRINGZTRUNC:
					return TRUE;
				default:
					return FALSE;
			}

		case FT_NUM_TYPES:
			ws_assert_not_reached();
	}

	ws_assert_not_reached();
	return FALSE;
}

/* Gets an fvalue from a string, and sets the error message on failure. */
WS_RETNONNULL
static fvalue_t*
dfilter_fvalue_from_unparsed(dfwork_t *dfw, ftenum_t ftype, stnode_t *st,
		gboolean allow_partial_value, header_field_info *hfinfo_value_string)
{
	fvalue_t *fv;
	const char *s = stnode_data(st);

	/* Don't set the error message if it's already set. */
	fv = fvalue_from_unparsed(ftype, s, allow_partial_value,
		dfw->error_message == NULL ? &dfw->error_message : NULL);
	if (fv == NULL && hfinfo_value_string) {
		/* check value_string */
		fv = mk_fvalue_from_val_string(dfw, hfinfo_value_string, s);
		/*
		 * Ignore previous errors if this can be mapped
		 * to an item from value_string.
		 */
		if (fv && dfw->error_message) {
			g_free(dfw->error_message);
			dfw->error_message = NULL;
		}
	}
	if (fv == NULL)
		THROW(TypeError);
	return fv;
}

/* Gets an fvalue from a string, and sets the error message on failure. */
WS_RETNONNULL
static fvalue_t*
dfilter_fvalue_from_string(dfwork_t *dfw, ftenum_t ftype, stnode_t *st,
		header_field_info *hfinfo_value_string)
{
	fvalue_t *fv;
	const char *s = stnode_data(st);

	fv = fvalue_from_string(ftype, s,
	    dfw->error_message == NULL ? &dfw->error_message : NULL);
	if (fv == NULL && hfinfo_value_string) {
		fv = mk_fvalue_from_val_string(dfw, hfinfo_value_string, s);
		/*
		 * Ignore previous errors if this can be mapped
		 * to an item from value_string.
		 */
		if (fv && dfw->error_message) {
			g_free(dfw->error_message);
			dfw->error_message = NULL;
		}
	}
	if (fv == NULL)
		THROW(TypeError);
	return fv;
}

/* Creates a FT_UINT32 fvalue with a given value. */
static fvalue_t*
mk_uint32_fvalue(guint32 val)
{
	fvalue_t *fv;

	fv = fvalue_new(FT_UINT32);
	fvalue_set_uinteger(fv, val);

	return fv;
}

/* Creates a FT_UINT64 fvalue with a given value. */
static fvalue_t*
mk_uint64_fvalue(guint64 val)
{
	fvalue_t *fv;

	fv = fvalue_new(FT_UINT64);
	fvalue_set_uinteger64(fv, val);

	return fv;
}

/* Try to make an fvalue from a string using a value_string or true_false_string.
 * This works only for ftypes that are integers. Returns the created fvalue_t*
 * or NULL if impossible. */
static fvalue_t*
mk_fvalue_from_val_string(dfwork_t *dfw, header_field_info *hfinfo, const char *s)
{
	static const true_false_string  default_tf = { "True", "False" };
	const true_false_string		*tf = &default_tf;

	/* Early return? */
	switch(hfinfo->type) {
		case FT_NONE:
		case FT_PROTOCOL:
		case FT_FLOAT:
		case FT_DOUBLE:
		case FT_IEEE_11073_SFLOAT:
		case FT_IEEE_11073_FLOAT:
		case FT_ABSOLUTE_TIME:
		case FT_RELATIVE_TIME:
		case FT_IPv4:
		case FT_IPv6:
		case FT_IPXNET:
		case FT_AX25:
		case FT_VINES:
		case FT_FCWWN:
		case FT_ETHER:
		case FT_BYTES:
		case FT_UINT_BYTES:
		case FT_STRING:
		case FT_STRINGZ:
		case FT_UINT_STRING:
		case FT_STRINGZPAD:
		case FT_STRINGZTRUNC:
		case FT_EUI64:
		case FT_GUID:
		case FT_OID:
		case FT_REL_OID:
		case FT_SYSTEM_ID:
		case FT_FRAMENUM: /* hfinfo->strings contains ft_framenum_type_t, not strings */
			return NULL;

		case FT_BOOLEAN:
		case FT_CHAR:
		case FT_UINT8:
		case FT_UINT16:
		case FT_UINT24:
		case FT_UINT32:
		case FT_UINT40:
		case FT_UINT48:
		case FT_UINT56:
		case FT_UINT64:
		case FT_INT8:
		case FT_INT16:
		case FT_INT24:
		case FT_INT32:
		case FT_INT40:
		case FT_INT48:
		case FT_INT56:
		case FT_INT64:
			break;

		case FT_NUM_TYPES:
			ws_assert_not_reached();
	}

	/* TRUE/FALSE *always* exist for FT_BOOLEAN. */
	if (hfinfo->type == FT_BOOLEAN) {
		if (hfinfo->strings) {
			tf = (const true_false_string *)hfinfo->strings;
		}

		if (g_ascii_strcasecmp(s, tf->true_string) == 0) {
			return mk_uint64_fvalue(TRUE);
		}
		else if (g_ascii_strcasecmp(s, tf->false_string) == 0) {
			return mk_uint64_fvalue(FALSE);
		}
		else {
			/*
			 * Prefer this error message to whatever error message
			 * has already been set.
			 */
			g_free(dfw->error_message);
			dfw->error_message = NULL;
			dfilter_fail(dfw, "\"%s\" cannot be found among the possible values for %s.",
				s, hfinfo->abbrev);
			return NULL;
		}
	}

	/* Do val_strings exist? */
	if (!hfinfo->strings) {
		dfilter_fail(dfw, "%s cannot accept strings as values.",
				hfinfo->abbrev);
		return NULL;
	}

	/* Reset the error message, since *something* interesting will happen,
	 * and the error message will be more interesting than any error message
	 * I happen to have now. */
	g_free(dfw->error_message);
	dfw->error_message = NULL;

	if (hfinfo->display & BASE_RANGE_STRING) {
		dfilter_fail(dfw, "\"%s\" cannot accept [range] strings as values.",
				hfinfo->abbrev);
	}
	else if (hfinfo->display & BASE_VAL64_STRING) {
		const val64_string *vals = (const val64_string *)hfinfo->strings;

		while (vals->strptr != NULL) {
			if (g_ascii_strcasecmp(s, vals->strptr) == 0) {
				return mk_uint64_fvalue(vals->value);
			}
			vals++;
		}
		dfilter_fail(dfw, "\"%s\" cannot be found among the possible values for %s.",
				s, hfinfo->abbrev);
	}
	else if (hfinfo->display == BASE_CUSTOM) {
		/*  If a user wants to match against a custom string, we would
		 *  somehow have to have the integer value here to pass it in
		 *  to the custom-display function.  But we don't have an
		 *  integer, we have the string they're trying to match.
		 *  -><-
		 */
		dfilter_fail(dfw, "\"%s\" cannot accept [custom] strings as values.",
				hfinfo->abbrev);
	}
	else {
		const value_string *vals = (const value_string *)hfinfo->strings;
		if (hfinfo->display & BASE_EXT_STRING)
			vals = VALUE_STRING_EXT_VS_P((const value_string_ext *) vals);

		while (vals->strptr != NULL) {
			if (g_ascii_strcasecmp(s, vals->strptr) == 0) {
				return mk_uint32_fvalue(vals->value);
			}
			vals++;
		}
		dfilter_fail(dfw, "\"%s\" cannot be found among the possible values for %s.",
				s, hfinfo->abbrev);
	}
	return NULL;
}

static gboolean
is_bytes_type(enum ftenum type)
{
	switch(type) {
		case FT_AX25:
		case FT_VINES:
		case FT_FCWWN:
		case FT_ETHER:
		case FT_BYTES:
		case FT_UINT_BYTES:
		case FT_IPv6:
		case FT_GUID:
		case FT_OID:
		case FT_REL_OID:
		case FT_SYSTEM_ID:
			return TRUE;

		case FT_NONE:
		case FT_PROTOCOL:
		case FT_FLOAT:
		case FT_DOUBLE:
		case FT_IEEE_11073_SFLOAT:
		case FT_IEEE_11073_FLOAT:
		case FT_ABSOLUTE_TIME:
		case FT_RELATIVE_TIME:
		case FT_IPv4:
		case FT_IPXNET:
		case FT_STRING:
		case FT_STRINGZ:
		case FT_UINT_STRING:
		case FT_STRINGZPAD:
		case FT_STRINGZTRUNC:
		case FT_BOOLEAN:
		case FT_FRAMENUM:
		case FT_CHAR:
		case FT_UINT8:
		case FT_UINT16:
		case FT_UINT24:
		case FT_UINT32:
		case FT_UINT40:
		case FT_UINT48:
		case FT_UINT56:
		case FT_UINT64:
		case FT_INT8:
		case FT_INT16:
		case FT_INT24:
		case FT_INT32:
		case FT_INT40:
		case FT_INT48:
		case FT_INT56:
		case FT_INT64:
		case FT_EUI64:
			return FALSE;

		case FT_NUM_TYPES:
			ws_assert_not_reached();
	}

	ws_assert_not_reached();
	return FALSE;
}

/* Check the semantics of an existence test. */
static void
check_exists(dfwork_t *dfw, stnode_t *st_arg1)
{
	static guint i = 0;

	ws_debug("4 check_exists() [%u]", i++);
	log_stnode(st_arg1);

	switch (stnode_type_id(st_arg1)) {
		case STTYPE_FIELD:
			/* This is OK */
			break;
		case STTYPE_STRING:
		case STTYPE_CHARCONST:
		case STTYPE_UNPARSED:
			dfilter_fail(dfw, "\"%s\" is neither a field nor a protocol name.",
					(char *)stnode_data(st_arg1));
			THROW(TypeError);
			break;

		case STTYPE_RANGE:
			/*
			 * XXX - why not?  Shouldn't "eth[3:2]" mean
			 * "check whether the 'eth' field is present and
			 * has at least 2 bytes starting at an offset of
			 * 3"?
			 */
			dfilter_fail(dfw, "You cannot test whether a range is present.");
			THROW(TypeError);
			break;

		case STTYPE_FUNCTION:
			/* XXX - Maybe we should change functions so they can return fields,
			 * in which case the 'exist' should be fine. */
			dfilter_fail(dfw, "You cannot test whether a function is present.");
			THROW(TypeError);
			break;

		case STTYPE_UNINITIALIZED:
		case STTYPE_TEST:
		case STTYPE_FVALUE:
		case STTYPE_SET:
		case STTYPE_PCRE:
		case STTYPE_NUM_TYPES:
			ws_assert_not_reached();
	}
}

static void
check_drange_sanity(dfwork_t *dfw, stnode_t *st)
{
	stnode_t		*entity1;
	header_field_info	*hfinfo1;
	ftenum_t		ftype1;

	entity1 = sttype_range_entity(st);
	if (entity1 && stnode_type_id(entity1) == STTYPE_FIELD) {
		hfinfo1 = (header_field_info *)stnode_data(entity1);
		ftype1 = hfinfo1->type;

		if (!ftype_can_slice(ftype1)) {
			dfilter_fail(dfw, "\"%s\" is a %s and cannot be sliced into a sequence of bytes.",
					hfinfo1->abbrev, ftype_pretty_name(ftype1));
			THROW(TypeError);
		}
	} else if (entity1 && stnode_type_id(entity1) == STTYPE_FUNCTION) {
		df_func_def_t *funcdef = sttype_function_funcdef(entity1);
		ftype1 = funcdef->retval_ftype;

		if (!ftype_can_slice(ftype1)) {
			dfilter_fail(dfw, "Return value of function \"%s\" is a %s and cannot be converted into a sequence of bytes.",
					funcdef->name, ftype_pretty_name(ftype1));
			THROW(TypeError);
		}

		check_function(dfw, entity1);
	} else if (entity1 && stnode_type_id(entity1) == STTYPE_RANGE) {
		/* Should this be rejected instead? */
		check_drange_sanity(dfw, entity1);
	} else if (entity1) {
		dfilter_fail(dfw, "Range is not supported for entity %s of type %s",
					stnode_todisplay(entity1), stnode_type_name(entity1));
		THROW(TypeError);
	} else {
		dfilter_fail(dfw, "Range is not supported, details: " G_STRLOC " entity: NULL");
		THROW(TypeError);
	}
}

static void
convert_to_bytes(stnode_t *arg)
{
	stnode_t      *entity1;
	drange_node   *rn;

	entity1 = stnode_dup(arg);
	rn = drange_node_new();
	drange_node_set_start_offset(rn, 0);
	drange_node_set_to_the_end(rn);

	stnode_replace(arg, STTYPE_RANGE, NULL);
	sttype_range_set1(arg, entity1, rn);
}

static void
check_function(dfwork_t *dfw, stnode_t *st_node)
{
	df_func_def_t *funcdef;
	GSList        *params;
	guint          iparam;
	guint          nparams;

	funcdef  = sttype_function_funcdef(st_node);
	params   = sttype_function_params(st_node);
	nparams  = g_slist_length(params);

	if (nparams < funcdef->min_nargs) {
		dfilter_fail(dfw, "Function %s needs at least %u arguments.",
			funcdef->name, funcdef->min_nargs);
		THROW(TypeError);
	} else if (nparams > funcdef->max_nargs) {
		dfilter_fail(dfw, "Function %s can only accept %u arguments.",
			funcdef->name, funcdef->max_nargs);
		THROW(TypeError);
	}

	iparam = 0;
	while (params) {
		params->data = check_param_entity(dfw, (stnode_t *)params->data);
		funcdef->semcheck_param_function(dfw, iparam, (stnode_t *)params->data);
		params = params->next;
		iparam++;
	}
}

/* Convert a character constant to a 1-byte BYTE_STRING containing the
 * character. */
WS_RETNONNULL
static fvalue_t *
dfilter_fvalue_from_charconst_string(dfwork_t *dfw, ftenum_t ftype, stnode_t *st, gboolean allow_partial_value)
{
	fvalue_t *fvalue;
	const char *s = stnode_data(st);

	fvalue = fvalue_from_unparsed(ftype, s, allow_partial_value,
			dfw->error_message == NULL ? &dfw->error_message : NULL);
	if (fvalue == NULL)
		THROW(TypeError);

	char *temp_string;
	/* It's valid. Create a 1-byte BYTE_STRING from its value. */
	temp_string = g_strdup_printf("%02x", fvalue->value.uinteger);
	FVALUE_FREE(fvalue);
	fvalue = fvalue_from_unparsed(ftype, temp_string, allow_partial_value, NULL);
	ws_assert(fvalue);
	g_free(temp_string);

	return fvalue;
}

/* If the LHS of a relation test is a FIELD, run some checks
 * and possibly some modifications of syntax tree nodes. */
static void
check_relation_LHS_FIELD(dfwork_t *dfw, const char *relation_string,
		FtypeCanFunc can_func, gboolean allow_partial_value,
		stnode_t *st_node, stnode_t *st_arg1, stnode_t *st_arg2)
{
	sttype_id_t		type2;
	header_field_info	*hfinfo1, *hfinfo2;
	df_func_def_t		*funcdef;
	ftenum_t		ftype1, ftype2;
	fvalue_t		*fvalue;

	type2 = stnode_type_id(st_arg2);

	hfinfo1 = (header_field_info*)stnode_data(st_arg1);
	ftype1 = hfinfo1->type;

	if (stnode_type_id(st_node) == STTYPE_TEST) {
		ws_debug("5 check_relation_LHS_FIELD(%s)", relation_string);
	} else {
		ws_debug("6 check_relation_LHS_FIELD(%s)", relation_string);
	}

	if (!can_func(ftype1)) {
		dfilter_fail(dfw, "%s (type=%s) cannot participate in '%s' comparison.",
				hfinfo1->abbrev, ftype_pretty_name(ftype1),
				relation_string);
		THROW(TypeError);
	}

	if (type2 == STTYPE_FIELD) {
		hfinfo2 = (header_field_info*)stnode_data(st_arg2);
		ftype2 = hfinfo2->type;

		if (!compatible_ftypes(ftype1, ftype2)) {
			dfilter_fail(dfw, "%s and %s are not of compatible types.",
					hfinfo1->abbrev, hfinfo2->abbrev);
			THROW(TypeError);
		}
		/* Do this check even though you'd think that if
		 * they're compatible, then can_func() would pass. */
		if (!can_func(ftype2)) {
			dfilter_fail(dfw, "%s (type=%s) cannot participate in specified comparison.",
					hfinfo2->abbrev, ftype_pretty_name(ftype2));
			THROW(TypeError);
		}
	}
	else if (type2 == STTYPE_STRING || type2 == STTYPE_UNPARSED ||
	         type2 == STTYPE_CHARCONST) {
		/* Skip incompatible fields */
		while (hfinfo1->same_name_prev_id != -1 &&
				((type2 == STTYPE_STRING && ftype1 != FT_STRING && ftype1!= FT_STRINGZ) ||
				(type2 != STTYPE_STRING && (ftype1 == FT_STRING || ftype1== FT_STRINGZ)))) {
			hfinfo1 = proto_registrar_get_nth(hfinfo1->same_name_prev_id);
			ftype1 = hfinfo1->type;
		}

		if (type2 == STTYPE_STRING) {
			fvalue = dfilter_fvalue_from_string(dfw, ftype1, st_arg2, hfinfo1);
		}
		else if (type2 == STTYPE_CHARCONST &&
		    strcmp(relation_string, "contains") == 0) {
			/* The RHS should be the same type as the LHS,
			 * but a character is just a one-byte byte
			 * string. */
			fvalue = dfilter_fvalue_from_charconst_string(dfw, ftype1, st_arg2, allow_partial_value);
		}
		else {
			fvalue = dfilter_fvalue_from_unparsed(dfw, ftype1, st_arg2, allow_partial_value, hfinfo1);
		}
		stnode_replace(st_arg2, STTYPE_FVALUE, fvalue);
	}
	else if (type2 == STTYPE_RANGE) {
		check_drange_sanity(dfw, st_arg2);
		if (!is_bytes_type(ftype1)) {
			if (!ftype_can_slice(ftype1)) {
				dfilter_fail(dfw, "\"%s\" is a %s and cannot be converted into a sequence of bytes.",
						hfinfo1->abbrev,
						ftype_pretty_name(ftype1));
				THROW(TypeError);
			}

			/* Convert entire field to bytes */
			convert_to_bytes(st_arg1);
		}
	}
	else if (type2 == STTYPE_FUNCTION) {
		funcdef = sttype_function_funcdef(st_arg2);
		ftype2 = funcdef->retval_ftype;

		if (!compatible_ftypes(ftype1, ftype2)) {
			dfilter_fail(dfw, "%s (type=%s) and return value of %s() (type=%s) are not of compatible types.",
					hfinfo1->abbrev, ftype_pretty_name(ftype1),
					funcdef->name, ftype_pretty_name(ftype2));
			THROW(TypeError);
		}

		if (!can_func(ftype2)) {
			dfilter_fail(dfw, "return value of %s() (type=%s) cannot participate in specified comparison.",
					funcdef->name, ftype_pretty_name(ftype2));
			THROW(TypeError);
		}

		check_function(dfw, st_arg2);
	}
	else if (type2 == STTYPE_SET) {
		GSList *nodelist;
		/* A set should only ever appear on RHS of 'in' operation */
		if (strcmp(relation_string, "in") != 0) {
			ws_assert_not_reached();
		}
		/* Attempt to interpret one element of the set at a time. Each
		 * element is represented by two items in the list, the element
		 * value and NULL. Both will be replaced by a lower and upper
		 * value if the element is a range. */
		nodelist = (GSList*)stnode_data(st_arg2);
		while (nodelist) {
			stnode_t *node = (stnode_t*)nodelist->data;
			/* Don't let a range on the RHS affect the LHS field. */
			if (stnode_type_id(node) == STTYPE_RANGE) {
				dfilter_fail(dfw, "A range may not appear inside a set.");
				THROW(TypeError);
				break;
			}

			nodelist = g_slist_next(nodelist);
			ws_assert(nodelist);
			stnode_t *node_right = (stnode_t *)nodelist->data;
			if (node_right) {
				/* range type, check if comparison is possible. */
				if (!ftype_can_ge(ftype1)) {
					dfilter_fail(dfw, "%s (type=%s) cannot participate in '%s' comparison.",
							hfinfo1->abbrev, ftype_pretty_name(ftype1),
							">=");
					THROW(TypeError);
				}
				check_relation_LHS_FIELD(dfw, ">=", ftype_can_ge,
						allow_partial_value, st_arg2, st_arg1, node);
				check_relation_LHS_FIELD(dfw, "<=", ftype_can_le,
						allow_partial_value, st_arg2, st_arg1, node_right);
			} else {
				check_relation_LHS_FIELD(dfw, "==", can_func,
						allow_partial_value, st_arg2, st_arg1, node);
			}
			nodelist = g_slist_next(nodelist);
		}
	}
	else if (type2 == STTYPE_PCRE) {
		ws_assert_streq(relation_string, "matches");
	}
	else {
		ws_assert_not_reached();
	}
}

static void
check_relation_LHS_STRING(dfwork_t *dfw, const char* relation_string,
		FtypeCanFunc can_func, gboolean allow_partial_value _U_,
		stnode_t *st_node _U_,
		stnode_t *st_arg1, stnode_t *st_arg2)
{
	sttype_id_t		type2;
	header_field_info	*hfinfo2;
	df_func_def_t		*funcdef;
	ftenum_t		ftype2;
	fvalue_t		*fvalue;

	type2 = stnode_type_id(st_arg2);

	ws_debug("5 check_relation_LHS_STRING()");

	if (type2 == STTYPE_FIELD) {
		hfinfo2 = (header_field_info*)stnode_data(st_arg2);
		ftype2 = hfinfo2->type;

		if (!can_func(ftype2)) {
			dfilter_fail(dfw, "%s (type=%s) cannot participate in '%s' comparison.",
					hfinfo2->abbrev, ftype_pretty_name(ftype2),
					relation_string);
			THROW(TypeError);
		}

		fvalue = dfilter_fvalue_from_string(dfw, ftype2, st_arg1, hfinfo2);
		stnode_replace(st_arg1, STTYPE_FVALUE, fvalue);
	}
	else if (type2 == STTYPE_STRING || type2 == STTYPE_UNPARSED ||
	         type2 == STTYPE_CHARCONST) {
		/* Well now that's silly... */
		dfilter_fail(dfw, "Neither \"%s\" nor \"%s\" are field or protocol names.",
				(char *)stnode_data(st_arg1),
				(char *)stnode_data(st_arg2));
		THROW(TypeError);
	}
	else if (type2 == STTYPE_RANGE) {
		check_drange_sanity(dfw, st_arg2);
		fvalue = dfilter_fvalue_from_string(dfw, FT_BYTES, st_arg1, NULL);
		stnode_replace(st_arg1, STTYPE_FVALUE, fvalue);
	}
	else if (type2 == STTYPE_FUNCTION) {
		check_function(dfw, st_arg2);

		funcdef = sttype_function_funcdef(st_arg2);
		ftype2  = funcdef->retval_ftype;

		if (!can_func(ftype2)) {
			dfilter_fail(dfw, "Return value of function %s (type=%s) cannot participate in '%s' comparison.",
				funcdef->name, ftype_pretty_name(ftype2),
				relation_string);
			THROW(TypeError);
		}

		fvalue = dfilter_fvalue_from_string(dfw, ftype2, st_arg1, NULL);
		stnode_replace(st_arg1, STTYPE_FVALUE, fvalue);
	}
	else if (type2 == STTYPE_SET) {
		dfilter_fail(dfw, "Only a field may be tested for membership in a set.");
		THROW(TypeError);
	}
	else {
		ws_assert_not_reached();
	}
}

static void
check_relation_LHS_UNPARSED(dfwork_t *dfw, const char* relation_string,
		FtypeCanFunc can_func, gboolean allow_partial_value,
		stnode_t *st_node _U_,
		stnode_t *st_arg1, stnode_t *st_arg2)
{
	sttype_id_t		type2;
	header_field_info	*hfinfo2;
	df_func_def_t		*funcdef;
	ftenum_t		ftype2;
	fvalue_t		*fvalue;

	type2 = stnode_type_id(st_arg2);

	ws_debug("5 check_relation_LHS_UNPARSED()");

	if (type2 == STTYPE_FIELD) {
		hfinfo2 = (header_field_info*)stnode_data(st_arg2);
		ftype2 = hfinfo2->type;

		if (!can_func(ftype2)) {
			dfilter_fail(dfw, "%s (type=%s) cannot participate in '%s' comparison.",
					hfinfo2->abbrev, ftype_pretty_name(ftype2),
					relation_string);
			THROW(TypeError);
		}

		fvalue = dfilter_fvalue_from_unparsed(dfw, ftype2, st_arg1, allow_partial_value, hfinfo2);
		stnode_replace(st_arg1, STTYPE_FVALUE, fvalue);
	}
	else if (type2 == STTYPE_STRING || type2 == STTYPE_UNPARSED ||
	         type2 == STTYPE_CHARCONST) {
		/* Well now that's silly... */
		dfilter_fail(dfw, "Neither \"%s\" nor \"%s\" are field or protocol names.",
				(char *)stnode_data(st_arg1),
				(char *)stnode_data(st_arg2));
		THROW(TypeError);
	}
	else if (type2 == STTYPE_RANGE) {
		check_drange_sanity(dfw, st_arg2);
		fvalue = dfilter_fvalue_from_unparsed(dfw, FT_BYTES, st_arg1, allow_partial_value, NULL);
		stnode_replace(st_arg1, STTYPE_FVALUE, fvalue);
	}
	else if (type2 == STTYPE_FUNCTION) {
		check_function(dfw, st_arg2);

		funcdef = sttype_function_funcdef(st_arg2);
		ftype2  = funcdef->retval_ftype;

		if (!can_func(ftype2)) {
			dfilter_fail(dfw, "return value of function %s() (type=%s) cannot participate in '%s' comparison.",
					funcdef->name, ftype_pretty_name(ftype2), relation_string);
			THROW(TypeError);
		}

		fvalue = dfilter_fvalue_from_unparsed(dfw, ftype2, st_arg1, allow_partial_value, NULL);
		stnode_replace(st_arg1, STTYPE_FVALUE, fvalue);
	}
	else if (type2 == STTYPE_SET) {
		dfilter_fail(dfw, "Only a field may be tested for membership in a set.");
		THROW(TypeError);
	}
	else {
		ws_assert_not_reached();
	}
}

static void
check_relation_LHS_RANGE(dfwork_t *dfw, const char *relation_string,
		FtypeCanFunc can_func _U_,
		gboolean allow_partial_value,
		stnode_t *st_node _U_,
		stnode_t *st_arg1, stnode_t *st_arg2)
{
	sttype_id_t		type2;
	header_field_info	*hfinfo2;
	ftenum_t		ftype2;
	fvalue_t		*fvalue;

	ws_debug("5 check_relation_LHS_RANGE(%s)", relation_string);

	check_drange_sanity(dfw, st_arg1);

	type2 = stnode_type_id(st_arg2);

	if (type2 == STTYPE_FIELD) {
		ws_debug("5 check_relation_LHS_RANGE(type2 = STTYPE_FIELD)");
		hfinfo2 = (header_field_info*)stnode_data(st_arg2);
		ftype2 = hfinfo2->type;

		if (!is_bytes_type(ftype2)) {
			if (!ftype_can_slice(ftype2)) {
				dfilter_fail(dfw, "\"%s\" is a %s and cannot be converted into a sequence of bytes.",
						hfinfo2->abbrev,
						ftype_pretty_name(ftype2));
				THROW(TypeError);
			}

			/* Convert entire field to bytes */
			convert_to_bytes(st_arg2);
		}
	}
	else if (type2 == STTYPE_STRING) {
		ws_debug("5 check_relation_LHS_RANGE(type2 = STTYPE_STRING)");
		fvalue = dfilter_fvalue_from_string(dfw, FT_BYTES, st_arg2, NULL);
		stnode_replace(st_arg2, STTYPE_FVALUE, fvalue);
	}
	else if (type2 == STTYPE_UNPARSED) {
		ws_debug("5 check_relation_LHS_RANGE(type2 = STTYPE_UNPARSED)");
		fvalue = dfilter_fvalue_from_unparsed(dfw, FT_BYTES, st_arg2, allow_partial_value, NULL);
		stnode_replace(st_arg2, STTYPE_FVALUE, fvalue);
	}
	else if (type2 == STTYPE_CHARCONST) {
		ws_debug("5 check_relation_LHS_RANGE(type2 = STTYPE_CHARCONST)");
		/* The RHS should be FT_BYTES, but a character is just a
		 * one-byte byte string. */
		fvalue = dfilter_fvalue_from_charconst_string(dfw, FT_BYTES, st_arg2, allow_partial_value);
		stnode_replace(st_arg2, STTYPE_FVALUE, fvalue);
	}
	else if (type2 == STTYPE_RANGE) {
		ws_debug("5 check_relation_LHS_RANGE(type2 = STTYPE_RANGE)");
		check_drange_sanity(dfw, st_arg2);
	}
	else if (type2 == STTYPE_FUNCTION) {
		df_func_def_t *funcdef = sttype_function_funcdef(st_arg2);
		ftype2  = funcdef->retval_ftype;

		if (!is_bytes_type(ftype2)) {
			if (!ftype_can_slice(ftype2)) {
				dfilter_fail(dfw, "Return value of function \"%s\" is a %s and cannot be converted into a sequence of bytes.",
					funcdef->name,
					ftype_pretty_name(ftype2));
				THROW(TypeError);
			}

			/* Convert function result to bytes */
			convert_to_bytes(st_arg2);
		}

		check_function(dfw, st_arg2);
	}
	else if (type2 == STTYPE_SET) {
		dfilter_fail(dfw, "Only a field may be tested for membership in a set.");
		THROW(TypeError);
	}
	else if (type2 == STTYPE_PCRE) {
		ws_assert_streq(relation_string, "matches");
	}
	else {
		ws_assert_not_reached();
	}
}

static stnode_t*
check_param_entity(dfwork_t *dfw, stnode_t *st_node)
{
	sttype_id_t		e_type;
	stnode_t		*new_st;
	fvalue_t		*fvalue;

	e_type = stnode_type_id(st_node);
	/* If there's an unparsed string, change it to an FT_STRING */
	if (e_type == STTYPE_UNPARSED || e_type == STTYPE_CHARCONST) {
		fvalue = dfilter_fvalue_from_unparsed(dfw, FT_STRING, st_node, TRUE, NULL);
		new_st = stnode_new(STTYPE_FVALUE, fvalue);
		stnode_free(st_node);
		return new_st;
	}
	return st_node;
}


/* If the LHS of a relation test is a FUNCTION, run some checks
 * and possibly some modifications of syntax tree nodes. */
static void
check_relation_LHS_FUNCTION(dfwork_t *dfw, const char *relation_string,
		FtypeCanFunc can_func,
		gboolean allow_partial_value,
		stnode_t *st_node _U_, stnode_t *st_arg1, stnode_t *st_arg2)
{
	sttype_id_t		type2;
	header_field_info	*hfinfo2;
	ftenum_t		ftype1, ftype2;
	fvalue_t		*fvalue;
	df_func_def_t		*funcdef;
	df_func_def_t		*funcdef2;
	/* GSList          *params; */

	check_function(dfw, st_arg1);
	type2 = stnode_type_id(st_arg2);

	funcdef = sttype_function_funcdef(st_arg1);
	ftype1 = funcdef->retval_ftype;

	ws_debug("5 check_relation_LHS_FUNCTION(%s)", relation_string);

	if (!can_func(ftype1)) {
		dfilter_fail(dfw, "Function %s (type=%s) cannot participate in '%s' comparison.",
				funcdef->name, ftype_pretty_name(ftype1),
				relation_string);
		THROW(TypeError);
	}

	if (type2 == STTYPE_FIELD) {
		hfinfo2 = (header_field_info*)stnode_data(st_arg2);
		ftype2 = hfinfo2->type;

		if (!compatible_ftypes(ftype1, ftype2)) {
			dfilter_fail(dfw, "Function %s and %s are not of compatible types.",
					funcdef->name, hfinfo2->abbrev);
			THROW(TypeError);
		}
		/* Do this check even though you'd think that if
		 * they're compatible, then can_func() would pass. */
		if (!can_func(ftype2)) {
			dfilter_fail(dfw, "%s (type=%s) cannot participate in specified comparison.",
					hfinfo2->abbrev, ftype_pretty_name(ftype2));
			THROW(TypeError);
		}
	}
	else if (type2 == STTYPE_STRING) {
		fvalue = dfilter_fvalue_from_string(dfw, ftype1, st_arg2, NULL);
		stnode_replace(st_arg2, STTYPE_FVALUE, fvalue);
	}
	else if (type2 == STTYPE_UNPARSED || type2 == STTYPE_CHARCONST) {
		fvalue = dfilter_fvalue_from_unparsed(dfw, ftype1, st_arg2, allow_partial_value, NULL);
		stnode_replace(st_arg2, STTYPE_FVALUE, fvalue);
	}
	else if (type2 == STTYPE_RANGE) {
		check_drange_sanity(dfw, st_arg2);
		if (!is_bytes_type(ftype1)) {
			if (!ftype_can_slice(ftype1)) {
				dfilter_fail(dfw, "Function \"%s\" is a %s and cannot be converted into a sequence of bytes.",
						funcdef->name,
						ftype_pretty_name(ftype1));
				THROW(TypeError);
			}

			/* Convert function result to bytes */
			convert_to_bytes(st_arg1);
		}
	}
	else if (type2 == STTYPE_FUNCTION) {
		funcdef2 = sttype_function_funcdef(st_arg2);
		ftype2 = funcdef2->retval_ftype;

		if (!compatible_ftypes(ftype1, ftype2)) {
			dfilter_fail(dfw, "Return values of function %s (type=%s) and function %s (type=%s) are not of compatible types.",
				     funcdef->name, ftype_pretty_name(ftype1), funcdef2->name, ftype_pretty_name(ftype2));
			THROW(TypeError);
		}

		/* Do this check even though you'd think that if
		 * they're compatible, then can_func() would pass. */
		if (!can_func(ftype2)) {
			dfilter_fail(dfw, "Return value of %s (type=%s) cannot participate in specified comparison.",
				     funcdef2->name, ftype_pretty_name(ftype2));
			THROW(TypeError);
		}

		check_function(dfw, st_arg2);
	}
	else if (type2 == STTYPE_SET) {
		dfilter_fail(dfw, "Only a field may be tested for membership in a set.");
		THROW(TypeError);
	}
	else if (type2 == STTYPE_PCRE) {
		ws_assert_streq(relation_string, "matches");
	}
	else {
		ws_assert_not_reached();
	}
}


/* Check the semantics of any relational test. */
static void
check_relation(dfwork_t *dfw, const char *relation_string,
		gboolean allow_partial_value,
		FtypeCanFunc can_func, stnode_t *st_node,
		stnode_t *st_arg1, stnode_t *st_arg2)
{
	static guint i = 0;
	header_field_info   *hfinfo;
	char                *s;

	ws_debug("4 check_relation(\"%s\") [%u]", relation_string, i++);
	log_stnode(st_arg1);
	log_stnode(st_arg2);

	/* Protocol can only be on LHS (for "contains" or "matches" operators).
	 * Check to see if protocol is on RHS, and re-interpret it as UNPARSED
	 * instead. The subsequent functions will parse it according to the
	 * existing rules for unparsed unquoted strings.
	 *
	 * This catches the case where the user has written "fc" on the RHS,
	 * probably intending a byte value rather than the fibre channel
	 * protocol, or similar for a number of other possibilities
	 * ("dc", "ff", "fefd"), and also catches the case where the user
	 * has written a generic string on the RHS for a "contains" or
	 * "matches" relation with a string field. (The now unparsed value
	 * will be interpreted in a way that matches the LHS; e.g.
	 * FT_PROTOCOL and FT_BYTES fields expect byte arrays whereas
	 * FT_STRING[Z][PAD] fields expect strings.)
	 *
	 * XXX: Is there a better way to do this in the grammar parser,
	 * which now determines whether something is a field?
	 */

	if (stnode_type_id(st_arg2) == STTYPE_FIELD) {
		hfinfo = (header_field_info*)stnode_data(st_arg2);
		if (hfinfo->type == FT_PROTOCOL) {
			/* Discard const qualifier from hfinfo->abbrev
			 * for sttnode_new, even though it duplicates the
			 * string.
			 */
			s = (char *)hfinfo->abbrev;
			/* Send it through as unparsed and all the other
			 * functions will take care of it as if it didn't
			 * match a protocol string.
			 */
			stnode_replace(st_arg2, STTYPE_UNPARSED, s);
		}
	}

	switch (stnode_type_id(st_arg1)) {
		case STTYPE_FIELD:
			check_relation_LHS_FIELD(dfw, relation_string, can_func,
					allow_partial_value, st_node, st_arg1, st_arg2);
			break;
		case STTYPE_STRING:
			check_relation_LHS_STRING(dfw, relation_string, can_func,
					allow_partial_value, st_node, st_arg1, st_arg2);
			break;
		case STTYPE_RANGE:
			check_relation_LHS_RANGE(dfw, relation_string, can_func,
					allow_partial_value, st_node, st_arg1, st_arg2);
			break;
		case STTYPE_UNPARSED:
		case STTYPE_CHARCONST:
			check_relation_LHS_UNPARSED(dfw, relation_string, can_func,
					allow_partial_value, st_node, st_arg1, st_arg2);
			break;
		case STTYPE_FUNCTION:
			check_relation_LHS_FUNCTION(dfw, relation_string, can_func,
					allow_partial_value, st_node, st_arg1, st_arg2);
			break;

		case STTYPE_UNINITIALIZED:
		case STTYPE_TEST:
		case STTYPE_FVALUE:
		case STTYPE_SET:
		default:
			ws_assert_not_reached();
	}
}

/* Check the semantics of any type of TEST */
static void
check_test(dfwork_t *dfw, stnode_t *st_node)
{
	test_op_t		st_op, st_arg_op;
	stnode_t		*st_arg1, *st_arg2;
	static guint i = 0;

	ws_debug("3 check_test(stnode_t *st_node = %p) [%u]\n", st_node, i++);
	log_stnode(st_node);

	sttype_test_get(st_node, &st_op, &st_arg1, &st_arg2);

	switch (st_op) {
		case TEST_OP_UNINITIALIZED:
			ws_assert_not_reached();
			break;

		case TEST_OP_EXISTS:
			check_exists(dfw, st_arg1);
			break;

		case TEST_OP_NOT:
			semcheck(dfw, st_arg1);
			break;

		case TEST_OP_AND:
		case TEST_OP_OR:
			if (stnode_type_id(st_arg1) == STTYPE_TEST) {
				sttype_test_get(st_arg1, &st_arg_op, NULL, NULL);
				if (st_arg_op == TEST_OP_AND || st_arg_op == TEST_OP_OR) {
					if (st_op != st_arg_op && !stnode_inside_parens(st_arg1))
						add_deprecated_token(dfw, "suggest parentheses around '&&' within '||'");
				}
			}

			if (stnode_type_id(st_arg2) == STTYPE_TEST) {
				sttype_test_get(st_arg2, &st_arg_op, NULL, NULL);
				if (st_arg_op == TEST_OP_AND || st_arg_op == TEST_OP_OR) {
					if (st_op != st_arg_op && !stnode_inside_parens(st_arg2))
						add_deprecated_token(dfw, "suggest parentheses around '&&' within '||'");
				}
			}

			semcheck(dfw, st_arg1);
			semcheck(dfw, st_arg2);
			break;

		case TEST_OP_ANY_EQ:
			check_relation(dfw, "==", FALSE, ftype_can_eq, st_node, st_arg1, st_arg2);
			break;
		case TEST_OP_ALL_NE:
			check_relation(dfw, "!=", FALSE, ftype_can_ne, st_node, st_arg1, st_arg2);
			break;
		case TEST_OP_ANY_NE:
			check_relation(dfw, "~=", FALSE, ftype_can_ne, st_node, st_arg1, st_arg2);
			break;
		case TEST_OP_GT:
			check_relation(dfw, ">", FALSE, ftype_can_gt, st_node, st_arg1, st_arg2);
			break;
		case TEST_OP_GE:
			check_relation(dfw, ">=", FALSE, ftype_can_ge, st_node, st_arg1, st_arg2);
			break;
		case TEST_OP_LT:
			check_relation(dfw, "<", FALSE, ftype_can_lt, st_node, st_arg1, st_arg2);
			break;
		case TEST_OP_LE:
			check_relation(dfw, "<=", FALSE, ftype_can_le, st_node, st_arg1, st_arg2);
			break;
		case TEST_OP_BITWISE_AND:
			check_relation(dfw, "&", FALSE, ftype_can_bitwise_and, st_node, st_arg1, st_arg2);
			break;
		case TEST_OP_CONTAINS:
			check_relation(dfw, "contains", TRUE, ftype_can_contains, st_node, st_arg1, st_arg2);
			break;
		case TEST_OP_MATCHES:
			check_relation(dfw, "matches", TRUE, ftype_can_matches, st_node, st_arg1, st_arg2);
			break;
		case TEST_OP_IN:
			/* Use the ftype_can_eq as the items in the set are evaluated using the
			 * semantics of equality. */
			check_relation(dfw, "in", FALSE, ftype_can_eq, st_node, st_arg1, st_arg2);
			break;

		default:
			ws_assert_not_reached();
	}
}


/* Check the entire syntax tree. */
static void
semcheck(dfwork_t *dfw, stnode_t *st_node)
{
	static guint i = 0;

	ws_debug("2 semcheck(stnode_t *st_node = %p) [%u]", st_node, i++);

	/* The parser assures that the top-most syntax-tree
	 * node will be a TEST node, no matter what. So assert that. */
	switch (stnode_type_id(st_node)) {
		case STTYPE_TEST:
			check_test(dfw, st_node);
			break;
		default:
			ws_assert_not_reached();
	}
}


/* Check the syntax tree for semantic errors, and convert
 * some of the nodes into the form they need to be in order to
 * later generate the DFVM bytecode. */
gboolean
dfw_semcheck(dfwork_t *dfw)
{
	volatile gboolean ok_filter = TRUE;
	static guint i = 0;

	ws_debug("1 dfw_semcheck(dfwork_t *dfw = %p) [%u]", dfw, i);

	/* Instead of having to check for errors at every stage of
	 * the semantic-checking, the semantic-checking code will
	 * throw an exception if a problem is found. */
	TRY {
		semcheck(dfw, dfw->st_root);
	}
	CATCH(TypeError) {
		ok_filter = FALSE;
	}
	ENDTRY;

	ws_debug("1 dfw_semcheck(dfwork_t *dfw = %p) [%u] - Returns %d",
				dfw, i++, ok_filter);
	return ok_filter;
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 8
 * tab-width: 8
 * indent-tabs-mode: t
 * End:
 *
 * vi: set shiftwidth=8 tabstop=8 noexpandtab:
 * :indentSize=8:tabSize=8:noTabs=false:
 */
