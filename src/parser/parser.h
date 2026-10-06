#ifndef PARSER_H
#define PARSER_H

#include "../lexer/lexer.h"
#include "../terminal/terminal.h"

#include <setjmp.h>
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
  NODE_FLOAT_LITERAL,  /**< Floating point literal */
  NODE_STRING_LITERAL, /**< String literal */
  NODE_ARRAY_LITERAL,  /**< Array literal */
  NODE_FUNC_CALL,      /**< Function call */
  NODE_ASSIGNMENT,     /**< Assignment statement */
  NODE_ADD_ASSIGN,     /**< Add and assign (+=) */
  NODE_SUB_ASSIGN,     /**< Subtract and assign (-=) */
  NODE_MEMBER_ACCESS,  /**< Member access (object.member) */
  NODE_STRUCT_DEF,     /**< Struct definition */
  NODE_CLASS_DEF,      /**< Class definition */
  NODE_METHOD_DEF,     /**< Method definition (inside classes only) */
  NODE_NEW,            /**< Struct/class instantiation (new Type(args)) */
  NODE_INDEX,          /**< String indexing (base[index]) */
  NODE_METHOD_CALL,    /**< Method call (object.method(args)) */
  NODE_MEMBER_ASSIGN,  /**< Member assignment (object.member = value) */
  NODE_BREAK,          /**< break (inside a loop) */
  NODE_CONTINUE,       /**< continue (inside a loop) */
  NODE_NULL,           /**< null literal (no reference) */
  NODE_INDEX_ASSIGN,   /**< Index a[i] = value / += / -= */
} NodeType;

/**
 * @brief Abstract Syntax Tree node
 */
typedef struct Node {
  NodeType type;      /**< Type of the node */
  struct Node *left;  /**< Unused (reserved) */
  struct Node *right; /**< Next node in statement, argument or element lists */
  int line;           /**< 1-based source line of the construct */
  int col;            /**< 1-based source column of the construct */
  int width;          /**< Source width for squiggles, at least 1 */
  const char *source; /**< File this node came from (borrowed, for imports) */
  union {
    struct {
      char *name;    /**< Function name */
      int is_public; /**< Non-zero if declared public (importable) */
      struct Node
          *params; /**< Function parameters (linked via right, NULL if none) */
      struct Node *body; /**< Function body (linked via right, NULL if empty) */
    } function;
    struct {
      char *var_type;     /**< Variable type (num, bool, string, char) */
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
      long long value; /**< Integer value */
    } int_literal;
    struct {
      double value; /**< Floating point value */
    } float_literal;
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
    struct {
      char *name;         /**< Variable name */
      struct Node *value; /**< Value to add/subtract */
    } add_assign;
    struct {
      char *name;         /**< Variable name */
      struct Node *value; /**< Value to add/subtract */
    } sub_assign;
    struct {
      char *object; /**< Object name (NULL when object_expr is set) */
      char *member; /**< Member name */
      struct Node *object_expr; /**< arr[i] base (NULL for plain identifiers) */
    } member_access;
    struct {
      char *name;          /**< Struct name */
      int is_public;       /**< Non-zero if declared public (importable) */
      struct Node *fields; /**< Field declarations (VAR_DECL, no values) */
    } struct_def;
    struct {
      char *name;           /**< Class name */
      char *base;           /**< Base class name after 'inherit', or NULL */
      int is_public;        /**< Non-zero if declared public (importable) */
      struct Node *fields;  /**< Field declarations (VAR_DECL, no values) */
      struct Node *methods; /**< Method definitions (METHOD_DEF nodes) */
    } class_def;
    struct {
      char *ret_type;      /**< Method return type (num, bool, string) */
      char *name;          /**< Method name */
      int is_public;       /**< Always 0: methods are private to the class */
      struct Node *params; /**< Method parameters (linked via right) */
      struct Node *body;   /**< Method body (linked via right) */
    } method_def;
    struct {
      char *type_name;   /**< Struct/class name after 'new' */
      struct Node *args; /**< Constructor arguments (linked via right) */
    } new_expr;
    struct {
      struct Node *base;  /**< Indexed expression (must be a string) */
      struct Node *index; /**< Index expression (must be a number) */
    } index;
    struct {
      char *object;      /**< Instance name (or "self" inside methods,
                              NULL when object_expr is set) */
      char *method;      /**< Method name */
      struct Node *args; /**< Call arguments (linked via right, NULL if none) */
      struct Node *object_expr; /**< arr[i] base (NULL for plain identifiers) */
    } method_call;
    struct {
      struct Node *base;  /**< Array variable (identifier base) */
      struct Node *index; /**< Index expression */
      char *op;           /**< "=", "+=", or "-=" */
      struct Node *value; /**< Assigned value */
    } index_assign;
    struct {
      char *object;             /**< Instance name (or "self" inside methods,
                                     NULL when object_expr is set) */
      char *member;             /**< Field name */
      char *op;                 /**< Assignment operator ("=", "+=", "-=") */
      struct Node *value;       /**< Assigned value */
      struct Node *object_expr; /**< arr[i] base (NULL for plain identifiers) */
    } member_assign;
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
