#include "opt_impl.h"

/**
 * @brief Tells whether an operand is an integer zero
 * @param v Operand
 * @return Non-zero for immediate 0 (int or null)
 */
static int is_zero(IrVal v) {
  return (v.kind == IRV_IMM_I && v.imm == 0) || v.kind == IRV_NULL;
}

/**
 * @brief Tells whether an operand is an integer one
 * @param v Operand
 * @return Non-zero for immediate 1
 */
static int is_one(IrVal v) {
  return v.kind == IRV_IMM_I && v.imm == 1;
}

/**
 * @brief Tells whether an operand is an empty string constant
 * @param m Module for string lookup
 * @param v Operand
 * @return Non-zero for ""
 */
static int is_empty_str(IrModule *m, IrVal v) {
  return v.kind == IRV_STR && m->strings[v.idx][0] == '\0';
}

/**
 * @brief Tells whether an operand is the double 1.0
 * @param v Operand
 * @return Non-zero for immediate 1.0
 */
static int is_one_float(IrVal v) {
  return v.kind == IRV_IMM_F && opt_bits_double(v.imm) == 1.0;
}

/**
 * @brief Rewrites an instruction into a copy of a value
 * @param ins Instruction to rewrite (keeps dst)
 * @param v Value to copy
 */
static void make_copy(IrInstr *ins, IrVal v) {
  if (ins->list != NULL) {
    free(ins->list);
    ins->list = NULL;
  }
  ins->nlist = 0;
  ins->op = IR_COPY;
  ins->nv = 1;
  ins->v[0] = v;
}

/**
 * @brief Rewrites an instruction into an integer constant
 * @param ins Instruction to rewrite (keeps dst)
 * @param value Constant value
 */
static void make_int(IrInstr *ins, long long value) {
  if (ins->list != NULL) {
    free(ins->list);
    ins->list = NULL;
  }
  ins->nlist = 0;
  ins->op = IR_CONST_I;
  ins->nv = 1;
  ins->v[0] = ir_imm_i(value, IR_INT);
}

int opt_algebra(IrModule *m) {
  int changed = 0;
  for (int fi = 0; fi < m->nfuncs; fi++) {
    IrFunc *f = &m->funcs[fi];
    for (int b = 0; b < f->nblocks; b++) {
      for (int k = 0; k < f->blocks[b].nins; k++) {
        IrInstr *ins = &f->blocks[b].ins[k];
        if (ins->nv != 2 || ins->nlist != 0) {
          continue;
        }
        if (ins->op == IR_ADD) {
          if (ins->type == IR_FLOAT) {
            continue;
          }
          if (is_zero(ins->v[1])) {
            make_copy(ins, ins->v[0]);
            changed = 1;
          } else if (is_zero(ins->v[0])) {
            make_copy(ins, ins->v[1]);
            changed = 1;
          }
        } else if (ins->op == IR_SUB) {
          if (ins->type == IR_FLOAT) {
            continue;
          }
          if (is_zero(ins->v[1])) {
            make_copy(ins, ins->v[0]);
            changed = 1;
          } else if (opt_val_equal(ins->v[0], ins->v[1])) {
            make_int(ins, 0);
            changed = 1;
          }
        } else if (ins->op == IR_MUL) {
          if (ins->type == IR_FLOAT) {
            if (is_one_float(ins->v[1])) {
              make_copy(ins, ins->v[0]);
              changed = 1;
            } else if (is_one_float(ins->v[0])) {
              make_copy(ins, ins->v[1]);
              changed = 1;
            }
            continue;
          }
          if (is_one(ins->v[1])) {
            make_copy(ins, ins->v[0]);
            changed = 1;
          } else if (is_one(ins->v[0])) {
            make_copy(ins, ins->v[1]);
            changed = 1;
          } else if (is_zero(ins->v[0]) || is_zero(ins->v[1])) {
            make_int(ins, 0);
            changed = 1;
          }
        } else if (ins->op == IR_DIV) {
          if (ins->type == IR_FLOAT) {
            if (is_one_float(ins->v[1])) {
              make_copy(ins, ins->v[0]);
              changed = 1;
            }
            continue;
          }
          if (is_one(ins->v[1])) {
            make_copy(ins, ins->v[0]);
            changed = 1;
          }
        } else if (ins->op == IR_MOD) {
          if (ins->type != IR_FLOAT && is_one(ins->v[1])) {
            make_int(ins, 0);
            changed = 1;
          }
        } else if (ins->op == IR_CONCAT) {
          if (is_empty_str(m, ins->v[0])) {
            make_copy(ins, ins->v[1]);
            changed = 1;
          } else if (is_empty_str(m, ins->v[1])) {
            make_copy(ins, ins->v[0]);
            changed = 1;
          }
        }
      }
    }
  }
  return changed;
}

int opt_cmp(IrModule *m) {
  int changed = 0;
  for (int fi = 0; fi < m->nfuncs; fi++) {
    IrFunc *f = &m->funcs[fi];
    for (int b = 0; b < f->nblocks; b++) {
      for (int k = 0; k < f->blocks[b].nins; k++) {
        IrInstr *ins = &f->blocks[b].ins[k];
        if (ins->op != IR_CMP || ins->type == IR_FLOAT) {
          continue;
        }
        if (!opt_val_equal(ins->v[0], ins->v[1])) {
          continue;
        }
        long long r = -1;
        if (ins->cond == IR_CEQ || ins->cond == IR_CLE ||
            ins->cond == IR_CGE) {
          r = 1;
        } else {
          r = 0;
        }
        make_int(ins, r);
        changed = 1;
      }
    }
  }
  return changed;
}

/**
 * @brief Tells whether an instruction is safe for local CSE
 * @param ins Instruction
 * @return Non-zero for pure non-allocating operations
 */
static int cse_ok(IrInstr *ins) {
  switch (ins->op) {
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
    return 1;
  default:
    return 0;
  }
}

/**
 * @brief Tells whether two instructions compute the same value
 * @param a First instruction
 * @param b Second instruction
 * @return Non-zero for same opcode, type, condition, and operands
 */
static int cse_same(IrInstr *a, IrInstr *b) {
  if (a->op != b->op || a->type != b->type || a->cond != b->cond ||
      a->nv != b->nv) {
    return 0;
  }
  for (int i = 0; i < a->nv; i++) {
    if (!opt_val_equal(a->v[i], b->v[i])) {
      return 0;
    }
  }
  return 1;
}

int opt_cse(IrModule *m) {
  int changed = 0;
  for (int fi = 0; fi < m->nfuncs; fi++) {
    IrFunc *f = &m->funcs[fi];
    for (int b = 0; b < f->nblocks; b++) {
      IrBlock *blk = &f->blocks[b];
      for (int j = 0; j < blk->nins; j++) {
        IrInstr *cur = &blk->ins[j];
        if (!cse_ok(cur) || cur->dst < 0) {
          continue;
        }
        for (int i = 0; i < j; i++) {
          IrInstr *prev = &blk->ins[i];
          if (!cse_ok(prev) || prev->dst < 0 || !cse_same(prev, cur)) {
            continue;
          }
          /* Operands must not be redefined between the two. */
          int clobbered = 0;
          for (int k = i + 1; k < j && !clobbered; k++) {
            IrInstr *mid = &blk->ins[k];
            if (mid->dst < 0) {
              continue;
            }
            for (int q = 0; q < cur->nv; q++) {
              if ((cur->v[q].kind == IRV_TEMP ||
                   cur->v[q].kind == IRV_VAR) &&
                  cur->v[q].idx == mid->dst) {
                clobbered = 1;
                break;
              }
            }
          }
          if (clobbered) {
            continue;
          }
          IrVal reuse;
          reuse.kind = IRV_TEMP;
          reuse.idx = prev->dst;
          reuse.imm = 0;
          reuse.type = ir_slot_type(f, prev->dst);
          make_copy(cur, reuse);
          changed = 1;
          break;
        }
      }
    }
  }
  return changed;
}

/**
 * @brief Tells whether an opcode may modify globals
 * @param op Operation code
 * @return Non-zero for calls (callees may store globals)
 */
static int clobbers_globals(IrOp op) {
  return op == IR_CALL || op == IR_CALLM;
}

/**
 * @brief Tells whether a store target matches a load target
 * @param store Store instruction (STORE or STOREG)
 * @param is_global Receives non-zero for globals
 * @return Target key (slot or negated global), -1 when not a plain store
 */
static int store_key(IrInstr *store, int *is_global) {
  *is_global = 0;
  if (store->nv != 2) {
    return -1;
  }
  if (store->op == IR_STORE && store->v[0].kind == IRV_VAR) {
    return store->v[0].idx;
  }
  if (store->op == IR_STOREG && store->v[0].kind == IRV_GLOBAL) {
    *is_global = 1;
    return -1000 - store->v[0].idx;
  }
  return -1;
}

/**
 * @brief Tells whether an instruction reads a variable or global
 * @param ins Instruction to inspect
 * @param is_global Non-zero for globals
 * @param key Target key (slot or negated global)
 * @return Non-zero when any operand reads the target
 */
static int reads_target(IrInstr *ins, int is_global, int key) {
  for (int i = 0; i < ins->nv; i++) {
    if (!is_global && ins->v[i].kind == IRV_VAR && ins->v[i].idx == key) {
      return 1;
    }
    if (is_global && ins->v[i].kind == IRV_GLOBAL &&
        -1000 - ins->v[i].idx == key) {
      return 1;
    }
  }
  for (int i = 0; i < ins->nlist; i++) {
    if (!is_global && ins->list[i].kind == IRV_VAR &&
        ins->list[i].idx == key) {
      return 1;
    }
    if (is_global && ins->list[i].kind == IRV_GLOBAL &&
        -1000 - ins->list[i].idx == key) {
      return 1;
    }
  }
  return 0;
}

/**
 * @brief Blanks stores overwritten before any read in a block
 * @param blk Block to clean
 * @return Non-zero when anything changed
 * @details Straight-line only: a second store to the same target with no
 * read between (and no call between for globals) makes the first dead.
 */
static int dead_stores(IrBlock *blk) {
  int changed = 0;
  for (int k = 0; k < blk->nins; k++) {
    IrInstr *ins = &blk->ins[k];
    if (ins->op != IR_STORE && ins->op != IR_STOREG) {
      continue;
    }
    int is_global = 0;
    int key = store_key(ins, &is_global);
    if (key == -1) {
      continue;
    }
    for (int j = k + 1; j < blk->nins; j++) {
      IrInstr *later = &blk->ins[j];
      if (later->op == IR_NOP) {
        continue;
      }
      /* Any operand read keeps the store alive. */
      if (reads_target(later, is_global, key)) {
        break;
      }
      if (is_global && clobbers_globals(later->op)) {
        break;
      }
      int later_global = 0;
      if ((later->op == IR_STORE || later->op == IR_STOREG) &&
          store_key(later, &later_global) == key &&
          later_global == is_global) {
        opt_blank(ins);
        changed = 1;
        break;
      }
    }
  }
  return changed;
}

int opt_loadstore(IrModule *m) {
  int changed = 0;
  for (int fi = 0; fi < m->nfuncs; fi++) {
    IrFunc *f = &m->funcs[fi];
    for (int b = 0; b < f->nblocks; b++) {
      IrBlock *blk = &f->blocks[b];
      if (dead_stores(blk)) {
        changed = 1;
      }
      /* Known values: slot -> temp holding a fresh load. */
      int known_slot[512];
      int known_temp[512];
      int nknown = 0;
      for (int k = 0; k < blk->nins; k++) {
        IrInstr *ins = &blk->ins[k];
        if (ins->op == IR_NOP) {
          continue;
        }
        if (ins->dst >= 0) {
          /* A redefined temp invalidates knowledge about it. */
          for (int q = 0; q < nknown; q++) {
            if (known_temp[q] == ins->dst) {
              known_slot[q] = known_slot[nknown - 1];
              known_temp[q] = known_temp[nknown - 1];
              nknown--;
              q--;
            }
          }
        }
        if (ins->op == IR_LOAD && ins->nv == 1 &&
            ins->v[0].kind == IRV_VAR) {
          int slot = ins->v[0].idx;
          int found = -1;
          for (int q = 0; q < nknown; q++) {
            if (known_slot[q] == slot) {
              found = q;
              break;
            }
          }
          if (found >= 0) {
            IrVal reuse;
            reuse.kind = IRV_TEMP;
            reuse.idx = known_temp[found];
            reuse.imm = 0;
            reuse.type = ir_slot_type(f, known_temp[found]);
            make_copy(ins, reuse);
            changed = 1;
            continue;
          }
          if (nknown < 512) {
            known_slot[nknown] = slot;
            known_temp[nknown] = ins->dst;
            nknown++;
          }
          continue;
        }
        if (ins->op == IR_LOADG && ins->nv == 1 &&
            ins->v[0].kind == IRV_GLOBAL) {
          int slot = -1000 - ins->v[0].idx;
          int found = -1;
          for (int q = 0; q < nknown; q++) {
            if (known_slot[q] == slot) {
              found = q;
              break;
            }
          }
          if (found >= 0) {
            IrVal reuse;
            reuse.kind = IRV_TEMP;
            reuse.idx = known_temp[found];
            reuse.imm = 0;
            reuse.type = ir_slot_type(f, known_temp[found]);
            make_copy(ins, reuse);
            changed = 1;
            continue;
          }
          if (nknown < 512) {
            known_slot[nknown] = slot;
            known_temp[nknown] = ins->dst;
            nknown++;
          }
          continue;
        }
        if (ins->op == IR_STORE && ins->nv == 2 &&
            ins->v[0].kind == IRV_VAR) {
          /* A stored slot is no longer known... unless the stored
             value itself is now the known content. Track it. */
          int slot = ins->v[0].idx;
          int found = -1;
          for (int q = 0; q < nknown; q++) {
            if (known_slot[q] == slot) {
              found = q;
              break;
            }
          }
          if (ins->v[1].kind == IRV_TEMP) {
            if (found >= 0) {
              known_temp[found] = ins->v[1].idx;
            } else if (nknown < 512) {
              known_slot[nknown] = slot;
              known_temp[nknown] = ins->v[1].idx;
              nknown++;
            }
          } else if (found >= 0) {
            known_slot[found] = known_slot[nknown - 1];
            known_temp[found] = known_temp[nknown - 1];
            nknown--;
          }
          continue;
        }
        if (ins->op == IR_STOREG && ins->nv == 2 &&
            ins->v[0].kind == IRV_GLOBAL) {
          int slot = -1000 - ins->v[0].idx;
          for (int q = 0; q < nknown; q++) {
            if (known_slot[q] == slot) {
              known_slot[q] = known_slot[nknown - 1];
              known_temp[q] = known_temp[nknown - 1];
              nknown--;
              break;
            }
          }
          continue;
        }
        if (clobbers_globals(ins->op)) {
          /* Calls may store globals: drop global knowledge. */
          int w = 0;
          for (int q = 0; q < nknown; q++) {
            if (known_slot[q] >= -1000) {
              known_slot[w] = known_slot[q];
              known_temp[w] = known_temp[q];
              w++;
            }
          }
          nknown = w;
        }
      }
    }
  }
  return changed;
}
