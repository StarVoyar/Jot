#ifndef CODEGEN_H
#define CODEGEN_H

#include "../ir/ir.h"
#include "../terminal/terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Generates assembly from optimized IR for the selected target
 * @param mod IR module (functions, globals, strings, vtables)
 * @param source Source file path used in error messages
 * @param output Output assembly file path
 * @param target "win64", "elf64", or NULL for the host default
 * @details Runs register allocation per function, then emits through
 * instruction selection. Pure emission: all names, types, and layouts
 * are resolved.
 */
void GenerateAssembly(IrModule *mod, const char *source, const char *output,
                      const char *target);

/**
 * @brief Generates assembly into an already-open stream
 * @param mod IR module (functions, globals, strings, vtables)
 * @param source Source file path used in error messages
 * @param out Open text stream receiving NASM assembly (LF line endings)
 * @param target "win64", "elf64", or NULL for the host default
 * @details Shared emission core behind GenerateAssembly; used for
 * --emit-asm to stdout without touching the filesystem.
 */
void GenerateAssemblyFp(IrModule *mod, const char *source, FILE *out,
                        const char *target);

#endif
