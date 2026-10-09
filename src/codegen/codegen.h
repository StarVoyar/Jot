#ifndef CODEGEN_H
#define CODEGEN_H

#include "../ir/ir.h"
#include "../terminal/terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Generates NASM x86-64 assembly from optimized IR
 * @param mod IR module (functions, globals, strings, vtables)
 * @param source Source file path used in error messages
 * @param output Output assembly file path
 * @details Pure emission: all names, types, and layouts are resolved.
 */
void GenerateAssembly(IrModule *mod, const char *source, const char *output);

#endif
