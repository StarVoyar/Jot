#include "regalloc.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Tells whether an instruction emits a machine call
 * @param ins Instruction
 * @return Non-zero when registers die across it (runtime or user call)
 * @details Traps are not calls (they never return); RET/EXIT end the
 * function, so nothing observes registers after them.
 */
static int calls_runtime(IrInstr *ins) {
  switch (ins->op) {
  case IR_CALL:
  case IR_CALLM:
  case IR_CONCAT:
  case IR_NEWARR:
  case IR_NEWINST:
  case IR_COPYINST:
  case IR_INPUT:
  case IR_LEN:
  case IR_TOSTR:
  case IR_TONUM:
  case IR_READFILE:
  case IR_WRITEFILE:
  case IR_CHR:
  case IR_ARGS:
  case IR_STR_IDX:
  case IR_PRINT_S:
  case IR_PRINT_V:
    return 1;
  case IR_CMP:
    return ins->type == IR_STR;
  default:
    return 0;
  }
}

/**
 * @brief Temp index of a frame slot, or -1 for non-temps
 * @param f Function
 * @param slot Frame slot
 * @return Temp index, or -1
 */
static int temp_of(IrFunc *f, int slot) {
  if (slot >= f->nvars && slot - f->nvars < f->ntemps) {
    return slot - f->nvars;
  }
  return -1;
}

/**
 * @brief Returns the lowest set bit position in a mask
 * @param mask Register mask (non-zero)
 * @return Bit position
 */
static int lowest_bit(uint32_t mask) {
  int bit = 0;
  while ((mask & 1u) == 0) {
    mask >>= 1;
    bit++;
  }
  return bit;
}

/**
 * @brief Sets a bit in a bitset
 * @param bs Bitset words
 * @param i Bit index
 */
static void bit_set(uint64_t *bs, int i) { bs[i / 64] |= 1ULL << (i % 64); }

/**
 * @brief Tests a bit in a bitset
 * @param bs Bitset words
 * @param i Bit index
 * @return Non-zero when set
 */
static int bit_test(uint64_t *bs, int i) {
  return (bs[i / 64] & (1ULL << (i % 64))) != 0;
}

void regalloc_func(IrFunc *f, const Target *t, RegAlloc *out) {
  memset(out, 0, sizeof(RegAlloc));
  out->ntemps = f->ntemps;
  if (f->ntemps == 0) {
    return;
  }
  out->temps = malloc((size_t)f->ntemps * sizeof(TempLoc));
  for (int i = 0; i < f->ntemps; i++) {
    out->temps[i].is_reg = 0;
    out->temps[i].reg = -1;
  }
  int n = f->ntemps;
  int words = (n + 63) / 64;
  int nb = f->nblocks;

  uint64_t *use = calloc((size_t)nb * words, sizeof(uint64_t));
  uint64_t *def = calloc((size_t)nb * words, sizeof(uint64_t));
  uint64_t *live_in = calloc((size_t)nb * words, sizeof(uint64_t));
  uint64_t *live_out = calloc((size_t)nb * words, sizeof(uint64_t));
  uint64_t *defined = malloc((size_t)words * sizeof(uint64_t));

  /* Per-block use/def: forward walk, uses before any definition count.
     A backward walk would wrongly discard a use that a later
     redefinition in the same block follows (loop indices). */
  for (int b = 0; b < nb; b++) {
    memset(defined, 0, (size_t)words * sizeof(uint64_t));
    IrBlock *blk = &f->blocks[b];
    for (int k = 0; k < blk->nins; k++) {
      IrInstr *ins = &blk->ins[k];
      if (ins->op == IR_NOP) {
        continue;
      }
      for (int i = 0; i < ins->nv; i++) {
        if (ins->v[i].kind == IRV_TEMP) {
          int ti = temp_of(f, ins->v[i].idx);
          if (ti >= 0 && !bit_test(defined, ti)) {
            bit_set(&use[b * words], ti);
          }
        }
      }
      for (int i = 0; i < ins->nlist; i++) {
        if (ins->list[i].kind == IRV_TEMP) {
          int ti = temp_of(f, ins->list[i].idx);
          if (ti >= 0 && !bit_test(defined, ti)) {
            bit_set(&use[b * words], ti);
          }
        }
      }
      if (ins->dst >= 0) {
        int ti = temp_of(f, ins->dst);
        if (ti >= 0) {
          bit_set(defined, ti);
          bit_set(&def[b * words], ti);
        }
      }
    }
  }

  /* Successors: jumps, branches, or layout fallthrough. */
  int *succ0 = malloc((size_t)nb * sizeof(int));
  int *succ1 = malloc((size_t)nb * sizeof(int));
  for (int b = 0; b < nb; b++) {
    succ0[b] = -1;
    succ1[b] = -1;
    IrBlock *blk = &f->blocks[b];
    int last = -1;
    for (int k = blk->nins - 1; k >= 0; k--) {
      if (blk->ins[k].op != IR_NOP) {
        last = k;
        break;
      }
    }
    if (last < 0) {
      if (b + 1 < nb) {
        succ0[b] = b + 1;
      }
      continue;
    }
    IrInstr *term = &blk->ins[last];
    if (term->op == IR_JUMP) {
      succ0[b] = term->t;
    } else if (term->op == IR_BR) {
      succ0[b] = term->t;
      succ1[b] = term->f;
    } else if (term->op == IR_RET || term->op == IR_EXIT) {
      succ0[b] = -1;
    } else if (b + 1 < nb) {
      succ0[b] = b + 1;
    }
  }

  /* Liveness fixpoint over temp bitsets. */
  uint64_t *tmp = malloc((size_t)words * sizeof(uint64_t));
  for (int iter = 0; iter < nb + 2; iter++) {
    int changed = 0;
    for (int b = nb - 1; b >= 0; b--) {
      memset(tmp, 0, (size_t)words * sizeof(uint64_t));
      if (succ0[b] >= 0) {
        for (int w = 0; w < words; w++) {
          tmp[w] |= live_in[succ0[b] * words + w];
        }
      }
      if (succ1[b] >= 0) {
        for (int w = 0; w < words; w++) {
          tmp[w] |= live_in[succ1[b] * words + w];
        }
      }
      for (int w = 0; w < words; w++) {
        if (live_out[b * words + w] != tmp[w]) {
          live_out[b * words + w] = tmp[w];
          changed = 1;
        }
        uint64_t in =
            use[b * words + w] | (tmp[w] & ~def[b * words + w]);
        if (live_in[b * words + w] != in) {
          live_in[b * words + w] = in;
          changed = 1;
        }
      }
    }
    if (!changed) {
      break;
    }
  }
  free(tmp);

  /* Number live instructions in layout order; record defs, uses, calls. */
  int *block_start = malloc((size_t)nb * sizeof(int));
  int *block_end = malloc((size_t)nb * sizeof(int));
  int *first_def = malloc((size_t)n * sizeof(int));
  int *last_use = malloc((size_t)n * sizeof(int));
  for (int i = 0; i < n; i++) {
    first_def[i] = INT_MAX;
    last_use[i] = -1;
  }
  int *call_pos = NULL;
  int ncalls = 0;
  int pos = 0;
  for (int b = 0; b < nb; b++) {
    block_start[b] = pos;
    IrBlock *blk = &f->blocks[b];
    for (int k = 0; k < blk->nins; k++) {
      IrInstr *ins = &blk->ins[k];
      if (ins->op == IR_NOP) {
        continue;
      }
      if (ins->dst >= 0) {
        int ti = temp_of(f, ins->dst);
        if (ti >= 0 && pos < first_def[ti]) {
          first_def[ti] = pos;
        }
      }
      for (int i = 0; i < ins->nv; i++) {
        if (ins->v[i].kind == IRV_TEMP) {
          int ti = temp_of(f, ins->v[i].idx);
          if (ti >= 0 && pos > last_use[ti]) {
            last_use[ti] = pos;
          }
        }
      }
      for (int i = 0; i < ins->nlist; i++) {
        if (ins->list[i].kind == IRV_TEMP) {
          int ti = temp_of(f, ins->list[i].idx);
          if (ti >= 0 && pos > last_use[ti]) {
            last_use[ti] = pos;
          }
        }
      }
      if (calls_runtime(ins)) {
        call_pos = realloc(call_pos, (size_t)(ncalls + 1) * sizeof(int));
        call_pos[ncalls++] = pos;
      }
      pos++;
    }
    block_end[b] = pos - 1;
  }

  /* Intervals, extended by liveness (conservative but safe). */
  int *start = malloc((size_t)n * sizeof(int));
  int *end = malloc((size_t)n * sizeof(int));
  for (int i = 0; i < n; i++) {
    start[i] = first_def[i] == INT_MAX ? last_use[i] : first_def[i];
    if (start[i] < 0) {
      start[i] = 0;
    }
    end[i] = last_use[i];
    if (end[i] < start[i]) {
      end[i] = start[i];
    }
  }
  for (int b = 0; b < nb; b++) {
    for (int i = 0; i < n; i++) {
      if (bit_test(&live_in[b * words], i) && block_start[b] < start[i]) {
        start[i] = block_start[b];
      }
      if (bit_test(&live_out[b * words], i) && block_end[b] > end[i]) {
        end[i] = block_end[b];
      }
    }
  }

  /* Order by start position. */
  int *order = malloc((size_t)n * sizeof(int));
  for (int i = 0; i < n; i++) {
    order[i] = i;
  }
  for (int i = 1; i < n; i++) {
    int key = order[i];
    int j = i - 1;
    while (j >= 0 && start[order[j]] > start[key]) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = key;
  }

  int *active = malloc((size_t)n * sizeof(int));
  int nactive = 0;
  for (int oi = 0; oi < n; oi++) {
    int ti = order[oi];
    int w = 0;
    for (int a = 0; a < nactive; a++) {
      if (end[active[a]] < start[ti]) {
        continue;
      }
      active[w++] = active[a];
    }
    nactive = w;
    uint32_t blocked = 0;
    for (int a = 0; a < nactive; a++) {
      if (out->temps[active[a]].is_reg) {
        blocked |= REG_BIT(out->temps[active[a]].reg);
      }
    }
    int crosses = 0;
    for (int c = 0; c < ncalls; c++) {
      if (start[ti] < call_pos[c] && call_pos[c] < end[ti]) {
        crosses = 1;
        break;
      }
    }
    int is_float = ir_slot_type(f, f->nvars + ti) == IR_FLOAT;
    uint32_t volatile_mask = is_float ? t->volatile_xmm : t->volatile_gp;
    uint32_t saved_mask = is_float ? t->saved_xmm : t->saved_gp;
    int reg = -1;
    if (!crosses) {
      uint32_t free_vol = volatile_mask & ~blocked;
      if (free_vol != 0) {
        reg = lowest_bit(free_vol);
      }
    }
    if (reg < 0) {
      uint32_t free_saved = saved_mask & ~blocked;
      if (free_saved != 0) {
        reg = lowest_bit(free_saved);
      }
    }
    if (reg >= 0) {
      out->temps[ti].is_reg = 1;
      out->temps[ti].reg = reg;
      if ((saved_mask & REG_BIT(reg)) != 0) {
        if (is_float) {
          out->used_saved_xmm |= REG_BIT(reg);
        } else {
          out->used_saved_gp |= REG_BIT(reg);
        }
      }
      /* Keep active sorted by end position. */
      int a = nactive;
      while (a > 0 && end[active[a - 1]] > end[ti]) {
        active[a] = active[a - 1];
        a--;
      }
      active[a] = ti;
      nactive++;
    } else {
      out->temps[ti].is_reg = 0;
      out->temps[ti].reg = -1;
    }
  }

  free(use);
  free(def);
  free(live_in);
  free(live_out);
  free(defined);
  free(succ0);
  free(succ1);
  free(block_start);
  free(block_end);
  free(first_def);
  free(last_use);
  free(call_pos);
  free(start);
  free(end);
  free(order);
  free(active);
}

void regalloc_free(RegAlloc *a) {
  free(a->temps);
  a->temps = NULL;
  a->ntemps = 0;
}
