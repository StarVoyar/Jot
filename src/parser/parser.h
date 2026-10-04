#ifndef PARSER_H
#define PARSER_H

#include "../lexer/lexer.h"
#include "../terminal/terminal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Enumeration of AST node types
 */
typedef enum {
  NODE_FUNCTION,       /**< Function definition */
  NODE_VAR_DECL,       /**< Variable declaration */
  NODE_ARRAY_DECL,     /**< Array declaration */
  NODE_IF,             /**< If/else statement */
  NODE_WHILE,          /**< While loop */
  NODE_FOR,            /**< For loop */
  NODE_PRINT,          /**< Print statement */
  NODE_RETURN,         /**< Return statement */
  NODE_BINARY_OP,      /**< Binary operation */
  NODE_IDENTIFIER,     /**< Identifier reference */
  NODE_INT_LITERAL,    /**< Integer literal */
  NODE_STRING_LITERAL, /**< String literal */
  NODE_ARRAY_LITERAL,  /**< Array literal */
  NODE_FUNC_CALL,      /**< Function call */
  NODE_ASSIGNMENT      /**< Assignment statement */
} NodeType;

/**
 * @brief Abstract Syntax Tree node
 */
typedef struct Node {
  NodeType type;      /**< Type of the node */
  struct Node *left;  /**< Unused (reserved) */
  struct Node *right; /**< Next node in statement, argument or element lists */
  union {
    struct {
      char *name; /**< Function name */
      struct Node
          *params; /**< Function parameters (linked via right, NULL if none) */
      struct Node *body; /**< Function body (linked via right, NULL if empty) */
    } function;
    struct {
      char *var_type;     /**< Variable type (int, bool, string, char) */
      char *name;         /**< Variable name */
      struct Node *value; /**< Initial value (optional) */
    } var_decl;
    struct {
      char *name;            /**< Array name */
      struct Node *elements; /**< Array elements */
    } array_decl;
    struct {
      struct Node *condition; /**< If condition */
      struct Node *body;      /**< If body */
      struct Node *else_body; /**< Else body (optional) */
    } if_stmt;
    struct {
      struct Node *condition; /**< While condition */
      struct Node *body;      /**< While body */
    } while_stmt;
    struct {
      char *var_name;    /**< Loop variable name */
      char *array_name;  /**< Array to iterate over */
      struct Node *body; /**< Loop body */
    } for_stmt;
    struct {
      struct Node *value; /**< Value to print */
    } print_stmt;
    struct {
      struct Node *value; /**< Return value */
    } return_stmt;
    struct {
      char *op;           /**< Operator string (+, -, *, /, ==, etc.) */
      struct Node *left;  /**< Left operand */
      struct Node *right; /**< Right operand */
    } binary_op;
    struct {
      char *name; /**< Identifier name */
    } identifier;
    struct {
      int value; /**< Integer value */
    } int_literal;
    struct {
      char *value; /**< String value */
    } string_literal;
    struct {
      struct Node *elements; /**< Array elements (linked via right) */
    } array_literal;
    struct {
      char *name;        /**< Function name */
      struct Node *args; /**< Call arguments (linked via right, NULL if none) */
    } func_call;
    struct {
      char *name;         /**< Variable name */
      struct Node *value; /**< Assigned value */
    } assignment;
  };
} Node;

/**
 * @brief Parses tokens into an Abstract Syntax Tree
 * @param tokens Array of tokens to parse
 * @param filename Source file name used in error messages
 * @return Root node of the AST (linked list of statements)
 */
Node *Parser(Token *tokens, const char *filename);

/**
 * @brief Prints the AST structure for debugging
 * @param root Root node of the AST to print
 */
void print_tree(Node *root);

#endif
