#include "ir.h"

IrVal ir_temp(int slot, IrType type) {
  IrVal v;
  v.kind = IRV_TEMP;
  v.idx = slot;
  v.imm = 0;
  v.type = type;
  return v;
}

IrVal ir_var(int slot, IrType type) {
  IrVal v;
  v.kind = IRV_VAR;
  v.idx = slot;
  v.imm = 0;
  v.type = type;
  return v;
}

IrVal ir_global(int gidx, IrType type) {
  IrVal v;
  v.kind = IRV_GLOBAL;
  v.idx = gidx;
  v.imm = 0;
  v.type = type;
  return v;
}

IrVal ir_imm_i(long long value, IrType type) {
  IrVal v;
  v.kind = IRV_IMM_I;
  v.idx = 0;
  v.imm = value;
  v.type = type;
  return v;
}

IrVal ir_imm_f(double value) {
  IrVal v;
  v.kind = IRV_IMM_F;
  v.idx = 0;
  memcpy(&v.imm, &value, sizeof(double));
  v.type = IR_FLOAT;
  return v;
}

IrVal ir_str(int sidx) {
  IrVal v;
  v.kind = IRV_STR;
  v.idx = sidx;
  v.imm = 0;
  v.type = IR_STR;
  return v;
}

IrVal ir_null(void) {
  IrVal v;
  v.kind = IRV_NULL;
  v.idx = 0;
  v.imm = 0;
  v.type = IR_NULL;
  return v;
}

IrModule *ir_module_new(void) {
  IrModule *m = malloc(sizeof(IrModule));
  memset(m, 0, sizeof(IrModule));
  return m;
}

/**
 * @brief Duplicates a string (strdup is unavailable on MSVC)
 * @param s String to copy
 * @return Owned copy
 */
static char *ir_dup(const char *s) {
  size_t len = strlen(s);
  char *out = malloc(len + 1);
  memcpy(out, s, len);
  out[len] = '\0';
  return out;
}

IrFunc *ir_add_func(IrModule *m, const char *name, const char *label,
                    IrType ret) {
  m->funcs = realloc(m->funcs, (size_t)(m->nfuncs + 1) * sizeof(IrFunc));
  IrFunc *f = &m->funcs[m->nfuncs++];
  memset(f, 0, sizeof(IrFunc));
  f->name = ir_dup(name);
  f->label = ir_dup(label);
  f->ret = ret;
  return f;
}

IrBlock *ir_add_block(IrFunc *f) {
  if (f->nblocks >= f->capblocks) {
    f->capblocks = f->capblocks != 0 ? f->capblocks * 2 : 8;
    f->blocks =
        realloc(f->blocks, (size_t)f->capblocks * sizeof(IrBlock));
  }
  IrBlock *b = &f->blocks[f->nblocks];
  memset(b, 0, sizeof(IrBlock));
  b->id = f->nblocks;
  f->nblocks++;
  return b;
}

IrInstr *ir_emit(IrBlock *b, IrInstr ins) {
  if (b->nins >= b->cap) {
    b->cap = b->cap != 0 ? b->cap * 2 : 16;
    b->ins = realloc(b->ins, (size_t)b->cap * sizeof(IrInstr));
  }
  b->ins[b->nins] = ins;
  return &b->ins[b->nins++];
}

IrInstr ir_instr(IrOp op) {
  IrInstr ins;
  memset(&ins, 0, sizeof(IrInstr));
  ins.op = op;
  ins.dst = -1;
  return ins;
}

int ir_add_string(IrModule *m, const char *value) {
  for (int i = 0; i < m->nstrings; i++) {
    if (strcmp(m->strings[i], value) == 0) {
      return i;
    }
  }
  m->strings = realloc(m->strings, (size_t)(m->nstrings + 1) * sizeof(char *));
  m->strings[m->nstrings] = ir_dup(value);
  return m->nstrings++;
}

IrType ir_slot_type(IrFunc *f, int slot) {
  if (slot >= 0 && slot < f->nvars) {
    return f->var_types[slot];
  }
  if (slot >= f->nvars && slot - f->nvars < f->ntemps) {
    return f->temp_types[slot - f->nvars];
  }
  return IR_INT;
}

IrType ir_val_type(IrFunc *f, IrVal v) {
  switch (v.kind) {
  case IRV_TEMP:
  case IRV_VAR:
    return ir_slot_type(f, v.idx);
  default:
    return v.type;
  }
}

/**
 * @brief Names a category for dumps
 * @param t Category
 * @return Short name
 */
static const char *dump_type(IrType t) {
  switch (t) {
  case IR_INT:
    return "int";
  case IR_FLOAT:
    return "float";
  case IR_BOOL:
    return "bool";
  case IR_STR:
    return "str";
  case IR_ARR:
    return "arr";
  case IR_STRUCT:
    return "struct";
  case IR_CLASS:
    return "class";
  case IR_NULL:
    return "null";
  default:
    return "void";
  }
}

/**
 * @brief Names an opcode for dumps
 * @param op Operation code
 * @return Mnemonic
 */
static const char *dump_op(IrOp op) {
  switch (op) {
  case IR_NOP:
    return "nop";
  case IR_CONST_I:
    return "const_i";
  case IR_CONST_F:
    return "const_f";
  case IR_CONST_S:
    return "const_s";
  case IR_CONST_NULL:
    return "const_null";
  case IR_COPY:
    return "copy";
  case IR_ADD:
    return "add";
  case IR_SUB:
    return "sub";
  case IR_MUL:
    return "mul";
  case IR_DIV:
    return "div";
  case IR_MOD:
    return "mod";
  case IR_CMP:
    return "cmp";
  case IR_I2F:
    return "i2f";
  case IR_F2I:
    return "f2i";
  case IR_TRUNC8:
    return "trunc8";
  case IR_LOAD:
    return "load";
  case IR_STORE:
    return "store";
  case IR_LOADG:
    return "loadg";
  case IR_STOREG:
    return "storeg";
  case IR_JUMP:
    return "jump";
  case IR_BR:
    return "br";
  case IR_CALL:
    return "call";
  case IR_CALLM:
    return "callm";
  case IR_RET:
    return "ret";
  case IR_EXIT:
    return "exit";
  case IR_PRINT_S:
    return "print_s";
  case IR_PRINT_V:
    return "print_v";
  case IR_INPUT:
    return "input";
  case IR_LEN:
    return "len";
  case IR_TOSTR:
    return "tostr";
  case IR_TONUM:
    return "tonum";
  case IR_READFILE:
    return "readfile";
  case IR_WRITEFILE:
    return "writefile";
  case IR_NEWARR:
    return "newarr";
  case IR_ARR_LOAD:
    return "arr_load";
  case IR_ARR_STORE:
    return "arr_store";
  case IR_ARR_LEN:
    return "arr_len";
  case IR_STR_IDX:
    return "str_idx";
  case IR_CONCAT:
    return "concat";
  case IR_NEWINST:
    return "newinst";
  case IR_COPYINST:
    return "copyinst";
  case IR_FIELD_LOAD:
    return "field_load";
  case IR_FIELD_STORE:
    return "field_store";
  }
  return "?";
}

/**
 * @brief Prints an operand
 * @param f Function owning temp slots
 * @param v Operand
 * @param out Output stream
 */
static void dump_val(IrFunc *f, IrVal v, FILE *out) {
  switch (v.kind) {
  case IRV_TEMP:
    fprintf(out, "%%t%d", v.idx);
    break;
  case IRV_VAR:
    if (v.idx >= 0 && v.idx < f->nvars && f->var_names[v.idx] != NULL) {
      fprintf(out, "%s", f->var_names[v.idx]);
    } else {
      fprintf(out, "%%v%d", v.idx);
    }
    break;
  case IRV_GLOBAL:
    fprintf(out, "@g%d", v.idx);
    break;
  case IRV_IMM_I:
    fprintf(out, "%lld", v.imm);
    break;
  case IRV_IMM_F: {
    double d;
    memcpy(&d, &v.imm, sizeof(double));
    fprintf(out, "%g", d);
    break;
  }
  case IRV_STR:
    fprintf(out, "str%d", v.idx);
    break;
  case IRV_NULL:
    fprintf(out, "null");
    break;
  }
}

/**
 * @brief Prints one instruction
 * @param f Owning function
 * @param ins Instruction
 * @param out Output stream
 */
static void dump_instr(IrFunc *f, IrInstr *ins, FILE *out) {
  if (ins->op == IR_NOP) {
    fprintf(out, "    nop\n");
    return;
  }
  fprintf(out, "    ");
  if (ins->dst >= 0) {
    fprintf(out, "%%t%d = ", ins->dst);
  }
  fprintf(out, "%s", dump_op(ins->op));
  switch (ins->op) {
  case IR_CONST_I:
    fprintf(out, " %lld", ins->v[0].imm);
    break;
  case IR_CONST_F: {
    double d;
    memcpy(&d, &ins->v[0].imm, sizeof(double));
    fprintf(out, " %g", d);
    break;
  }
  case IR_CMP: {
    const char *cc = "?";
    if (ins->cond == IR_CEQ) {
      cc = "==";
    } else if (ins->cond == IR_CNE) {
      cc = "!=";
    } else if (ins->cond == IR_CLT) {
      cc = "<";
    } else if (ins->cond == IR_CGT) {
      cc = ">";
    } else if (ins->cond == IR_CLE) {
      cc = "<=";
    } else if (ins->cond == IR_CGE) {
      cc = ">=";
    }
    fprintf(out, ".%s %s ", dump_type(ins->type), cc);
    dump_val(f, ins->v[0], out);
    fprintf(out, ", ");
    dump_val(f, ins->v[1], out);
    break;
  }
  case IR_JUMP:
    fprintf(out, " bb%d", ins->t);
    break;
  case IR_BR:
    dump_val(f, ins->v[0], out);
    fprintf(out, " bb%d bb%d", ins->t, ins->f);
    break;
  case IR_CALL:
    fprintf(out, " f%d(", ins->callee);
    break;
  case IR_CALLM:
    fprintf(out, " vslot%d(", ins->callee);
    break;
  case IR_PRINT_S:
    fprintf(out, " str%d", ins->aux);
    break;
  default:
    break;
  }
  if (ins->op != IR_CONST_I && ins->op != IR_CONST_F && ins->op != IR_CMP &&
      ins->op != IR_JUMP && ins->op != IR_BR && ins->op != IR_CALL &&
      ins->op != IR_CALLM && ins->op != IR_PRINT_S) {
    for (int i = 0; i < ins->nv; i++) {
      fprintf(out, "%s", i == 0 ? " " : ", ");
      dump_val(f, ins->v[i], out);
    }
    for (int i = 0; i < ins->nlist; i++) {
      fprintf(out, "%s", (i == 0 && ins->nv == 0) ? " " : ", ");
      dump_val(f, ins->list[i], out);
    }
    if (ins->op == IR_ADD || ins->op == IR_SUB || ins->op == IR_MUL ||
        ins->op == IR_DIV || ins->op == IR_MOD) {
      fprintf(out, " <%s>", dump_type(ins->type));
    }
  } else if (ins->op == IR_CALL || ins->op == IR_CALLM) {
    for (int i = 0; i < ins->nlist; i++) {
      fprintf(out, "%s", i == 0 ? "" : ", ");
      dump_val(f, ins->list[i], out);
    }
    fprintf(out, ")");
    if (ins->name != NULL) {
      fprintf(out, " ; %s", ins->name);
    }
  }
  if (ins->op == IR_NEWARR || ins->op == IR_NEWINST ||
      ins->op == IR_FIELD_LOAD || ins->op == IR_FIELD_STORE ||
      ins->op == IR_ARR_STORE || ins->op == IR_COPYINST) {
    fprintf(out, " <%s aux=%d,%d>", dump_type(ins->type), ins->aux,
            ins->aux2);
  }
  fprintf(out, "\n");
}

void ir_dump(IrModule *m, FILE *f) {
  for (int g = 0; g < m->nglobals; g++) {
    fprintf(f, "global @g%d %s%s\n", g, dump_type(m->globals[g].type),
            m->globals[g].has_init ? " init" : "");
  }
  for (int i = 0; i < m->nstrings; i++) {
    fprintf(f, "str%d = \"%s\"\n", i, m->strings[i]);
  }
  for (int fi = 0; fi < m->nfuncs; fi++) {
    IrFunc *fn = &m->funcs[fi];
    fprintf(f, "fn %s (%s):\n", fn->label, dump_type(fn->ret));
    for (int b = 0; b < fn->nblocks; b++) {
      fprintf(f, "  bb%d:\n", fn->blocks[b].id);
      for (int k = 0; k < fn->blocks[b].nins; k++) {
        dump_instr(fn, &fn->blocks[b].ins[k], f);
      }
    }
  }
}
