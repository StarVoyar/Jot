#include "opt_impl.h"

int opt_dce(IrModule *m) {
  int changed = 0;
  for (int fi = 0; fi < m->nfuncs; fi++) {
    IrFunc *f = &m->funcs[fi];
    int nslots = f->nvars + f->ntemps;
    for (int s = f->nvars; s < nslots; s++) {
      int uses, defs;
      opt_slot_info(f, s, &uses, &defs);
      if (uses != 0 || defs == 0) {
        continue;
      }
      for (int b = 0; b < f->nblocks; b++) {
        for (int k = 0; k < f->blocks[b].nins; k++) {
          IrInstr *ins = &f->blocks[b].ins[k];
          if (ins->op != IR_NOP && ins->dst == s &&
              opt_is_pure(ins->op)) {
            opt_blank(ins);
            changed = 1;
          }
        }
      }
    }
  }
  return changed;
}

int opt_unreachable(IrModule *m) {
  int changed = 0;
  for (int fi = 0; fi < m->nfuncs; fi++) {
    IrFunc *f = &m->funcs[fi];
    char *seen = calloc((size_t)f->nblocks, 1);
    int *stack = malloc((size_t)f->nblocks * sizeof(int));
    int nstack = 0;
    stack[nstack++] = 0;
    seen[0] = 1;
    while (nstack > 0) {
      int b = stack[--nstack];
      for (int k = 0; k < f->blocks[b].nins; k++) {
        IrInstr *ins = &f->blocks[b].ins[k];
        if (ins->op == IR_JUMP) {
          if (!seen[ins->t]) {
            seen[ins->t] = 1;
            stack[nstack++] = ins->t;
          }
        } else if (ins->op == IR_BR) {
          if (!seen[ins->t]) {
            seen[ins->t] = 1;
            stack[nstack++] = ins->t;
          }
          if (!seen[ins->f]) {
            seen[ins->f] = 1;
            stack[nstack++] = ins->f;
          }
        }
      }
      /* Fallthrough reaches the next block in layout order. */
      if (b + 1 < f->nblocks) {
        int falls = 1;
        for (int k = f->blocks[b].nins - 1; k >= 0; k--) {
          IrOp op = f->blocks[b].ins[k].op;
          if (op == IR_NOP) {
            continue;
          }
          if (op == IR_JUMP || op == IR_BR || op == IR_RET ||
              op == IR_EXIT) {
            falls = 0;
          }
          break;
        }
        if (falls && !seen[b + 1]) {
          seen[b + 1] = 1;
          stack[nstack++] = b + 1;
        }
      }
    }
    for (int b = 0; b < f->nblocks; b++) {
      if (!seen[b]) {
        for (int k = 0; k < f->blocks[b].nins; k++) {
          if (f->blocks[b].ins[k].op != IR_NOP) {
            opt_blank(&f->blocks[b].ins[k]);
            changed = 1;
          }
        }
      }
    }
    free(seen);
    free(stack);
    if (opt_compact(f)) {
      changed = 1;
    }
  }
  return changed;
}

/**
 * @brief Follows jump chains to the final target
 * @param f Function
 * @param b Block index
 * @return Threaded block index
 */
static int thread_jump(IrFunc *f, int b) {
  for (int i = 0; i < f->nblocks; i++) {
    int first = -1;
    for (int k = 0; k < f->blocks[b].nins; k++) {
      if (f->blocks[b].ins[k].op != IR_NOP) {
        first = k;
        break;
      }
    }
    if (first < 0 || f->blocks[b].ins[first].op != IR_JUMP) {
      return b;
    }
    b = f->blocks[b].ins[first].t;
  }
  return b;
}

int opt_branch(IrModule *m) {
  int changed = 0;
  for (int fi = 0; fi < m->nfuncs; fi++) {
    IrFunc *f = &m->funcs[fi];
    for (int b = 0; b < f->nblocks; b++) {
      for (int k = 0; k < f->blocks[b].nins; k++) {
        IrInstr *ins = &f->blocks[b].ins[k];
        if (ins->op == IR_BR && ins->nv == 1 &&
            ins->v[0].kind == IRV_IMM_I) {
          ins->op = IR_JUMP;
          ins->t = ins->v[0].imm != 0 ? ins->t : ins->f;
          ins->nv = 0;
          changed = 1;
        } else if (ins->op == IR_JUMP) {
          int t = thread_jump(f, ins->t);
          if (t != ins->t) {
            ins->t = t;
            changed = 1;
          }
        } else if (ins->op == IR_BR && ins->t == ins->f) {
          ins->op = IR_JUMP;
          ins->nv = 0;
          changed = 1;
        }
      }
    }
  }
  return changed;
}

int opt_blocks(IrModule *m) {
  int changed = 0;
  for (int fi = 0; fi < m->nfuncs; fi++) {
    IrFunc *f = &m->funcs[fi];
    for (int b = 0; b < f->nblocks; b++) {
      int last = -1;
      for (int k = 0; k < f->blocks[b].nins; k++) {
        if (f->blocks[b].ins[k].op != IR_NOP) {
          last = k;
        }
      }
      if (last < 0 || f->blocks[b].ins[last].op != IR_JUMP) {
        continue;
      }
      int t = f->blocks[b].ins[last].t;
      if (t == b || t >= f->nblocks) {
        continue;
      }
      int preds = 0;
      for (int o = 0; o < f->nblocks; o++) {
        for (int k = 0; k < f->blocks[o].nins; k++) {
          IrInstr *oi = &f->blocks[o].ins[k];
          if ((oi->op == IR_JUMP && oi->t == t) ||
              (oi->op == IR_BR && (oi->t == t || oi->f == t))) {
            preds++;
          }
        }
      }
      if (preds != 1) {
        continue;
      }
      /* Splice the target into the predecessor. */
      IrBlock *src = &f->blocks[b];
      opt_blank(&src->ins[last]);
      for (int k = 0; k < f->blocks[t].nins; k++) {
        IrInstr *mv = &f->blocks[t].ins[k];
        if (mv->op == IR_NOP) {
          continue;
        }
        IrInstr copy = *mv;
        if (copy.list != NULL) {
          IrVal *nl = malloc((size_t)copy.nlist * sizeof(IrVal));
          memcpy(nl, copy.list, (size_t)copy.nlist * sizeof(IrVal));
          copy.list = nl;
        }
        ir_emit(&f->blocks[b], copy);
        opt_blank(mv);
      }
      changed = 1;
    }
    if (opt_compact(f)) {
      changed = 1;
    }
  }
  return changed;
}

void opt_squeeze(IrModule *m) {
  for (int fi = 0; fi < m->nfuncs; fi++) {
    IrFunc *f = &m->funcs[fi];
    for (int b = 0; b < f->nblocks; b++) {
      int w = 0;
      for (int k = 0; k < f->blocks[b].nins; k++) {
        if (f->blocks[b].ins[k].op == IR_NOP) {
          if (f->blocks[b].ins[k].list != NULL) {
            free(f->blocks[b].ins[k].list);
          }
          continue;
        }
        if (w != k) {
          f->blocks[b].ins[w] = f->blocks[b].ins[k];
        }
        w++;
      }
      f->blocks[b].nins = w;
    }
    opt_compact(f);
  }
}
