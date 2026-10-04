#include "parser.h"

/** Current token being parsed */
static Token *current_token;

/** Forward declaration for recursive parsing */
static Node *parse_expression();

/** Forward declaration for statement parsing */
static Node *parse_statement();

/**
 * @brief Parses a block of statements up to a closing brace
 * @return First statement in block (linked via right), NULL if empty
 */
static Node *parse_block_statements();

/**
 * @brief Parses a function call expression (name already verified)
 * @return AST node for function call
 */
static Node *parse_func_call_expr();

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
 * @brief Parses a function call expression
 * @return AST node for function call
 */
static Node *parse_func_call_expr() {
  Node *node = create_node(NODE_FUNC_CALL);
  size_t len = strlen(current_token->value);
  node->func_call.name = malloc(len + 1);
  memcpy(node->func_call.name, current_token->value, len);
  node->func_call.name[len] = '\0';
  current_token++;

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, "(") != 0) {
    printf("Error: Expected '(' after function name\n");
    exit(1);
  }
  current_token++;

  node->func_call.args = NULL;
  Node *tail = NULL;

  if (current_token->type != END_OF_TOKENS &&
      strcmp(current_token->value, ")") != 0) {
    while (1) {
      Node *arg = parse_expression();
      if (node->func_call.args == NULL) {
        node->func_call.args = arg;
        tail = arg;
      } else {
        tail->right = arg;
        tail = arg;
      }
      if (current_token->type != END_OF_TOKENS &&
          strcmp(current_token->value, ",") == 0) {
        current_token++;
        continue;
      }
      break;
    }
  }

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ")") != 0) {
    printf("Error: Expected ')' after function call\n");
    exit(1);
  }
  current_token++;

  return node;
}

/**
 * @brief Parses a primary expression (literals, identifiers, calls,
 * parenthesized expressions)
 * @return AST node for primary expression
 */
static Node *parse_primary() {
  if (current_token->type == END_OF_TOKENS) {
    printf("Error: Unexpected end of input in expression\n");
    exit(1);
  }
  if (current_token->type == INT) {
    return parse_int_literal();
  } else if (current_token->type == IDENTIFIER) {
    if (current_token[1].value != NULL &&
        strcmp(current_token[1].value, "(") == 0) {
      return parse_func_call_expr();
    }
    return parse_identifier();
  } else if (current_token->type == STRING) {
    return parse_string_literal();
  } else if (strcmp(current_token->value, "(") == 0) {
    current_token++;
    Node *expr = parse_expression();
    if (current_token->type == END_OF_TOKENS ||
        strcmp(current_token->value, ")") != 0) {
      printf("Error: Expected closing parenthesis\n");
      exit(1);
    }
    current_token++;
    return expr;
  } else if (strcmp(current_token->value, "[") == 0) {
    Node *node = create_node(NODE_ARRAY_LITERAL);
    current_token++;

    node->array_literal.elements = NULL;
    Node *tail = NULL;

    if (current_token->type != END_OF_TOKENS &&
        strcmp(current_token->value, "]") != 0) {
      while (1) {
        Node *element = parse_expression();
        if (node->array_literal.elements == NULL) {
          node->array_literal.elements = element;
          tail = element;
        } else {
          tail->right = element;
          tail = element;
        }
        if (current_token->type != END_OF_TOKENS &&
            strcmp(current_token->value, ",") == 0) {
          current_token++;
          continue;
        }
        break;
      }
    }

    if (current_token->type == END_OF_TOKENS ||
        strcmp(current_token->value, "]") != 0) {
      printf("Error: Expected ']' after array literal\n");
      exit(1);
    }
    current_token++;
    return node;
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
    op->binary_op.op = malloc(len + 1);
    memcpy(op->binary_op.op, current_token->value, len);
    op->binary_op.op[len] = '\0';
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

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, "(") != 0) {
    printf("Error: Expected '(' after return\n");
    exit(1);
  }
  current_token++;

  node->return_stmt.value = parse_expression();

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ")") != 0) {
    printf("Error: Expected ')' after return value\n");
    exit(1);
  }
  current_token++;

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ";") != 0) {
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

  node->var_decl.value = NULL;

  if (current_token->type != END_OF_TOKENS &&
      strcmp(current_token->value, "=") == 0) {
    current_token++;
    node->var_decl.value = parse_expression();
  }

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ";") != 0) {
    printf("Error: Expected ';' after variable declaration\n");
    exit(1);
  }
  current_token++;

  return node;
}

/**
 * @brief Parses an assignment statement
 * @return AST node for assignment statement
 */
static Node *parse_assignment() {
  Node *node = create_node(NODE_ASSIGNMENT);
  size_t len = strlen(current_token->value);
  node->assignment.name = malloc(len + 1);
  memcpy(node->assignment.name, current_token->value, len);
  node->assignment.name[len] = '\0';
  current_token++;

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, "=") != 0) {
    printf("Error: Expected '=' after identifier\n");
    exit(1);
  }
  current_token++;

  node->assignment.value = parse_expression();

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ";") != 0) {
    printf("Error: Expected ';' after assignment\n");
    exit(1);
  }
  current_token++;

  return node;
}

/**
 * @brief Parses a block of statements up to a closing brace
 * @return First statement in block (linked via right), NULL if empty
 */
static Node *parse_block_statements() {
  Node *head = NULL;
  Node *tail = NULL;

  while (current_token->type != END_OF_TOKENS &&
         (current_token->value == NULL ||
          strcmp(current_token->value, "}") != 0)) {
    Node *stmt = parse_statement();
    if (head == NULL) {
      head = stmt;
      tail = stmt;
    } else {
      tail->right = stmt;
      tail = stmt;
    }
  }

  return head;
}

/**
 * @brief Parses a print statement
 * @return AST node for print statement
 */
static Node *parse_print() {
  Node *node = create_node(NODE_PRINT);
  current_token++;

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, "(") != 0) {
    printf("Error: Expected '(' after print\n");
    exit(1);
  }
  current_token++;

  node->print_stmt.value = parse_expression();

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ")") != 0) {
    printf("Error: Expected ')' after print value\n");
    exit(1);
  }
  current_token++;

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ";") != 0) {
    printf("Error: Expected ';' after print statement\n");
    exit(1);
  }
  current_token++;

  return node;
}

/**
 * @brief Parses an if/else statement
 * @return AST node for if statement
 */
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

  node->if_stmt.body = parse_block_statements();

  if (strcmp(current_token->value, "}") != 0) {
    printf("Error: Expected '}' after if body\n");
    exit(1);
  }
  current_token++;

  if (current_token->type == KEYWORD &&
      strcmp(current_token->value, "else") == 0) {
    current_token++;
    if (current_token->type == KEYWORD &&
        strcmp(current_token->value, "if") == 0) {
      node->if_stmt.else_body = parse_if();
    } else {
      if (strcmp(current_token->value, "{") != 0) {
        printf("Error: Expected '{' after else\n");
        exit(1);
      }
      current_token++;
      node->if_stmt.else_body = parse_block_statements();
      if (strcmp(current_token->value, "}") != 0) {
        printf("Error: Expected '}' after else body\n");
        exit(1);
      }
      current_token++;
    }
  } else {
    node->if_stmt.else_body = NULL;
  }

  return node;
}

/**
 * @brief Parses a while loop
 * @return AST node for while loop
 */
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

  node->while_stmt.body = parse_block_statements();

  if (strcmp(current_token->value, "}") != 0) {
    printf("Error: Expected '}' after while body\n");
    exit(1);
  }
  current_token++;

  return node;
}

/**
 * @brief Parses a for loop
 * @return AST node for for loop
 */
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

  node->for_stmt.body = parse_block_statements();

  if (strcmp(current_token->value, "}") != 0) {
    printf("Error: Expected '}' after for body\n");
    exit(1);
  }
  current_token++;

  return node;
}

/**
 * @brief Parses an array declaration
 * @return AST node for array declaration
 */
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
  Node *tail = NULL;

  if (current_token->type != END_OF_TOKENS &&
      strcmp(current_token->value, "]") != 0) {
    while (1) {
      Node *element = parse_expression();
      if (node->array_decl.elements->array_literal.elements == NULL) {
        node->array_decl.elements->array_literal.elements = element;
        tail = element;
      } else {
        tail->right = element;
        tail = element;
      }
      if (current_token->type != END_OF_TOKENS &&
          strcmp(current_token->value, ",") == 0) {
        current_token++;
        continue;
      }
      break;
    }
  }

  if (strcmp(current_token->value, "]") != 0) {
    printf("Error: Expected ']' after array elements\n");
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

/**
 * @brief Parses a function definition
 * @return AST node for function definition
 */
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

  node->function.params = NULL;
  Node *param_tail = NULL;

  if (current_token->type != END_OF_TOKENS &&
      strcmp(current_token->value, ")") != 0) {
    while (1) {
      Node *param = NULL;
      if (current_token->type == KEYWORD &&
          (strcmp(current_token->value, "int") == 0 ||
           strcmp(current_token->value, "bool") == 0 ||
           strcmp(current_token->value, "string") == 0 ||
           strcmp(current_token->value, "char") == 0 ||
           strcmp(current_token->value, "array") == 0)) {
        param = create_node(NODE_VAR_DECL);
        len = strlen(current_token->value);
        param->var_decl.var_type = malloc(len + 1);
        memcpy(param->var_decl.var_type, current_token->value, len);
        param->var_decl.var_type[len] = '\0';
        current_token++;
        if (current_token->type != IDENTIFIER) {
          printf("Error: Expected identifier after type in parameter list\n");
          exit(1);
        }
        len = strlen(current_token->value);
        param->var_decl.name = malloc(len + 1);
        memcpy(param->var_decl.name, current_token->value, len);
        param->var_decl.name[len] = '\0';
        param->var_decl.value = NULL;
        current_token++;
      } else if (current_token->type == IDENTIFIER) {
        param = create_node(NODE_IDENTIFIER);
        len = strlen(current_token->value);
        param->identifier.name = malloc(len + 1);
        memcpy(param->identifier.name, current_token->value, len);
        param->identifier.name[len] = '\0';
        current_token++;
      } else {
        printf("Error: Expected parameter in function definition\n");
        exit(1);
      }

      if (node->function.params == NULL) {
        node->function.params = param;
        param_tail = param;
      } else {
        param_tail->right = param;
        param_tail = param;
      }

      if (current_token->type != END_OF_TOKENS &&
          strcmp(current_token->value, ",") == 0) {
        current_token++;
        continue;
      }
      break;
    }
  }

  if (strcmp(current_token->value, ")") != 0) {
    printf("Error: Expected ')' after function parameters\n");
    exit(1);
  }
  current_token++;

  if (strcmp(current_token->value, "{") != 0) {
    printf("Error: Expected '{' after function declaration\n");
    exit(1);
  }
  current_token++;

  node->function.body = parse_block_statements();

  if (strcmp(current_token->value, "}") != 0) {
    printf("Error: Expected '}' after function body\n");
    exit(1);
  }
  current_token++;

  return node;
}

/**
 * @brief Parses a single statement
 * @return AST node for statement
 */
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
  } else if (current_token->type == IDENTIFIER) {
    if (current_token[1].value != NULL &&
        strcmp(current_token[1].value, "(") == 0) {
      Node *call = parse_func_call_expr();
      if (current_token->type == END_OF_TOKENS ||
          strcmp(current_token->value, ";") != 0) {
        printf("Error: Expected ';' after function call\n");
        exit(1);
      }
      current_token++;
      return call;
    } else if (current_token[1].value != NULL &&
               strcmp(current_token[1].value, "=") == 0) {
      return parse_assignment();
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
    printf("Function(%s, params: ", root->function.name);
    print_tree(root->function.params);
    printf(", body: ");
    print_tree(root->function.body);
    printf(")");
    break;
  case NODE_VAR_DECL:
    printf("VarDecl(%s %s", root->var_decl.var_type, root->var_decl.name);
    if (root->var_decl.value) {
      printf(" = ");
      print_tree(root->var_decl.value);
    }
    printf(")");
    break;
  case NODE_ARRAY_DECL:
    printf("ArrayDecl(%s = ", root->array_decl.name);
    print_tree(root->array_decl.elements);
    printf(")");
    break;
  case NODE_IF:
    printf("If(cond: ");
    print_tree(root->if_stmt.condition);
    printf(", then: ");
    print_tree(root->if_stmt.body);
    printf(", else: ");
    print_tree(root->if_stmt.else_body);
    printf(")");
    break;
  case NODE_WHILE:
    printf("While(cond: ");
    print_tree(root->while_stmt.condition);
    printf(", body: ");
    print_tree(root->while_stmt.body);
    printf(")");
    break;
  case NODE_FOR:
    printf("For(%s in %s, body: ", root->for_stmt.var_name,
           root->for_stmt.array_name);
    print_tree(root->for_stmt.body);
    printf(")");
    break;
  case NODE_PRINT:
    printf("Print(");
    print_tree(root->print_stmt.value);
    printf(")");
    break;
  case NODE_RETURN:
    printf("Return(");
    print_tree(root->return_stmt.value);
    printf(")");
    break;
  case NODE_BINARY_OP:
    printf("BinOp(%s, ", root->binary_op.op);
    print_tree(root->binary_op.left);
    printf(", ");
    print_tree(root->binary_op.right);
    printf(")");
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
    printf("ArrayLiteral(");
    print_tree(root->array_literal.elements);
    printf(")");
    break;
  case NODE_FUNC_CALL:
    printf("Call(%s, args: ", root->func_call.name);
    print_tree(root->func_call.args);
    printf(")");
    break;
  case NODE_ASSIGNMENT:
    printf("Assign(%s = ", root->assignment.name);
    print_tree(root->assignment.value);
    printf(")");
    break;
  default:
    printf("Unknown");
    break;
  }

  if (root->right) {
    printf(", ");
    print_tree(root->right);
  }
}
