#ifndef X86_H
#define X86_H

#include "../ir/ir.h"
#include "regalloc.h"
#include "target.h"

#include <stdio.h>

/**
 * @brief Names a register for NASM output
 * @param reg Register number
 * @return 64-bit name ("rax", "xmm0", ...)
 */
const char *reg_name(int reg);

/**
 * @brief Names the low 8 bits of a GP register (for setcc)
 * @param reg GP register number
 * @return 8-bit name ("al", "r8b", ...)
 */
const char *reg_name8(int reg);

/**
 * @brief Emits one function with register-allocated temps
 * @param m Module (globals, strings, classes)
 * @param f Function to emit
 * @param fi Function index (for block labels)
 * @param is_entry Non-zero for the entry program (no epilogue)
 * @param t Target descriptor (convention, register classes)
 * @param a Allocation result for the function
 * @param out Assembly stream
 */
void x86_emit_func(IrModule *m, IrFunc *f, int fi, int is_entry,
                   const Target *t, const RegAlloc *a, FILE *out);

/**
 * @brief Emits the data section (formats, globals, strings, vtables)
 * @param m Module
 * @param out Assembly stream
 */
void x86_data_section(IrModule *m, FILE *out);

/**
 * @brief Emits the runtime trap handlers
 * @param t Target descriptor (argument register for printf/exit)
 * @param out Assembly stream
 */
void x86_emit_traps(const Target *t, FILE *out);

/**
 * @brief Emits a safety halt after the entry program
 * @param t Target descriptor
 * @param out Assembly stream
 */
void x86_emit_halt(const Target *t, FILE *out);

#endif
