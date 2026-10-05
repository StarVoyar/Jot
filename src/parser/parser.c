#define _CRT_SECURE_NO_WARNINGS

#include "parser.h"

/** Current token being parsed */
static Token *current_token;

/** Source file name for error messages */
static const char *source_filename;

/** First top level statement (functions merge here from imports) */
static Node *program_head;

/** Last top level statement */
static Node *program_tail;

/** Maximum nested import depth */
#define MAX_IMPORT_DEPTH 64

/** Files currently being parsed (cycle detection, borrowed pointers) */
static const char *import_stack[MAX_IMPORT_DEPTH];

/** Nested import depth */
static int import_depth;

/** Already imported files (borrowed pointers owned by the done list) */
static const char *import_done[MAX_IMPORT_DEPTH];

/** Parsed roots of completed imports, parallel to import_done */
static Node *import_roots[MAX_IMPORT_DEPTH];

/** Number of completed imports */
static int import_done_count;

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

/** Forward declaration for import parsing */
static Node *parse_import();

/** Forward declaration for error reporting */
static NORETURN void parse_error(const char *message);

/**
 * @brief Appends a statement to the program list
 * @param stmt Statement node to append, NULL is ignored
 */
static void emit_statement(Node *stmt) {
  if (stmt == NULL) {
    return;
  }
  if (program_head == NULL) {
    program_head = stmt;
    program_tail = stmt;
  } else {
    program_tail->right = stmt;
    program_tail = stmt;
  }
}

/**
 * @brief Pushes a file onto the import stack
 * @param filename File being parsed
 */
static void push_import(const char *filename) {
  if (import_depth >= MAX_IMPORT_DEPTH) {
    parse_error("Import depth exceeded\n");
  }
  import_stack[import_depth] = filename;
  import_depth++;
}

/**
 * @brief Pops a file off the import stack
 */
static void pop_import() { import_depth--; }

/**
 * @brief Checks if a file is already being parsed
 * @param path File path to look up
 * @return Non-zero if the file is on the import stack
 */
static int import_in_progress(const char *path) {
  for (int i = 0; i < import_depth; i++) {
    if (strcmp(import_stack[i], path) == 0) {
      return 1;
    }
  }
  return 0;
}

/**
 * @brief Checks if a file was already imported
 * @param path File path to look up
 * @return Non-zero if the file is fully imported
 */
static int import_is_done(const char *path) {
  for (int i = 0; i < import_done_count; i++) {
    if (strcmp(import_done[i], path) == 0) {
      return 1;
    }
  }
  return 0;
}

/**
 * @brief Prints an error at an explicit position and exits
 * @param line 1-based line number
 * @param col 1-based column number
 * @param width Squiggle width, at least 1
 * @param message Error message without the Error: prefix
 */
static NORETURN void parse_error_at(int line, int col, int width,
                                    const char *message) {
  term_report(TERM_ERROR, source_filename, line, col, width, message);
  exit(1);
}

/**
 * @brief Prints a warning at an explicit position
 * @param line 1-based line number
 * @param col 1-based column number
 * @param width Squiggle width, at least 1
 * @param message Warning message without the Warning: prefix
 */
static void parse_warning_at(int line, int col, int width,
                             const char *message) {
  term_report(TERM_WARNING, source_filename, line, col, width, message);
}

/**
 * @brief Display width of a raw column range with tab expansion
 * @param line 1-based line number
 * @param from_col 1-based start column, inclusive
 * @param to_col 1-based end column, exclusive
 * @return Display width, at least 1
 */
static int display_span(int line, int from_col, int to_col) {
  int text_len = 0;
  const char *text = lexer_source_line(line, &text_len);
  if (text == NULL || to_col <= from_col) {
    return 1;
  }
  int start = 0;
  for (int i = 0; i < from_col - 1 && i < text_len; i++) {
    if (text[i] == '\t') {
      start = ((start / 4) + 1) * 4;
    } else {
      start++;
    }
  }
  int end = start;
  for (int i = from_col - 1; i < to_col - 1 && i < text_len; i++) {
    if (text[i] == '\t') {
      end = ((end / 4) + 1) * 4;
    } else {
      end++;
    }
  }
  if (end <= start) {
    return 1;
  }
  return end - start;
}
/**
 * @brief Finds a defined function by name
 * @param root List to search
 * @param name Function name to find
 * @return Function node or NULL
 */
static Node *find_function_in(Node *root, const char *name) {
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_FUNCTION && strcmp(s->function.name, name) == 0) {
      return s;
    }
  }
  return NULL;
}

/**
 * @brief Errors if a function name is already defined
 * @param name Function name to check
 * @param line Position line for the error
 * @param col Position column for the error
 * @param width Squiggle width for the error
 */
static void check_duplicate_fn(const char *name, int line, int col, int width) {
  if (find_function_in(program_head, name) != NULL) {
    char message[96];
    snprintf(message, sizeof(message), "Function '%s' is already defined",
             name);
    parse_error_at(line, col, width, message);
  }
}

/**
 * @brief Gathers call nodes in a subtree
 * @param node Subtree root (follows right chains)
 * @param out_calls Found call nodes, may repeat
 * @param out_count Number found so far, updated
 * @param cap Capacity of out_calls
 */
static void collect_calls(Node *node, Node **out_calls, int *out_count,
                          int cap) {
  for (Node *s = node; s != NULL; s = s->right) {
    if (s->type == NODE_FUNC_CALL) {
      if (*out_count < cap) {
        out_calls[*out_count] = s;
        (*out_count)++;
      }
    }
    switch (s->type) {
    case NODE_FUNCTION:
      collect_calls(s->function.body, out_calls, out_count, cap);
      break;
    case NODE_VAR_DECL:
      collect_calls(s->var_decl.value, out_calls, out_count, cap);
      break;
    case NODE_ARRAY_DECL:
      collect_calls(s->array_decl.elements, out_calls, out_count, cap);
      break;
    case NODE_IF:
      collect_calls(s->if_stmt.condition, out_calls, out_count, cap);
      collect_calls(s->if_stmt.body, out_calls, out_count, cap);
      collect_calls(s->if_stmt.else_body, out_calls, out_count, cap);
      break;
    case NODE_WHILE:
      collect_calls(s->while_stmt.condition, out_calls, out_count, cap);
      collect_calls(s->while_stmt.body, out_calls, out_count, cap);
      break;
    case NODE_FOR:
      collect_calls(s->for_stmt.body, out_calls, out_count, cap);
      break;
    case NODE_PRINT:
      collect_calls(s->print_stmt.value, out_calls, out_count, cap);
      break;
    case NODE_RETURN:
      collect_calls(s->return_stmt.value, out_calls, out_count, cap);
      break;
    case NODE_BINARY_OP:
      collect_calls(s->binary_op.left, out_calls, out_count, cap);
      collect_calls(s->binary_op.right, out_calls, out_count, cap);
      break;
    case NODE_FUNC_CALL:
      collect_calls(s->func_call.args, out_calls, out_count, cap);
      break;
    case NODE_ASSIGNMENT:
      collect_calls(s->assignment.value, out_calls, out_count, cap);
      break;
    case NODE_ARRAY_LITERAL:
      collect_calls(s->array_literal.elements, out_calls, out_count, cap);
      break;
    default:
      break;
    }
  }
}

/**
 * @brief Width of the current token for squiggles
 * @return Display width, at least 1
 */
static int token_width() {
  if (current_token->value != NULL && current_token->value[0] != '\0') {
    return (int)strlen(current_token->value);
  }
  return 1;
}
/**
 * @brief Checks if a position starts its line (only whitespace before it)
 * @param line 1-based line number
 * @param col 1-based column number
 * @return Non-zero if nothing but whitespace precedes the position
 */
static int is_first_on_line(int line, int col) {
  int text_len = 0;
  const char *text = lexer_source_line(line, &text_len);
  if (text == NULL) {
    return 0;
  }
  int i = 0;
  while (i < col - 1 && i < text_len &&
         (text[i] == ' ' || text[i] == '\t' || text[i] == '\r')) {
    i++;
  }
  return i == col - 1;
}

/**
 * @brief Moves a position back to the end of the previous code line
 * @param line Current line, updated in place
 * @param col Current column, updated in place
 * @return Non-zero if placed at the end of real code
 */
static int step_to_prev_code_end(int *line, int *col) {
  int probe_len = 0;
  while (*line > 1) {
    const char *probe_text = lexer_source_line(*line - 1, &probe_len);
    if (probe_text == NULL) {
      break;
    }
    int i = 0;
    while (i < probe_len && (probe_text[i] == ' ' || probe_text[i] == '\t' ||
                             probe_text[i] == '\r')) {
      i++;
    }
    if (i < probe_len) {
      (*line)--;
      *col = probe_len + 1;
      return 1;
    }
    (*line)--;
  }
  return 0;
}

/**
 * @brief Prints a clang-style error with source context and exits
 * @param message Error message without the Error: prefix
 */
static NORETURN void parse_error(const char *message) {
  int line = current_token->line;
  int col = current_token->col;
  if (line < 1) {
    line = 1;
  }
  if (col < 1) {
    col = 1;
  }

  if (current_token->type == END_OF_TOKENS && is_first_on_line(line, col)) {
    step_to_prev_code_end(&line, &col);
  }

  term_report(TERM_ERROR, source_filename, line, col, token_width(), message);
  exit(1);
}

/**
 * @brief Prints an expected-token error, favouring the insertion point
 * @param message Error message without the Error: prefix
 * @details If the offending token starts its line, the missing token
 * almost certainly belongs at the end of the previous code line, so
 * the caret goes there instead of at the offender.
 */
static NORETURN void parse_error_expected(const char *message) {
  int line = current_token->line;
  int col = current_token->col;
  if (line < 1) {
    line = 1;
  }
  if (col < 1) {
    col = 1;
  }

  int width = token_width();
  if (is_first_on_line(line, col)) {
    if (step_to_prev_code_end(&line, &col)) {
      width = 1;
    }
  }

  term_report(TERM_ERROR, source_filename, line, col, width, message);
  exit(1);
}

/**
 * @brief Display width from a position to the end of its line
 * @param line 1-based line number
 * @param col 1-based column number
 * @return Display width, at least 1
 */
static int line_rest_width(int line, int col) {
  int text_len = 0;
  const char *text = lexer_source_line(line, &text_len);
  if (text == NULL) {
    return 1;
  }
  int start = 0;
  for (int i = 0; i < col - 1 && i < text_len; i++) {
    if (text[i] == '\t') {
      start = ((start / 4) + 1) * 4;
    } else {
      start++;
    }
  }
  int end = start;
  for (int i = col - 1; i < text_len; i++) {
    if (text[i] == '\t') {
      end = ((end / 4) + 1) * 4;
    } else {
      end++;
    }
  }
  if (end <= start) {
    return 1;
  }
  return end - start;
}

/**
 * @brief Prints a gcc-style warning with source context
 * @param message Warning message without the Warning: prefix
 */
static void parse_warning(const char *message) {
  term_report(TERM_WARNING, source_filename, current_token->line,
              current_token->col, token_width(), message);
}

/**
 * @brief Prints a gcc-style warning spanning the rest of the line
 * @param message Warning message without the Warning: prefix
 */
static void parse_warning_line(const char *message) {
  term_report(
      TERM_WARNING, source_filename, current_token->line, current_token->col,
      line_rest_width(current_token->line, current_token->col), message);
}

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
  node->line = current_token->line;
  node->col = current_token->col;
  node->width = 1;
  if (current_token->value != NULL && current_token->value[0] != '\0') {
    node->width = (int)strlen(current_token->value);
  }
  return node;
}

/**
 * @brief Parses an integer literal
 * @return AST node for integer literal
 */
static Node *parse_int_literal() {
  Node *node = create_node(NODE_INT_LITERAL);
  node->int_literal.value = atoll(current_token->value);
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
 * @brief Parses a member access (object.member)
 * @param object Object identifier node (already parsed)
 * @return AST node for member access
 */
static Node *parse_member_access(Node *object) {
  Node *node = create_node(NODE_MEMBER_ACCESS);
  size_t len = strlen(object->identifier.name);
  node->member_access.object = malloc(len + 1);
  memcpy(node->member_access.object, object->identifier.name, len);
  node->member_access.object[len] = '\0';
  current_token++;

  if (current_token->type != IDENTIFIER) {
    parse_error_expected("Expected member name after '.'\n");
  }
  len = strlen(current_token->value);
  node->member_access.member = malloc(len + 1);
  memcpy(node->member_access.member, current_token->value, len);
  node->member_access.member[len] = '\0';
  node->line = current_token->line;
  node->col = current_token->col;
  node->width = (int)len;
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
    parse_error_expected("Expected '(' after function name\n");
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
    parse_error_expected("Expected ')' after function call\n");
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
    parse_error("Unexpected end of input in expression\n");
  }
  if (current_token->type == INT) {
    return parse_int_literal();
  } else if (current_token->type == IDENTIFIER) {
    if (current_token[1].value != NULL &&
        strcmp(current_token[1].value, "(") == 0) {
      return parse_func_call_expr();
    }
    Node *object = parse_identifier();
    if (current_token->type != END_OF_TOKENS &&
        strcmp(current_token->value, ".") == 0) {
      return parse_member_access(object);
    }
    return object;
  } else if (current_token->type == STRING) {
    return parse_string_literal();
  } else if (strcmp(current_token->value, "(") == 0) {
    current_token++;
    Node *expr = parse_expression();
    if (current_token->type == END_OF_TOKENS ||
        strcmp(current_token->value, ")") != 0) {
      parse_error_expected("Expected closing parenthesis\n");
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
      parse_error_expected("Expected ']' after array literal\n");
    }
    current_token++;
    return node;
  }
  if (current_token->value != NULL && strcmp(current_token->value, "{") == 0) {
    parse_error(
        "Unexpected '{' in expression ('{...}' only works inside strings)\n");
  }
  if (current_token->value != NULL) {
    char message[96];
    snprintf(message, sizeof(message), "Unexpected '%s' in expression",
             current_token->value);
    parse_error(message);
  }
  parse_error("Unexpected token in expression\n");
}

/**
 * @brief Creates a binary operation node for the current operator
 * @param left Left operand (already parsed)
 * @return New node with operator filled, right operand unset
 */
static Node *take_operator(Node *left) {
  Node *op = create_node(NODE_BINARY_OP);
  size_t len = strlen(current_token->value);
  op->binary_op.op = malloc(len + 1);
  memcpy(op->binary_op.op, current_token->value, len);
  op->binary_op.op[len] = '\0';
  current_token++;

  op->binary_op.left = left;
  return op;
}

/**
 * @brief Parses *, / and % (highest precedence, left-associative)
 * @return AST node for expression
 */
static Node *parse_multiplicative() {
  Node *left = parse_primary();

  while (current_token->type == OPERATOR &&
         (strcmp(current_token->value, "*") == 0 ||
          strcmp(current_token->value, "/") == 0 ||
          strcmp(current_token->value, "%") == 0)) {
    Node *op = take_operator(left);
    op->binary_op.right = parse_primary();
    left = op;
  }

  return left;
}

/**
 * @brief Parses + and - (left-associative)
 * @return AST node for expression
 */
static Node *parse_additive() {
  Node *left = parse_multiplicative();

  while (current_token->type == OPERATOR &&
         (strcmp(current_token->value, "+") == 0 ||
          strcmp(current_token->value, "-") == 0)) {
    Node *op = take_operator(left);
    op->binary_op.right = parse_multiplicative();
    left = op;
  }

  return left;
}

/**
 * @brief Parses <, >, <= and >= (left-associative)
 * @return AST node for expression
 */
static Node *parse_relational() {
  Node *left = parse_additive();

  while (current_token->type == OPERATOR &&
         (strcmp(current_token->value, "<") == 0 ||
          strcmp(current_token->value, ">") == 0 ||
          strcmp(current_token->value, "<=") == 0 ||
          strcmp(current_token->value, ">=") == 0)) {
    Node *op = take_operator(left);
    op->binary_op.right = parse_additive();
    left = op;
  }

  return left;
}

/**
 * @brief Parses == and != (lowest precedence, left-associative)
 * @return AST node for expression
 */
static Node *parse_equality() {
  Node *left = parse_relational();

  while (current_token->type == OPERATOR &&
         (strcmp(current_token->value, "==") == 0 ||
          strcmp(current_token->value, "!=") == 0)) {
    Node *op = take_operator(left);
    op->binary_op.right = parse_relational();
    left = op;
  }

  return left;
}

/**
 * @brief Parses an expression with binary operations
 * @return AST node for expression
 */
static Node *parse_expression() { return parse_equality(); }

/**
 * @brief Parses a return statement
 * @return AST node for return statement
 */
static Node *parse_return() {
  Node *node = create_node(NODE_RETURN);
  current_token++;

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, "(") != 0) {
    parse_error_expected("Expected '(' after return\n");
  }
  current_token++;

  node->return_stmt.value = parse_expression();

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ")") != 0) {
    parse_error_expected("Expected ')' after return value\n");
  }
  current_token++;

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ";") != 0) {
    parse_error_expected("Expected ';' after return statement\n");
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
    parse_error_expected("Expected identifier after type\n");
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
    parse_error_expected("Expected ';' after variable declaration\n");
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
    parse_error_expected("Expected '=' after identifier\n");
  }
  current_token++;

  node->assignment.value = parse_expression();

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ";") != 0) {
    parse_error_expected("Expected ';' after assignment\n");
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
    if (tail != NULL && tail->type == NODE_RETURN) {
      parse_warning_line("Unreachable code after return");
    }
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
    parse_error_expected("Expected '(' after print\n");
  }
  current_token++;

  node->print_stmt.value = parse_expression();

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ")") != 0) {
    parse_error_expected("Expected ')' after print value\n");
  }
  current_token++;

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ";") != 0) {
    parse_error_expected("Expected ';' after print statement\n");
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
    parse_error_expected("Expected '(' after if\n");
  }
  current_token++;

  node->if_stmt.condition = parse_expression();

  if (strcmp(current_token->value, ")") != 0) {
    parse_error_expected("Expected ')' after if condition\n");
  }
  current_token++;

  if (strcmp(current_token->value, "{") != 0) {
    parse_error_expected("Expected '{' after if condition\n");
  }
  current_token++;

  node->if_stmt.body = parse_block_statements();

  if (strcmp(current_token->value, "}") != 0) {
    parse_error_expected("Expected '}' after if body\n");
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
        parse_error_expected("Expected '{' after else\n");
      }
      current_token++;
      node->if_stmt.else_body = parse_block_statements();
      if (strcmp(current_token->value, "}") != 0) {
        parse_error_expected("Expected '}' after else body\n");
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
    parse_error_expected("Expected '(' after while\n");
  }
  current_token++;

  node->while_stmt.condition = parse_expression();

  if (strcmp(current_token->value, ")") != 0) {
    parse_error_expected("Expected ')' after while condition\n");
  }
  current_token++;

  if (strcmp(current_token->value, "{") != 0) {
    parse_error_expected("Expected '{' after while condition\n");
  }
  current_token++;

  node->while_stmt.body = parse_block_statements();

  if (strcmp(current_token->value, "}") != 0) {
    parse_error_expected("Expected '}' after while body\n");
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
    parse_error_expected("Expected '(' after for\n");
  }
  current_token++;

  if (current_token->type != IDENTIFIER) {
    parse_error_expected("Expected identifier in for loop\n");
  }
  size_t len = strlen(current_token->value);
  node->for_stmt.var_name = malloc(len + 1);
  memcpy(node->for_stmt.var_name, current_token->value, len);
  node->for_stmt.var_name[len] = '\0';
  current_token++;

  if (strcmp(current_token->value, "in") != 0) {
    parse_error_expected("Expected 'in' in for loop\n");
  }
  current_token++;

  if (current_token->type != IDENTIFIER) {
    parse_error_expected("Expected array identifier in for loop\n");
  }
  len = strlen(current_token->value);
  node->for_stmt.array_name = malloc(len + 1);
  memcpy(node->for_stmt.array_name, current_token->value, len);
  node->for_stmt.array_name[len] = '\0';
  current_token++;

  if (strcmp(current_token->value, ")") != 0) {
    parse_error_expected("Expected ')' after for loop declaration\n");
  }
  current_token++;

  if (strcmp(current_token->value, "{") != 0) {
    parse_error_expected("Expected '{' after for loop\n");
  }
  current_token++;

  node->for_stmt.body = parse_block_statements();

  if (strcmp(current_token->value, "}") != 0) {
    parse_error_expected("Expected '}' after for body\n");
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
    parse_error_expected("Expected identifier after array\n");
  }
  size_t len = strlen(current_token->value);
  node->array_decl.name = malloc(len + 1);
  memcpy(node->array_decl.name, current_token->value, len);
  node->array_decl.name[len] = '\0';
  current_token++;

  if (strcmp(current_token->value, "=") != 0) {
    parse_error_expected("Expected '=' after array name\n");
  }
  current_token++;

  if (strcmp(current_token->value, "[") != 0) {
    parse_error_expected("Expected '[' for array literal\n");
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
    parse_error_expected("Expected ']' after array elements\n");
  }
  current_token++;

  if (strcmp(current_token->value, ";") != 0) {
    parse_error_expected("Expected ';' after array declaration\n");
  }
  current_token++;

  return node;
}

/**
 * @brief Deep-copies a right-linked node list for import merging
 * @param head First node of the list, may be NULL
 * @return Fresh list with no links into the original
 */
static Node *clone_list(Node *head);

/**
 * @brief Deep-copies an AST subtree for import merging
 * @param node Subtree root, sibling chain is not copied
 * @return Fresh copy with right set to NULL
 */
static Node *clone_node(Node *node) {
  if (node == NULL) {
    return NULL;
  }
  Node *copy = malloc(sizeof(Node));
  copy->type = node->type;
  copy->left = NULL;
  copy->right = NULL;
  copy->line = node->line;
  copy->col = node->col;
  copy->width = node->width;
  size_t len = 0;
  switch (node->type) {
  case NODE_FUNCTION:
    len = strlen(node->function.name);
    copy->function.name = malloc(len + 1);
    memcpy(copy->function.name, node->function.name, len);
    copy->function.name[len] = '\0';
    copy->function.is_public = node->function.is_public;
    copy->function.params = clone_list(node->function.params);
    copy->function.body = clone_list(node->function.body);
    break;
  case NODE_VAR_DECL:
    len = strlen(node->var_decl.var_type);
    copy->var_decl.var_type = malloc(len + 1);
    memcpy(copy->var_decl.var_type, node->var_decl.var_type, len);
    copy->var_decl.var_type[len] = '\0';
    len = strlen(node->var_decl.name);
    copy->var_decl.name = malloc(len + 1);
    memcpy(copy->var_decl.name, node->var_decl.name, len);
    copy->var_decl.name[len] = '\0';
    copy->var_decl.value = clone_node(node->var_decl.value);
    break;
  case NODE_ARRAY_DECL:
    len = strlen(node->array_decl.name);
    copy->array_decl.name = malloc(len + 1);
    memcpy(copy->array_decl.name, node->array_decl.name, len);
    copy->array_decl.name[len] = '\0';
    copy->array_decl.elements = clone_node(node->array_decl.elements);
    break;
  case NODE_IF:
    copy->if_stmt.condition = clone_node(node->if_stmt.condition);
    copy->if_stmt.body = clone_list(node->if_stmt.body);
    copy->if_stmt.else_body = clone_list(node->if_stmt.else_body);
    break;
  case NODE_WHILE:
    copy->while_stmt.condition = clone_node(node->while_stmt.condition);
    copy->while_stmt.body = clone_list(node->while_stmt.body);
    break;
  case NODE_FOR:
    len = strlen(node->for_stmt.var_name);
    copy->for_stmt.var_name = malloc(len + 1);
    memcpy(copy->for_stmt.var_name, node->for_stmt.var_name, len);
    copy->for_stmt.var_name[len] = '\0';
    len = strlen(node->for_stmt.array_name);
    copy->for_stmt.array_name = malloc(len + 1);
    memcpy(copy->for_stmt.array_name, node->for_stmt.array_name, len);
    copy->for_stmt.array_name[len] = '\0';
    copy->for_stmt.body = clone_list(node->for_stmt.body);
    break;
  case NODE_PRINT:
    copy->print_stmt.value = clone_node(node->print_stmt.value);
    break;
  case NODE_RETURN:
    copy->return_stmt.value = clone_node(node->return_stmt.value);
    break;
  case NODE_BINARY_OP:
    len = strlen(node->binary_op.op);
    copy->binary_op.op = malloc(len + 1);
    memcpy(copy->binary_op.op, node->binary_op.op, len);
    copy->binary_op.op[len] = '\0';
    copy->binary_op.left = clone_node(node->binary_op.left);
    copy->binary_op.right = clone_node(node->binary_op.right);
    break;
  case NODE_IDENTIFIER:
    len = strlen(node->identifier.name);
    copy->identifier.name = malloc(len + 1);
    memcpy(copy->identifier.name, node->identifier.name, len);
    copy->identifier.name[len] = '\0';
    break;
  case NODE_INT_LITERAL:
    copy->int_literal.value = node->int_literal.value;
    break;
  case NODE_STRING_LITERAL:
    len = strlen(node->string_literal.value);
    copy->string_literal.value = malloc(len + 1);
    memcpy(copy->string_literal.value, node->string_literal.value, len);
    copy->string_literal.value[len] = '\0';
    break;
  case NODE_ARRAY_LITERAL:
    copy->array_literal.elements = clone_list(node->array_literal.elements);
    break;
  case NODE_FUNC_CALL:
    len = strlen(node->func_call.name);
    copy->func_call.name = malloc(len + 1);
    memcpy(copy->func_call.name, node->func_call.name, len);
    copy->func_call.name[len] = '\0';
    copy->func_call.args = clone_list(node->func_call.args);
    break;
  case NODE_ASSIGNMENT:
    len = strlen(node->assignment.name);
    copy->assignment.name = malloc(len + 1);
    memcpy(copy->assignment.name, node->assignment.name, len);
    copy->assignment.name[len] = '\0';
    copy->assignment.value = clone_node(node->assignment.value);
    break;
  case NODE_MEMBER_ACCESS:
    len = strlen(node->member_access.object);
    copy->member_access.object = malloc(len + 1);
    memcpy(copy->member_access.object, node->member_access.object, len);
    copy->member_access.object[len] = '\0';
    len = strlen(node->member_access.member);
    copy->member_access.member = malloc(len + 1);
    memcpy(copy->member_access.member, node->member_access.member, len);
    copy->member_access.member[len] = '\0';
    break;
  }
  return copy;
}

/**
 * @brief Deep-copies a right-linked node list for import merging
 * @param head First node of the list, may be NULL
 * @return Fresh list with no links into the original
 */
static Node *clone_list(Node *head) {
  Node *new_head = NULL;
  Node *tail = NULL;
  for (Node *s = head; s != NULL; s = s->right) {
    Node *copy = clone_node(s);
    if (new_head == NULL) {
      new_head = copy;
      tail = copy;
    } else {
      tail->right = copy;
      tail = copy;
    }
  }
  return new_head;
}

/**
 * @brief Parses an import statement and merges the functions
 * @return Always NULL (imported functions append to the program directly)
 */
static Node *parse_import() {
  int stmt_line = current_token->line;
  int stmt_col = current_token->col;
  int stmt_width = token_width();
  current_token++;

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, "[") != 0) {
    parse_error_expected("Expected '[' after from\n");
  }
  current_token++;

  char path[512];
  size_t path_len = 0;
  while (current_token->type != END_OF_TOKENS &&
         strcmp(current_token->value, "]") != 0) {
    int ok = 0;
    if (current_token->type == IDENTIFIER || current_token->type == INT) {
      ok = 1;
    } else if (current_token->value != NULL &&
               strlen(current_token->value) == 1 &&
               (current_token->value[0] == '.' ||
                current_token->value[0] == '/' ||
                current_token->value[0] == '\\' ||
                current_token->value[0] == '-' ||
                current_token->value[0] == ':')) {
      ok = 1;
    }
    if (!ok) {
      parse_error("Invalid character in import path\n");
    }
    size_t chunk = strlen(current_token->value);
    if (path_len + chunk >= sizeof(path)) {
      parse_error("Import path too long\n");
    }
    memcpy(path + path_len, current_token->value, chunk);
    path_len += chunk;
    current_token++;
  }
  if (current_token->type == END_OF_TOKENS) {
    parse_error_expected("Expected ']' after import path\n");
  }
  if (path_len == 0) {
    parse_error("Empty import path\n");
  }
  current_token++;
  path[path_len] = '\0';

  if (current_token->type != IDENTIFIER ||
      strcmp(current_token->value, "import") != 0) {
    parse_error_expected("Expected 'import' after import path\n");
  }
  current_token++;

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, "[") != 0) {
    parse_error_expected("Expected '[' after import\n");
  }
  current_token++;

  int empty_list = current_token->type != END_OF_TOKENS &&
                   strcmp(current_token->value, "]") == 0;
  if (empty_list) {
    parse_warning("Empty import list\n");
  }

  char *names[64];
  int name_count = 0;
  int star = 0;
  while (!empty_list) {
    if (current_token->type == OPERATOR &&
        strcmp(current_token->value, "*") == 0) {
      if (name_count > 0) {
        parse_warning("Redundant names with import *\n");
      }
      star = 1;
      current_token++;
    } else if (current_token->type == IDENTIFIER) {
      if (star) {
        parse_warning("Redundant names with import *\n");
      }
      if (name_count >= 64) {
        parse_error("Too many imports\n");
      }
      size_t len = strlen(current_token->value);
      names[name_count] = malloc(len + 1);
      memcpy(names[name_count], current_token->value, len);
      names[name_count][len] = '\0';
      name_count++;
      current_token++;
    } else {
      parse_error_expected("Expected import name\n");
    }
    if (current_token->type != END_OF_TOKENS &&
        strcmp(current_token->value, ",") == 0) {
      current_token++;
      continue;
    }
    if (current_token->type != END_OF_TOKENS &&
        strcmp(current_token->value, "]") == 0) {
      break;
    }
    parse_error_expected("Expected ',' or ']' in import list\n");
  }
  current_token++;

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ";") != 0) {
    parse_error_expected("Expected ';' after import statement\n");
  }
  current_token++;

  FILE *target = fopen(path, "r");
  if (!target) {
    char message[640];
    snprintf(message, sizeof(message), "Could not open file '%s'", path);
    parse_error_at(stmt_line, stmt_col, stmt_width, message);
  }
  if (import_in_progress(path)) {
    char message[640];
    snprintf(message, sizeof(message), "Import cycle detected for '%s'", path);
    parse_error_at(stmt_line, stmt_col, stmt_width, message);
  }

  Node *sub_root = NULL;
  if (!import_is_done(path)) {
    Token *saved_token = current_token;
    const char *saved_source = source_filename;
    Node *saved_head = program_head;
    Node *saved_tail = program_tail;
    LexerSnapshot lexer_state;
    lexer_save(&lexer_state);

    program_head = NULL;
    program_tail = NULL;
    Token *sub_tokens = Lexer(target);
    sub_root = Parser(sub_tokens, path);

    lexer_restore(&lexer_state);
    current_token = saved_token;
    source_filename = saved_source;
    program_head = saved_head;
    program_tail = saved_tail;

    if (import_done_count >= MAX_IMPORT_DEPTH) {
      parse_error_at(stmt_line, stmt_col, stmt_width,
                     "Too many imported files\n");
    }
    size_t copy_len = strlen(path);
    char *path_copy = malloc(copy_len + 1);
    memcpy(path_copy, path, copy_len);
    path_copy[copy_len] = '\0';
    import_done[import_done_count] = path_copy;
    import_roots[import_done_count] = sub_root;
    import_done_count++;
  } else {
    for (int i = 0; i < import_done_count; i++) {
      if (strcmp(import_done[i], path) == 0) {
        sub_root = import_roots[i];
        break;
      }
    }
  }
  for (int i = 0; i < name_count; i++) {
    Node *found = find_function_in(sub_root, names[i]);
    if (found == NULL) {
      char message[640];
      snprintf(message, sizeof(message), "Function '%s' is not defined in '%s'",
               names[i], path);
      parse_error_at(stmt_line, stmt_col, stmt_width, message);
    }
    if (!found->function.is_public) {
      char message[640];
      snprintf(message, sizeof(message), "Function '%s' is private in '%s'",
               names[i], path);
      parse_error_at(stmt_line, stmt_col, stmt_width, message);
    }
    check_duplicate_fn(names[i], stmt_line, stmt_col, stmt_width);
    emit_statement(clone_node(found));
  }

  if (star) {
    for (Node *s = sub_root; s != NULL; s = s->right) {
      if (s->type != NODE_FUNCTION || !s->function.is_public) {
        continue;
      }
      int listed = 0;
      for (int i = 0; i < name_count; i++) {
        if (strcmp(names[i], s->function.name) == 0) {
          listed = 1;
          break;
        }
      }
      if (listed) {
        continue;
      }
      check_duplicate_fn(s->function.name, stmt_line, stmt_col, stmt_width);
      emit_statement(clone_node(s));
    }
  }

  return NULL;
}

/**
 * @brief Parses a function definition
 * @return AST node for function definition
 */
static Node *parse_function() {
  Node *node = create_node(NODE_FUNCTION);
  current_token++;

  int is_public = 0;
  int has_visibility = 0;
  if (current_token->type == IDENTIFIER &&
      (strcmp(current_token->value, "public") == 0 ||
       strcmp(current_token->value, "private") == 0)) {
    has_visibility = 1;
    is_public = strcmp(current_token->value, "public") == 0;
    current_token++;
  }

  if (current_token->type != IDENTIFIER) {
    parse_error_expected("Expected function name\n");
  }
  size_t len = strlen(current_token->value);
  node->function.name = malloc(len + 1);
  memcpy(node->function.name, current_token->value, len);
  node->function.name[len] = '\0';
  if (strcmp(node->function.name, "input") == 0) {
    parse_error("Function name 'input' is reserved\n");
  }
  check_duplicate_fn(node->function.name, current_token->line,
                     current_token->col, token_width());
  int name_line = current_token->line;
  int name_col = current_token->col;
  int name_width = token_width();
  current_token++;

  if (strcmp(current_token->value, "(") != 0) {
    parse_error_expected("Expected '(' after function name\n");
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
           strcmp(current_token->value, "array") == 0)) {
        param = create_node(NODE_VAR_DECL);
        len = strlen(current_token->value);
        param->var_decl.var_type = malloc(len + 1);
        memcpy(param->var_decl.var_type, current_token->value, len);
        param->var_decl.var_type[len] = '\0';
        current_token++;
        if (current_token->type != IDENTIFIER) {
          parse_error_expected(
              "Expected identifier after type in parameter list\n");
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
        parse_error_expected("Expected parameter in function definition\n");
      }

      if (node->function.params == NULL) {
        node->function.params = param;
        param_tail = param;
      } else {
        param_tail->right = param;
        param_tail = param;
      }

      const char *pname = param->type == NODE_VAR_DECL ? param->var_decl.name
                                                       : param->identifier.name;
      for (Node *p = node->function.params; p != NULL; p = p->right) {
        if (p == param) {
          continue;
        }
        const char *other =
            p->type == NODE_VAR_DECL ? p->var_decl.name : p->identifier.name;
        if (strcmp(other, pname) == 0) {
          char message[96];
          snprintf(message, sizeof(message), "Duplicate parameter '%s'", pname);
          parse_error(message);
        }
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
    parse_error_expected("Expected ')' after function parameters\n");
  }
  int paren_line = current_token->line;
  int paren_col = current_token->col;
  current_token++;

  if (!has_visibility) {
    char message[96];
    if (strcmp(node->function.name, "main") == 0) {
      node->function.is_public = 1;
      snprintf(message, sizeof(message),
               "Function 'main' has no visibility, defaulting to public\n");
    } else {
      node->function.is_public = 0;
      snprintf(message, sizeof(message),
               "Function '%s' has no visibility, defaulting to private\n",
               node->function.name);
    }
    if (paren_line == node->line) {
      int width = display_span(node->line, node->col, paren_col + 1);
      parse_warning_at(node->line, node->col, width, message);
    } else {
      parse_warning_at(name_line, name_col, name_width, message);
    }
  } else {
    node->function.is_public = is_public;
  }

  if (strcmp(current_token->value, "{") != 0) {
    parse_error_expected("Expected '{' after function declaration\n");
  }
  current_token++;

  node->function.body = parse_block_statements();

  if (strcmp(current_token->value, "}") != 0) {
    parse_error_expected("Expected '}' after function body\n");
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
               strcmp(current_token->value, "string") == 0) {
      return parse_var_decl();
    } else if (strcmp(current_token->value, "array") == 0) {
      return parse_array_decl();
    } else if (strcmp(current_token->value, "fn") == 0) {
      return parse_function();
    }
  } else if (current_token->type == IDENTIFIER) {
    if (strcmp(current_token->value, "from") == 0 &&
        current_token[1].value != NULL &&
        strcmp(current_token[1].value, "[") == 0) {
      return parse_import();
    }
    if (strcmp(current_token->value, "import") == 0 &&
        current_token[1].value != NULL &&
        strcmp(current_token[1].value, "[") == 0) {
      parse_error(
          "Expected 'from' before import, use from [file] import [...]\n");
    }
    if (current_token[1].value != NULL &&
        strcmp(current_token[1].value, "(") == 0) {
      Node *call = parse_func_call_expr();
      if (current_token->type == END_OF_TOKENS ||
          strcmp(current_token->value, ";") != 0) {
        parse_error_expected("Expected ';' after function call\n");
      }
      current_token++;
      return call;
    } else if (current_token[1].value != NULL &&
               strcmp(current_token[1].value, "=") == 0) {
      return parse_assignment();
    } else if (current_token[1].value != NULL &&
               strcmp(current_token[1].value, ".") == 0) {
      parse_error("Unexpected member access in statement\n");
    }
  }
  if (current_token->value != NULL) {
    char message[96];
    snprintf(message, sizeof(message), "Unexpected '%s' in statement",
             current_token->value);
    parse_error(message);
  }
  parse_error("Unexpected token in statement\n");
}

Node *Parser(Token *tokens, const char *filename) {
  source_filename = filename;
  current_token = tokens;
  program_head = NULL;
  program_tail = NULL;
  push_import(filename);

  Node *current = NULL;

  while (current_token->type != END_OF_TOKENS) {
    if (current != NULL && current->type == NODE_RETURN) {
      parse_warning_line("Unreachable code after return");
    }
    Node *stmt = parse_statement();
    emit_statement(stmt);
    if (stmt != NULL) {
      current = stmt;
    }
  }

  int progressed = 1;
  while (progressed) {
    progressed = 0;
    for (Node *s = program_head; s != NULL; s = s->right) {
      Node *calls[1024];
      int call_count = 0;
      collect_calls(s, calls, &call_count, 1024);
      for (int k = 0; k < call_count; k++) {
        if (find_function_in(program_head, calls[k]->func_call.name) != NULL) {
          continue;
        }
        Node *dep = NULL;
        for (int r = 0; r < import_done_count && dep == NULL; r++) {
          dep = find_function_in(import_roots[r], calls[k]->func_call.name);
        }
        if (dep == NULL) {
          continue;
        }
        emit_statement(clone_node(dep));
        progressed = 1;
      }
    }
  }

  if (import_depth == 1) {
    for (Node *s = program_head; s != NULL; s = s->right) {
      Node *calls[1024];
      int call_count = 0;
      collect_calls(s, calls, &call_count, 1024);
      for (int k = 0; k < call_count; k++) {
        if (strcmp(calls[k]->func_call.name, "input") == 0) {
          continue;
        }
        Node *def = find_function_in(program_head, calls[k]->func_call.name);
        if (def == NULL) {
          char message[96];
          snprintf(message, sizeof(message), "Function '%s' is not defined",
                   calls[k]->func_call.name);
          parse_error_at(calls[k]->line, calls[k]->col, calls[k]->width,
                         message);
        }
        int want = 0;
        for (Node *p = def->function.params; p != NULL; p = p->right) {
          want++;
        }
        int got = 0;
        for (Node *a = calls[k]->func_call.args; a != NULL; a = a->right) {
          got++;
        }
        if (want != got) {
          char message[96];
          snprintf(message, sizeof(message), "Expected %d arguments, got %d",
                   want, got);
          parse_error_at(calls[k]->line, calls[k]->col, calls[k]->width,
                         message);
        }
      }
    }
  }

  pop_import();

  Node *root = program_head;
  program_head = NULL;
  program_tail = NULL;
  return root;
}

void print_tree(Node *root) {
  if (!root) {
    printf("NULL");
    return;
  }

  switch (root->type) {
  case NODE_FUNCTION:
    printf("Function(%s %s, params: ",
           root->function.is_public ? "public" : "private",
           root->function.name);
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
    printf("Int(%lld)", root->int_literal.value);
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
  case NODE_MEMBER_ACCESS:
    printf("Member(%s.%s)", root->member_access.object,
           root->member_access.member);
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
