#ifndef OPT_H
#define OPT_H

#include "../ir/ir.h"

/**
 * @brief Optimization levels
 * @details O0 emits straightforward code, O1 enables safe basic passes,
 * O2 adds local common-subexpression elimination on top.
 */
typedef enum {
  OPT_O0, /**< No optimization passes */
  OPT_O1, /**< Safe basic optimizations */
  OPT_O2  /**< O1 plus more aggressive (but proven) passes */
} OptLevel;

/**
 * @brief Runs the optimization pipeline at a level
 * @param m Module to optimize in place
 * @param level Optimization level
 */
void opt_run(IrModule *m, OptLevel level);

/**
 * @brief Folds constant expressions (never folds trapping or
 * non-finite float operations)
 * @param m Module to optimize
 * @return Non-zero when anything changed
 */
int opt_fold(IrModule *m);

/**
 * @brief Propagates constants into uses
 * @param m Module to optimize
 * @return Non-zero when anything changed
 */
int opt_prop(IrModule *m);

/**
 * @brief Eliminates redundant copies (single-def temps)
 * @param m Module to optimize
 * @return Non-zero when anything changed
 */
int opt_copies(IrModule *m);

/**
 * @brief Removes dead side-effect-free definitions
 * @param m Module to optimize
 * @return Non-zero when anything changed
 */
int opt_dce(IrModule *m);

/**
 * @brief Removes unreachable blocks and renumbers
 * @param m Module to optimize
 * @return Non-zero when anything changed
 */
int opt_unreachable(IrModule *m);

/**
 * @brief Simplifies branches (const conditions, jump threading)
 * @param m Module to optimize
 * @return Non-zero when anything changed
 */
int opt_branch(IrModule *m);

/**
 * @brief Merges single-predecessor jump chains into predecessors
 * @param m Module to optimize
 * @return Non-zero when anything changed
 */
int opt_blocks(IrModule *m);

/**
 * @brief Algebraic identities (int and exact float ones only)
 * @param m Module to optimize
 * @return Non-zero when anything changed
 */
int opt_algebra(IrModule *m);

/**
 * @brief Self-comparison simplification (never for floats)
 * @param m Module to optimize
 * @return Non-zero when anything changed
 */
int opt_cmp(IrModule *m);

/**
 * @brief Local common-subexpression elimination within blocks
 * @param m Module to optimize
 * @return Non-zero when anything changed
 */
int opt_cse(IrModule *m);

/**
 * @brief Safe within-block load/store elimination
 * @param m Module to optimize
 * @return Non-zero when anything changed
 */
int opt_loadstore(IrModule *m);

#endif
