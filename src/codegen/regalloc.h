#ifndef REGALLOC_H
#define REGALLOC_H

#include "../ir/ir.h"
#include "target.h"

/**
 * @brief Location of one compiler temporary
 */
typedef struct {
  int is_reg; /**< Non-zero when the temp lives in a register */
  int reg;    /**< Register number (valid when is_reg) */
} TempLoc;

/**
 * @brief Allocation result for one function (temps only; variables and
 * globals keep their frame and data slots)
 */
typedef struct {
  TempLoc *temps;        /**< One entry per temp (slot - nvars) */
  int ntemps;            /**< Temp count */
  uint32_t used_saved_gp;  /**< Callee-saved GP regs needing prologue saves */
  uint32_t used_saved_xmm; /**< Callee-saved XMM regs needing saves */
} RegAlloc;

/**
 * @brief Runs linear-scan allocation over a function
 * @param f Function with layout-ordered blocks
 * @param t Target descriptor (decides call-clobbered classes)
 * @param out Receives the allocation (owned, free with regalloc_free)
 * @details Liveness runs over temps only. Intervals overlapping a
 * runtime call avoid call-clobbered registers; the rest prefer them.
 * Spilled temps keep their existing frame slots.
 */
void regalloc_func(IrFunc *f, const Target *t, RegAlloc *out);

/**
 * @brief Frees an allocation result
 * @param a Allocation to free
 */
void regalloc_free(RegAlloc *a);

#endif
