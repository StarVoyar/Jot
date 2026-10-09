#include "ir_build.h"

/**
 * @brief Byte size of an instance (every field is one 8-byte slot)
 * @param kind Struct or class name
 * @return Byte size, at least 8
 */
static long long build_instance_size(const char *kind) {
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
static IrVal build_copy(BuildCtx *ctx, IrVal v, const char *kind) {
  long long size = build_instance_size(kind);
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
 * @brief Finds a field index on a struct or class kind
 * @param kind Struct or class name
 * @param member Field name
 * @return Field index
 */
static int build_field_index(const char *kind, const char *member) {
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
 * @brief Returns the instance kind of the next field in a chain
 * @param kind Current struct or class name
 * @param member Field name
 * @return Next kind, or NULL
 */
static const char *build_field_kind(const char *kind, const char *member) {
  SemProg *sp = sem_prog();
  for (int i = 0; i < sp->nstructs; i++) {
    if (strcmp(sp->structs[i].name, kind) == 0) {
      for (int f = 0; f < sp->structs[i].nfields; f++) {
        if (strcmp(sp->structs[i].fields[f].name, member) == 0) {
          return sp->structs[i].fields[f].kind;
        }
      }
    }
  }
  for (int i = 0; i < sp->nclasses; i++) {
    if (strcmp(sp->classes[i].name, kind) == 0) {
      for (int f = 0; f < sp->classes[i].nfields; f++) {
        if (strcmp(sp->classes[i].fields[f].name, member) == 0) {
          return sp->classes[i].fields[f].kind;
        }
      }
    }
  }
  return NULL;
}

/**
 * @brief Returns the frame slot type from the context variables
 * @param ctx Builder context
 * @param slot Frame slot
 * @return Slot category
 */
static IrType build_slot_type(BuildCtx *ctx, int slot) {
  for (int i = 0; i < ctx->nvars; i++) {
    if (ctx->vars[i].slot == slot) {
      SemType st = ctx->vars[i].type;
      return st == ST_FLOAT ? IR_FLOAT
             : st == ST_BOOL ? IR_BOOL
             : st == ST_STR  ? IR_STR
             : st == ST_ARR  ? IR_ARR
                             : IR_INT;
    }
  }
  return ir_slot_type(ctx->f, slot);
}

/**
 * @brief Makes sure emission continues in a live block
 * @param ctx Builder context
 */
static void ensure_live(BuildCtx *ctx) {
  if (ctx->terminated) {
    irb_block(ctx);
    ctx->terminated = 0;
  }
}

/**
 * @brief Lowers a condition to an int test temp
 * @param ctx Builder context
 * @param cond Condition node
 * @return Int temp holding 0 or 1
 */
static IrVal cond_val(BuildCtx *ctx, Node *cond) {
  IrVal v = irb_expr(ctx, cond);
  if (ir_val_type(ctx->f, v) == IR_FLOAT) {
    int t = irb_temp(ctx, IR_BOOL);
    IrInstr ins = ir_instr(IR_CMP);
    ins.dst = t;
    ins.type = IR_FLOAT;
    ins.cond = IR_CNE;
    ins.v[0] = v;
    ins.v[1] = ir_imm_f(0.0);
    ins.nv = 2;
    irb_emit(ctx, ins);
    return ir_temp(t, IR_BOOL);
  }
  return v;
}

/**
 * @brief Emits a store of a value into a frame slot with conversion
 * @param ctx Builder context
 * @param slot Destination slot
 * @param v Value to store
 */
static void store_slot(BuildCtx *ctx, int slot, IrVal v) {
  IrType want = build_slot_type(ctx, slot);
  if (want == IR_FLOAT || want == IR_BOOL || want == IR_INT) {
    IrType given = ir_val_type(ctx->f, v);
    if (want == IR_FLOAT && (given == IR_INT || given == IR_BOOL)) {
      v = irb_convert(ctx, v, IR_FLOAT);
    } else if (want == IR_INT && given == IR_FLOAT) {
      v = irb_convert(ctx, v, IR_INT);
    } else if (want == IR_BOOL && given == IR_FLOAT) {
      v = irb_convert(ctx, v, IR_INT);
    }
  }
  IrInstr ins = ir_instr(IR_STORE);
  ins.v[0] = ir_var(slot, want);
  ins.v[1] = v;
  ins.nv = 2;
  irb_emit(ctx, ins);
}

/**
 * @brief Copies an instance value when sources bind by copy
 * @param ctx Builder context
 * @param v Value to maybe copy
 * @param src Source node (copy applies to identifiers and index reads)
 * @return Value holding the (possibly fresh) pointer
 */
static IrVal maybe_copy(BuildCtx *ctx, IrVal v, Node *src) {
  IrType t = ir_val_type(ctx->f, v);
  if ((t == IR_STRUCT || t == IR_CLASS) &&
      (src->type == NODE_IDENTIFIER || src->type == NODE_INDEX)) {
    const char *kind = sem_expr_kind(src);
    if (kind != NULL) {
      return build_copy(ctx, v, kind);
    }
  }
  return v;
}

/**
 * @brief Lowers a variable declaration
 * @param ctx Builder context
 * @param node VAR_DECL node
 */
static void gen_var_decl(BuildCtx *ctx, Node *node) {
  if (node->var_decl.is_global) {
    int g = irb_global_at(ctx, node->var_decl.name);
    if (g < 0 || node->var_decl.value == NULL) {
      return;
    }
    IrVal v = irb_expr(ctx, node->var_decl.value);
    IrType want = irb_type(sem_prog()->globals[g].type);
    if (want == IR_FLOAT || want == IR_BOOL || want == IR_INT) {
      IrType given = ir_val_type(ctx->f, v);
      if (want == IR_FLOAT && (given == IR_INT || given == IR_BOOL)) {
        v = irb_convert(ctx, v, IR_FLOAT);
      } else if (want == IR_INT && given == IR_FLOAT) {
        v = irb_convert(ctx, v, IR_INT);
      } else if (want == IR_BOOL && given == IR_FLOAT) {
        v = irb_convert(ctx, v, IR_INT);
      }
    }
    IrInstr ins = ir_instr(IR_STOREG);
    ins.v[0] = ir_global(g, want);
    ins.v[1] = v;
    ins.nv = 2;
    irb_emit(ctx, ins);
    return;
  }
  int slot = sem_decl_slot(node);
  irb_scope_declare(ctx, node->var_decl.name, slot);
  if (node->var_decl.value == NULL) {
    return;
  }
  IrVal v = irb_expr(ctx, node->var_decl.value);
  v = maybe_copy(ctx, v, node->var_decl.value);
  store_slot(ctx, slot, v);
}

/**
 * @brief Lowers an assignment
 * @param ctx Builder context
 * @param node ASSIGNMENT node
 */
static void gen_assignment(BuildCtx *ctx, Node *node) {
  IrVal v = irb_expr(ctx, node->assignment.value);
  v = maybe_copy(ctx, v, node->assignment.value);
  int slot = irb_scope_find(ctx, node->assignment.name);
  if (slot >= 0) {
    store_slot(ctx, slot, v);
    return;
  }
  int g = irb_global_at(ctx, node->assignment.name);
  IrType want = irb_type(sem_prog()->globals[g].type);
  IrType given = ir_val_type(ctx->f, v);
  if (want == IR_FLOAT && (given == IR_INT || given == IR_BOOL)) {
    v = irb_convert(ctx, v, IR_FLOAT);
  } else if (want == IR_INT && given == IR_FLOAT) {
    v = irb_convert(ctx, v, IR_INT);
  } else if (want == IR_BOOL && given == IR_FLOAT) {
    v = irb_convert(ctx, v, IR_INT);
  }
  IrInstr ins = ir_instr(IR_STOREG);
  ins.v[0] = ir_global(g, want);
  ins.v[1] = v;
  ins.nv = 2;
  irb_emit(ctx, ins);
}

/**
 * @brief Lowers += or -=
 * @param ctx Builder context
 * @param node ADD_ASSIGN or SUB_ASSIGN node
 * @param is_add Non-zero for +=
 */
static void gen_add_sub(BuildCtx *ctx, Node *node, int is_add) {
  const char *name = is_add ? node->add_assign.name : node->sub_assign.name;
  Node *value = is_add ? node->add_assign.value : node->sub_assign.value;
  IrVal rhs = irb_expr(ctx, value);
  int slot = irb_scope_find(ctx, name);
  int is_global = (slot < 0);
  int g = is_global ? irb_global_at(ctx, name) : -1;
  IrType st = is_global ? irb_type(sem_prog()->globals[g].type)
                        : build_slot_type(ctx, slot);
  if (st == IR_STR) {
    IrVal lhs;
    if (is_global) {
      int t = irb_temp(ctx, IR_STR);
      IrInstr ld = ir_instr(IR_LOADG);
      ld.dst = t;
      ld.v[0] = ir_global(g, IR_STR);
      ld.nv = 1;
      irb_emit(ctx, ld);
      lhs = ir_temp(t, IR_STR);
    } else {
      lhs = ir_var(slot, IR_STR);
    }
    int t = irb_temp(ctx, IR_STR);
    IrInstr ins = ir_instr(IR_CONCAT);
    ins.dst = t;
    ins.v[0] = lhs;
    ins.v[1] = rhs;
    ins.nv = 2;
    irb_emit(ctx, ins);
    IrVal res = ir_temp(t, IR_STR);
    if (is_global) {
      IrInstr st2 = ir_instr(IR_STOREG);
      st2.v[0] = ir_global(g, IR_STR);
      st2.v[1] = res;
      st2.nv = 2;
      irb_emit(ctx, st2);
    } else {
      store_slot(ctx, slot, res);
    }
    return;
  }
  IrType rt = ir_val_type(ctx->f, rhs);
  IrType common = (st == IR_FLOAT || rt == IR_FLOAT) ? IR_FLOAT : IR_INT;
  if (st == IR_BOOL && rt == IR_FLOAT) {
    /* bool stays int: truncate the float operand. */
    rhs = irb_convert(ctx, rhs, IR_INT);
    common = IR_INT;
  } else if (common == IR_FLOAT) {
    rhs = irb_convert(ctx, rhs, IR_FLOAT);
  }
  IrVal lhs;
  if (is_global) {
    int t = irb_temp(ctx, st);
    IrInstr ld = ir_instr(IR_LOADG);
    ld.dst = t;
    ld.v[0] = ir_global(g, st);
    ld.nv = 1;
    irb_emit(ctx, ld);
    lhs = ir_temp(t, st);
  } else {
    lhs = ir_var(slot, st);
  }
  if (common == IR_FLOAT && st != IR_FLOAT) {
    lhs = irb_convert(ctx, lhs, IR_FLOAT);
  }
  int t = irb_temp(ctx, common);
  IrInstr ins = ir_instr(is_add ? IR_ADD : IR_SUB);
  ins.dst = t;
  ins.type = common;
  ins.v[0] = lhs;
  ins.v[1] = rhs;
  ins.nv = 2;
  irb_emit(ctx, ins);
  IrVal res = ir_temp(t, common);
  if (is_global) {
    IrInstr st2 = ir_instr(IR_STOREG);
    st2.v[0] = ir_global(g, st);
    st2.v[1] = res;
    st2.nv = 2;
    irb_emit(ctx, st2);
  } else {
    IrInstr st2 = ir_instr(IR_STORE);
    st2.v[0] = ir_var(slot, st);
    st2.v[1] = res;
    st2.nv = 2;
    irb_emit(ctx, st2);
  }
}

/**
 * @brief Lowers array element assignment
 * @param ctx Builder context
 * @param node INDEX_ASSIGN node
 */
static void gen_index_assign(BuildCtx *ctx, Node *node) {
  Node *base = node->index_assign.base;
  Node *index = node->index_assign.index;
  Node *value = node->index_assign.value;
  const char *op = node->index_assign.op;
  int slot = irb_scope_find(ctx, base->identifier.name);
  IrVal arr = ir_var(slot, IR_ARR);
  IrVal idx = irb_expr(ctx, index);
  idx = irb_index(ctx, idx);
  IrVal v = irb_expr(ctx, value);
  SemType elem = ST_INT;
  for (int i = 0; i < ctx->nvars; i++) {
    if (ctx->vars[i].slot == slot) {
      elem = ctx->vars[i].elem;
      break;
    }
  }
  if (elem == ST_FLOAT) {
    v = irb_convert(ctx, v, IR_FLOAT);
  } else if (elem == ST_INT && ir_val_type(ctx->f, v) == IR_BOOL) {
    v = irb_trunc8(ctx, v);
  } else if ((elem == ST_STRUCT || elem == ST_CLASS) &&
             (value->type == NODE_IDENTIFIER ||
              value->type == NODE_INDEX)) {
    const char *kind = sem_expr_kind(value);
    if (kind != NULL) {
      v = build_copy(ctx, v, kind);
    }
  }
  IrInstr ins = ir_instr(IR_ARR_STORE);
  ins.v[0] = arr;
  ins.v[1] = idx;
  ins.v[2] = v;
  ins.nv = 3;
  ins.aux = (strcmp(op, "+=") == 0) ? 1 : (strcmp(op, "-=") == 0 ? 2 : 0);
  ins.aux2 = (int)irb_type(elem);
  irb_emit(ctx, ins);
}

/**
 * @brief Lowers member assignment
 * @param ctx Builder context
 * @param node MEMBER_ASSIGN node
 */
static void gen_member_assign(BuildCtx *ctx, Node *node) {
  const char *member = node->member_assign.member;
  const char *op = node->member_assign.op;
  Node *value = node->member_assign.value;
  int pslot = -1;
  if (node->member_assign.object_expr == NULL &&
      strcmp(node->member_assign.object, "self") == 0) {
    pslot = irb_scope_find(ctx, member);
    int is_param = 0;
    for (int i = 0; i < ctx->nvars; i++) {
      if (ctx->vars[i].slot == pslot && ctx->vars[i].is_param) {
        is_param = 1;
        break;
      }
    }
    if (is_param && pslot >= 0) {
      IrVal v = irb_expr(ctx, value);
      store_slot(ctx, pslot, v);
      return;
    }
  }
  IrVal v = irb_expr(ctx, value);
  v = maybe_copy(ctx, v, value);
  /* Resolve the target object and field. */
  IrVal obj;
  const char *kind = NULL;
  if (node->member_assign.object_expr != NULL) {
    obj = irb_expr(ctx, node->member_assign.object_expr);
    kind = sem_expr_kind(node->member_assign.object_expr);
  } else if (strcmp(node->member_assign.object, "self") == 0) {
    obj = ir_var(0, IR_CLASS);
    kind = sem_prog()->classes[ctx->cls].name;
  } else {
    int slot = irb_scope_find(ctx, node->member_assign.object);
    obj = ir_var(slot, IR_CLASS);
    for (int i = 0; i < ctx->nvars; i++) {
      if (ctx->vars[i].slot == slot) {
        kind = ctx->vars[i].kind;
        break;
      }
    }
  }
  int fidx = build_field_index(kind, member);
  SemType fs = ST_INT;
  {
    SemProg *sp = sem_prog();
    for (int i = 0; i < sp->nstructs; i++) {
      if (strcmp(sp->structs[i].name, kind) == 0) {
        fs = sp->structs[i].fields[fidx].type;
      }
    }
    for (int i = 0; i < sp->nclasses; i++) {
      if (strcmp(sp->classes[i].name, kind) == 0) {
        fs = sp->classes[i].fields[fidx].type;
      }
    }
  }
  IrType want = irb_type(fs);
  if (want == IR_FLOAT || want == IR_BOOL || want == IR_INT) {
    IrType given = ir_val_type(ctx->f, v);
    if (want == IR_FLOAT && (given == IR_INT || given == IR_BOOL)) {
      v = irb_convert(ctx, v, IR_FLOAT);
    } else if (want == IR_INT && given == IR_FLOAT) {
      v = irb_convert(ctx, v, IR_INT);
    } else if (want == IR_BOOL && given == IR_FLOAT) {
      v = irb_convert(ctx, v, IR_INT);
    }
  }
  if (strcmp(op, "=") == 0) {
    IrInstr ins = ir_instr(IR_FIELD_STORE);
    ins.type = want;
    ins.v[0] = obj;
    ins.v[1] = v;
    ins.nv = 2;
    ins.aux = fidx;
    irb_emit(ctx, ins);
    return;
  }
  if (want == IR_STR) {
    int ft = irb_temp(ctx, IR_STR);
    IrInstr ld = ir_instr(IR_FIELD_LOAD);
    ld.dst = ft;
    ld.type = IR_STR;
    ld.v[0] = obj;
    ld.nv = 1;
    ld.aux = fidx;
    irb_emit(ctx, ld);
    int t = irb_temp(ctx, IR_STR);
    IrInstr cc = ir_instr(IR_CONCAT);
    cc.dst = t;
    cc.v[0] = ir_temp(ft, IR_STR);
    cc.v[1] = v;
    cc.nv = 2;
    irb_emit(ctx, cc);
    IrInstr st = ir_instr(IR_FIELD_STORE);
    st.type = IR_STR;
    st.v[0] = obj;
    st.v[1] = ir_temp(t, IR_STR);
    st.nv = 2;
    st.aux = fidx;
    irb_emit(ctx, st);
    return;
  }
  /* Read-modify-write through a temp holding the old field. */
  int ft = irb_temp(ctx, want);
  IrInstr ld = ir_instr(IR_FIELD_LOAD);
  ld.dst = ft;
  ld.type = want;
  ld.v[0] = obj;
  ld.nv = 1;
  ld.aux = fidx;
  irb_emit(ctx, ld);
  IrVal old = ir_temp(ft, want);
  IrType common = want;
  IrVal rhs = v;
  if (want == IR_FLOAT) {
    rhs = irb_convert(ctx, rhs, IR_FLOAT);
    common = IR_FLOAT;
  } else if (want == IR_BOOL && ir_val_type(ctx->f, rhs) == IR_FLOAT) {
    rhs = irb_convert(ctx, rhs, IR_INT);
    common = IR_INT;
  }
  int t = irb_temp(ctx, common);
  IrInstr ins = ir_instr(strcmp(op, "+=") == 0 ? IR_ADD : IR_SUB);
  ins.dst = t;
  ins.type = common;
  ins.v[0] = old;
  ins.v[1] = rhs;
  ins.nv = 2;
  irb_emit(ctx, ins);
  IrInstr st = ir_instr(IR_FIELD_STORE);
  st.type = want;
  st.v[0] = obj;
  st.v[1] = ir_temp(t, common);
  st.nv = 2;
  st.aux = fidx;
  irb_emit(ctx, st);
}

/**
 * @brief Evaluates one print placeholder path to a value
 * @param ctx Builder context
 * @param part Placeholder part with segments
 * @return Value holding the placeholder
 */
static IrVal placeholder_val(BuildCtx *ctx, SemPrintPart *part) {
  IrVal v;
  const char *kind = NULL;
  if (part->base == 1) {
    int slot = irb_scope_find(ctx, part->segs[1]);
    v = ir_var(slot, irb_type(part->type));
    for (int i = 0; i < ctx->nvars; i++) {
      if (ctx->vars[i].slot == slot) {
        kind = ctx->vars[i].kind;
        break;
      }
    }
    int start = 2;
    for (int i = start; i < part->nsegs; i++) {
      int t = irb_temp(ctx, IR_INT);
      IrInstr ins = ir_instr(IR_FIELD_LOAD);
      ins.dst = t;
      ins.type = IR_INT;
      ins.v[0] = v;
      ins.nv = 1;
      ins.aux = build_field_index(kind, part->segs[i]);
      irb_emit(ctx, ins);
      v = ir_temp(t, IR_INT);
      kind = build_field_kind(kind, part->segs[i]);
    }
    return v;
  }
  if (part->base == 2) {
    v = ir_var(0, IR_CLASS);
    kind = sem_prog()->classes[ctx->cls].name;
    int t = irb_temp(ctx, IR_INT);
    IrInstr ins = ir_instr(IR_FIELD_LOAD);
    ins.dst = t;
    ins.type = IR_INT;
    ins.v[0] = v;
    ins.nv = 1;
    ins.aux = build_field_index(kind, part->segs[1]);
    irb_emit(ctx, ins);
    v = ir_temp(t, IR_INT);
    kind = build_field_kind(kind, part->segs[1]);
    for (int i = 2; i < part->nsegs; i++) {
      t = irb_temp(ctx, IR_INT);
      ins = ir_instr(IR_FIELD_LOAD);
      ins.dst = t;
      ins.type = IR_INT;
      ins.v[0] = v;
      ins.nv = 1;
      ins.aux = build_field_index(kind, part->segs[i]);
      irb_emit(ctx, ins);
      v = ir_temp(t, IR_INT);
      kind = build_field_kind(kind, part->segs[i]);
    }
    return v;
  }
  int slot = irb_scope_find(ctx, part->segs[0]);
  if (slot >= 0) {
    v = ir_var(slot, irb_type(part->type));
    for (int i = 0; i < ctx->nvars; i++) {
      if (ctx->vars[i].slot == slot) {
        kind = ctx->vars[i].kind;
        break;
      }
    }
  } else {
    int g = irb_global_at(ctx, part->segs[0]);
    v = ir_global(g, irb_type(part->type));
    kind = sem_prog()->globals[g].kind;
  }
  for (int i = 1; i < part->nsegs; i++) {
    int t = irb_temp(ctx, IR_INT);
    IrInstr ins = ir_instr(IR_FIELD_LOAD);
    ins.dst = t;
    ins.type = IR_INT;
    ins.v[0] = v;
    ins.nv = 1;
    ins.aux = build_field_index(kind, part->segs[i]);
    irb_emit(ctx, ins);
    v = ir_temp(t, IR_INT);
    kind = build_field_kind(kind, part->segs[i]);
  }
  return v;
}

/**
 * @brief Lowers a print statement
 * @param ctx Builder context
 * @param node PRINT node
 */
static void gen_print(BuildCtx *ctx, Node *node) {
  Node *value = node->print_stmt.value;
  if (value != NULL && value->type == NODE_STRING_LITERAL) {
    SemPrintInfo *info = sem_print_info(node);
    if (info == NULL) {
      irb_bug("missing print info in IR builder");
    }
    for (int i = 0; i < info->nparts; i++) {
      SemPrintPart *p = &info->parts[i];
      if (p->is_text) {
        int sidx = ir_add_string(ctx->m, p->text);
        IrInstr ins = ir_instr(IR_PRINT_S);
        ins.aux = sidx;
        irb_emit(ctx, ins);
      } else {
        IrVal v = placeholder_val(ctx, p);
        IrInstr ins = ir_instr(IR_PRINT_V);
        ins.type = irb_type(p->type);
        ins.v[0] = v;
        ins.nv = 1;
        ins.aux = 1;
        irb_emit(ctx, ins);
      }
    }
    return;
  }
  IrVal v = irb_expr(ctx, value);
  IrInstr ins = ir_instr(IR_PRINT_V);
  ins.type = ir_val_type(ctx->f, v);
  ins.v[0] = v;
  ins.nv = 1;
  irb_emit(ctx, ins);
}

/**
 * @brief Lowers a return statement
 * @param ctx Builder context
 * @param node RETURN node
 */
static void gen_return(BuildCtx *ctx, Node *node) {
  if (node->return_stmt.value == NULL) {
    IrInstr ins = ir_instr(IR_RET);
    ins.nv = 0;
    irb_emit(ctx, ins);
    ctx->terminated = 1;
    return;
  }
  IrVal v = irb_expr(ctx, node->return_stmt.value);
  IrType given = ir_val_type(ctx->f, v);
  if (ctx->sfunc == NULL) {
    IrInstr ins = ir_instr(IR_EXIT);
    ins.v[0] = v;
    ins.nv = 1;
    irb_emit(ctx, ins);
    ctx->terminated = 1;
    return;
  }
  IrType want = given;
  if (!ctx->is_method) {
    /* Plain functions normalize int results when the signature floats. */
    if (ctx->ret_float && (given == IR_INT || given == IR_BOOL)) {
      want = IR_FLOAT;
    }
  } else if (strcmp(ctx->ret_kind, "num") == 0 &&
             (given == IR_INT || given == IR_BOOL)) {
    want = IR_FLOAT;
  } else if (strcmp(ctx->ret_kind, "bool") == 0 && given == IR_FLOAT) {
    want = IR_INT;
  }
  if (want != given) {
    v = irb_convert(ctx, v, want);
  }
  IrInstr ins = ir_instr(IR_RET);
  ins.type = want;
  ins.v[0] = v;
  ins.nv = 1;
  irb_emit(ctx, ins);
  ctx->terminated = 1;
}

static void gen_stmt(BuildCtx *ctx, Node *node);

/**
 * @brief Lowers a block of statements with scope save/restore
 * @param ctx Builder context
 * @param list First statement (linked via right)
 */
static void gen_block(BuildCtx *ctx, Node *list) {
  int saved = ctx->nscope;
  for (Node *s = list; s != NULL; s = s->right) {
    gen_stmt(ctx, s);
  }
  ctx->nscope = saved;
}

/**
 * @brief Lowers if/else with explicit blocks
 * @param ctx Builder context
 * @param node IF node
 */
static void gen_if(BuildCtx *ctx, Node *node) {
  IrVal c = cond_val(ctx, node->if_stmt.condition);
  int then_id = irb_fresh(ctx);
  int else_id = irb_fresh(ctx);
  int end_id = irb_fresh(ctx);
  IrInstr br = ir_instr(IR_BR);
  br.v[0] = c;
  br.nv = 1;
  br.t = then_id;
  br.f = else_id;
  irb_emit(ctx, br);
  ctx->terminated = 1;
  irb_goto(ctx, then_id);
  gen_block(ctx, node->if_stmt.body);
  if (!ctx->terminated) {
    IrInstr j = ir_instr(IR_JUMP);
    j.t = end_id;
    irb_emit(ctx, j);
  }
  irb_goto(ctx, else_id);
  if (node->if_stmt.else_body != NULL) {
    if (node->if_stmt.else_body->type == NODE_IF) {
      gen_stmt(ctx, node->if_stmt.else_body);
    } else {
      gen_block(ctx, node->if_stmt.else_body);
    }
  }
  if (!ctx->terminated) {
    IrInstr j = ir_instr(IR_JUMP);
    j.t = end_id;
    irb_emit(ctx, j);
  }
  irb_goto(ctx, end_id);
}

/**
 * @brief Lowers a while loop (continue re-runs the condition)
 * @param ctx Builder context
 * @param node WHILE node
 */
static void gen_while(BuildCtx *ctx, Node *node) {
  int cond_id = irb_fresh(ctx);
  int body_id = irb_fresh(ctx);
  int end_id = irb_fresh(ctx);
  IrInstr j = ir_instr(IR_JUMP);
  j.t = cond_id;
  irb_emit(ctx, j);
  ctx->terminated = 1;
  irb_goto(ctx, cond_id);
  IrVal c = cond_val(ctx, node->while_stmt.condition);
  IrInstr br = ir_instr(IR_BR);
  br.v[0] = c;
  br.nv = 1;
  br.t = body_id;
  br.f = end_id;
  irb_emit(ctx, br);
  ctx->terminated = 1;
  ctx->break_blk[ctx->loop_depth] = end_id;
  ctx->cont_blk[ctx->loop_depth] = cond_id;
  ctx->loop_depth++;
  irb_goto(ctx, body_id);
  gen_block(ctx, node->while_stmt.body);
  if (!ctx->terminated) {
    IrInstr back = ir_instr(IR_JUMP);
    back.t = cond_id;
    irb_emit(ctx, back);
  }
  ctx->loop_depth--;
  irb_goto(ctx, end_id);
}

/**
 * @brief Lowers for-in to an indexed loop with copy-on-bind for instances
 * @param ctx Builder context
 * @param node FOR node
 */
static void gen_for(BuildCtx *ctx, Node *node) {
  int aslot = irb_scope_find(ctx, node->for_stmt.array_name);
  int saved = ctx->nscope;
  int var_slot = sem_decl_slot(node);
  irb_scope_declare(ctx, node->for_stmt.var_name, var_slot);
  SemType elem = ST_INT;
  for (int i = 0; i < ctx->nvars; i++) {
    if (ctx->vars[i].slot == aslot) {
      elem = ctx->vars[i].elem;
      break;
    }
  }
  IrVal arr = ir_var(aslot, IR_ARR);
  int len_t = irb_temp(ctx, IR_INT);
  IrInstr ln = ir_instr(IR_ARR_LEN);
  ln.dst = len_t;
  ln.type = IR_INT;
  ln.v[0] = arr;
  ln.nv = 1;
  irb_emit(ctx, ln);
  int idx_t = irb_temp(ctx, IR_INT);
  IrInstr z = ir_instr(IR_COPY);
  z.dst = idx_t;
  z.v[0] = ir_imm_i(0, IR_INT);
  z.nv = 1;
  irb_emit(ctx, z);
  int cond_id = irb_fresh(ctx);
  int body_id = irb_fresh(ctx);
  int incr_id = irb_fresh(ctx);
  int end_id = irb_fresh(ctx);
  IrInstr j = ir_instr(IR_JUMP);
  j.t = cond_id;
  irb_emit(ctx, j);
  ctx->terminated = 1;
  irb_goto(ctx, cond_id);
  ctx->terminated = 0;
  int c_t = irb_temp(ctx, IR_BOOL);
  IrInstr cp = ir_instr(IR_CMP);
  cp.dst = c_t;
  cp.type = IR_INT;
  cp.cond = IR_CLT;
  cp.v[0] = ir_temp(idx_t, IR_INT);
  cp.v[1] = ir_temp(len_t, IR_INT);
  cp.nv = 2;
  irb_emit(ctx, cp);
  IrInstr br = ir_instr(IR_BR);
  br.v[0] = ir_temp(c_t, IR_BOOL);
  br.nv = 1;
  br.t = body_id;
  br.f = end_id;
  irb_emit(ctx, br);
  ctx->terminated = 1;
  ctx->break_blk[ctx->loop_depth] = end_id;
  ctx->cont_blk[ctx->loop_depth] = incr_id;
  ctx->loop_depth++;
  irb_goto(ctx, body_id);
  ctx->terminated = 0;
  int e_t = irb_temp(ctx, irb_type(elem));
  IrInstr ld = ir_instr(IR_ARR_LOAD);
  ld.dst = e_t;
  ld.type = irb_type(elem);
  ld.v[0] = arr;
  ld.v[1] = ir_temp(idx_t, IR_INT);
  ld.nv = 2;
  /* For-loop indices stay in bounds by construction. */
  ld.aux = 1;
  irb_emit(ctx, ld);
  if (elem == ST_STRUCT || elem == ST_CLASS) {
    /* Null elements stay null, others bind by copy. */
    const char *kind = NULL;
    for (int i = 0; i < ctx->nvars; i++) {
      if (ctx->vars[i].slot == aslot) {
        kind = ctx->vars[i].elem_kind;
        break;
      }
    }
    int do_copy_id = irb_fresh(ctx);
    int do_store_id = irb_fresh(ctx);
    int join_id = irb_fresh(ctx);
    int nn = irb_temp(ctx, IR_BOOL);
    IrInstr ncmp = ir_instr(IR_CMP);
    ncmp.dst = nn;
    ncmp.type = IR_INT;
    ncmp.cond = IR_CEQ;
    ncmp.v[0] = ir_temp(e_t, irb_type(elem));
    ncmp.v[1] = ir_null();
    ncmp.nv = 2;
    irb_emit(ctx, ncmp);
    IrInstr nb = ir_instr(IR_BR);
    nb.v[0] = ir_temp(nn, IR_BOOL);
    nb.nv = 1;
    nb.t = do_store_id;
    nb.f = do_copy_id;
    irb_emit(ctx, nb);
    ctx->terminated = 1;
    irb_goto(ctx, do_copy_id);
    ctx->terminated = 0;
    IrVal cpv = build_copy(ctx, ir_temp(e_t, irb_type(elem)), kind);
    IrInstr cs = ir_instr(IR_STORE);
    cs.v[0] = ir_var(var_slot, irb_type(elem));
    cs.v[1] = cpv;
    cs.nv = 2;
    irb_emit(ctx, cs);
    IrInstr cj = ir_instr(IR_JUMP);
    cj.t = join_id;
    irb_emit(ctx, cj);
    ctx->terminated = 1;
    irb_goto(ctx, do_store_id);
    ctx->terminated = 0;
    IrInstr ds = ir_instr(IR_STORE);
    ds.v[0] = ir_var(var_slot, irb_type(elem));
    ds.v[1] = ir_temp(e_t, irb_type(elem));
    ds.nv = 2;
    irb_emit(ctx, ds);
    IrInstr cj2 = ir_instr(IR_JUMP);
    cj2.t = join_id;
    irb_emit(ctx, cj2);
    ctx->terminated = 1;
    irb_goto(ctx, join_id);
    ctx->terminated = 0;
  } else {
    IrInstr st = ir_instr(IR_STORE);
    st.v[0] = ir_var(var_slot, irb_type(elem));
    st.v[1] = ir_temp(e_t, irb_type(elem));
    st.nv = 2;
    irb_emit(ctx, st);
  }
  gen_block(ctx, node->for_stmt.body);
  if (!ctx->terminated) {
    IrInstr to_incr = ir_instr(IR_JUMP);
    to_incr.t = incr_id;
    irb_emit(ctx, to_incr);
  }
  irb_goto(ctx, incr_id);
  int n_t = irb_temp(ctx, IR_INT);
  IrInstr ad = ir_instr(IR_ADD);
  ad.dst = n_t;
  ad.type = IR_INT;
  ad.v[0] = ir_temp(idx_t, IR_INT);
  ad.v[1] = ir_imm_i(1, IR_INT);
  ad.nv = 2;
  irb_emit(ctx, ad);
  IrInstr cp2 = ir_instr(IR_COPY);
  cp2.dst = idx_t;
  cp2.v[0] = ir_temp(n_t, IR_INT);
  cp2.nv = 1;
  irb_emit(ctx, cp2);
  IrInstr back = ir_instr(IR_JUMP);
  back.t = cond_id;
  irb_emit(ctx, back);
  ctx->terminated = 1;
  ctx->loop_depth--;
  irb_goto(ctx, end_id);
  ctx->terminated = 0;
  ctx->nscope = saved;
}

/**
 * @brief Lowers an array declaration (alias or literal)
 * @param ctx Builder context
 * @param node ARRAY_DECL node
 */
static void gen_array_decl(BuildCtx *ctx, Node *node) {
  int slot = sem_decl_slot(node);
  irb_scope_declare(ctx, node->array_decl.name, slot);
  Node *rhs = node->array_decl.elements;
  if (rhs != NULL && rhs->type == NODE_IDENTIFIER) {
    int src = irb_scope_find(ctx, rhs->identifier.name);
    IrInstr ins = ir_instr(IR_COPY);
    int t = irb_temp(ctx, IR_ARR);
    ins.dst = t;
    ins.v[0] = ir_var(src, IR_ARR);
    ins.nv = 1;
    irb_emit(ctx, ins);
    IrInstr st = ir_instr(IR_STORE);
    st.v[0] = ir_var(slot, IR_ARR);
    st.v[1] = ir_temp(t, IR_ARR);
    st.nv = 2;
    irb_emit(ctx, st);
    return;
  }
  SemType elem = ST_INT;
  for (int i = 0; i < ctx->nvars; i++) {
    if (ctx->vars[i].slot == slot) {
      elem = ctx->vars[i].elem;
      break;
    }
  }
  IrVal *vals = NULL;
  int nvals = 0;
  for (Node *e = rhs->array_literal.elements; e != NULL; e = e->right) {
    IrVal v = irb_expr(ctx, e);
    IrType given = ir_val_type(ctx->f, v);
    if (elem == ST_FLOAT && (given == IR_INT || given == IR_BOOL)) {
      v = irb_convert(ctx, v, IR_FLOAT);
    } else if (elem == ST_INT && given == IR_BOOL) {
      v = irb_trunc8(ctx, v);
    } else if ((elem == ST_STRUCT || elem == ST_CLASS) &&
               (e->type == NODE_IDENTIFIER || e->type == NODE_INDEX)) {
      const char *kind = sem_expr_kind(e);
      if (kind != NULL) {
        v = build_copy(ctx, v, kind);
      }
    }
    vals = realloc(vals, (size_t)(nvals + 1) * sizeof(IrVal));
    vals[nvals++] = v;
  }
  int t = irb_temp(ctx, IR_ARR);
  IrInstr ins = ir_instr(IR_NEWARR);
  ins.dst = t;
  ins.type = IR_ARR;
  ins.list = vals;
  ins.nlist = nvals;
  ins.aux = nvals;
  ins.aux2 = (int)irb_type(elem);
  irb_emit(ctx, ins);
  IrInstr st = ir_instr(IR_STORE);
  st.v[0] = ir_var(slot, IR_ARR);
  st.v[1] = ir_temp(t, IR_ARR);
  st.nv = 2;
  irb_emit(ctx, st);
}

static void gen_stmt(BuildCtx *ctx, Node *node) {
  ensure_live(ctx);
  switch (node->type) {
  case NODE_VAR_DECL:
    gen_var_decl(ctx, node);
    break;
  case NODE_ASSIGNMENT:
    gen_assignment(ctx, node);
    break;
  case NODE_ADD_ASSIGN:
    gen_add_sub(ctx, node, 1);
    break;
  case NODE_SUB_ASSIGN:
    gen_add_sub(ctx, node, 0);
    break;
  case NODE_INDEX_ASSIGN:
    gen_index_assign(ctx, node);
    break;
  case NODE_MEMBER_ASSIGN:
    gen_member_assign(ctx, node);
    break;
  case NODE_METHOD_CALL:
  case NODE_FUNC_CALL:
    irb_expr(ctx, node);
    break;
  case NODE_PRINT:
    gen_print(ctx, node);
    break;
  case NODE_RETURN:
    gen_return(ctx, node);
    break;
  case NODE_IF:
    gen_if(ctx, node);
    break;
  case NODE_WHILE:
    gen_while(ctx, node);
    break;
  case NODE_FOR:
    gen_for(ctx, node);
    break;
  case NODE_BREAK: {
    IrInstr ins = ir_instr(IR_JUMP);
    ins.t = ctx->break_blk[ctx->loop_depth - 1];
    irb_emit(ctx, ins);
    ctx->terminated = 1;
    break;
  }
  case NODE_CONTINUE: {
    IrInstr ins = ir_instr(IR_JUMP);
    ins.t = ctx->cont_blk[ctx->loop_depth - 1];
    irb_emit(ctx, ins);
    ctx->terminated = 1;
    break;
  }
  case NODE_ARRAY_DECL:
    gen_array_decl(ctx, node);
    break;
  default:
    irb_bug("unsupported statement in IR builder");
    break;
  }
}

void irb_block_stmts(BuildCtx *ctx, Node *list) {
  for (Node *s = list; s != NULL; s = s->right) {
    gen_stmt(ctx, s);
  }
}

/**
 * @brief Copies semantic variables into an IR function frame
 * @param f IR function
 * @param vars Semantic variables in slot order
 * @param nvars Variable count
 */
static void copy_frame(IrFunc *f, SemVar *vars, int nvars) {
  f->nvars = nvars;
  if (nvars == 0) {
    return;
  }
  f->var_names = malloc((size_t)nvars * sizeof(char *));
  f->var_types = malloc((size_t)nvars * sizeof(IrType));
  f->var_kinds = malloc((size_t)nvars * sizeof(char *));
  f->var_is_param = malloc((size_t)nvars * sizeof(int));
  f->var_elems = malloc((size_t)nvars * sizeof(IrType));
  f->var_elem_kinds = malloc((size_t)nvars * sizeof(char *));
  for (int i = 0; i < nvars; i++) {
    size_t len = strlen(vars[i].name);
    f->var_names[i] = malloc(len + 1);
    memcpy(f->var_names[i], vars[i].name, len + 1);
    f->var_types[i] = irb_type(vars[i].type);
    f->var_kinds[i] = NULL;
    if (vars[i].kind != NULL) {
      len = strlen(vars[i].kind);
      f->var_kinds[i] = malloc(len + 1);
      memcpy(f->var_kinds[i], vars[i].kind, len + 1);
    }
    f->var_is_param[i] = vars[i].is_param;
    f->var_elems[i] = irb_type(vars[i].elem);
    f->var_elem_kinds[i] = NULL;
    if (vars[i].elem_kind != NULL) {
      len = strlen(vars[i].elem_kind);
      f->var_elem_kinds[i] = malloc(len + 1);
      memcpy(f->var_elem_kinds[i], vars[i].elem_kind, len + 1);
    }
  }
}

/**
 * @brief Lowers one function, method, or entry body into an IR function
 * @param m Module
 * @param name Source name
 * @param label Assembly label
 * @param ret Return category
 * @param vars Frame variables
 * @param nvars Frame variable count
 * @param body Statement list
 * @param sfunc Semantic function record, NULL for entry
 * @param is_method Non-zero for methods
 * @param cls Class index for methods
 * @param ret_float Plain functions normalize int results when set
 * @param ret_kind Declared return keyword, NULL for entry
 * @return IR function
 */
static IrFunc *lower_body(IrModule *m, const char *name, const char *label,
                          IrType ret, SemVar *vars, int nvars, Node *body,
                          SemFunc *sfunc, int is_method, int cls,
                          int ret_float, const char *ret_kind) {
  IrFunc *f = ir_add_func(m, name, label, ret);
  copy_frame(f, vars, nvars);
  BuildCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.m = m;
  ctx.f = f;
  ctx.vars = vars;
  ctx.nvars = nvars;
  ctx.sfunc = sfunc;
  ctx.is_method = is_method;
  ctx.cls = cls;
  ctx.ret_float = ret_float;
  ctx.ret_kind = ret_kind;
  irb_block(&ctx);
  /* Declare every frame variable in the builder scope. */
  for (int i = 0; i < nvars; i++) {
    irb_scope_declare(&ctx, vars[i].name, vars[i].slot);
  }
  irb_block_stmts(&ctx, body);
  return f;
}

IrModule *ir_build(Node *root) {
  SemProg *sp = sem_prog();
  IrModule *m = ir_module_new();
  m->has_user_main = sp->has_user_main;
  for (int g = 0; g < sp->nglobals; g++) {
    m->globals = realloc(m->globals, (size_t)(g + 1) * sizeof(IrGlobal));
    size_t len = strlen(sp->globals[g].name);
    m->globals[g].name = malloc(len + 1);
    memcpy(m->globals[g].name, sp->globals[g].name, len + 1);
    m->globals[g].type = irb_type(sp->globals[g].type);
    m->globals[g].kind = NULL;
    if (sp->globals[g].kind != NULL) {
      len = strlen(sp->globals[g].kind);
      m->globals[g].kind = malloc(len + 1);
      memcpy(m->globals[g].kind, sp->globals[g].kind, len + 1);
    }
    m->globals[g].has_init = sp->globals[g].init != NULL;
    m->nglobals++;
  }
  m->nclasses = sp->nclasses;
  if (m->nclasses > 0) {
    m->classes = malloc((size_t)m->nclasses * sizeof(IrClass));
    for (int c = 0; c < sp->nclasses; c++) {
      size_t len = strlen(sp->classes[c].name);
      m->classes[c].name = malloc(len + 1);
      memcpy(m->classes[c].name, sp->classes[c].name, len + 1);
      m->classes[c].class_id = sp->classes[c].class_id;
      m->classes[c].nslots = sp->nmeth_slots;
      m->classes[c].vtable = NULL;
      m->classes[c].vtable_mth = NULL;
      if (sp->nmeth_slots > 0) {
        m->classes[c].vtable =
            malloc((size_t)sp->nmeth_slots * sizeof(int));
        m->classes[c].vtable_mth =
            malloc((size_t)sp->nmeth_slots * sizeof(int));
        for (int s = 0; s < sp->nmeth_slots; s++) {
          int owner = c;
          int mm = -1;
          for (int cc = c; cc >= 0; cc = sp->classes[cc].base_idx) {
            for (int i = 0; i < sp->classes[cc].nmethods; i++) {
              if (strcmp(sp->classes[cc].methods[i].name,
                         sp->meth_slots[s]) == 0) {
                owner = cc;
                mm = i;
                goto slot_done;
              }
            }
          }
        slot_done:
          m->classes[c].vtable[s] = mm < 0 ? -1 : owner;
          m->classes[c].vtable_mth[s] = mm;
        }
      }
      m->classes[c].nmethods = sp->classes[c].nmethods;
      m->classes[c].method_names =
          malloc((size_t)m->classes[c].nmethods * sizeof(char *));
      for (int i = 0; i < m->classes[c].nmethods; i++) {
        size_t ll = strlen(sp->classes[c].methods[i].name);
        m->classes[c].method_names[i] = malloc(ll + 1);
        memcpy(m->classes[c].method_names[i],
               sp->classes[c].methods[i].name, ll + 1);
      }
    }
  }
  /* Entry program: top level statements run under main. */
  {
    IrFunc *f =
        lower_body(m, "main", "main", IR_INT, sp->entry_vars,
                   sp->nentry_vars, NULL, NULL, 0, -1, 0, NULL);
    BuildCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.m = m;
    ctx.f = f;
    ctx.vars = sp->entry_vars;
    ctx.nvars = sp->nentry_vars;
    ctx.cur = 0;
    for (int i = 0; i < sp->nentry_vars; i++) {
      irb_scope_declare(&ctx, sp->entry_vars[i].name,
                        sp->entry_vars[i].slot);
    }
    for (Node *s = root; s != NULL; s = s->right) {
      if (s->type != NODE_FUNCTION && s->type != NODE_STRUCT_DEF &&
          s->type != NODE_CLASS_DEF) {
        gen_stmt(&ctx, s);
      }
    }
    ensure_live(&ctx);
    IrInstr ins = ir_instr(IR_EXIT);
    ins.v[0] = ir_imm_i(0, IR_INT);
    ins.nv = 1;
    irb_emit(&ctx, ins);
    ctx.terminated = 1;
  }
  for (int i = 0; i < sp->nfuncs; i++) {
    SemFunc *fn = &sp->funcs[i];
    char label[256];
    if (m->has_user_main && strcmp(fn->name, "main") == 0) {
      snprintf(label, sizeof(label), "jot_main");
    } else {
      snprintf(label, sizeof(label), "%s", fn->name);
    }
    IrType ret = IR_INT;
    if (fn->ret != NULL) {
      if (strcmp(fn->ret, "str") == 0) {
        ret = IR_STR;
      } else if (strcmp(fn->ret, "arr") == 0) {
        ret = IR_ARR;
      } else if (strcmp(fn->ret, "void") == 0) {
        ret = IR_VOID;
      }
    }
    lower_body(m, fn->name, label, ret, fn->vars, fn->nvars, fn->body, fn,
               0, -1, fn->ret_float, fn->ret);
  }
  for (int c = 0; c < sp->nclasses; c++) {
    for (int i = 0; i < sp->classes[c].nmethods; i++) {
      SemFunc *md = &sp->classes[c].methods[i];
      char label[256];
      snprintf(label, sizeof(label), "%s__%s", sp->classes[c].name,
               md->name);
      IrType ret = IR_FLOAT;
      if (strcmp(md->ret, "str") == 0) {
        ret = IR_STR;
      } else if (strcmp(md->ret, "bool") == 0) {
        ret = IR_BOOL;
      }
      lower_body(m, md->name, label, ret, md->vars, md->nvars, md->body,
                 md, 1, c, 0, md->ret);
    }
  }
  return m;
}
