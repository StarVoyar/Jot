#ifndef OPT_IMPL_H
#define OPT_IMPL_H

#include "opt.h"

/**
 * @brief Tells whether an opcode has no observable side effects
 * @param op Operation code
 * @return Non-zero for pure ALU, conversions, loads of fixed slots
 */
int opt_is_pure(IrOp op);

/**
 * @brief Compares two operands for value equality
 * @param a First operand
 * @param b Second operand
 * @return Non-zero when kind, index, and immediate all match
 */
int opt_val_equal(IrVal a, IrVal b);

/**
 * @brief Counts uses and definitions of a frame slot
 * @param f Function
 * @param slot Frame slot
 * @param uses Receives use count (may be NULL)
 * @param defs Receives definition count (may be NULL)
 */
void opt_slot_info(IrFunc *f, int slot, int *uses, int *defs);

/**
 * @brief Blanks an instruction into a tombstone
 * @param ins Instruction to blank
 */
void opt_blank(IrInstr *ins);

/**
 * @brief Rewrites every use of a slot to a value
 * @param f Function
 * @param slot Frame slot to replace
 * @param v Replacement value
 */
void opt_replace_uses(IrFunc *f, int slot, IrVal v);

/**
 * @brief Removes empty blocks and renumbers targets
 * @param f Function
 * @return Non-zero when blocks were removed
 */
int opt_compact(IrFunc *f);

/**
 * @brief Reads a double from float bits
 * @param bits Double bits
 * @return Double value
 */
double opt_bits_double(long long bits);

/**
 * @brief Writes a double to float bits
 * @param d Double value
 * @return Double bits
 */
long long opt_double_bits(double d);

/**
 * @brief Removes tombstone instructions left by the passes
 * @param m Module to squeeze
 */
void opt_squeeze(IrModule *m);

#endif
