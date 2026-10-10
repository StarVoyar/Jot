#include "sem_impl.h"

/** Scan-time float states (mirrors the historic RF_* scale) */
#define SST_INT 0
#define SST_FLOAT 1
#define SST_STR 2

/** Per-function scan context */
typedef struct {
  char vars[128][64];
  int state[128];
  int nvars;
  Node *params;
  SemFunc *fn;
  int saw_float;
  char arrays[64][64];
  int astate[64];
  int narrays;
} ScanCtx;

/**
 * @brief Reads a variable state from the scan context
 * @param ctx Function context
 * @param name Variable name
 * @return SST_INT, SST_FLOAT, or SST_STR
 */
static int scan_get_state(ScanCtx *ctx, const char *name) {
  for (int i = ctx->nvars - 1; i >= 0; i--) {
    if (strcmp(ctx->vars[i], name) == 0) {
      return ctx->state[i];
    }
  }
  int idx = 0;
  for (Node *p = ctx->params; p != NULL; p = p->right, idx++) {
    const char *pn =
        p->type == NODE_VAR_DECL ? p->var_decl.name : p->identifier.name;
    if (strcmp(pn, name) == 0) {
      if (ctx->fn != NULL && idx < SEM_MAX_PARAMS && ctx->fn->pfloat[idx]) {
        return SST_FLOAT;
      }
      return SST_INT;
    }
  }
  SemProg *sp = sem_prog();
  for (int g = 0; g < sp->nglobals; g++) {
    if (strcmp(sp->globals[g].name, name) == 0) {
      if (sp->globals[g].type == ST_FLOAT) {
        return SST_FLOAT;
      }
      if (sp->globals[g].type == ST_STR) {
        return SST_STR;
      }
      return SST_INT;
    }
  }
  return SST_INT;
}

/**
 * @brief Merges a variable state into the scan context
 * @param ctx Function context
 * @param name Variable name
 * @param st New state
 * @param exact Non-zero to replace instead of merging upward
 */
static void scan_update_var(ScanCtx *ctx, const char *name, int st, int exact) {
  int old = scan_get_state(ctx, name);
  int merged = exact ? st : (old > st ? old : st);
  for (int i = ctx->nvars - 1; i >= 0; i--) {
    if (strcmp(ctx->vars[i], name) == 0) {
      ctx->state[i] = merged;
      return;
    }
  }
  if (ctx->nvars >= 128) {
    return;
  }
  snprintf(ctx->vars[ctx->nvars], sizeof(ctx->vars[0]), "%s", name);
  ctx->state[ctx->nvars] = merged;
  ctx->nvars++;
}

/**
 * @brief Reads an array state from the scan context
 * @param ctx Function context
 * @param name Array name
 * @return Array element state
 */
static int scan_array_state(ScanCtx *ctx, const char *name) {
  for (int i = ctx->narrays - 1; i >= 0; i--) {
    if (strcmp(ctx->arrays[i], name) == 0) {
      return ctx->astate[i];
    }
  }
  return SST_INT;
}

/**
 * @brief Stores an array state in the scan context
 * @param ctx Function context
 * @param name Array name
 * @param st Element state
 */
static void scan_set_array(ScanCtx *ctx, const char *name, int st) {
  for (int i = ctx->narrays - 1; i >= 0; i--) {
    if (strcmp(ctx->arrays[i], name) == 0) {
      ctx->astate[i] = st;
      return;
    }
  }
  if (ctx->narrays >= 64) {
    return;
  }
  snprintf(ctx->arrays[ctx->narrays], sizeof(ctx->arrays[0]), "%s", name);
  ctx->astate[ctx->narrays] = st;
  ctx->narrays++;
}

/**
 * @brief Converts a scan state to a semantic category
 * @param st Scan state
 * @return Matching category
 */
static SemType scan_state_type(int st) {
  if (st == SST_FLOAT) {
    return ST_FLOAT;
  }
  if (st == SST_STR) {
    return ST_STR;
  }
  return ST_INT;
}

/**
 * @brief Resolves the static kind of a new-expression receiver
 * @param node Expression node
 * @return Type name for new expressions, else NULL
 */
static const char *scan_new_kind(Node *node) {
  if (node != NULL && node->type == NODE_NEW) {
    return node->new_expr.type_name;
  }
  return NULL;
}

/**
 * @brief Scan-time peek mirroring the historic silent type query
 * @param node Expression to inspect
 * @param ctx Function context (locals are unregistered, like before)
 * @return Best-effort category
 * @details Local variables are unknown during scans (no frame exists yet),
 * so member and method receivers only resolve through new expressions,
 * exactly like the historic scan-time behavior.
 */
static SemType scan_peek(Node *node, ScanCtx *ctx) {
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
  case NODE_IDENTIFIER:
    return scan_state_type(scan_get_state(ctx, node->identifier.name));
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
    if (strcmp(nm, "char") == 0) {
      return ST_STR;
    }
    if (strcmp(nm, "system") == 0) {
      return ST_INT;
    }
    if (strcmp(nm, "args") == 0) {
      return ST_ARR;
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
    SemType lt = scan_peek(node->binary_op.left, ctx);
    SemType rt = scan_peek(node->binary_op.right, ctx);
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
      const char *kind = scan_new_kind(node->member_access.object_expr);
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
    SemType btype = scan_peek(node->index.base, ctx);
    if (btype == ST_STR) {
      return ST_INT;
    }
    if (btype == ST_ARR) {
      return ST_INT;
    }
    return ST_INT;
  }
  case NODE_METHOD_CALL: {
    const char *kind = scan_new_kind(node->method_call.object_expr);
    int c = kind != NULL ? sem_find_class(kind) : -1;
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
 * @brief Infers the scan-time float/string state of an expression
 * @param node Expression to inspect
 * @param ctx Function context
 * @return SST_INT, SST_FLOAT, or SST_STR
 */
static int scan_expr_state(Node *node, ScanCtx *ctx) {
  if (node == NULL) {
    return SST_INT;
  }
  switch (node->type) {
  case NODE_FLOAT_LITERAL:
    return SST_FLOAT;
  case NODE_STRING_LITERAL:
    return SST_STR;
  case NODE_INT_LITERAL:
  case NODE_NULL:
    return SST_INT;
  case NODE_IDENTIFIER:
    return scan_get_state(ctx, node->identifier.name);
  case NODE_BINARY_OP: {
    const char *op = node->binary_op.op;
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
        strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
        strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) {
      return SST_INT;
    }
    int l = scan_expr_state(node->binary_op.left, ctx);
    int r = scan_expr_state(node->binary_op.right, ctx);
    if (strcmp(op, "+") == 0 && (l == SST_STR || r == SST_STR)) {
      return SST_STR;
    }
    if (l == SST_FLOAT || r == SST_FLOAT) {
      return SST_FLOAT;
    }
    return SST_INT;
  }
  case NODE_FUNC_CALL: {
    const char *nm = node->func_call.name;
    if (strcmp(nm, "tonum") == 0) {
      return SST_FLOAT;
    }
    if (strcmp(nm, "tostr") == 0 || strcmp(nm, "readFile") == 0) {
      return SST_STR;
    }
    if (strcmp(nm, "char") == 0) {
      return SST_STR;
    }
    if (strcmp(nm, "system") == 0) {
      return SST_INT;
    }
    if (strcmp(nm, "input") == 0) {
      return SST_INT;
    }
    SemProg *sp = sem_prog();
    for (int s = 0; s < sp->nfuncs; s++) {
      if (strcmp(sp->funcs[s].name, nm) == 0) {
        if (sp->funcs[s].ret != NULL) {
          if (strcmp(sp->funcs[s].ret, "str") == 0) {
            return SST_STR;
          }
          if (strcmp(sp->funcs[s].ret, "void") == 0) {
            return SST_INT;
          }
        }
        if (sp->funcs[s].ret_float) {
          return SST_FLOAT;
        }
        for (int j = 0; j < SEM_MAX_PARAMS; j++) {
          if (sp->funcs[s].pfloat[j]) {
            return SST_FLOAT;
          }
        }
        return SST_INT;
      }
    }
    return SST_INT;
  }
  case NODE_METHOD_CALL:
    return SST_INT;
  case NODE_INDEX: {
    if (node->index.base != NULL &&
        node->index.base->type == NODE_IDENTIFIER) {
      return scan_array_state(ctx, node->index.base->identifier.name);
    }
    return SST_INT;
  }
  case NODE_ARRAY_LITERAL: {
    int st = SST_INT;
    for (Node *e = node->array_literal.elements; e != NULL; e = e->right) {
      int es = scan_expr_state(e, ctx);
      if (es > st) {
        st = es;
      }
    }
    return st;
  }
  case NODE_MEMBER_ACCESS:
  case NODE_NEW:
    return SST_INT;
  default:
    return SST_INT;
  }
}

/**
 * @brief Walks statements, tracking float flow into returns
 * @param list Statement list (follows right chain, recurses into blocks)
 * @param ctx Function context, saw_float set on a float return
 */
static void scan_walk(Node *list, ScanCtx *ctx) {
  for (Node *s = list; s != NULL; s = s->right) {
    switch (s->type) {
    case NODE_VAR_DECL: {
      int st = SST_INT;
      if (s->var_decl.var_type != NULL &&
          strcmp(s->var_decl.var_type, "str") == 0) {
        st = SST_STR;
      } else if (s->var_decl.value != NULL) {
        st = scan_expr_state(s->var_decl.value, ctx);
      }
      scan_update_var(ctx, s->var_decl.name, st, 1);
      break;
    }
    case NODE_ARRAY_DECL: {
      int st = SST_INT;
      Node *rhs = s->array_decl.elements;
      if (rhs != NULL && rhs->type == NODE_IDENTIFIER) {
        st = scan_array_state(ctx, rhs->identifier.name);
      } else if (rhs != NULL && rhs->type == NODE_ARRAY_LITERAL) {
        for (Node *e = rhs->array_literal.elements; e != NULL; e = e->right) {
          int es = scan_expr_state(e, ctx);
          if (es > st) {
            st = es;
          }
        }
      }
      scan_set_array(ctx, s->array_decl.name, st);
      scan_update_var(ctx, s->array_decl.name, SST_INT, 1);
      break;
    }
    case NODE_ASSIGNMENT: {
      int st = scan_expr_state(s->assignment.value, ctx);
      scan_update_var(ctx, s->assignment.name, st, 0);
      break;
    }
    case NODE_ADD_ASSIGN:
    case NODE_SUB_ASSIGN: {
      const char *nm = s->type == NODE_ADD_ASSIGN ? s->add_assign.name
                                                 : s->sub_assign.name;
      Node *val = s->type == NODE_ADD_ASSIGN ? s->add_assign.value
                                             : s->sub_assign.value;
      int st = scan_expr_state(val, ctx);
      scan_update_var(ctx, nm, st, 0);
      break;
    }
    case NODE_RETURN: {
      if (s->return_stmt.value != NULL &&
          scan_expr_state(s->return_stmt.value, ctx) == SST_FLOAT) {
        ctx->saw_float = 1;
      }
      break;
    }
    case NODE_IF:
      scan_walk(s->if_stmt.body, ctx);
      scan_walk(s->if_stmt.else_body, ctx);
      break;
    case NODE_WHILE:
      scan_walk(s->while_stmt.body, ctx);
      break;
    case NODE_FOR: {
      scan_update_var(ctx, s->for_stmt.var_name,
                      scan_array_state(ctx, s->for_stmt.array_name), 1);
      scan_walk(s->for_stmt.body, ctx);
      break;
    }
    default:
      break;
    }
  }
}

/**
 * @brief Marks functions whose returns yield floats
 * @param root Top level statements
 */
static void scan_return_floats(Node *root) {
  SemProg *sp = sem_prog();
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type != NODE_FUNCTION) {
      continue;
    }
    int sig = sem_func_at(s->function.name);
    if (sig < 0 || sp->funcs[sig].ret_float) {
      continue;
    }
    ScanCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = sp->funcs[sig].params;
    ctx.fn = &sp->funcs[sig];
    scan_walk(s->function.body, &ctx);
    if (ctx.saw_float) {
      sp->funcs[sig].ret_float = 1;
    }
  }
}

/**
 * @brief Promotes num globals initialized with a float
 * @param root Top level statements
 */
static void scan_promote_globals(Node *root) {
  SemProg *sp = sem_prog();
  ScanCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_VAR_DECL && s->var_decl.is_global &&
        s->var_decl.value != NULL) {
      for (int g = 0; g < sp->nglobals; g++) {
        if (strcmp(sp->globals[g].name, s->var_decl.name) == 0 &&
            sp->globals[g].type == ST_INT &&
            scan_peek(s->var_decl.value, &ctx) == ST_FLOAT) {
          sp->globals[g].type = ST_FLOAT;
        }
      }
    }
  }
}

/**
 * @brief Scans returns for bool-only status
 * @param node Statement list (follows right chain, recurses into blocks)
 * @param found Receives non-zero if any return was seen
 * @param all_bool Receives non-zero if all returns are comparisons
 */
static void scan_bool_only(Node *node, int *found, int *all_bool) {
  for (Node *s = node; s != NULL; s = s->right) {
    switch (s->type) {
    case NODE_RETURN: {
      *found = 1;
      Node *v = s->return_stmt.value;
      if (!(v != NULL && v->type == NODE_BINARY_OP &&
            (strcmp(v->binary_op.op, "==") == 0 ||
             strcmp(v->binary_op.op, "!=") == 0 ||
             strcmp(v->binary_op.op, "<") == 0 ||
             strcmp(v->binary_op.op, ">") == 0 ||
             strcmp(v->binary_op.op, "<=") == 0 ||
             strcmp(v->binary_op.op, ">=") == 0))) {
        *all_bool = 0;
      }
      break;
    }
    case NODE_IF:
      scan_bool_only(s->if_stmt.body, found, all_bool);
      scan_bool_only(s->if_stmt.else_body, found, all_bool);
      break;
    case NODE_WHILE:
      scan_bool_only(s->while_stmt.body, found, all_bool);
      break;
    case NODE_FOR:
      scan_bool_only(s->for_stmt.body, found, all_bool);
      break;
    case NODE_CLASS_DEF:
      scan_bool_only(s->class_def.methods, found, all_bool);
      break;
    case NODE_METHOD_DEF:
      scan_bool_only(s->method_def.body, found, all_bool);
      break;
    default:
      break;
    }
  }
}

/**
 * @brief Visits every expression in a subtree with a callback
 * @param node Subtree root (follows right chains)
 * @param visit Callback receiving each expression node
 */
static void scan_visit_exprs(Node *node, void (*visit)(Node *)) {
  if (node == NULL) {
    return;
  }
  visit(node);
  switch (node->type) {
  case NODE_VAR_DECL:
    scan_visit_exprs(node->var_decl.value, visit);
    break;
  case NODE_PRINT:
    scan_visit_exprs(node->print_stmt.value, visit);
    break;
  case NODE_RETURN:
    scan_visit_exprs(node->return_stmt.value, visit);
    break;
  case NODE_BINARY_OP:
    scan_visit_exprs(node->binary_op.left, visit);
    scan_visit_exprs(node->binary_op.right, visit);
    break;
  case NODE_IF:
    scan_visit_exprs(node->if_stmt.condition, visit);
    scan_visit_exprs(node->if_stmt.body, visit);
    scan_visit_exprs(node->if_stmt.else_body, visit);
    break;
  case NODE_WHILE:
    scan_visit_exprs(node->while_stmt.condition, visit);
    scan_visit_exprs(node->while_stmt.body, visit);
    break;
  case NODE_FOR:
    scan_visit_exprs(node->for_stmt.body, visit);
    break;
  case NODE_FUNCTION:
    scan_visit_exprs(node->function.params, visit);
    scan_visit_exprs(node->function.body, visit);
    break;
  case NODE_ASSIGNMENT:
    scan_visit_exprs(node->assignment.value, visit);
    break;
  case NODE_ADD_ASSIGN:
    scan_visit_exprs(node->add_assign.value, visit);
    break;
  case NODE_SUB_ASSIGN:
    scan_visit_exprs(node->sub_assign.value, visit);
    break;
  case NODE_INDEX_ASSIGN:
    scan_visit_exprs(node->index_assign.value, visit);
    scan_visit_exprs(node->index_assign.index, visit);
    break;
  case NODE_ARRAY_DECL:
    scan_visit_exprs(node->array_decl.elements, visit);
    break;
  case NODE_ARRAY_LITERAL:
    scan_visit_exprs(node->array_literal.elements, visit);
    break;
  case NODE_NEW:
    scan_visit_exprs(node->new_expr.args, visit);
    break;
  case NODE_INDEX:
    scan_visit_exprs(node->index.base, visit);
    scan_visit_exprs(node->index.index, visit);
    break;
  case NODE_METHOD_CALL:
    scan_visit_exprs(node->method_call.args, visit);
    scan_visit_exprs(node->method_call.object_expr, visit);
    break;
  case NODE_MEMBER_ACCESS:
    scan_visit_exprs(node->member_access.object_expr, visit);
    break;
  case NODE_MEMBER_ASSIGN:
    scan_visit_exprs(node->member_assign.value, visit);
    scan_visit_exprs(node->member_assign.object_expr, visit);
    break;
  case NODE_CLASS_DEF:
    scan_visit_exprs(node->class_def.methods, visit);
    break;
  case NODE_METHOD_DEF:
    scan_visit_exprs(node->method_def.body, visit);
    break;
  case NODE_FUNC_CALL:
    scan_visit_exprs(node->func_call.args, visit);
    break;
  default:
    break;
  }
  scan_visit_exprs(node->right, visit);
}

/**
 * @brief Promotes plain params receiving floats (call callback)
 * @param node Call node visited by scan_visit_exprs
 */
static void scan_plain_call(Node *node) {
  if (node == NULL || node->type != NODE_FUNC_CALL) {
    return;
  }
  if (strcmp(node->func_call.name, "input") == 0) {
    return;
  }
  int fi = sem_func_at(node->func_call.name);
  if (fi < 0) {
    return;
  }
  ScanCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  int idx = 0;
  for (Node *a = node->func_call.args; a != NULL && idx < SEM_MAX_PARAMS;
       a = a->right, idx++) {
    if (scan_peek(a, &ctx) == ST_FLOAT) {
      sem_prog()->funcs[fi].pfloat[idx] = 1;
    }
  }
}

/**
 * @brief Promotes method params receiving floats (matched by method name)
 * @param node Call node visited by scan_visit_exprs
 */
static void scan_method_call(Node *node) {
  if (node == NULL || node->type != NODE_METHOD_CALL) {
    return;
  }
  const char *method = node->method_call.method;
  ScanCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  int idx = 0;
  for (Node *a = node->method_call.args; a != NULL && idx < SEM_MAX_PARAMS;
       a = a->right, idx++) {
    if (scan_peek(a, &ctx) == ST_FLOAT) {
      SemProg *sp = sem_prog();
      for (int c = 0; c < sp->nclasses; c++) {
        for (int m = 0; m < sp->classes[c].nmethods; m++) {
          if (strcmp(sp->classes[c].methods[m].name, method) == 0) {
            sp->classes[c].methods[m].pfloat[idx] = 1;
          }
        }
      }
    }
  }
}

void sem_scan_fix(Node *root) {
  SemProg *sp = sem_prog();
  /* Method signatures carry no bool-only flag: mangled method labels can
     never match a call name, so only plain functions are scanned. */
  for (int i = 0; i < sp->nfuncs; i++) {
    for (Node *s = root; s != NULL; s = s->right) {
      if (s->type == NODE_FUNCTION &&
          strcmp(s->function.name, sp->funcs[i].name) == 0) {
        int found = 0;
        int all_bool = 1;
        scan_bool_only(s->function.body, &found, &all_bool);
        if (found && all_bool) {
          sp->funcs[i].bool_only = 1;
        }
        break;
      }
    }
  }
  scan_visit_exprs(root, scan_method_call);
  for (int pass = 0; pass < 3; pass++) {
    scan_visit_exprs(root, scan_plain_call);
    scan_return_floats(root);
    scan_promote_globals(root);
  }
}
