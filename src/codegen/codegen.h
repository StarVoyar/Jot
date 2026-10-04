#ifndef CODEGEN_H
#define CODEGEN_H

#include "../parser/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Generates NASM x86-64 assembly for a Jot program
 * @param root Root of the AST (linked list of top level statements)
 * @param filename Output assembly file path
 */
void GenerateAssembly(Node *root, const char *filename);

#endif
