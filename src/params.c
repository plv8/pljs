#include "pljs.h"

#include <limits.h>

#include "nodes/params.h"
#include "parser/parse_node.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

static Node *pljs_variable_paramref_hook(ParseState *pstate, ParamRef *pref);
static Node *pljs_variable_coerce_param_hook(ParseState *pstate, Param *param,
                                             Oid targetTypeId,
                                             int32 targetTypeMod, int location);

void pljs_variable_param_setup(ParseState *pstate, void *arg) {
  pljs_param_state *parstate = (pljs_param_state *)arg;

  pstate->p_ref_hook_state = (void *)parstate;
  pstate->p_paramref_hook = pljs_variable_paramref_hook;
  pstate->p_coerce_param_hook = pljs_variable_coerce_param_hook;
}

static Node *pljs_variable_paramref_hook(ParseState *pstate, ParamRef *pref) {
  pljs_param_state *parstate = (pljs_param_state *)pstate->p_ref_hook_state;
  int paramno = pref->number;
  Oid *pptype;
  Param *param;

  /* Check parameter number is in range */
  if (paramno <= 0 || paramno > (int)(INT_MAX / sizeof(Oid)))
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_PARAMETER),
                    errmsg("missing parameter $%d", paramno),
                    parser_errposition(pstate, pref->location)));
  if (paramno > parstate->nparams) {
    MemoryContext oldcontext;

    oldcontext = MemoryContextSwitchTo(parstate->memory_context);
    /* Need to enlarge param array */
    if (parstate->param_types) {
      parstate->param_types =
          (Oid *)repalloc(parstate->param_types, paramno * sizeof(Oid));
    } else {
      parstate->param_types = (Oid *)palloc(paramno * sizeof(Oid));
    }

    /* Zero out the previously-unreferenced slots */
    MemSet(parstate->param_types + parstate->nparams, 0,
           (paramno - parstate->nparams) * sizeof(Oid));
    parstate->nparams = paramno;

    MemoryContextSwitchTo(oldcontext);
  }

  /* Locate param's slot in array */
  pptype = &(parstate->param_types)[paramno - 1];

  /* If not seen before, initialize to UNKNOWN type */
  if (*pptype == InvalidOid)
    *pptype = UNKNOWNOID;

  param = makeNode(Param);
  param->paramkind = PARAM_EXTERN;
  param->paramid = paramno;
  param->paramtype = *pptype;
  param->paramtypmod = -1;
  param->paramcollid = get_typcollation(param->paramtype);
  param->location = pref->location;

  return (Node *)param;
}

static Node *pljs_variable_coerce_param_hook(ParseState *pstate, Param *param,
                                             Oid targetTypeId,
                                             int32 targetTypeMod,
                                             int location) {
  if (param->paramkind == PARAM_EXTERN && param->paramtype == UNKNOWNOID) {
    /*
     * Input is a Param of previously undetermined type, and we want to
     * update our knowledge of the Param's type.
     */
    pljs_param_state *parstate = (pljs_param_state *)pstate->p_ref_hook_state;
    Oid *paramTypes = parstate->param_types;
    int paramno = param->paramid;

    if (paramno <= 0 || /* shouldn't happen, but... */
        paramno > parstate->nparams)
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_PARAMETER),
                      errmsg("there is no parameter $%d", paramno),
                      parser_errposition(pstate, param->location)));

    if (paramTypes[paramno - 1] == UNKNOWNOID) {
      /* We've successfully resolved the type */
      paramTypes[paramno - 1] = targetTypeId;
    } else if (paramTypes[paramno - 1] == targetTypeId) {
      /* We previously resolved the type, and it matches */
    } else {
      /* Ooops */
      ereport(
          ERROR,
          (errcode(ERRCODE_AMBIGUOUS_PARAMETER),
           errmsg("inconsistent types deduced for parameter $%d", paramno),
           errdetail("%s versus %s", format_type_be(paramTypes[paramno - 1]),
                     format_type_be(targetTypeId)),
           parser_errposition(pstate, param->location)));
    }

    param->paramtype = targetTypeId;

    /*
     * Note: it is tempting here to set the Param's paramtypmod to
     * targetTypeMod, but that is probably unwise because we have no
     * infrastructure that enforces that the value delivered for a Param
     * will match any particular typmod.  Leaving it -1 ensures that a
     * run-time length check/coercion will occur if needed.
     */
    param->paramtypmod = -1;

    /*
     * This module always sets a Param's collation to be the default for
     * its datatype.  If that's not what you want, you should be using the
     * more general parser substitution hooks.
     */
    param->paramcollid = get_typcollation(param->paramtype);

    /* Use the leftmost of the param's and coercion's locations */
    if (location >= 0 && (param->location < 0 || location < param->location))
      param->location = location;

    return (Node *)param;
  }

  /* Else signal to proceed with normal coercion */
  return NULL;
}

ParamListInfo pljs_setup_variable_paramlist(pljs_param_state *parstate,
                                            Datum *values, char *nulls) {

  ParamListInfo param_li =
      (ParamListInfo)palloc0(offsetof(ParamListInfoData, params) +
                             sizeof(ParamExternData) * parstate->nparams);
  param_li->numParams = parstate->nparams;
  for (int i = 0; i < parstate->nparams; i++) {
    ParamExternData *param = &param_li->params[i];

    param->value = values[i];
    param->isnull = nulls[i] == 'n';
    param->pflags = PARAM_FLAG_CONST;
    param->ptype = parstate->param_types[i];
  }

  return param_li;
}
