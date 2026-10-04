#ifndef PARSER_H
#define PARSER_H

#include "../lexer/lexer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  NODE_FUNCTION,
  NODE_VAR_DECL,
  NODE_ARRAY_DECL,
  NODE_IF,
  NODE_WHILE,
  NODE_FOR,
  NODE_PRINT,
  NODE_RETURN,
  NODE_BINARY_OP,
  NODE_IDENTIFIER,
  NODE_INT_LITERAL,
  NODE_STRING_LITERAL,
  NODE_ARRAY_LITERAL
} NodeType;

typedef struct Node {
  NodeType type;
  struct Node *left;
  struct Node *right;
  union {
    struct {
      char *name;
      struct Node *body;
    } function;
    struct {
      char *var_type;
      char *name;
      struct Node *value;
    } var_decl;
    struct {
      char *name;
      struct Node *elements;
    } array_decl;
    struct {
      struct Node *condition;
      struct Node *body;
      struct Node *else_body;
    } if_stmt;
    struct {
      struct Node *condition;
      struct Node *body;
    } while_stmt;
    struct {
      char *var_name;
      char *array_name;
      struct Node *body;
    } for_stmt;
    struct {
      struct Node *value;
    } print_stmt;
    struct {
      struct Node *value;
    } return_stmt;
    struct {
      char *operator;
      struct Node *left;
      struct Node *right;
    } binary_op;
    struct {
      char *name;
    } identifier;
    struct {
      int value;
    } int_literal;
    struct {
      char *value;
    } string_literal;
    struct {
      struct Node *elements;
    } array_literal;
  };
} Node;

Node *Parser(Token *tokens);

void print_tree(Node *root);

#endif
