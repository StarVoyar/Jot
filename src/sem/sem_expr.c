#include "sem_impl.h"

/**
 * @brief Tells whether a call name is a builtin runtime operation
 * @param name Call name
 * @return Non-zero for input, len, tostr, tonum, readFile, writeFile
 */
static int is_builtin(const char *name) {
  return strcmp(name, "input") == 0 || strcmp(name, "len") == 0 ||
         strcmp(name, "tostr") == 0 || strcmp(name, "tonum") == 0 ||
         strcmp(name, "readFile") == 0 || strcmp(name, "writeFile") == 0;
}

/**
 * @brief Counts call arguments
 * @param args Argument list (linked via right)
 * @return Argument count
 */
static int count_args(Node *args) {
  int n = 0;
  for (Node *a = args; a != NULL; a = a->right) {
    n++;
  }
  return n;
}

/**
 * @brief Resolves the static kind of an expression from scope state
 * @param node Expression node
 * @return Struct/class/array-element name, or NULL
 * @details Reads declarations only, never recorded types, so it behaves
 * identically before and after checking (mirrors instance_type_of).
 */
const char *sem_peek_kind(Node *node) {
  if (node == NULL) {
    return NULL;
  }
  if (node->type == NODE_IDENTIFIER) {
    int slot = sem_scope_find(node->identifier.name);
    if (slot >= 0 &&
        (sem_scope.vars[slot].type == ST_STRUCT ||
         sem_scope.vars[slot].type == ST_CLASS)) {
      return sem_scope.vars[slot].kind;
    }
    return NULL;
  }
  if (node->type == NODE_NEW) {
    return node->new_expr.type_name;
  }
  if (node->type == NODE_INDEX &&
      node->index.base->type == NODE_IDENTIFIER) {
    int slot = sem_scope_find(node->index.base->identifier.name);
    if (slot >= 0) {
      return sem_scope.vars[slot].elem_kind;
    }
    return NULL;
  }
  if (node->type == NODE_MEMBER_ACCESS) {
    const char *owner = node->member_access.object;
    const char *field = node->member_access.member;
    if (owner != NULL && strcmp(owner, "self") == 0) {
      if (sem_scope.in_method && sem_scope.cls >= 0) {
        int f = sem_find_class_field(sem_scope.cls, field);
        if (f >= 0) {
          return sem_prog()->classes[sem_scope.cls].fields[f].kind;
        }
      }
      int pslot = sem_scope_find(field);
      if (pslot >= 0 && sem_scope.vars[pslot].is_param) {
        return sem_scope.vars[pslot].kind;
      }
      return NULL;
    }
    if (node->member_access.object_expr != NULL) {
      const char *base_kind = sem_peek_kind(node->member_access.object_expr);
      if (base_kind == NULL) {
        return NULL;
      }
      SemField *def = NULL;
      if (sem_find_instance_field(base_kind, field, &def) >= 0 &&
          def != NULL) {
        return def->kind;
      }
      return NULL;
    }
    int oslot = sem_scope_find(owner);
    if (oslot < 0) {
      return NULL;
    }
    const char *owner_kind = sem_scope.vars[oslot].kind;
    if (owner_kind == NULL) {
      return NULL;
    }
    int s = sem_find_struct(owner_kind);
    if (s >= 0) {
      int f = sem_find_struct_field(s, field);
      if (f >= 0) {
        return sem_prog()->structs[s].fields[f].kind;
      }
      return NULL;
    }
    int c = sem_find_class(owner_kind);
    if (c >= 0) {
      int f = sem_find_class_field(c, field);
      if (f >= 0) {
        return sem_prog()->classes[c].fields[f].kind;
      }
    }
    return NULL;
  }
  return NULL;
}

/**
 * @brief Silent expression category without warnings or use-marking
 * @param node Expression node
 * @return Best-effort category
 */
SemType sem_peek(Node *node) {
  if (node == NULL) {
    return ST_INT;
  }
  switch (node->type) {
  case NODE_INT_LITERAL:
    return ST_INT;
  case NODE_FLOAT_LITERAL:
    return ST_FLOAT;
  case NODE_STRING_LITERAL:
    return ST_STR;
  case NODE_NULL:
    return ST_NULL;
  case NODE_IDENTIFIER: {
    int slot = sem_scope_find(node->identifier.name);
    if (slot >= 0) {
      return sem_scope.vars[slot].type;
    }
    SemProg *sp = sem_prog();
    for (int g = 0; g < sp->nglobals; g++) {
      if (strcmp(sp->globals[g].name, node->identifier.name) == 0) {
        return sp->globals[g].type;
      }
    }
    return ST_INT;
  }
  case NODE_FUNC_CALL: {
    const char *nm = node->func_call.name;
    if (strcmp(nm, "input") == 0) {
      return ST_INT;
    }
    if (strcmp(nm, "tostr") == 0) {
      return ST_STR;
    }
    if (strcmp(nm, "tonum") == 0) {
      return ST_FLOAT;
    }
    if (strcmp(nm, "readFile") == 0) {
      return ST_STR;
    }
    SemProg *sp = sem_prog();
    for (int s = 0; s < sp->nfuncs; s++) {
      if (strcmp(sp->funcs[s].name, nm) == 0) {
        if (sp->funcs[s].ret != NULL) {
          if (strcmp(sp->funcs[s].ret, "str") == 0) {
            return ST_STR;
          }
          if (strcmp(sp->funcs[s].ret, "arr") == 0) {
            return ST_ARR;
          }
          if (strcmp(sp->funcs[s].ret, "void") == 0) {
            return ST_NULL;
          }
        }
        if (sp->funcs[s].ret_float) {
          return ST_FLOAT;
        }
        if (sp->funcs[s].bool_only) {
          return ST_INT;
        }
        for (int j = 0; j < SEM_MAX_PARAMS; j++) {
          if (sp->funcs[s].pfloat[j]) {
            return ST_FLOAT;
          }
        }
        return ST_INT;
      }
    }
    return ST_INT;
  }
  case NODE_BINARY_OP: {
    const char *op = node->binary_op.op;
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
        strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
        strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) {
      return ST_BOOL;
    }
    SemType lt = sem_peek(node->binary_op.left);
    SemType rt = sem_peek(node->binary_op.right);
    if (strcmp(op, "+") == 0 && (lt == ST_STR || rt == ST_STR)) {
      return ST_STR;
    }
    if (lt == ST_FLOAT || rt == ST_FLOAT) {
      return ST_FLOAT;
    }
    return ST_INT;
  }
  case NODE_MEMBER_ACCESS: {
    if (node->member_access.object_expr != NULL) {
      const char *kind = sem_peek_kind(node->member_access.object_expr);
      if (kind == NULL) {
        return ST_INT;
      }
      SemField *def = NULL;
      int f = sem_find_instance_field(kind, node->member_access.member, &def);
      if (f >= 0 && def != NULL) {
        return def->type;
      }
      return ST_INT;
    }
    if (strcmp(node->member_access.object, "self") == 0) {
      int slot = sem_scope_find(node->member_access.member);
      if (slot >= 0 && sem_scope.vars[slot].is_param) {
        return sem_scope.vars[slot].type;
      }
      if (sem_scope.in_method && sem_scope.cls >= 0) {
        int f = sem_find_class_field(sem_scope.cls,
                                     node->member_access.member);
        if (f >= 0) {
          return sem_prog()->classes[sem_scope.cls].fields[f].type;
        }
      }
      return ST_INT;
    }
    int slot = sem_scope_find(node->member_access.object);
    if (slot < 0) {
      return ST_INT;
    }
    if (sem_scope.vars[slot].type == ST_STRUCT) {
      int s = sem_find_struct(sem_scope.vars[slot].kind);
      int f = sem_find_struct_field(s, node->member_access.member);
      if (f >= 0) {
        return sem_prog()->structs[s].fields[f].type;
      }
      return ST_INT;
    }
    if (sem_scope.vars[slot].type == ST_CLASS) {
      int c = sem_find_class(sem_scope.vars[slot].kind);
      int f = sem_find_class_field(c, node->member_access.member);
      if (f >= 0) {
        return sem_prog()->classes[c].fields[f].type;
      }
      return ST_INT;
    }
    return ST_INT;
  }
  case NODE_NEW: {
    if (sem_find_struct(node->new_expr.type_name) >= 0) {
      return ST_STRUCT;
    }
    if (sem_find_class(node->new_expr.type_name) >= 0) {
      return ST_CLASS;
    }
    return ST_INT;
  }
  case NODE_INDEX: {
    SemType btype = sem_peek(node->index.base);
    if (btype == ST_STR) {
      return ST_INT;
    }
    if (btype == ST_ARR && node->index.base->type == NODE_IDENTIFIER) {
      int slot = sem_scope_find(node->index.base->identifier.name);
      if (slot >= 0) {
        return sem_scope.vars[slot].elem;
      }
    }
    if (btype == ST_ARR) {
      return ST_INT;
    }
    return ST_INT;
  }
  case NODE_METHOD_CALL: {
    int c = -1;
    if (node->method_call.object_expr != NULL) {
      const char *kind = sem_peek_kind(node->method_call.object_expr);
      if (kind != NULL) {
        c = sem_find_class(kind);
      }
    } else if (strcmp(node->method_call.object, "self") == 0) {
      if (sem_scope.in_method) {
        c = sem_scope.cls;
      }
    } else {
      int slot = sem_scope_find(node->method_call.object);
      if (slot >= 0 &&
          (sem_scope.vars[slot].type == ST_STRUCT ||
           sem_scope.vars[slot].type == ST_CLASS)) {
        c = sem_find_class(sem_scope.vars[slot].kind);
      }
    }
    if (c >= 0) {
      int owner = c;
      int m = sem_find_method(c, node->method_call.method, &owner);
      if (m >= 0) {
        const char *rt = sem_prog()->classes[owner].methods[m].ret;
        if (rt != NULL && strcmp(rt, "str") == 0) {
          return ST_STR;
        }
        if (rt != NULL && strcmp(rt, "bool") == 0) {
          return ST_BOOL;
        }
        return ST_FLOAT;
      }
    }
    return ST_INT;
  }
  default:
    return ST_INT;
  }
}

/**
 * @brief Compares two operator strings for equality
 * @param op Operator text
 * @param want Expected operator
 * @return Non-zero when equal
 */
static int op_is(const char *op, const char *want) {
  return strcmp(op, want) == 0;
}

/**
 * @brief Tells whether an operator is a comparison
 * @param op Operator text
 * @return Non-zero for ==, !=, <, >, <=, >=
 */
static int is_comparison_op(const char *op) {
  return op_is(op, "==") || op_is(op, "!=") || op_is(op, "<") ||
         op_is(op, ">") || op_is(op, "<=") || op_is(op, ">=");
}

/**
 * @brief Resolves member access with optional emission checks
 * @param node Member access node
 * @param touch Non-zero to run emission-time checks (type-name rule,
 * checked receiver) and touch the base like expression emission does
 * @return Field category
 * @details Validation mirrors the silent type query (undeclared names and
 * bad fields error, but type names used as values pass through); the
 * touch pass mirrors emission (type names error first, receivers are
 * checked, then the base is evaluated).
 */
static SemType member_resolve(Node *node, int touch) {
  const char *object = node->member_access.object;
  const char *member = node->member_access.member;
  if (node->member_access.object_expr != NULL) {
    const char *kind = sem_peek_kind(node->member_access.object_expr);
    if (!touch) {
      if (kind == NULL) {
        sem_record_type(node, ST_INT, NULL);
        return ST_INT;
      }
      SemField *def = NULL;
      int f = sem_find_instance_field(kind, member, &def);
      SemType ft = (f >= 0 && def != NULL) ? def->type : ST_INT;
      sem_record_type(node, ft, def != NULL ? def->kind : NULL);
      return ft;
    }
    if (kind == NULL) {
      sem_error(node->member_access.object_expr,
                "Can only access fields on a struct or class instance");
    }
    SemField *def = NULL;
    int f = sem_find_instance_field(kind, member, &def);
    if (f < 0) {
      sem_error(node, "'%s' has no field '%s'", kind, member);
    }
    SemType ft = def != NULL ? def->type : ST_INT;
    sem_record_type(node, ft, def != NULL ? def->kind : NULL);
    sem_touch(node->member_access.object_expr);
    return ft;
  }
  if (strcmp(object, "self") == 0) {
    int pslot = sem_scope_find(member);
    if (pslot >= 0 && sem_scope.vars[pslot].is_param) {
      sem_scope.vars[pslot].used = 1;
      SemType pt = sem_scope.vars[pslot].type;
      sem_record_type(node, pt, sem_scope.vars[pslot].kind);
      return pt;
    }
    if (sem_scope.in_method && sem_scope.cls >= 0) {
      int f = sem_find_class_field(sem_scope.cls, member);
      if (f >= 0) {
        int tslot = sem_scope_find("this ");
        if (tslot >= 0) {
          sem_scope.vars[tslot].used = 1;
        }
        SemField *fd = &sem_prog()->classes[sem_scope.cls].fields[f];
        sem_record_type(node, fd->type, fd->kind);
        return fd->type;
      }
      sem_error(node, "'%s' is not a parameter or field of class '%s'",
                member, sem_prog()->classes[sem_scope.cls].name);
    }
    int slot = sem_scope_require_param(node, member);
    SemType pt = sem_scope.vars[slot].type;
    sem_record_type(node, pt, sem_scope.vars[slot].kind);
    return pt;
  }
  if (touch) {
    if (sem_find_struct(object) >= 0 || sem_find_class(object) >= 0) {
      sem_error(node, "Cannot use type '%s' as a value, use an instance",
                object);
    }
  }
  int slot = sem_scope_find(object);
  if (slot < 0) {
    sem_error(node, "Variable '%s' not declared", object);
  }
  sem_scope.vars[slot].used = 1;
  SemProg *sp = sem_prog();
  SemType ft = ST_INT;
  const char *fkind = NULL;
  if (sem_scope.vars[slot].type == ST_STRUCT) {
    int s = sem_find_struct(sem_scope.vars[slot].kind);
    int f = sem_find_struct_field(s, member);
    if (f < 0) {
      sem_error(node, "Struct '%s' has no field '%s'",
                sem_scope.vars[slot].kind, member);
    }
    ft = sp->structs[s].fields[f].type;
    fkind = sp->structs[s].fields[f].kind;
  } else if (sem_scope.vars[slot].type == ST_CLASS) {
    int c = sem_find_class(sem_scope.vars[slot].kind);
    int f = sem_find_class_field(c, member);
    if (f < 0) {
      sem_error(node, "Class '%s' has no field '%s'",
                sem_scope.vars[slot].kind, member);
    }
    ft = sp->classes[c].fields[f].type;
    fkind = sp->classes[c].fields[f].kind;
  } else {
    sem_error(node, "'%s' is not a struct or class instance", object);
  }
  sem_record_type(node, ft, fkind);
  return ft;
}

/**
 * @brief Validates an expression like the historic type query
 * @param node Expression node
 * @return Resolved category
 * @details Mirrors expr_type: identifiers resolve (warning, marking),
 * calls check voidness, member access resolves fields, but children are
 * never validated here (that happens in the emission mirror below).
 */
SemType sem_validate(Node *node) {
  if (node == NULL) {
    sem_error(NULL, "NULL expression in codegen");
    return ST_ERR;
  }
  switch (node->type) {
  case NODE_INT_LITERAL:
    sem_record_type(node, ST_INT, NULL);
    return ST_INT;
  case NODE_FLOAT_LITERAL:
    sem_record_type(node, ST_FLOAT, NULL);
    return ST_FLOAT;
  case NODE_STRING_LITERAL:
    sem_record_type(node, ST_STR, NULL);
    return ST_STR;
  case NODE_NULL:
    sem_record_type(node, ST_NULL, NULL);
    return ST_NULL;
  case NODE_IDENTIFIER: {
    int ref = sem_scope_require(node, node->identifier.name);
    if (sem_ref_is_global(ref)) {
      SemProg *sp = sem_prog();
      SemGlobal *g = &sp->globals[sem_ref_to_global(ref)];
      sem_record_type(node, g->type, g->kind);
      return g->type;
    }
    SemVar *v = &sem_scope.vars[ref];
    const char *kind = NULL;
    if (v->type == ST_STRUCT || v->type == ST_CLASS) {
      kind = v->kind;
    }
    sem_record_type(node, v->type, kind);
    return v->type;
  }
  case NODE_FUNC_CALL: {
    const char *name = node->func_call.name;
    if (!is_builtin(name)) {
      int fi = sem_func_at(name);
      if (fi >= 0) {
        SemFunc *f = &sem_prog()->funcs[fi];
        if (f->ret != NULL && strcmp(f->ret, "void") == 0) {
          sem_error(node, "Cannot use result of void function '%s'", name);
        }
      }
    }
    SemType t = sem_peek(node);
    const char *kind = NULL;
    if (t == ST_STRUCT || t == ST_CLASS) {
      kind = sem_peek_kind(node);
    }
    sem_record_type(node, t, kind);
    return t;
  }
  case NODE_BINARY_OP: {
    SemType t = sem_peek(node);
    sem_record_type(node, t, NULL);
    return t;
  }
  case NODE_MEMBER_ACCESS:
    return member_resolve(node, 0);
  case NODE_NEW: {
    const char *type = node->new_expr.type_name;
    int s = sem_find_struct(type);
    int c = sem_find_class(type);
    SemType rt = ST_INT;
    const char *kind = NULL;
    if (s >= 0) {
      rt = ST_STRUCT;
      kind = type;
    } else if (c >= 0) {
      rt = ST_CLASS;
      kind = type;
    }
    sem_record_type(node, rt, kind);
    return rt;
  }
  case NODE_INDEX:
  case NODE_METHOD_CALL: {
    SemType t = sem_peek(node);
    const char *kind = sem_peek_kind(node);
    sem_record_type(node, t, kind);
    return t;
  }
  case NODE_ARRAY_LITERAL:
    sem_error(node, "Array literal not supported in codegen expression");
    sem_record_type(node, ST_ERR, NULL);
    return ST_ERR;
  default:
    sem_error(node, "Unexpected expression in codegen");
    sem_record_type(node, ST_ERR, NULL);
    return ST_ERR;
  }
}

/**
 * @brief Resolves one parameter's declared type without caching
 * @param p VAR_DECL or IDENTIFIER parameter node
 * @param idx Parameter position (for float promotion)
 * @param pfloat Promotion flags of the callee, may be NULL
 * @param out_kind Receives instance name for struct/class params
 * @return Resolved category
 */
static SemType param_want(Node *p, int idx, int *pfloat,
                          const char **out_kind) {
  if (out_kind != NULL) {
    *out_kind = NULL;
  }
  SemType want = ST_INT;
  if (p->type == NODE_VAR_DECL) {
    want = sem_resolve_decl(p->var_decl.var_type, p, out_kind);
  }
  if (want == ST_INT && pfloat != NULL && idx < SEM_MAX_PARAMS &&
      pfloat[idx]) {
    want = ST_FLOAT;
  }
  return want;
}

/**
 * @brief Checks one call argument against its parameter type
 * @param arg Argument expression node
 * @param want Declared parameter category
 * @param want_kind Struct/class name when want is an instance, else NULL
 */
static void check_call_arg(Node *arg, SemType want, const char *want_kind) {
  SemType given = sem_validate(arg);
  if (want == ST_STRUCT || want == ST_CLASS) {
    if (given == want) {
      const char *given_kind = sem_peek_kind(arg);
      if (given_kind != NULL && want_kind != NULL &&
          !sem_is_subclass(given_kind, want_kind)) {
        sem_error(arg, "Cannot pass %s to %s parameter", given_kind,
                  want_kind);
      }
    } else if (given == ST_INT && arg->type == NODE_FUNC_CALL) {
      /* Optimistic: assume the call returns a matching instance. */
    } else {
      sem_error(arg, "Cannot pass %s to %s parameter",
                sem_type_name(given), want_kind);
    }
  } else if (!sem_compatible(want, given)) {
    sem_error(arg, "Cannot pass %s to %s parameter", sem_type_name(given),
              sem_type_name(want));
  }
}

SemType sem_expr(Node *node) { return sem_validate(node); }

/**
 * @brief Mirrors binary emission: operator checks, then children in order
 * @param node Binary operation node
 * @return Result category
 */
static SemType touch_binary(Node *node) {
  const char *op = node->binary_op.op;
  SemType lt = sem_peek(node->binary_op.left);
  SemType rt = sem_peek(node->binary_op.right);
  if (!op_is(op, "+") && !op_is(op, "-") && !op_is(op, "*") &&
      !op_is(op, "/") && !op_is(op, "%") && !is_comparison_op(op)) {
    sem_error(node, "Unsupported operator '%s' in codegen", op);
  }
  if (is_comparison_op(op)) {
    sem_record_type(node, ST_BOOL, NULL);
    if (lt == ST_NULL || rt == ST_NULL) {
      sem_touch(node->binary_op.left);
      sem_touch(node->binary_op.right);
      return ST_BOOL;
    }
    if (lt == ST_STR || rt == ST_STR) {
      if ((lt == ST_STR) != (rt == ST_STR)) {
        SemType bad = (lt == ST_STR) ? rt : lt;
        sem_error(node, "Operator '%s' cannot compare string with %s", op,
                  sem_type_name(bad));
      }
      sem_touch(node->binary_op.left);
      sem_touch(node->binary_op.right);
      return ST_BOOL;
    }
    if (!sem_is_numeric(lt) || !sem_is_numeric(rt)) {
      SemType bad = !sem_is_numeric(lt) ? lt : rt;
      sem_error(node, "Operator '%s' cannot be applied to %s", op,
                sem_type_name(bad));
    }
    sem_touch(node->binary_op.left);
    sem_touch(node->binary_op.right);
    return ST_BOOL;
  }
  if (lt == ST_NULL || rt == ST_NULL) {
    sem_error(node, "Operator '%s' cannot be applied to null", op);
  }
  if (lt == ST_STR || rt == ST_STR) {
    if (op_is(op, "+")) {
      if (lt != ST_STR || rt != ST_STR) {
        SemType bad = (lt == ST_STR) ? rt : lt;
        sem_error(node, "Cannot concatenate string with %s",
                  sem_type_name(bad));
      }
      sem_record_type(node, ST_STR, NULL);
      sem_touch(node->binary_op.left);
      sem_touch(node->binary_op.right);
      return ST_STR;
    }
    SemType bad = (lt == ST_STR) ? lt : rt;
    sem_error(node, "Operator '%s' cannot be applied to %s", op,
              sem_type_name(bad));
  }
  if (!sem_is_numeric(lt) || !sem_is_numeric(rt)) {
    SemType bad = !sem_is_numeric(lt) ? lt : rt;
    sem_error(node, "Operator '%s' cannot be applied to %s", op,
              sem_type_name(bad));
  }
  int is_float = (lt == ST_FLOAT || rt == ST_FLOAT);
  if (op_is(op, "%") && is_float) {
    sem_error(node, "Operator '%%' cannot be applied to float");
  }
  if (!is_float && (op_is(op, "/") || op_is(op, "%")) &&
      node->binary_op.right->type == NODE_INT_LITERAL &&
      node->binary_op.right->int_literal.value == 0) {
    sem_error(node->binary_op.right, "Division by zero");
  }
  SemType res = is_float ? ST_FLOAT : ST_INT;
  sem_record_type(node, res, NULL);
  sem_touch(node->binary_op.left);
  sem_touch(node->binary_op.right);
  return res;
}

/**
 * @brief Mirrors index emission: conditional checks, base then index
 * @param node Index node
 * @return Element category
 */
static SemType touch_index(Node *node) {
  SemType btype = sem_peek(node->index.base);
  SemType index_type = sem_peek(node->index.index);
  if (!sem_is_numeric(index_type)) {
    sem_validate(node->index.index);
    sem_error(node->index.index, "Index must be a number, got %s",
              sem_type_name(index_type));
  }
  if (btype == ST_ARR) {
    if (node->index.base->type != NODE_IDENTIFIER) {
      sem_error(node->index.base, "Cannot index a non-variable array base");
    }
    int slot = sem_scope_find(node->index.base->identifier.name);
    if (slot < 0 || sem_scope.vars[slot].type != ST_ARR) {
      sem_error(node->index.base, "Array '%s' is not declared",
                node->index.base->identifier.name);
    }
    SemType elem = sem_scope.vars[slot].elem;
    sem_record_type(node, elem, sem_scope.vars[slot].elem_kind);
    sem_touch(node->index.base);
    sem_touch(node->index.index);
    return elem;
  }
  if (btype != ST_STR) {
    sem_validate(node->index.base);
    sem_error(node->index.base, "Only strings can be indexed, got %s",
              sem_type_name(btype));
  }
  sem_validate(node->index.base);
  sem_record_type(node, ST_INT, NULL);
  sem_touch(node->index.base);
  sem_touch(node->index.index);
  return ST_INT;
}

/**
 * @brief Mirrors new-expression emission: checks, then arguments in order
 * @param node New-expression node
 * @return ST_STRUCT or ST_CLASS
 */
static SemType touch_new(Node *node) {
  const char *type = node->new_expr.type_name;
  int s = sem_find_struct(type);
  int c = sem_find_class(type);
  if (s < 0 && c < 0) {
    if (strcmp(type, "num") == 0 || strcmp(type, "bool") == 0 ||
        strcmp(type, "str") == 0 || strcmp(type, "arr") == 0) {
      sem_error(node, "Only structs and classes can be created with 'new'");
    }
    sem_error(node, "Unknown struct or class '%s'", type);
  }
  SemProg *sp = sem_prog();
  int nfields = (s >= 0) ? sp->structs[s].nfields : sp->classes[c].nfields;
  SemField *fields =
      (s >= 0) ? sp->structs[s].fields : sp->classes[c].fields;
  const char *kind = (s >= 0) ? sp->structs[s].name : sp->classes[c].name;
  int nargs = count_args(node->new_expr.args);
  if (nargs != nfields) {
    sem_error(node, "Expected %d arguments for '%s', got %d", nfields, kind,
              nargs);
  }
  int i = 0;
  for (Node *a = node->new_expr.args; a != NULL; a = a->right, i++) {
    SemType given = sem_validate(a);
    SemType want = fields[i].type;
    if (want == ST_STR) {
      if (given != ST_STR && given != ST_NULL) {
        sem_error(a, "Cannot assign %s to str field '%s'",
                  sem_type_name(given), fields[i].name);
      }
    } else if (want == ST_STRUCT || want == ST_CLASS) {
      if (given == ST_NULL) {
        /* Null clears the field. */
      } else if (given == want) {
        if (fields[i].kind != NULL) {
          const char *ak = sem_peek_kind(a);
          if (ak != NULL && !sem_is_subclass(ak, fields[i].kind)) {
            sem_error(a, "Cannot assign %s to %s field '%s'", ak,
                      fields[i].kind, fields[i].name);
          }
        }
      } else {
        sem_error(a, "Cannot assign %s to %s field '%s'",
                  sem_type_name(given),
                  fields[i].kind != NULL ? fields[i].kind : "instance",
                  fields[i].name);
      }
    } else if (!sem_is_numeric(given)) {
      sem_error(a, "Cannot assign %s to num field '%s'", sem_type_name(given),
                fields[i].name);
    }
  }
  for (Node *a = node->new_expr.args; a != NULL; a = a->right) {
    sem_touch(a);
  }
  SemType rt = (s >= 0) ? ST_STRUCT : ST_CLASS;
  sem_record_type(node, rt, kind);
  return rt;
}

/**
 * @brief Mirrors method-call emission: checks, receiver, then arguments
 * @param node Method call node
 * @return Result category
 */
static SemType touch_method_call(Node *node) {
  const char *object = node->method_call.object;
  const char *method = node->method_call.method;
  int cls = -1;
  int expr_this = (node->method_call.object_expr != NULL);
  if (expr_this) {
    const char *kind = sem_peek_kind(node->method_call.object_expr);
    if (kind == NULL || sem_find_class(kind) < 0) {
      sem_error(node->method_call.object_expr,
                "Can only call methods on a class instance");
    }
    cls = sem_find_class(kind);
  } else if (strcmp(object, "self") == 0) {
    if (!sem_scope.in_method) {
      sem_error(node, "Only self.member access is supported");
    }
    cls = sem_scope.cls;
  } else {
    if (sem_find_struct(object) >= 0 || sem_find_class(object) >= 0) {
      sem_error(node, "Cannot call method on type '%s', use an instance",
                object);
    }
    int obj_slot = sem_scope_find(object);
    if (obj_slot < 0) {
      sem_error(node, "Variable '%s' not declared", object);
    }
    if (sem_scope.vars[obj_slot].type == ST_STRUCT) {
      sem_error(node, "Struct '%s' has no methods",
                sem_scope.vars[obj_slot].kind);
    }
    if (sem_scope.vars[obj_slot].type != ST_CLASS) {
      sem_error(node, "'%s' is not a class instance", object);
    }
    sem_scope.vars[obj_slot].used = 1;
    cls = sem_find_class(sem_scope.vars[obj_slot].kind);
  }
  SemProg *sp = sem_prog();
  int owner = cls;
  int m = sem_find_method(cls, method, &owner);
  if (m < 0) {
    sem_error(node, "Class '%s' has no method '%s'",
              sp->classes[cls].name, method);
  }
  SemFunc *md = &sp->classes[owner].methods[m];
  int want_n = sem_count_params(md->params);
  int got_n = count_args(node->method_call.args);
  if (want_n != got_n) {
    sem_error(node, "Expected %d arguments for method '%s', got %d", want_n,
              method, got_n);
  }
  Node *p = md->params;
  Node *a = node->method_call.args;
  int i = 0;
  for (; p != NULL && a != NULL; p = p->right, a = a->right, i++) {
    const char *kind = NULL;
    SemType want = param_want(p, i, md->pfloat, &kind);
    check_call_arg(a, want, kind);
  }
  /* Emission order touches the receiver before the arguments. */
  if (expr_this) {
    sem_touch(node->method_call.object_expr);
  } else if (strcmp(object, "self") != 0) {
    int obj_slot = sem_scope_find(object);
    if (obj_slot >= 0) {
      sem_scope.vars[obj_slot].used = 1;
    }
  } else {
    int tslot = sem_scope_find("this ");
    if (tslot >= 0) {
      sem_scope.vars[tslot].used = 1;
    }
  }
  for (a = node->method_call.args; a != NULL; a = a->right) {
    sem_touch(a);
  }
  const char *rt = md->ret;
  SemType res = ST_FLOAT;
  if (rt != NULL && strcmp(rt, "str") == 0) {
    res = ST_STR;
  } else if (rt != NULL && strcmp(rt, "bool") == 0) {
    res = ST_BOOL;
  }
  sem_record_type(node, res, NULL);
  return res;
}

/**
 * @brief Mirrors user-call emission: wants, argument checks, arguments
 * @param node Call node
 * @param fi Function index
 * @return Result category
 */
static SemType touch_user_call(Node *node, int fi) {
  SemProg *sp = sem_prog();
  SemFunc *f = &sp->funcs[fi];
  Node *a = node->func_call.args;
  Node *p = f->params;
  int idx = 0;
  for (; a != NULL && p != NULL; a = a->right, p = p->right, idx++) {
    const char *want_kind = NULL;
    SemType want = param_want(p, idx, f->pfloat, &want_kind);
    check_call_arg(a, want, want_kind);
  }
  for (a = node->func_call.args; a != NULL; a = a->right) {
    sem_touch(a);
  }
  SemType rt = ST_INT;
  if (f->ret != NULL) {
    if (strcmp(f->ret, "str") == 0) {
      rt = ST_STR;
    } else if (strcmp(f->ret, "arr") == 0) {
      rt = ST_ARR;
    } else if (strcmp(f->ret, "void") == 0) {
      rt = ST_VOID;
    }
  }
  if (rt == ST_VOID) {
    sem_record_type(node, ST_VOID, NULL);
    return ST_VOID;
  }
  if (rt == ST_INT) {
    if (f->ret_float) {
      rt = ST_FLOAT;
    } else if (!f->bool_only) {
      for (int j = 0; j < SEM_MAX_PARAMS; j++) {
        if (f->pfloat[j]) {
          rt = ST_FLOAT;
          break;
        }
      }
    }
  }
  sem_record_type(node, rt, NULL);
  return rt;
}

/**
 * @brief Mirrors builtin-call emission: arity, checks, then arguments
 * @param node Call node
 * @return Result category
 */
static SemType touch_builtin_call(Node *node) {
  const char *name = node->func_call.name;
  Node *args = node->func_call.args;
  int nargs = count_args(args);
  if (strcmp(name, "input") == 0) {
    if (nargs > 1) {
      sem_error(node, "input takes at most 1 argument");
    }
    if (args != NULL) {
      SemType pt = sem_validate(args);
      if (pt != ST_STR) {
        sem_error(args, "input prompt must be a str, got %s",
                  sem_type_name(pt));
      }
      sem_touch(args);
    }
    sem_record_type(node, ST_INT, NULL);
    return ST_INT;
  }
  if (strcmp(name, "len") == 0) {
    if (nargs != 1) {
      sem_error(node, "len takes exactly 1 argument");
    }
    if (args != NULL) {
      SemType given = sem_validate(args);
      if (given != ST_STR) {
        sem_error(args, "len expects a str, got %s", sem_type_name(given));
      }
      sem_touch(args);
    }
    sem_record_type(node, ST_INT, NULL);
    return ST_INT;
  }
  if (strcmp(name, "tostr") == 0) {
    if (nargs != 1) {
      sem_error(node, "tostr takes exactly 1 argument");
    }
    if (args != NULL) {
      SemType given = sem_validate(args);
      if (!sem_is_numeric(given)) {
        sem_error(args, "tostr expects a num, got %s", sem_type_name(given));
      }
      sem_touch(args);
    }
    sem_record_type(node, ST_STR, NULL);
    return ST_STR;
  }
  if (strcmp(name, "tonum") == 0) {
    if (nargs != 1) {
      sem_error(node, "tonum takes exactly 1 argument");
    }
    if (args != NULL) {
      SemType given = sem_validate(args);
      if (given != ST_STR) {
        sem_error(args, "tonum expects a str, got %s", sem_type_name(given));
      }
      sem_touch(args);
    }
    sem_record_type(node, ST_FLOAT, NULL);
    return ST_FLOAT;
  }
  if (strcmp(name, "readFile") == 0) {
    if (nargs != 1) {
      sem_error(node, "readFile takes exactly 1 argument");
    }
    if (args != NULL) {
      SemType given = sem_validate(args);
      if (given != ST_STR) {
        sem_error(args, "readFile expects a str path, got %s",
                  sem_type_name(given));
      }
      sem_touch(args);
    }
    sem_record_type(node, ST_STR, NULL);
    return ST_STR;
  }
  if (nargs != 2) {
    sem_error(node, "writeFile takes exactly 2 arguments");
  }
  if (args != NULL) {
    SemType ptype = sem_validate(args);
    if (ptype != ST_STR) {
      sem_error(args, "writeFile expects a str path, got %s",
                sem_type_name(ptype));
    }
    if (args->right != NULL) {
      SemType ttype = sem_validate(args->right);
      if (ttype != ST_STR) {
        sem_error(args->right, "writeFile expects str text, got %s",
                  sem_type_name(ttype));
      }
    }
    sem_touch(args);
    if (args->right != NULL) {
      sem_touch(args->right);
    }
  }
  sem_record_type(node, ST_INT, NULL);
  return ST_INT;
}

SemType sem_check_call(Node *node, int allow_void) {
  const char *name = node->func_call.name;
  (void)allow_void;
  if (is_builtin(name)) {
    return touch_builtin_call(node);
  }
  int fi = sem_func_at(name);
  if (fi < 0) {
    sem_record_type(node, ST_INT, NULL);
    return ST_INT;
  }
  return touch_user_call(node, fi);
}

SemType sem_touch(Node *node) {
  if (node == NULL) {
    sem_error(NULL, "NULL expression in codegen");
    return ST_ERR;
  }
  switch (node->type) {
  case NODE_INT_LITERAL:
    sem_record_type(node, ST_INT, NULL);
    return ST_INT;
  case NODE_FLOAT_LITERAL:
    sem_record_type(node, ST_FLOAT, NULL);
    return ST_FLOAT;
  case NODE_STRING_LITERAL:
    sem_record_type(node, ST_STR, NULL);
    return ST_STR;
  case NODE_NULL:
    sem_record_type(node, ST_NULL, NULL);
    return ST_NULL;
  case NODE_IDENTIFIER: {
    int ref = sem_scope_require(node, node->identifier.name);
    if (sem_ref_is_global(ref)) {
      SemProg *sp = sem_prog();
      SemGlobal *g = &sp->globals[sem_ref_to_global(ref)];
      sem_record_type(node, g->type, g->kind);
      return g->type;
    }
    SemVar *v = &sem_scope.vars[ref];
    const char *kind = NULL;
    if (v->type == ST_STRUCT || v->type == ST_CLASS) {
      kind = v->kind;
    }
    sem_record_type(node, v->type, kind);
    return v->type;
  }
  case NODE_FUNC_CALL:
    return sem_check_call(node, 1);
  case NODE_BINARY_OP:
    return touch_binary(node);
  case NODE_MEMBER_ACCESS:
    return member_resolve(node, 1);
  case NODE_NEW:
    return touch_new(node);
  case NODE_INDEX:
    return touch_index(node);
  case NODE_METHOD_CALL:
    return touch_method_call(node);
  case NODE_ARRAY_LITERAL:
    sem_error(node, "Array literal not supported in codegen expression");
    sem_record_type(node, ST_ERR, NULL);
    return ST_ERR;
  default:
    sem_error(node, "Unexpected expression in codegen");
    sem_record_type(node, ST_ERR, NULL);
    return ST_ERR;
  }
}
