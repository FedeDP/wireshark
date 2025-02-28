/*
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 2001 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include "dfvm.h"

#include <ftypes/ftypes-int.h>
#include <wsutil/ws_assert.h>

dfvm_insn_t*
dfvm_insn_new(dfvm_opcode_t op)
{
	dfvm_insn_t	*insn;

	insn = g_new(dfvm_insn_t, 1);
	insn->op = op;
	insn->arg1 = NULL;
	insn->arg2 = NULL;
	insn->arg3 = NULL;
	insn->arg4 = NULL;
	return insn;
}

static void
dfvm_value_free(dfvm_value_t *v)
{
	switch (v->type) {
		case FVALUE:
			FVALUE_FREE(v->value.fvalue);
			break;
		case DRANGE:
			drange_free(v->value.drange);
			break;
		case PCRE:
			fvalue_regex_free(v->value.pcre);
			break;
		default:
			/* nothing */
			;
	}
	g_free(v);
}

void
dfvm_insn_free(dfvm_insn_t *insn)
{
	if (insn->arg1) {
		dfvm_value_free(insn->arg1);
	}
	if (insn->arg2) {
		dfvm_value_free(insn->arg2);
	}
	if (insn->arg3) {
		dfvm_value_free(insn->arg3);
	}
	if (insn->arg4) {
		dfvm_value_free(insn->arg4);
	}
	g_free(insn);
}


dfvm_value_t*
dfvm_value_new(dfvm_value_type_t type)
{
	dfvm_value_t	*v;

	v = g_new(dfvm_value_t, 1);
	v->type = type;
	return v;
}


void
dfvm_dump(FILE *f, dfilter_t *df)
{
	int		id, length;
	dfvm_insn_t	*insn;
	dfvm_value_t	*arg1;
	dfvm_value_t	*arg2;
	dfvm_value_t	*arg3;
	dfvm_value_t	*arg4;
	char		*value_str;
	GSList		*range_list;
	drange_node	*range_item;

	/* First dump the constant initializations */
	fprintf(f, "Constants:\n");
	length = df->consts->len;
	for (id = 0; id < length; id++) {

		insn = (dfvm_insn_t	*)g_ptr_array_index(df->consts, id);
		arg1 = insn->arg1;
		arg2 = insn->arg2;

		switch (insn->op) {
			case PUT_FVALUE:
				value_str = fvalue_to_string_repr(NULL, arg1->value.fvalue,
					FTREPR_DFILTER, BASE_NONE);
				fprintf(f, "%05d PUT_FVALUE\t%s <%s> -> reg#%u\n",
					id, value_str,
					fvalue_type_name(arg1->value.fvalue),
					arg2->value.numeric);
				wmem_free(NULL, value_str);
				break;
			case PUT_PCRE:
				fprintf(f, "%05d PUT_PCRE  \t%s <GRegex> -> reg#%u\n",
					id,
					fvalue_regex_pattern(arg1->value.pcre),
					arg2->value.numeric);
				break;
			case CHECK_EXISTS:
			case READ_TREE:
			case CALL_FUNCTION:
			case MK_RANGE:
			case ANY_EQ:
			case ALL_NE:
			case ANY_NE:
			case ANY_GT:
			case ANY_GE:
			case ANY_LT:
			case ANY_LE:
			case ANY_BITWISE_AND:
			case ANY_CONTAINS:
			case ANY_MATCHES:
			case ANY_IN_RANGE:
			case NOT:
			case RETURN:
			case IF_TRUE_GOTO:
			case IF_FALSE_GOTO:
			default:
				ws_assert_not_reached();
				break;
		}
	}

	fprintf(f, "\nInstructions:\n");
	/* Now dump the operations */
	length = df->insns->len;
	for (id = 0; id < length; id++) {

		insn = (dfvm_insn_t	*)g_ptr_array_index(df->insns, id);
		arg1 = insn->arg1;
		arg2 = insn->arg2;
		arg3 = insn->arg3;
		arg4 = insn->arg4;

		switch (insn->op) {
			case CHECK_EXISTS:
				fprintf(f, "%05d CHECK_EXISTS\t%s\n",
					id, arg1->value.hfinfo->abbrev);
				break;

			case READ_TREE:
				fprintf(f, "%05d READ_TREE\t\t%s -> reg#%u\n",
					id, arg1->value.hfinfo->abbrev,
					arg2->value.numeric);
				break;

			case CALL_FUNCTION:
				fprintf(f, "%05d CALL_FUNCTION\t%s (",
					id, arg1->value.funcdef->name);
				if (arg3) {
					fprintf(f, "reg#%u", arg3->value.numeric);
				}
				if (arg4) {
					fprintf(f, ", reg#%u", arg4->value.numeric);
				}
				fprintf(f, ") --> reg#%u\n", arg2->value.numeric);
				break;

			case PUT_FVALUE:
				/* We already dumped these */
				ws_assert_not_reached();
				break;

			case PUT_PCRE:
				/* We already dumped these */
				g_assert_not_reached();
				break;

			case MK_RANGE:
				arg3 = insn->arg3;
				fprintf(f, "%05d MK_RANGE\t\treg#%u[",
					id,
					arg1->value.numeric);
				for (range_list = arg3->value.drange->range_list;
				     range_list != NULL;
				     range_list = range_list->next) {
					range_item = (drange_node *)range_list->data;
					switch (range_item->ending) {

					case DRANGE_NODE_END_T_UNINITIALIZED:
						fprintf(f, "?");
						break;

					case DRANGE_NODE_END_T_LENGTH:
						fprintf(f, "%d:%d",
						    range_item->start_offset,
						    range_item->length);
						break;

					case DRANGE_NODE_END_T_OFFSET:
						fprintf(f, "%d-%d",
						    range_item->start_offset,
						    range_item->end_offset);
						break;

					case DRANGE_NODE_END_T_TO_THE_END:
						fprintf(f, "%d:",
						    range_item->start_offset);
						break;
					}
					if (range_list->next != NULL)
						fprintf(f, ",");
				}
				fprintf(f, "] -> reg#%u\n",
					arg2->value.numeric);
				break;

			case ANY_EQ:
				fprintf(f, "%05d ANY_EQ\t\treg#%u == reg#%u\n",
					id, arg1->value.numeric, arg2->value.numeric);
				break;

			case ALL_NE:
				fprintf(f, "%05d ALL_NE\t\treg#%u == reg#%u\n",
					id, arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_NE:
				fprintf(f, "%05d ANY_NE\t\treg#%u == reg#%u\n",
					id, arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_GT:
				fprintf(f, "%05d ANY_GT\t\treg#%u == reg#%u\n",
					id, arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_GE:
				fprintf(f, "%05d ANY_GE\t\treg#%u == reg#%u\n",
					id, arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_LT:
				fprintf(f, "%05d ANY_LT\t\treg#%u == reg#%u\n",
					id, arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_LE:
				fprintf(f, "%05d ANY_LE\t\treg#%u == reg#%u\n",
					id, arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_BITWISE_AND:
				fprintf(f, "%05d ANY_BITWISE_AND\t\treg#%u == reg#%u\n",
					id, arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_CONTAINS:
				fprintf(f, "%05d ANY_CONTAINS\treg#%u contains reg#%u\n",
					id, arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_MATCHES:
				fprintf(f, "%05d ANY_MATCHES\treg#%u matches reg#%u\n",
					id, arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_IN_RANGE:
				fprintf(f, "%05d ANY_IN_RANGE\treg#%u in range reg#%u,reg#%u\n",
					id, arg1->value.numeric, arg2->value.numeric,
					arg3->value.numeric);
				break;

			case NOT:
				fprintf(f, "%05d NOT\n", id);
				break;

			case RETURN:
				fprintf(f, "%05d RETURN\n", id);
				break;

			case IF_TRUE_GOTO:
				fprintf(f, "%05d IF-TRUE-GOTO\t%u\n",
						id, arg1->value.numeric);
				break;

			case IF_FALSE_GOTO:
				fprintf(f, "%05d IF-FALSE-GOTO\t%u\n",
						id, arg1->value.numeric);
				break;

			default:
				ws_assert_not_reached();
				break;
		}
	}
}

/* Reads a field from the proto_tree and loads the fvalues into a register,
 * if that field has not already been read. */
static gboolean
read_tree(dfilter_t *df, proto_tree *tree, header_field_info *hfinfo, int reg)
{
	GPtrArray	*finfos;
	field_info	*finfo;
	int		i, len;
	GList		*fvalues = NULL;
	gboolean	found_something = FALSE;

	/* Already loaded in this run of the dfilter? */
	if (df->attempted_load[reg]) {
		if (df->registers[reg]) {
			return TRUE;
		}
		else {
			return FALSE;
		}
	}

	df->attempted_load[reg] = TRUE;

	while (hfinfo) {
		finfos = proto_get_finfo_ptr_array(tree, hfinfo->id);
		if ((finfos == NULL) || (g_ptr_array_len(finfos) == 0)) {
			hfinfo = hfinfo->same_name_next;
			continue;
		}
		else {
			found_something = TRUE;
		}

		len = finfos->len;
		for (i = 0; i < len; i++) {
			finfo = (field_info *)g_ptr_array_index(finfos, i);
			fvalues = g_list_prepend(fvalues, &finfo->value);
		}

		hfinfo = hfinfo->same_name_next;
	}

	if (!found_something) {
		return FALSE;
	}

	df->registers[reg] = fvalues;
	// These values are referenced only, do not try to free it later.
	df->owns_memory[reg] = FALSE;
	return TRUE;
}


/* Put a constant value in a register. These will not be cleared by
 * free_register_overhead. */
static gboolean
put_fvalue(dfilter_t *df, fvalue_t *fv, int reg)
{
	df->registers[reg] = g_list_append(NULL, fv);
	df->owns_memory[reg] = FALSE;
	return TRUE;
}

/* Put a constant PCRE in a register. These will not be cleared by
 * free_register_overhead. */
static gboolean
put_pcre(dfilter_t *df, fvalue_regex_t *pcre, int reg)
{
	df->registers[reg] = g_list_append(NULL, pcre);
	df->owns_memory[reg] = FALSE;
	return TRUE;
}

enum match_how {
	MATCH_ANY,
	MATCH_ALL
};

typedef gboolean (*DFVMMatchFunc)(const fvalue_t*, const fvalue_t*);

static gboolean
cmp_test(dfilter_t *df, enum match_how how, DFVMMatchFunc match_func, int reg1, int reg2)
{
	GList	*list_a, *list_b;
	gboolean want_all = (how == MATCH_ALL);
	gboolean want_any = (how == MATCH_ANY);
	gboolean have_match;

	list_a = df->registers[reg1];

	while (list_a) {
		list_b = df->registers[reg2];
		while (list_b) {
			have_match = match_func(list_a->data, list_b->data);
			if (want_all && !have_match) {
				return FALSE;
			}
			else if (want_any && have_match) {
				return TRUE;
			}
			list_b = g_list_next(list_b);
		}
		list_a = g_list_next(list_a);
	}
	/* want_all || !want_any */
	return want_all;
}

static inline gboolean
any_test(dfilter_t *df, DFVMMatchFunc cmp, int reg1, int reg2)
{
	/* cmp(A) <=> cmp(a1) OR cmp(a2) OR cmp(a3) OR ... */
	return cmp_test(df, MATCH_ANY, cmp, reg1, reg2);
}

static inline gboolean
all_test(dfilter_t *df, DFVMMatchFunc cmp, int reg1, int reg2)
{
	/* cmp(A) <=> cmp(a1) AND cmp(a2) AND cmp(a3) AND ... */
	return cmp_test(df, MATCH_ALL, cmp, reg1, reg2);
}

static inline gboolean
any_eq(dfilter_t *df, int reg1, int reg2)
{
	/* A any_eq B <=> a1 == b1 OR a2 == b2 OR a3 == b3 OR ... */
	return any_test(df, fvalue_eq, reg1, reg2);
}

static inline gboolean
any_ne(dfilter_t *df, int reg1, int reg2)
{
	/* A any_ne B <=> a1 != b1 OR a2 != b2 OR a3 != b3 OR ... */
	return any_test(df, fvalue_ne, reg1, reg2);
}

static inline gboolean
all_ne(dfilter_t *df, int reg1, int reg2)
{
	/* A all_ne B <=> a1 != b1 AND a2 != b2 AND a3 != b3 AND ... */
	return all_test(df, fvalue_ne, reg1, reg2);
}

static gboolean
any_matches(dfilter_t *df, int reg1, int reg2)
{
	GList	*list_a, *list_b;

	list_a = df->registers[reg1];

	while (list_a) {
		list_b = df->registers[reg2];
		while (list_b) {
			if (fvalue_matches((fvalue_t *)list_a->data, list_b->data)) {
				return TRUE;
			}
			list_b = g_list_next(list_b);
		}
		list_a = g_list_next(list_a);
	}
	return FALSE;
}

static gboolean
any_in_range(dfilter_t *df, int reg1, int reg2, int reg3)
{
	GList	*list1, *list_low, *list_high;
	fvalue_t *low, *high;

	list1 = df->registers[reg1];
	list_low = df->registers[reg2];
	list_high = df->registers[reg3];

	/* The first register contains the values associated with a field, the
	 * second and third arguments are expected to be a single value for the
	 * lower and upper bound respectively. These cannot be fields and thus
	 * the list length MUST be one. This should have been enforced by
	 * grammar.lemon.
	 */
	ws_assert(list_low && !g_list_next(list_low));
	ws_assert(list_high && !g_list_next(list_high));
	low = (fvalue_t *)list_low->data;
	high = (fvalue_t *)list_high->data;

	while (list1) {
		fvalue_t *value = (fvalue_t *)list1->data;
		if (fvalue_ge(value, low) && fvalue_le(value, high)) {
			return TRUE;
		}
		list1 = g_list_next(list1);
	}
	return FALSE;
}


static void
free_owned_register(gpointer data, gpointer user_data _U_)
{
	fvalue_t *value = (fvalue_t *)data;
	FVALUE_FREE(value);
}

/* Clear registers that were populated during evaluation (leaving constants
 * intact). If we created the values, then these will be freed as well. */
static void
free_register_overhead(dfilter_t* df)
{
	guint i;

	for (i = 0; i < df->num_registers; i++) {
		df->attempted_load[i] = FALSE;
		if (df->registers[i]) {
			if (df->owns_memory[i]) {
				g_list_foreach(df->registers[i], free_owned_register, NULL);
				df->owns_memory[i] = FALSE;
			}
			g_list_free(df->registers[i]);
			df->registers[i] = NULL;
		}
	}
}

/* Takes the list of fvalue_t's in a register, uses fvalue_slice()
 * to make a new list of fvalue_t's (which are ranges, or byte-slices),
 * and puts the new list into a new register. */
static void
mk_range(dfilter_t *df, int from_reg, int to_reg, drange_t *d_range)
{
	GList		*from_list, *to_list;
	fvalue_t	*old_fv, *new_fv;

	to_list = NULL;
	from_list = df->registers[from_reg];

	while (from_list) {
		old_fv = (fvalue_t*)from_list->data;
		new_fv = fvalue_slice(old_fv, d_range);
		/* Assert here because semcheck.c should have
		 * already caught the cases in which a slice
		 * cannot be made. */
		ws_assert(new_fv);
		to_list = g_list_append(to_list, new_fv);

		from_list = g_list_next(from_list);
	}

	df->registers[to_reg] = to_list;
	df->owns_memory[to_reg] = TRUE;
}



gboolean
dfvm_apply(dfilter_t *df, proto_tree *tree)
{
	int		id, length;
	gboolean	accum = TRUE;
	dfvm_insn_t	*insn;
	dfvm_value_t	*arg1;
	dfvm_value_t	*arg2;
	dfvm_value_t	*arg3 = NULL;
	dfvm_value_t	*arg4 = NULL;
	header_field_info	*hfinfo;
	GList		*param1;
	GList		*param2;

	ws_assert(tree);

	length = df->insns->len;

	for (id = 0; id < length; id++) {

	  AGAIN:
		insn = (dfvm_insn_t	*)g_ptr_array_index(df->insns, id);
		arg1 = insn->arg1;
		arg2 = insn->arg2;

		switch (insn->op) {
			case CHECK_EXISTS:
				hfinfo = arg1->value.hfinfo;
				while(hfinfo) {
					accum = proto_check_for_protocol_or_field(tree,
							hfinfo->id);
					if (accum) {
						break;
					}
					else {
						hfinfo = hfinfo->same_name_next;
					}
				}
				break;

			case READ_TREE:
				accum = read_tree(df, tree,
						arg1->value.hfinfo, arg2->value.numeric);
				break;

			case CALL_FUNCTION:
				arg3 = insn->arg3;
				arg4 = insn->arg4;
				param1 = NULL;
				param2 = NULL;
				if (arg3) {
					param1 = df->registers[arg3->value.numeric];
				}
				if (arg4) {
					param2 = df->registers[arg4->value.numeric];
				}
				accum = arg1->value.funcdef->function(param1, param2,
						&df->registers[arg2->value.numeric]);
				// functions create a new value, so own it.
				df->owns_memory[arg2->value.numeric] = TRUE;
				break;

			case MK_RANGE:
				arg3 = insn->arg3;
				mk_range(df,
						arg1->value.numeric, arg2->value.numeric,
						arg3->value.drange);
				break;

			case ANY_EQ:
				accum = any_eq(df, arg1->value.numeric, arg2->value.numeric);
				break;

			case ALL_NE:
				accum = all_ne(df, arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_NE:
				accum = any_ne(df, arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_GT:
				accum = any_test(df, fvalue_gt,
						arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_GE:
				accum = any_test(df, fvalue_ge,
						arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_LT:
				accum = any_test(df, fvalue_lt,
						arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_LE:
				accum = any_test(df, fvalue_le,
						arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_BITWISE_AND:
				accum = any_test(df, fvalue_bitwise_and,
						arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_CONTAINS:
				accum = any_test(df, fvalue_contains,
						arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_MATCHES:
				accum = any_matches(df,
						arg1->value.numeric, arg2->value.numeric);
				break;

			case ANY_IN_RANGE:
				arg3 = insn->arg3;
				accum = any_in_range(df, arg1->value.numeric,
						arg2->value.numeric,
						arg3->value.numeric);
				break;

			case NOT:
				accum = !accum;
				break;

			case RETURN:
				free_register_overhead(df);
				return accum;

			case IF_TRUE_GOTO:
				if (accum) {
					id = arg1->value.numeric;
					goto AGAIN;
				}
				break;

			case IF_FALSE_GOTO:
				if (!accum) {
					id = arg1->value.numeric;
					goto AGAIN;
				}
				break;

			case PUT_FVALUE:
#if 0
				/* These were handled in the constants initialization */
				accum = put_fvalue(df,
						arg1->value.fvalue, arg2->value.numeric);
				break;
#endif

			case PUT_PCRE:
#if 0
				/* These were handled in the constants initialization */
				accum = put_pcre(df,
						arg1->value.pcre, arg2->value.numeric);
				break;
#endif

			default:
				ws_assert_not_reached();
				break;
		}
	}

	ws_assert_not_reached();
	return FALSE; /* to appease the compiler */
}

void
dfvm_init_const(dfilter_t *df)
{
	int		id, length;
	dfvm_insn_t	*insn;
	dfvm_value_t	*arg1;
	dfvm_value_t	*arg2;

	length = df->consts->len;

	for (id = 0; id < length; id++) {

		insn = (dfvm_insn_t	*)g_ptr_array_index(df->consts, id);
		arg1 = insn->arg1;
		arg2 = insn->arg2;

		switch (insn->op) {
			case PUT_FVALUE:
				put_fvalue(df,
						arg1->value.fvalue, arg2->value.numeric);
				break;
			case PUT_PCRE:
				put_pcre(df,
						arg1->value.pcre, arg2->value.numeric);
				break;
			case CHECK_EXISTS:
			case READ_TREE:
			case CALL_FUNCTION:
			case MK_RANGE:
			case ANY_EQ:
			case ALL_NE:
			case ANY_NE:
			case ANY_GT:
			case ANY_GE:
			case ANY_LT:
			case ANY_LE:
			case ANY_BITWISE_AND:
			case ANY_CONTAINS:
			case ANY_MATCHES:
			case ANY_IN_RANGE:
			case NOT:
			case RETURN:
			case IF_TRUE_GOTO:
			case IF_FALSE_GOTO:
			default:
				ws_assert_not_reached();
				break;
		}
	}

	return;
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
