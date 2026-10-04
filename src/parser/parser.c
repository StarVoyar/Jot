#include "parser.h"

/** Current token being parsed */
static Token *current_token;

/** Forward declaration for recursive parsing */
static Node *parse_expression();

/**
 * @brief Creates a new AST node
 * @param type Type of node to create
 * @return Newly allocated node
 */
static Node *create_node(NodeType type) {
  Node *node = malloc(sizeof(Node));
  node->type = type;
  node->left = NULL;
  node->right = NULL;
  return node;
}

/**
 * @brief Parses an integer literal
 * @return AST node for integer literal
 */
static Node *parse_int_literal() {
  Node *node = create_node(NODE_INT_LITERAL);
  node->int_literal.value = atoi(current_token->value);
  current_token++;
  return node;
}

/**
 * @brief Parses an identifier
 * @return AST node for identifier
 */
static Node *parse_identifier() {
  Node *node = create_node(NODE_IDENTIFIER);
  size_t len = strlen(current_token->value);
  node->identifier.name = malloc(len + 1);
  memcpy(node->identifier.name, current_token->value, len);
  node->identifier.name[len] = '\0';
  current_token++;
  return node;
}

/**
 * @brief Parses a string literal
 * @return AST node for string literal
 */
static Node *parse_string_literal() {
  Node *node = create_node(NODE_STRING_LITERAL);
  size_t len = strlen(current_token->value);
  node->string_literal.value = malloc(len + 1);
  memcpy(node->string_literal.value, current_token->value, len);
  node->string_literal.value[len] = '\0';
  current_token++;
  return node;
}

/**
 * @brief Parses a primary expression (literals, identifiers, parenthesized expressions)
 * @return AST node for primary expression
 */
static Node *parse_primary() {
  if (current_token->type == INT) {
    return parse_int_literal();
  } else if (current_token->type == IDENTIFIER) {
    return parse_identifier();
  } else if (current_token->type == STRING) {
    return parse_string_literal();
  } else if (strcmp(current_token->value, "(") == 0) {
    current_token++;
    Node *expr = parse_expression();
    if (strcmp(current_token->value, ")") != 0) {
      printf("Error: Expected closing parenthesis\n");
      exit(1);
    }
    current_token++;
    return expr;
  }
  printf("Error: Unexpected token in expression\n");
  exit(1);
}

/**
 * @brief Parses an expression with binary operations
 * @return AST node for expression
 */
static Node *parse_expression() {
  Node *left = parse_primary();

  while (current_token->type == OPERATOR) {
    Node *op = create_node(NODE_BINARY_OP);
    size_t len = strlen(current_token->value);
    op->binary_op.operator = malloc(len + 1);
    memcpy(op->binary_op.operator, current_token->value, len);
    op->binary_op.operator[len] = '\0';
    current_token++;

    op->binary_op.left = left;
    op->binary_op.right = parse_primary();
    left = op;
  }

  return left;
}

/**
 * @brief Parses a return statement
 * @return AST node for return statement
 */
static Node *parse_return() {
  Node *node = create_node(NODE_RETURN);
  current_token++;

  if (strcmp(current_token->value, "(") != 0) {
    printf("Error: Expected '(' after return\n");
    exit(1);
  }
  current_token++;

  node->return_stmt.value = parse_expression();

  if (strcmp(current_token->value, ")") != 0) {
    printf("Error: Expected ')' after return value\n");
    exit(1);
  }
  current_token++;

  if (strcmp(current_token->value, ";") != 0) {
    printf("Error: Expected ';' after return statement\n");
    exit(1);
  }
  current_token++;

  return node;
}

/**
 * @brief Parses a variable declaration
 * @return AST node for variable declaration
 */
static Node *parse_var_decl() {
  Node *node = create_node(NODE_VAR_DECL);
  size_t len = strlen(current_token->value);
  node->var_decl.var_type = malloc(len + 1);
  memcpy(node->var_decl.var_type, current_token->value, len);
  node->var_decl.var_type[len] = '\0';
  current_token++;

  if (current_token->type != IDENTIFIER) {
    printf("Error: Expected identifier after type\n");
    exit(1);
  }
  len = strlen(current_token->value);
  node->var_decl.name = malloc(len + 1);
  memcpy(node->var_decl.name, current_token->value, len);
  node->var_decl.name[len] = '\0';
  current_token++;

  if (strcmp(current_token->value, "=") == 0) {
    current_token++;
    node->var_decl.value = parse_expression();
  }

  if (strcmp(current_token->value, ";") != 0) {
    printf("Error: Expected ';' after variable declaration\n");
    exit(1);
  }
  current_token++;

  return node;
}

/**
 * @brief Parses a print statement
 * @return AST node for print statement
 */
static Node *parse_print() {
  Node *node = create_node(NODE_PRINT);
  current_token++;

  if (strcmp(current_token->value, "(") != 0) {
    printf("Error: Expected '(' after print\n");
    exit(1);
  }
  current_token++;

  node->print_stmt.value = parse_expression();

  if (strcmp(current_token->value, ")") != 0) {
    printf("Error: Expected ')' after print value\n");
    exit(1);
  }
  current_token++;

  if (strcmp(current_token->value, ";") != 0) {
    printf("Error: Expected ';' after print statement\n");
    exit(1);
  }
  current_token++;

  return node;
}

static Node *parse_if() {
  Node *node = create_node(NODE_IF);
  current_token++;

  if (strcmp(current_token->value, "(") != 0) {
    printf("Error: Expected '(' after if\n");
    exit(1);
  }
  current_token++;

  node->if_stmt.condition = parse_expression();

  if (strcmp(current_token->value, ")") != 0) {
    printf("Error: Expected ')' after if condition\n");
    exit(1);
  }
  current_token++;

  if (strcmp(current_token->value, "{") != 0) {
    printf("Error: Expected '{' after if condition\n");
    exit(1);
  }
  current_token++;

  node->if_stmt.body = create_node(NODE_FUNCTION);
  node->if_stmt.body->function.body = NULL;

  if (strcmp(current_token->value, "}") != 0) {
    printf("Error: if body parsing not implemented\n");
    exit(1);
  }
  current_token++;

  if (strcmp(current_token->value, "else") == 0) {
    current_token++;
    if (strcmp(current_token->value, "if") == 0) {
      printf("Error: else if not implemented\n");
      exit(1);
    }
    if (strcmp(current_token->value, "{") != 0) {
      printf("Error: Expected '{' after else\n");
      exit(1);
    }
    current_token++;
    node->if_stmt.else_body = create_node(NODE_FUNCTION);
    node->if_stmt.else_body->function.body = NULL;
    if (strcmp(current_token->value, "}") != 0) {
      printf("Error: else body parsing not implemented\n");
      exit(1);
    }
    current_token++;
  } else {
    node->if_stmt.else_body = NULL;
  }

  return node;
}

static Node *parse_while() {
  Node *node = create_node(NODE_WHILE);
  current_token++;

  if (strcmp(current_token->value, "(") != 0) {
    printf("Error: Expected '(' after while\n");
    exit(1);
  }
  current_token++;

  node->while_stmt.condition = parse_expression();

  if (strcmp(current_token->value, ")") != 0) {
    printf("Error: Expected ')' after while condition\n");
    exit(1);
  }
  current_token++;

  if (strcmp(current_token->value, "{") != 0) {
    printf("Error: Expected '{' after while condition\n");
    exit(1);
  }
  current_token++;

  node->while_stmt.body = create_node(NODE_FUNCTION);
  node->while_stmt.body->function.body = NULL;

  if (strcmp(current_token->value, "}") != 0) {
    printf("Error: while body parsing not implemented\n");
    exit(1);
  }
  current_token++;

  return node;
}

static Node *parse_for() {
  Node *node = create_node(NODE_FOR);
  current_token++;

  if (strcmp(current_token->value, "(") != 0) {
    printf("Error: Expected '(' after for\n");
    exit(1);
  }
  current_token++;

  if (current_token->type != IDENTIFIER) {
    printf("Error: Expected identifier in for loop\n");
    exit(1);
  }
  size_t len = strlen(current_token->value);
  node->for_stmt.var_name = malloc(len + 1);
  memcpy(node->for_stmt.var_name, current_token->value, len);
  node->for_stmt.var_name[len] = '\0';
  current_token++;

  if (strcmp(current_token->value, "in") != 0) {
    printf("Error: Expected 'in' in for loop\n");
    exit(1);
  }
  current_token++;

  if (current_token->type != IDENTIFIER) {
    printf("Error: Expected array identifier in for loop\n");
    exit(1);
  }
  len = strlen(current_token->value);
  node->for_stmt.array_name = malloc(len + 1);
  memcpy(node->for_stmt.array_name, current_token->value, len);
  node->for_stmt.array_name[len] = '\0';
  current_token++;

  if (strcmp(current_token->value, ")") != 0) {
    printf("Error: Expected ')' after for loop declaration\n");
    exit(1);
  }
  current_token++;

  if (strcmp(current_token->value, "{") != 0) {
    printf("Error: Expected '{' after for loop\n");
    exit(1);
  }
  current_token++;

  node->for_stmt.body = create_node(NODE_FUNCTION);
  node->for_stmt.body->function.body = NULL;

  if (strcmp(current_token->value, "}") != 0) {
    printf("Error: for body parsing not implemented\n");
    exit(1);
  }
  current_token++;

  return node;
}

static Node *parse_array_decl() {
  Node *node = create_node(NODE_ARRAY_DECL);
  current_token++;

  if (current_token->type != IDENTIFIER) {
    printf("Error: Expected identifier after array\n");
    exit(1);
  }
  size_t len = strlen(current_token->value);
  node->array_decl.name = malloc(len + 1);
  memcpy(node->array_decl.name, current_token->value, len);
  node->array_decl.name[len] = '\0';
  current_token++;

  if (strcmp(current_token->value, "=") != 0) {
    printf("Error: Expected '=' after array name\n");
    exit(1);
  }
  current_token++;

  if (strcmp(current_token->value, "[") != 0) {
    printf("Error: Expected '[' for array literal\n");
    exit(1);
  }
  current_token++;

  node->array_decl.elements = create_node(NODE_ARRAY_LITERAL);
  node->array_decl.elements->array_literal.elements = NULL;

  if (strcmp(current_token->value, "]") != 0) {
    printf("Error: Array element parsing not implemented\n");
    exit(1);
  }
  current_token++;

  if (strcmp(current_token->value, ";") != 0) {
    printf("Error: Expected ';' after array declaration\n");
    exit(1);
  }
  current_token++;

  return node;
}

static Node *parse_function() {
  Node *node = create_node(NODE_FUNCTION);
  current_token++;

  if (current_token->type != IDENTIFIER) {
    printf("Error: Expected function name\n");
    exit(1);
  }
  size_t len = strlen(current_token->value);
  node->function.name = malloc(len + 1);
  memcpy(node->function.name, current_token->value, len);
  node->function.name[len] = '\0';
  current_token++;

  if (strcmp(current_token->value, "(") != 0) {
    printf("Error: Expected '(' after function name\n");
    exit(1);
  }
  current_token++;

  if (strcmp(current_token->value, ")") != 0) {
    printf("Error: Function parameters not implemented\n");
    exit(1);
  }
  current_token++;

  if (strcmp(current_token->value, "{") != 0) {
    printf("Error: Expected '{' after function declaration\n");
    exit(1);
  }
  current_token++;

  node->function.body = create_node(NODE_FUNCTION);
  node->function.body->function.body = NULL;

  if (strcmp(current_token->value, "}") != 0) {
    printf("Error: Function body parsing not implemented\n");
    exit(1);
  }
  current_token++;

  return node;
}

static Node *parse_statement() {
  if (current_token->type == KEYWORD) {
    if (strcmp(current_token->value, "return") == 0) {
      return parse_return();
    } else if (strcmp(current_token->value, "if") == 0) {
      return parse_if();
    } else if (strcmp(current_token->value, "while") == 0) {
      return parse_while();
    } else if (strcmp(current_token->value, "for") == 0) {
      return parse_for();
    } else if (strcmp(current_token->value, "print") == 0) {
      return parse_print();
    } else if (strcmp(current_token->value, "int") == 0 ||
               strcmp(current_token->value, "bool") == 0 ||
               strcmp(current_token->value, "string") == 0 ||
               strcmp(current_token->value, "char") == 0) {
      return parse_var_decl();
    } else if (strcmp(current_token->value, "array") == 0) {
      return parse_array_decl();
    } else if (strcmp(current_token->value, "fn") == 0) {
      return parse_function();
    }
  }
  printf("Error: Unexpected token in statement\n");
  exit(1);
}

Node *Parser(Token *tokens) {
  current_token = tokens;

  Node *statements = NULL;
  Node *current = NULL;

  while (current_token->type != END_OF_TOKENS) {
    Node *stmt = parse_statement();

    if (statements == NULL) {
      statements = stmt;
      current = stmt;
    } else {
      current->right = stmt;
      current = stmt;
    }
  }

  return statements;
}

void print_tree(Node *root) {
  if (!root) {
    printf("NULL");
    return;
  }

  switch (root->type) {
  case NODE_FUNCTION:
    printf("Function(%s)", root->function.name);
    break;
  case NODE_VAR_DECL:
    printf("VarDecl(%s %s)", root->var_decl.var_type, root->var_decl.name);
    break;
  case NODE_ARRAY_DECL:
    printf("ArrayDecl(%s)", root->array_decl.name);
    break;
  case NODE_IF:
    printf("If");
    break;
  case NODE_WHILE:
    printf("While");
    break;
  case NODE_FOR:
    printf("For(%s in %s)", root->for_stmt.var_name, root->for_stmt.array_name);
    break;
  case NODE_PRINT:
    printf("Print");
    break;
  case NODE_RETURN:
    printf("Return");
    break;
  case NODE_BINARY_OP:
    printf("BinOp(%s)", root->binary_op.operator);
    break;
  case NODE_IDENTIFIER:
    printf("Id(%s)", root->identifier.name);
    break;
  case NODE_INT_LITERAL:
    printf("Int(%d)", root->int_literal.value);
    break;
  case NODE_STRING_LITERAL:
    printf("String(\"%s\")", root->string_literal.value);
    break;
  case NODE_ARRAY_LITERAL:
    printf("ArrayLiteral");
    break;
  default:
    printf("Unknown");
    break;
  }

  if (root->left || root->right) {
    printf("[");
    if (root->left) {
      print_tree(root->left);
    }
    if (root->right) {
      printf(", ");
      print_tree(root->right);
    }
    printf("]");
  }
}
