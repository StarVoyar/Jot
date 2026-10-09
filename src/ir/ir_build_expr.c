#include "ir_build.h"

void irb_bug(const char *what) {
  fprintf(stderr, "Internal error in IR builder: %s\n", what);
  exit(1);
}

IrType irb_type(SemType t) {
  switch (t) {
  case ST_FLOAT:
    return IR_FLOAT;
  case ST_BOOL:
    return IR_BOOL;
  case ST_STR:
    return IR_STR;
  case ST_ARR:
    return IR_ARR;
  case ST_STRUCT:
    return IR_STRUCT;
  case ST_CLASS:
    return IR_CLASS;
  case ST_NULL:
    return IR_NULL;
  case ST_VOID:
    return IR_VOID;
  default:
    return IR_INT;
  }
}

int irb_temp(BuildCtx *ctx, IrType t) {
  int slot = ctx->f->nvars + ctx->f->ntemps;
  ctx->f->temp_types =
      realloc(ctx->f->temp_types, (size_t)(ctx->f->ntemps + 1) * sizeof(IrType));
  ctx->f->temp_types[ctx->f->ntemps] = t;
  ctx->f->ntemps++;
  return slot;
}

IrBlock *irb_cur(BuildCtx *ctx) { return &ctx->f->blocks[ctx->cur]; }

IrInstr *irb_emit(BuildCtx *ctx, IrInstr ins) {
  return ir_emit(irb_cur(ctx), ins);
}

IrBlock *irb_block(BuildCtx *ctx) {
  ctx->cur = ir_add_block(ctx->f)->id;
  return irb_cur(ctx);
}

int irb_fresh(BuildCtx *ctx) { return ir_add_block(ctx->f)->id; }

void irb_goto(BuildCtx *ctx, int id) {
  ctx->cur = id;
  ctx->terminated = 0;
}

int irb_scope_find(BuildCtx *ctx, const char *name) {
  for (int i = ctx->nscope - 1; i >= 0; i--) {
    if (strcmp(ctx->names[i], name) == 0) {
      return ctx->slots[i];
    }
  }
  return -1;
}

void irb_scope_declare(BuildCtx *ctx, const char *name, int slot) {
  ctx->names[ctx->nscope] = (char *)name;
  ctx->slots[ctx->nscope] = slot;
  ctx->nscope++;
}

int irb_global_at(BuildCtx *ctx, const char *name) {
  (void)ctx;
  SemProg *sp = sem_prog();
  for (int g = 0; g < sp->nglobals; g++) {
    if (strcmp(sp->globals[g].name, name) == 0) {
      return g;
    }
  }
  return -1;
}

/**
 * @brief Converts a value to a category with an explicit instruction
 * @param ctx Builder context
 * @param v Value to convert
 * @param want Target category
 * @return Converted value (a temp when conversion runs)
 */
IrVal irb_convert(BuildCtx *ctx, IrVal v, IrType want) {
  IrType given = ir_val_type(ctx->f, v);
  if ((want == IR_INT || want == IR_BOOL) && given == IR_FLOAT) {
    int t = irb_temp(ctx, IR_INT);
    IrInstr ins = ir_instr(IR_F2I);
    ins.dst = t;
    ins.v[0] = v;
    ins.nv = 1;
    irb_emit(ctx, ins);
    return ir_temp(t, IR_INT);
  }
  if (want == IR_FLOAT && (given == IR_INT || given == IR_BOOL)) {
    int t = irb_temp(ctx, IR_FLOAT);
    IrInstr ins = ir_instr(IR_I2F);
    ins.dst = t;
    ins.v[0] = v;
    ins.nv = 1;
    irb_emit(ctx, ins);
    return ir_temp(t, IR_FLOAT);
  }
  return v;
}

/**
 * @brief Truncates an integer value to its low byte
 * @param ctx Builder context
 * @param v Value to truncate
 * @return Truncated value
 */
IrVal irb_trunc8(BuildCtx *ctx, IrVal v) {
  int t = irb_temp(ctx, IR_INT);
  IrInstr ins = ir_instr(IR_TRUNC8);
  ins.dst = t;
  ins.v[0] = v;
  ins.nv = 1;
  irb_emit(ctx, ins);
  return ir_temp(t, IR_INT);
}

/**
 * @brief Converts an index value to a plain integer
 * @param ctx Builder context
 * @param v Index value
 * @return Integer index value
 */
IrVal irb_index(BuildCtx *ctx, IrVal v) {
  IrType t = ir_val_type(ctx->f, v);
  if (t == IR_FLOAT) {
    return irb_convert(ctx, v, IR_INT);
  }
  if (t == IR_BOOL) {
    return irb_trunc8(ctx, v);
  }
  return v;
}

/**
 * @brief Converts a call argument to its parameter want
 * @param ctx Builder context
 * @param v Argument value
 * @param want Wanted category
 * @return Converted argument value
 */
static IrVal convert_arg(BuildCtx *ctx, IrVal v, IrType want) {
  IrType given = ir_val_type(ctx->f, v);
  if ((want == IR_INT || want == IR_BOOL) && given == IR_FLOAT) {
    return irb_convert(ctx, v, IR_INT);
  }
  if (want == IR_FLOAT && (given == IR_INT || given == IR_BOOL)) {
    return irb_convert(ctx, v, IR_FLOAT);
  }
  return v;
}

/**
 * @brief Lowers a user-function call
 * @param ctx Builder context
 * @param node Call node
 * @param fi Function index
 * @return Result temp (void calls still emit, result is ignored)
 */
IrVal irb_call(BuildCtx *ctx, Node *node, int fi) {
  SemProg *sp = sem_prog();
  SemFunc *f = &sp->funcs[fi];
  IrVal *args = NULL;
  int nargs = 0;
  int idx = 0;
  for (Node *a = node->func_call.args; a != NULL; a = a->right, idx++) {
    IrVal v = irb_expr(ctx, a);
    IrType want = IR_INT;
    if (idx < f->nparams) {
      want = irb_type(f->ptypes[idx]);
    }
    v = convert_arg(ctx, v, want);
    args = realloc(args, (size_t)(nargs + 1) * sizeof(IrVal));
    args[nargs++] = v;
  }
  IrInstr ins = ir_instr(IR_CALL);
  ins.callee = fi;
  ins.list = args;
  ins.nlist = nargs;
  ins.name = f->name;
  SemType rs = sem_expr_type(node);
  if (rs == ST_VOID) {
    ins.dst = -1;
    irb_emit(ctx, ins);
    return ir_null();
  }
  int t = irb_temp(ctx, irb_type(rs));
  ins.dst = t;
  irb_emit(ctx, ins);
  return ir_temp(t, irb_type(rs));
}

/**
 * @brief Lowers a builtin call
 * @param ctx Builder context
 * @param node Call node
 * @return Result temp
 */
static IrVal builtin_call(BuildCtx *ctx, Node *node) {
  const char *name = node->func_call.name;
  IrOp op = IR_INPUT;
  if (strcmp(name, "len") == 0) {
    op = IR_LEN;
  } else if (strcmp(name, "tostr") == 0) {
    op = IR_TOSTR;
  } else if (strcmp(name, "tonum") == 0) {
    op = IR_TONUM;
  } else if (strcmp(name, "readFile") == 0) {
    op = IR_READFILE;
  } else if (strcmp(name, "writeFile") == 0) {
    op = IR_WRITEFILE;
  }
  IrInstr ins = ir_instr(op);
  for (Node *a = node->func_call.args; a != NULL; a = a->right) {
    IrVal v = irb_expr(ctx, a);
    ins.list =
        realloc(ins.list, (size_t)(ins.nlist + 1) * sizeof(IrVal));
    ins.list[ins.nlist++] = v;
  }
  SemType rs = sem_expr_type(node);
  int t = irb_temp(ctx, irb_type(rs));
  ins.dst = t;
  irb_emit(ctx, ins);
  return ir_temp(t, irb_type(rs));
}

/**
 * @brief Lowers a binary operation
 * @param ctx Builder context
 * @param node Binary operation node
 * @return Result temp
 */
static IrVal binary_val(BuildCtx *ctx, Node *node) {
  const char *op = node->binary_op.op;
  IrVal l = irb_expr(ctx, node->binary_op.left);
  IrVal r = irb_expr(ctx, node->binary_op.right);
  IrType lt = ir_val_type(ctx->f, l);
  IrType rt = ir_val_type(ctx->f, r);
  int is_cmp = (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
                strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0);
  int cond = IR_CEQ;
  if (strcmp(op, "!=") == 0) {
    cond = IR_CNE;
  } else if (strcmp(op, "<") == 0) {
    cond = IR_CLT;
  } else if (strcmp(op, ">") == 0) {
    cond = IR_CGT;
  } else if (strcmp(op, "<=") == 0) {
    cond = IR_CLE;
  } else if (strcmp(op, ">=") == 0) {
    cond = IR_CGE;
  }
  if (!is_cmp && strcmp(op, "+") == 0 && (lt == IR_STR || rt == IR_STR)) {
    int t = irb_temp(ctx, IR_STR);
    IrInstr ins = ir_instr(IR_CONCAT);
    ins.dst = t;
    ins.v[0] = l;
    ins.v[1] = r;
    ins.nv = 2;
    irb_emit(ctx, ins);
    return ir_temp(t, IR_STR);
  }
  if (is_cmp && (lt == IR_STR || rt == IR_STR)) {
    int t = irb_temp(ctx, IR_BOOL);
    IrInstr ins = ir_instr(IR_CMP);
    ins.dst = t;
    ins.type = IR_STR;
    ins.cond = cond;
    ins.v[0] = l;
    ins.v[1] = r;
    ins.nv = 2;
    irb_emit(ctx, ins);
    return ir_temp(t, IR_BOOL);
  }
  if (is_cmp && (lt == IR_NULL || rt == IR_NULL)) {
    int t = irb_temp(ctx, IR_BOOL);
    IrInstr ins = ir_instr(IR_CMP);
    ins.dst = t;
    ins.type = IR_INT;
    ins.cond = cond;
    ins.v[0] = l;
    ins.v[1] = r;
    ins.nv = 2;
    irb_emit(ctx, ins);
    return ir_temp(t, IR_BOOL);
  }
  IrType common = (lt == IR_FLOAT || rt == IR_FLOAT) ? IR_FLOAT : IR_INT;
  if (common == IR_FLOAT) {
    l = irb_convert(ctx, l, IR_FLOAT);
    r = irb_convert(ctx, r, IR_FLOAT);
  }
  if (is_cmp) {
    int t = irb_temp(ctx, IR_BOOL);
    IrInstr ins = ir_instr(IR_CMP);
    ins.dst = t;
    ins.type = common;
    ins.cond = cond;
    ins.v[0] = l;
    ins.v[1] = r;
    ins.nv = 2;
    irb_emit(ctx, ins);
    return ir_temp(t, IR_BOOL);
  }
  int t = irb_temp(ctx, common);
  IrInstr ins = ir_instr(IR_ADD);
  if (strcmp(op, "-") == 0) {
    ins.op = IR_SUB;
  } else if (strcmp(op, "*") == 0) {
    ins.op = IR_MUL;
  } else if (strcmp(op, "/") == 0) {
    ins.op = IR_DIV;
  } else if (strcmp(op, "%") == 0) {
    ins.op = IR_MOD;
  }
  ins.dst = t;
  ins.type = common;
  ins.v[0] = l;
  ins.v[1] = r;
  ins.nv = 2;
  irb_emit(ctx, ins);
  return ir_temp(t, common);
}

/**
 * @brief Finds a field index on a struct or class kind
 * @param kind Struct or class name
 * @param member Field name
 * @return Field index (valid programs always resolve)
 */
static int field_index(const char *kind, const char *member) {
  SemProg *sp = sem_prog();
  for (int i = 0; i < sp->nstructs; i++) {
    if (strcmp(sp->structs[i].name, kind) == 0) {
      for (int f = 0; f < sp->structs[i].nfields; f++) {
        if (strcmp(sp->structs[i].fields[f].name, member) == 0) {
          return f;
        }
      }
    }
  }
  for (int i = 0; i < sp->nclasses; i++) {
    if (strcmp(sp->classes[i].name, kind) == 0) {
      for (int f = 0; f < sp->classes[i].nfields; f++) {
        if (strcmp(sp->classes[i].fields[f].name, member) == 0) {
          return f;
        }
      }
    }
  }
  irb_bug("unresolvable field in IR builder");
  return -1;
}

/**
 * @brief Returns the IR category of a frame slot
 * @param ctx Builder context
 * @param slot Frame slot
 * @return Slot category
 */
static IrType var_irtype(BuildCtx *ctx, int slot) {
  for (int i = 0; i < ctx->nvars; i++) {
    if (ctx->vars[i].slot == slot) {
      return irb_type(ctx->vars[i].type);
    }
  }
  return ir_slot_type(ctx->f, slot);
}

/**
 * @brief Returns the instance kind of a frame slot
 * @param ctx Builder context
 * @param slot Frame slot
 * @return Kind name, or NULL
 */
static const char *var_kind(BuildCtx *ctx, int slot) {
  for (int i = 0; i < ctx->nvars; i++) {
    if (ctx->vars[i].slot == slot) {
      return ctx->vars[i].kind;
    }
  }
  return NULL;
}

/**
 * @brief Lowers member access to a value
 * @param ctx Builder context
 * @param node Member access node
 * @return Value holding the field
 */
static IrVal member_val(BuildCtx *ctx, Node *node) {
  const char *object = node->member_access.object;
  const char *member = node->member_access.member;
  SemType rs = sem_expr_type(node);
  IrType rt = irb_type(rs);
  if (node->member_access.object_expr != NULL) {
    IrVal base = irb_expr(ctx, node->member_access.object_expr);
    const char *kind = sem_expr_kind(node->member_access.object_expr);
    int t = irb_temp(ctx, rt);
    IrInstr ins = ir_instr(IR_FIELD_LOAD);
    ins.dst = t;
    ins.type = rt;
    ins.v[0] = base;
    ins.nv = 1;
    ins.aux = field_index(kind, member);
    irb_emit(ctx, ins);
    return ir_temp(t, rt);
  }
  if (strcmp(object, "self") == 0) {
    int pslot = irb_scope_find(ctx, member);
    if (pslot >= 0) {
      for (int i = 0; i < ctx->nvars; i++) {
        if (ctx->vars[i].slot == pslot && ctx->vars[i].is_param) {
          return ir_var(pslot, var_irtype(ctx, pslot));
        }
      }
    }
    /* Field access through the receiver in slot 0. */
    if (!ctx->is_method || ctx->cls < 0) {
      irb_bug("self field outside a method in IR builder");
    }
    const char *clsname = sem_prog()->classes[ctx->cls].name;
    int t = irb_temp(ctx, rt);
    IrInstr ins = ir_instr(IR_FIELD_LOAD);
    ins.dst = t;
    ins.type = rt;
    ins.v[0] = ir_var(0, IR_CLASS);
    ins.nv = 1;
    ins.aux = field_index(clsname, member);
    irb_emit(ctx, ins);
    return ir_temp(t, rt);
  }
  int slot = irb_scope_find(ctx, object);
  if (slot < 0) {
    irb_bug("unresolvable member base in IR builder");
  }
  int t = irb_temp(ctx, rt);
  IrInstr ins = ir_instr(IR_FIELD_LOAD);
  ins.dst = t;
  ins.type = rt;
  ins.v[0] = ir_var(slot, var_irtype(ctx, slot));
  ins.nv = 1;
  ins.aux = field_index(var_kind(ctx, slot), member);
  irb_emit(ctx, ins);
  return ir_temp(t, rt);
}

/**
 * @brief Lowers indexing to a value
 * @param ctx Builder context
 * @param node Index node
 * @return Element temp
 */
static IrVal index_val(BuildCtx *ctx, Node *node) {
  IrVal base = irb_expr(ctx, node->index.base);
  IrVal idx = irb_expr(ctx, node->index.index);
  idx = irb_index(ctx, idx);
  SemType bs = sem_expr_type(node->index.base);
  SemType rs = sem_expr_type(node);
  if (bs == ST_ARR) {
    int t = irb_temp(ctx, irb_type(rs));
    IrInstr ins = ir_instr(IR_ARR_LOAD);
    ins.dst = t;
    ins.type = irb_type(rs);
    ins.v[0] = base;
    ins.v[1] = idx;
    ins.nv = 2;
    irb_emit(ctx, ins);
    return ir_temp(t, irb_type(rs));
  }
  int t = irb_temp(ctx, IR_INT);
  IrInstr ins = ir_instr(IR_STR_IDX);
  ins.dst = t;
  ins.type = IR_INT;
  ins.v[0] = base;
  ins.v[1] = idx;
  ins.nv = 2;
  irb_emit(ctx, ins);
  return ir_temp(t, IR_INT);
}

/**
 * @brief Byte size of an instance (every field is one 8-byte slot)
 * @param kind Struct or class name
 * @return Byte size, at least 8
 */
static long long instance_size(const char *kind) {
  SemProg *sp = sem_prog();
  for (int i = 0; i < sp->nstructs; i++) {
    if (strcmp(sp->structs[i].name, kind) == 0) {
      int n = sp->structs[i].nfields;
      return n == 0 ? 8 : (long long)n * 8;
    }
  }
  for (int i = 0; i < sp->nclasses; i++) {
    if (strcmp(sp->classes[i].name, kind) == 0) {
      int n = sp->classes[i].nfields;
      return n == 0 ? 8 : (long long)n * 8;
    }
  }
  return 8;
}

/**
 * @brief Copies an instance-typed value into a fresh heap object
 * @param ctx Builder context
 * @param v Value holding the source pointer
 * @param kind Instance kind name
 * @return Fresh pointer temp
 */
static IrVal copy_instance(BuildCtx *ctx, IrVal v, const char *kind) {
  long long size = instance_size(kind);
  int is_cls = 0;
  int class_id = -1;
  SemProg *sp = sem_prog();
  for (int i = 0; i < sp->nclasses; i++) {
    if (strcmp(sp->classes[i].name, kind) == 0) {
      is_cls = 1;
      class_id = sp->classes[i].class_id;
      break;
    }
  }
  int t = irb_temp(ctx, is_cls ? IR_CLASS : IR_STRUCT);
  IrInstr ins = ir_instr(IR_COPYINST);
  ins.dst = t;
  ins.type = is_cls ? IR_CLASS : IR_STRUCT;
  ins.v[0] = v;
  ins.nv = 1;
  ins.aux = (int)(size + (is_cls ? 8 : 0));
  ins.aux2 = (int)size;
  ins.callee = class_id;
  ins.name = kind;
  irb_emit(ctx, ins);
  return ir_temp(t, ins.type);
}

/**
 * @brief Lowers 'new Type(args)' to a fresh instance pointer
 * @param ctx Builder context
 * @param node New-expression node
 * @return Instance temp
 */
static IrVal new_val(BuildCtx *ctx, Node *node) {
  const char *type = node->new_expr.type_name;
  SemProg *sp = sem_prog();
  int s = -1;
  int c = -1;
  for (int i = 0; i < sp->nstructs; i++) {
    if (strcmp(sp->structs[i].name, type) == 0) {
      s = i;
      break;
    }
  }
  for (int i = 0; i < sp->nclasses; i++) {
    if (strcmp(sp->classes[i].name, type) == 0) {
      c = i;
      break;
    }
  }
  if (s < 0 && c < 0) {
    irb_bug("unknown type in new-expression");
  }
  int nfields = (s >= 0) ? sp->structs[s].nfields : sp->classes[c].nfields;
  IrVal *vals = NULL;
  int nvals = 0;
  int i = 0;
  for (Node *a = node->new_expr.args; a != NULL; a = a->right, i++) {
    IrVal v = irb_expr(ctx, a);
    SemType fs = (s >= 0) ? sp->structs[s].fields[i].type
                          : sp->classes[c].fields[i].type;
    IrType want = fs == ST_FLOAT ? IR_FLOAT
                  : fs == ST_BOOL ? IR_BOOL
                  : fs == ST_STR  ? IR_STR
                                  : IR_INT;
    if (want == IR_FLOAT || want == IR_BOOL || want == IR_INT) {
      IrType given = ir_val_type(ctx->f, v);
      if (fs == ST_FLOAT && (given == IR_INT || given == IR_BOOL)) {
        v = irb_convert(ctx, v, IR_FLOAT);
      } else if (fs == ST_BOOL && given == IR_FLOAT) {
        v = irb_convert(ctx, v, IR_INT);
      }
    }
    SemType vs = sem_expr_type(a);
    if ((vs == ST_STRUCT || vs == ST_CLASS) &&
        (a->type == NODE_IDENTIFIER || a->type == NODE_INDEX)) {
      const char *ak = sem_expr_kind(a);
      v = copy_instance(ctx, v, ak != NULL ? ak : type);
    }
    vals = realloc(vals, (size_t)(nvals + 1) * sizeof(IrVal));
    vals[nvals++] = v;
  }
  long long size = nfields == 0 ? 8 : (long long)nfields * 8;
  int is_cls = (c >= 0);
  int t = irb_temp(ctx, is_cls ? IR_CLASS : IR_STRUCT);
  IrInstr ins = ir_instr(IR_NEWINST);
  ins.dst = t;
  ins.type = is_cls ? IR_CLASS : IR_STRUCT;
  ins.list = vals;
  ins.nlist = nvals;
  ins.aux = (int)(size + (is_cls ? 8 : 0));
  ins.aux2 = is_cls ? sp->classes[c].class_id + 1 : 0;
  ins.name = type;
  irb_emit(ctx, ins);
  return ir_temp(t, ins.type);
}

/**
 * @brief Lowers a method call through the vtable
 * @param ctx Builder context
 * @param node Method call node
 * @return Result temp
 */
static IrVal method_val(BuildCtx *ctx, Node *node) {
  const char *object = node->method_call.object;
  IrVal obj;
  int cls = -1;
  if (node->method_call.object_expr != NULL) {
    obj = irb_expr(ctx, node->method_call.object_expr);
    const char *kind = sem_expr_kind(node->method_call.object_expr);
    for (int i = 0; i < sem_prog()->nclasses; i++) {
      if (strcmp(sem_prog()->classes[i].name, kind) == 0) {
        cls = i;
        break;
      }
    }
  } else if (strcmp(object, "self") == 0) {
    obj = ir_var(0, IR_CLASS);
    cls = ctx->cls;
  } else {
    int slot = irb_scope_find(ctx, object);
    if (slot < 0) {
      irb_bug("unresolvable method receiver in IR builder");
    }
    obj = ir_var(slot, var_irtype(ctx, slot));
    const char *kind = var_kind(ctx, slot);
    for (int i = 0; i < sem_prog()->nclasses; i++) {
      if (kind != NULL && strcmp(sem_prog()->classes[i].name, kind) == 0) {
        cls = i;
        break;
      }
    }
  }
  if (cls < 0) {
    irb_bug("unresolvable method class in IR builder");
  }
  SemProg *sp = sem_prog();
  int owner = cls;
  int m = -1;
  for (int c = cls; c >= 0; c = sp->classes[c].base_idx) {
    for (int i = 0; i < sp->classes[c].nmethods; i++) {
      if (strcmp(sp->classes[c].methods[i].name, node->method_call.method) ==
          0) {
        owner = c;
        m = i;
        goto found_method;
      }
    }
  }
found_method:
  if (m < 0) {
    irb_bug("unresolvable method in IR builder");
  }
  SemFunc *md = &sp->classes[owner].methods[m];
  IrVal *args = NULL;
  int nargs = 0;
  int idx = 0;
  for (Node *a = node->method_call.args; a != NULL; a = a->right, idx++) {
    IrVal v = irb_expr(ctx, a);
    IrType want = IR_INT;
    if (idx < md->nparams) {
      want = irb_type(md->ptypes[idx]);
    }
    v = convert_arg(ctx, v, want);
    args = realloc(args, (size_t)(nargs + 1) * sizeof(IrVal));
    args[nargs++] = v;
  }
  char label[128];
  snprintf(label, sizeof(label), "%s__%s", sp->classes[owner].name,
           node->method_call.method);
  IrInstr ins = ir_instr(IR_CALLM);
  ins.callee = sem_method_slot(node->method_call.method);
  ins.v[0] = obj;
  ins.nv = 1;
  ins.list = args;
  ins.nlist = nargs;
  ins.name = NULL;
  {
    size_t ll = strlen(label);
    char *lab = malloc(ll + 1);
    memcpy(lab, label, ll + 1);
    ins.name = lab;
  }
  SemType rs = sem_expr_type(node);
  int t = irb_temp(ctx, irb_type(rs));
  ins.dst = t;
  irb_emit(ctx, ins);
  return ir_temp(t, irb_type(rs));
}

IrVal irb_expr(BuildCtx *ctx, Node *node) {
  if (node == NULL) {
    irb_bug("NULL expression in IR builder");
  }
  switch (node->type) {
  case NODE_INT_LITERAL:
    return ir_imm_i(node->int_literal.value, IR_INT);
  case NODE_FLOAT_LITERAL:
    return ir_imm_f(node->float_literal.value);
  case NODE_STRING_LITERAL: {
    int sidx = ir_add_string(ctx->m, node->string_literal.value);
    return ir_str(sidx);
  }
  case NODE_NULL:
    return ir_null();
  case NODE_IDENTIFIER: {
    int slot = irb_scope_find(ctx, node->identifier.name);
    if (slot >= 0) {
      return ir_var(slot, var_irtype(ctx, slot));
    }
    int g = irb_global_at(ctx, node->identifier.name);
    if (g >= 0) {
      SemType gt = sem_prog()->globals[g].type;
      return ir_global(g, irb_type(gt));
    }
    irb_bug("unresolvable identifier in IR builder");
    return ir_null();
  }
  case NODE_FUNC_CALL: {
    const char *name = node->func_call.name;
    if (strcmp(name, "input") == 0 || strcmp(name, "len") == 0 ||
        strcmp(name, "tostr") == 0 || strcmp(name, "tonum") == 0 ||
        strcmp(name, "readFile") == 0 || strcmp(name, "writeFile") == 0) {
      return builtin_call(ctx, node);
    }
    int fi = sem_func_at(name);
    if (fi < 0) {
      irb_bug("unresolvable call in IR builder");
    }
    return irb_call(ctx, node, fi);
  }
  case NODE_BINARY_OP:
    return binary_val(ctx, node);
  case NODE_MEMBER_ACCESS:
    return member_val(ctx, node);
  case NODE_NEW:
    return new_val(ctx, node);
  case NODE_INDEX:
    return index_val(ctx, node);
  case NODE_METHOD_CALL:
    return method_val(ctx, node);
  default:
    irb_bug("unsupported expression in IR builder");
    return ir_null();
  }
}
