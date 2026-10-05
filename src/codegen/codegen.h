#ifndef CODEGEN_H
#define CODEGEN_H

#include "../parser/parser.h"
#include "../terminal/terminal.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Generates NASM x86-64 assembly for a Jot program
 * @param root Root of the AST (linked list of top level statements)
 * @param source Source file path used in error messages
 * @param output Output assembly file path
 */
void GenerateAssembly(Node *root, const char *source, const char *output);

#endif
