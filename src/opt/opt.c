#include "opt_impl.h"

#include <float.h>
#include <limits.h>
#include <math.h>

int opt_is_pure(IrOp op) {
  switch (op) {
  case IR_CONST_I:
  case IR_CONST_F:
  case IR_CONST_S:
  case IR_CONST_NULL:
  case IR_COPY:
  case IR_ADD:
  case IR_SUB:
  case IR_MUL:
  case IR_DIV:
  case IR_MOD:
  case IR_AND:
  case IR_OR:
  case IR_XOR:
  case IR_SHL:
  case IR_SHR:
  case IR_NOT:
  case IR_CMP:
  case IR_I2F:
  case IR_F2I:
  case IR_TRUNC8:
  case IR_LOAD:
  case IR_LOADG:
  case IR_ARR_LEN:
    return 1;
  default:
    return 0;
  }
}

int opt_val_equal(IrVal a, IrVal b) {
  return a.kind == b.kind && a.idx == b.idx && a.imm == b.imm;
}

void opt_blank(IrInstr *ins) {
  if (ins->list != NULL) {
    free(ins->list);
    ins->list = NULL;
  }
  ins->nlist = 0;
  ins->op = IR_NOP;
  ins->dst = -1;
}

int opt_compact(IrFunc *f) {
  int *keep = malloc((size_t)f->nblocks * sizeof(int));
  int *remap = malloc((size_t)f->nblocks * sizeof(int));
  int n = 0;
  for (int b = 0; b < f->nblocks; b++) {
    int empty = (f->blocks[b].nins == 0);
    if (b == 0) {
      empty = 0;
    }
    keep[b] = !empty;
    if (!empty) {
      remap[b] = n++;
    } else {
      remap[b] = -1;
    }
  }
  if (n == f->nblocks) {
    free(keep);
    free(remap);
    return 0;
  }
  IrBlock *nb = malloc((size_t)n * sizeof(IrBlock));
  int w = 0;
  for (int b = 0; b < f->nblocks; b++) {
    if (!keep[b]) {
      free(f->blocks[b].ins);
      continue;
    }
    nb[w] = f->blocks[b];
    nb[w].id = w;
    for (int k = 0; k < nb[w].nins; k++) {
      if (nb[w].ins[k].op == IR_JUMP) {
        nb[w].ins[k].t = remap[nb[w].ins[k].t];
      } else if (nb[w].ins[k].op == IR_BR) {
        nb[w].ins[k].t = remap[nb[w].ins[k].t];
        nb[w].ins[k].f = remap[nb[w].ins[k].f];
      }
    }
    w++;
  }
  free(f->blocks);
  f->blocks = nb;
  f->nblocks = n;
  free(keep);
  free(remap);
  return 1;
}

double opt_bits_double(long long bits) {
  double d;
  memcpy(&d, &bits, sizeof(double));
  return d;
}

long long opt_double_bits(double d) {
  long long bits;
  memcpy(&bits, &d, sizeof(double));
  return bits;
}

/**
 * @brief Turns an instruction into a constant definition
 * @param ins Instruction to rewrite (keeps dst)
 * @param v Constant value
 * @return Non-zero (always changed)
 */
static int make_const(IrInstr *ins, IrVal v) {
  if (ins->list != NULL) {
    free(ins->list);
    ins->list = NULL;
  }
  ins->nlist = 0;
  ins->nv = 1;
  ins->cond = 0;
  ins->t = 0;
  ins->f = 0;
  ins->callee = 0;
  ins->aux = 0;
  ins->aux2 = 0;
  if (v.kind == IRV_IMM_I) {
    ins->op = IR_CONST_I;
  } else if (v.kind == IRV_IMM_F) {
    ins->op = IR_CONST_F;
  } else if (v.kind == IRV_STR) {
    ins->op = IR_CONST_S;
  } else {
    ins->op = IR_CONST_NULL;
  }
  ins->v[0] = v;
  return 1;
}

/**
 * @brief Reads an integer operand value
 * @param v Operand (immediate int or null)
 * @param out Receives the value
 * @return Non-zero when the operand is a known integer
 */
static int const_int(IrVal v, long long *out) {
  if (v.kind == IRV_IMM_I) {
    *out = v.imm;
    return 1;
  }
  if (v.kind == IRV_NULL) {
    *out = 0;
    return 1;
  }
  return 0;
}

/**
 * @brief Reads a float operand value
 * @param v Operand (immediate float)
 * @param out Receives the value
 * @return Non-zero when the operand is a known float
 */
static int const_float(IrVal v, double *out) {
  if (v.kind == IRV_IMM_F) {
    *out = opt_bits_double(v.imm);
    return 1;
  }
  return 0;
}

/**
 * @brief Folds one instruction when all operands are constant
 * @param m Module (for interned strings)
 * @param ins Instruction to fold
 * @return Non-zero when folded
 */
static int fold_instr(IrModule *m, IrInstr *ins) {
  long long a, b;
  double fa, fb;
  switch (ins->op) {
  case IR_COPY:
    if (ins->nv == 1 &&
        (ins->v[0].kind == IRV_IMM_I || ins->v[0].kind == IRV_IMM_F ||
         ins->v[0].kind == IRV_STR || ins->v[0].kind == IRV_NULL)) {
      return make_const(ins, ins->v[0]);
    }
    return 0;
  case IR_I2F:
    if (ins->nv == 1 && const_int(ins->v[0], &a)) {
      double d = (double)a;
      return make_const(ins, ir_imm_f(d));
    }
    return 0;
  case IR_F2I:
    if (ins->nv == 1 && const_float(ins->v[0], &fa) && isfinite(fa) &&
        fa >= -9223372036854775808.0 && fa < 9223372036854775808.0) {
      return make_const(ins, ir_imm_i((long long)fa, IR_INT));
    }
    return 0;
  case IR_TRUNC8:
    if (ins->nv == 1 && const_int(ins->v[0], &a)) {
      return make_const(ins, ir_imm_i(a & 0xFF, IR_INT));
    }
    return 0;
  case IR_NOT:
    if (ins->nv == 1 && const_int(ins->v[0], &a)) {
      return make_const(ins, ir_imm_i(~a, IR_INT));
    }
    return 0;
  case IR_AND:
  case IR_OR:
  case IR_XOR:
  case IR_SHL:
  case IR_SHR:
    if (ins->nv != 2 || ins->type != IR_INT) {
      return 0;
    }
    if (!const_int(ins->v[0], &a) || !const_int(ins->v[1], &b)) {
      return 0;
    }
    /* Bitwise ops wrap and mask like the backend: always safe to fold. */
    if (ins->op == IR_AND) {
      return make_const(ins, ir_imm_i(a & b, IR_INT));
    }
    if (ins->op == IR_OR) {
      return make_const(ins, ir_imm_i(a | b, IR_INT));
    }
    if (ins->op == IR_XOR) {
      return make_const(ins, ir_imm_i(a ^ b, IR_INT));
    }
    {
      unsigned long long ua = (unsigned long long)a;
      unsigned long long count = (unsigned long long)b & 63u;
      long long r;
      if (ins->op == IR_SHL) {
        r = (long long)(ua << count);
      } else if (a >= 0) {
        r = (long long)(ua >> count);
      } else {
        /* Arithmetic shift of a negative: shift the inverted bits. */
        r = ~((long long)((~ua) >> count));
      }
      return make_const(ins, ir_imm_i(r, IR_INT));
    }
  case IR_ADD:
  case IR_SUB:
  case IR_MUL:
  case IR_DIV:
  case IR_MOD:
    if (ins->nv != 2 || ins->type == IR_STR) {
      return 0;
    }
    if (ins->type == IR_FLOAT) {
      double r;
      if (!const_float(ins->v[0], &fa) || !const_float(ins->v[1], &fb) ||
          !isfinite(fa) || !isfinite(fb)) {
        return 0;
      }
      if (ins->op == IR_ADD) {
        r = fa + fb;
      } else if (ins->op == IR_SUB) {
        r = fa - fb;
      } else if (ins->op == IR_MUL) {
        r = fa * fb;
      } else if (fb == 0.0) {
        return 0;
      } else {
        r = fa / fb;
      }
      if (!isfinite(r)) {
        return 0;
      }
      return make_const(ins, ir_imm_f(r));
    }
    if (!const_int(ins->v[0], &a) || !const_int(ins->v[1], &b)) {
      return 0;
    }
    if (ins->op == IR_ADD) {
      unsigned long long ur =
          (unsigned long long)a + (unsigned long long)b;
      long long r = (long long)ur;
      /* Overflow keeps the runtime trap: only fold exact results. */
      if ((b > 0 && r < a) || (b < 0 && r > a)) {
        return 0;
      }
      return make_const(ins, ir_imm_i(r, IR_INT));
    }
    if (ins->op == IR_SUB) {
      unsigned long long ur =
          (unsigned long long)a - (unsigned long long)b;
      long long r = (long long)ur;
      if ((b > 0 && r > a) || (b < 0 && r < a)) {
        return 0;
      }
      return make_const(ins, ir_imm_i(r, IR_INT));
    }
    if (ins->op == IR_MUL) {
      if (a == 0 || b == 0) {
        return make_const(ins, ir_imm_i(0, IR_INT));
      }
      int overflow = 0;
      if (a > 0) {
        if (b > 0) {
          overflow = (a > LLONG_MAX / b);
        } else {
          overflow = (b < LLONG_MIN / a);
        }
      } else {
        if (b > 0) {
          overflow = (a < LLONG_MIN / b);
        } else {
          overflow = (b < LLONG_MAX / a);
        }
      }
      if (overflow) {
        return 0;
      }
      return make_const(ins, ir_imm_i(a * b, IR_INT));
    }
    if (b == 0) {
      return 0;
    }
    if (a == LLONG_MIN && b == -1) {
      return 0;
    }
    if (ins->op == IR_DIV) {
      return make_const(ins, ir_imm_i(a / b, IR_INT));
    }
    return make_const(ins, ir_imm_i(a % b, IR_INT));
  case IR_CMP: {
    if (ins->nv != 2) {
      return 0;
    }
    long long r = -1;
    if (ins->type == IR_STR) {
      if (ins->v[0].kind != IRV_STR || ins->v[1].kind != IRV_STR) {
        return 0;
      }
      int c = strcmp(m->strings[ins->v[0].idx], m->strings[ins->v[1].idx]);
      if (ins->cond == IR_CEQ) {
        r = (c == 0);
      } else if (ins->cond == IR_CNE) {
        r = (c != 0);
      } else if (ins->cond == IR_CLT) {
        r = (c < 0);
      } else if (ins->cond == IR_CGT) {
        r = (c > 0);
      } else if (ins->cond == IR_CLE) {
        r = (c <= 0);
      } else {
        r = (c >= 0);
      }
      return make_const(ins, ir_imm_i(r, IR_INT));
    }
    if (ins->type == IR_FLOAT) {
      if (!const_float(ins->v[0], &fa) || !const_float(ins->v[1], &fb) ||
          !isfinite(fa) || !isfinite(fb)) {
        return 0;
      }
      if (ins->cond == IR_CEQ) {
        r = (fa == fb);
      } else if (ins->cond == IR_CNE) {
        r = (fa != fb);
      } else if (ins->cond == IR_CLT) {
        r = (fa < fb);
      } else if (ins->cond == IR_CGT) {
        r = (fa > fb);
      } else if (ins->cond == IR_CLE) {
        r = (fa <= fb);
      } else {
        r = (fa >= fb);
      }
      return make_const(ins, ir_imm_i(r, IR_INT));
    }
    if (!const_int(ins->v[0], &a) || !const_int(ins->v[1], &b)) {
      return 0;
    }
    if (ins->cond == IR_CEQ) {
      r = (a == b);
    } else if (ins->cond == IR_CNE) {
      r = (a != b);
    } else if (ins->cond == IR_CLT) {
      r = (a < b);
    } else if (ins->cond == IR_CGT) {
      r = (a > b);
    } else if (ins->cond == IR_CLE) {
      r = (a <= b);
    } else {
      r = (a >= b);
    }
    return make_const(ins, ir_imm_i(r, IR_INT));
  }
  case IR_CONCAT:
    if (ins->nv == 2 && ins->v[0].kind == IRV_STR &&
        ins->v[1].kind == IRV_STR) {
      const char *x = m->strings[ins->v[0].idx];
      const char *y = m->strings[ins->v[1].idx];
      size_t nx = strlen(x);
      size_t ny = strlen(y);
      char *both = malloc(nx + ny + 1);
      memcpy(both, x, nx);
      memcpy(both + nx, y, ny + 1);
      int sidx = ir_add_string(m, both);
      free(both);
      IrVal v;
      v.kind = IRV_STR;
      v.idx = sidx;
      v.imm = 0;
      v.type = IR_STR;
      return make_const(ins, v);
    }
    return 0;
  case IR_LEN:
    /* String length of a constant cannot trap. */
    if (ins->nv == 1 && ins->v[0].kind == IRV_STR) {
      long long n = (long long)strlen(m->strings[ins->v[0].idx]);
      return make_const(ins, ir_imm_i(n, IR_INT));
    }
    return 0;
  default:
    return 0;
  }
}

int opt_fold(IrModule *m) {
  int changed = 0;
  for (int fi = 0; fi < m->nfuncs; fi++) {
    IrFunc *f = &m->funcs[fi];
    for (int b = 0; b < f->nblocks; b++) {
      for (int k = 0; k < f->blocks[b].nins; k++) {
        IrInstr *ins = &f->blocks[b].ins[k];
        if (ins->op != IR_NOP && fold_instr(m, ins)) {
          changed = 1;
        }
      }
    }
  }
  return changed;
}

int opt_prop(IrModule *m) {
  int changed = 0;
  for (int fi = 0; fi < m->nfuncs; fi++) {
    IrFunc *f = &m->funcs[fi];
    int nslots = f->nvars + f->ntemps;
    int *uses = calloc((size_t)nslots, sizeof(int));
    int *defs = calloc((size_t)nslots, sizeof(int));
    IrVal *cval = malloc((size_t)nslots * sizeof(IrVal));
    char *is_const = calloc((size_t)nslots, 1);
    /* Pass 1: count uses/defs, remembering single constant definitions. */
    for (int b = 0; b < f->nblocks; b++) {
      for (int k = 0; k < f->blocks[b].nins; k++) {
        IrInstr *ins = &f->blocks[b].ins[k];
        if (ins->op == IR_NOP) {
          continue;
        }
        if (ins->dst >= f->nvars && ins->dst < nslots) {
          if (defs[ins->dst] == 0 &&
              (ins->op == IR_CONST_I || ins->op == IR_CONST_F ||
               ins->op == IR_CONST_S || ins->op == IR_CONST_NULL)) {
            cval[ins->dst] = ins->v[0];
            is_const[ins->dst] = 1;
          } else {
            is_const[ins->dst] = 0;
          }
          defs[ins->dst]++;
        }
        for (int i = 0; i < ins->nv; i++) {
          if ((ins->v[i].kind == IRV_TEMP || ins->v[i].kind == IRV_VAR) &&
              ins->v[i].idx >= 0 && ins->v[i].idx < nslots) {
            uses[ins->v[i].idx]++;
          }
        }
        for (int i = 0; i < ins->nlist; i++) {
          if ((ins->list[i].kind == IRV_TEMP ||
               ins->list[i].kind == IRV_VAR) &&
              ins->list[i].idx >= 0 && ins->list[i].idx < nslots) {
            uses[ins->list[i].idx]++;
          }
        }
      }
    }
    /* Pass 2: substitute the constants (same single-def rule as before). */
    for (int b = 0; b < f->nblocks; b++) {
      for (int k = 0; k < f->blocks[b].nins; k++) {
        IrInstr *ins = &f->blocks[b].ins[k];
        if (ins->op == IR_NOP) {
          continue;
        }
        for (int i = 0; i < ins->nv; i++) {
          int idx = ins->v[i].idx;
          if ((ins->v[i].kind == IRV_TEMP || ins->v[i].kind == IRV_VAR) &&
              idx >= f->nvars && idx < nslots && uses[idx] > 0 &&
              defs[idx] == 1 && is_const[idx]) {
            ins->v[i] = cval[idx];
            changed = 1;
          }
        }
        for (int i = 0; i < ins->nlist; i++) {
          int idx = ins->list[i].idx;
          if ((ins->list[i].kind == IRV_TEMP ||
               ins->list[i].kind == IRV_VAR) &&
              idx >= f->nvars && idx < nslots && uses[idx] > 0 &&
              defs[idx] == 1 && is_const[idx]) {
            ins->list[i] = cval[idx];
            changed = 1;
          }
        }
      }
    }
    free(uses);
    free(defs);
    free(cval);
    free(is_const);
  }
  return changed;
}

int opt_copies(IrModule *m) {
  int changed = 0;
  for (int fi = 0; fi < m->nfuncs; fi++) {
    IrFunc *f = &m->funcs[fi];
    int nslots = f->nvars + f->ntemps;
    int *uses = calloc((size_t)nslots, sizeof(int));
    int *defs = calloc((size_t)nslots, sizeof(int));
    IrVal *src = malloc((size_t)nslots * sizeof(IrVal));
    char *is_copy = calloc((size_t)nslots, 1);
    /* Pass 1: count uses/defs, remembering single copy definitions. */
    for (int b = 0; b < f->nblocks; b++) {
      for (int k = 0; k < f->blocks[b].nins; k++) {
        IrInstr *ins = &f->blocks[b].ins[k];
        if (ins->op == IR_NOP) {
          continue;
        }
        if (ins->dst >= f->nvars && ins->dst < nslots) {
          if (defs[ins->dst] == 0 && ins->op == IR_COPY && ins->nv == 1) {
            src[ins->dst] = ins->v[0];
            is_copy[ins->dst] = 1;
          } else {
            is_copy[ins->dst] = 0;
          }
          defs[ins->dst]++;
        }
        for (int i = 0; i < ins->nv; i++) {
          if ((ins->v[i].kind == IRV_TEMP || ins->v[i].kind == IRV_VAR) &&
              ins->v[i].idx >= 0 && ins->v[i].idx < nslots) {
            uses[ins->v[i].idx]++;
          }
        }
        for (int i = 0; i < ins->nlist; i++) {
          if ((ins->list[i].kind == IRV_TEMP ||
               ins->list[i].kind == IRV_VAR) &&
              ins->list[i].idx >= 0 && ins->list[i].idx < nslots) {
            uses[ins->list[i].idx]++;
          }
        }
      }
    }
    /* Pass 2: forward the copies (same single-def rule as before). */
    for (int b = 0; b < f->nblocks; b++) {
      for (int k = 0; k < f->blocks[b].nins; k++) {
        IrInstr *ins = &f->blocks[b].ins[k];
        if (ins->op == IR_NOP) {
          continue;
        }
        for (int i = 0; i < ins->nv; i++) {
          int idx = ins->v[i].idx;
          if ((ins->v[i].kind == IRV_TEMP || ins->v[i].kind == IRV_VAR) &&
              idx >= f->nvars && idx < nslots && uses[idx] > 0 &&
              defs[idx] == 1 && is_copy[idx]) {
            ins->v[i] = src[idx];
            changed = 1;
          }
        }
        for (int i = 0; i < ins->nlist; i++) {
          int idx = ins->list[i].idx;
          if ((ins->list[i].kind == IRV_TEMP ||
               ins->list[i].kind == IRV_VAR) &&
              idx >= f->nvars && idx < nslots && uses[idx] > 0 &&
              defs[idx] == 1 && is_copy[idx]) {
            ins->list[i] = src[idx];
            changed = 1;
          }
        }
      }
    }
    free(uses);
    free(defs);
    free(src);
    free(is_copy);
  }
  return changed;
}

void opt_run(IrModule *m, OptLevel level) {
  if (level == OPT_O0) {
    return;
  }
  for (int i = 0; i < 10; i++) {
    int changed = 0;
    changed |= opt_fold(m);
    changed |= opt_prop(m);
    changed |= opt_copies(m);
    changed |= opt_algebra(m);
    changed |= opt_cmp(m);
    changed |= opt_branch(m);
    if (level == OPT_O2) {
      changed |= opt_cse(m);
    }
    if (!changed) {
      break;
    }
  }
  opt_loadstore(m);
  opt_dce(m);
  opt_unreachable(m);
  opt_blocks(m);
  opt_dce(m);
  opt_fold(m);
  opt_prop(m);
  opt_dce(m);
  opt_squeeze(m);
}
