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
 * @brief Blanks an instruction into a tombstone
 * @param ins Instruction to blank
 */
void opt_blank(IrInstr *ins);

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
