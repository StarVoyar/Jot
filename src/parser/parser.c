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

/** Forward declaration for primary parsing (used by unary ~) */
static Node *parse_primary();

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

/** Forward declaration for duplicate type/function checking */
static void check_duplicate_def(const char *name, const char *kind, int line,
                                int col, int width);

/** Jump target for abandoning the current statement after an error */
static jmp_buf stmt_jmp;

/** Forward declaration for error reporting (syncs, then abandons statement) */
static NORETURN void parse_error(const char *message);

/** Forward declaration for the error-sync helper */
static void sync_after_error(void);

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
 * @brief Prints an error at an explicit position and continues
 * @param line 1-based line number
 * @param col 1-based column number
 * @param width Squiggle width, at least 1
 * @param message Error message without the Error: prefix
 * @details Counted via term_report; callers keep going so that all
 * diagnostics print before the driver refuses to compile
 */
static void parse_error_at(int line, int col, int width, const char *message) {
  term_report(TERM_ERROR, source_filename, line, col, width, message);
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
 * @brief Finds a struct definition by name
 * @param root List to search
 * @param name Struct name to find
 * @return Struct node or NULL
 */
static Node *find_struct_in(Node *root, const char *name) {
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_STRUCT_DEF && strcmp(s->struct_def.name, name) == 0) {
      return s;
    }
  }
  return NULL;
}

/**
 * @brief Finds a class definition by name
 * @param root List to search
 * @param name Class name to find
 * @return Class node or NULL
 */
static Node *find_class_in(Node *root, const char *name) {
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_CLASS_DEF && strcmp(s->class_def.name, name) == 0) {
      return s;
    }
  }
  return NULL;
}

/**
 * @brief Finds the class owning a method name
 * @param root List to search (methods are private to their class)
 * @param name Method name to find
 * @return Owning class node or NULL
 */
static Node *find_method_owner(Node *root, const char *name) {
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_CLASS_DEF) {
      for (Node *m = s->class_def.methods; m != NULL; m = m->right) {
        if (m->type == NODE_METHOD_DEF &&
            strcmp(m->method_def.name, name) == 0) {
          return s;
        }
      }
    }
  }
  return NULL;
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
    case NODE_METHOD_CALL:
      collect_calls(s->method_call.args, out_calls, out_count, cap);
      collect_calls(s->method_call.object_expr, out_calls, out_count, cap);
      break;
    case NODE_MEMBER_ACCESS:
      collect_calls(s->member_access.object_expr, out_calls, out_count, cap);
      break;
    case NODE_NEW:
      collect_calls(s->new_expr.args, out_calls, out_count, cap);
      break;
    case NODE_INDEX:
      collect_calls(s->index.base, out_calls, out_count, cap);
      collect_calls(s->index.index, out_calls, out_count, cap);
      break;
    case NODE_MEMBER_ASSIGN:
      collect_calls(s->member_assign.value, out_calls, out_count, cap);
      collect_calls(s->member_assign.object_expr, out_calls, out_count, cap);
      break;
    case NODE_CLASS_DEF:
      for (Node *m = s->class_def.methods; m != NULL; m = m->right) {
        collect_calls(m->method_def.params, out_calls, out_count, cap);
        collect_calls(m->method_def.body, out_calls, out_count, cap);
      }
      break;
    case NODE_METHOD_DEF:
      collect_calls(s->method_def.body, out_calls, out_count, cap);
      break;
    case NODE_ASSIGNMENT:
      collect_calls(s->assignment.value, out_calls, out_count, cap);
      break;
    case NODE_INDEX_ASSIGN:
      collect_calls(s->index_assign.base, out_calls, out_count, cap);
      collect_calls(s->index_assign.value, out_calls, out_count, cap);
      collect_calls(s->index_assign.index, out_calls, out_count, cap);
      break;
    case NODE_ADD_ASSIGN:
      collect_calls(s->add_assign.value, out_calls, out_count, cap);
      break;
    case NODE_SUB_ASSIGN:
      collect_calls(s->sub_assign.value, out_calls, out_count, cap);
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
  sync_after_error();
  longjmp(stmt_jmp, 1);
}

/**
 * @brief Prints an expected-token error, favouring the insertion point
 * @param message Error message without the Error: prefix
 * @details If the offending token starts its line, the missing token
 * almost certainly belongs at the end of the previous code line, so
 * the caret goes there instead of at the offender. Afterwards the
 * token stream syncs and the current statement is abandoned so that
 * remaining errors still print instead of exiting at the first one.
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
  sync_after_error();
  longjmp(stmt_jmp, 1);
}

/**
 * @brief Skips tokens to the next likely statement boundary
 * @details Stops (without consuming) at ',', ')', ']', '{', '}' or any
 * keyword so enclosing constructs can complete normally; consumes a
 * lone ';'. Always advances at least one token (unless at end of input)
 * so error recovery always terminates.
 */
static void sync_after_error(void) {
  if (current_token->type == END_OF_TOKENS) {
    return;
  }
  if (current_token->value != NULL &&
      (strcmp(current_token->value, ",") == 0 ||
       strcmp(current_token->value, ")") == 0 ||
       strcmp(current_token->value, "]") == 0 ||
       strcmp(current_token->value, "{") == 0 ||
       strcmp(current_token->value, "}") == 0)) {
    return;
  }
  if (current_token->type == KEYWORD) {
    return;
  }
  current_token++;
  while (current_token->type != END_OF_TOKENS) {
    if (current_token->value != NULL &&
        (strcmp(current_token->value, ",") == 0 ||
         strcmp(current_token->value, ")") == 0 ||
         strcmp(current_token->value, "]") == 0 ||
         strcmp(current_token->value, "{") == 0 ||
         strcmp(current_token->value, "}") == 0)) {
      return;
    }
    if (current_token->type == KEYWORD) {
      return;
    }
    if (current_token->value != NULL &&
        strcmp(current_token->value, ";") == 0) {
      current_token++;
      return;
    }
    current_token++;
  }
}

/**
 * @brief Parses one statement, abandoning it (NULL) on error
 * @return Statement node, or NULL if an error was reported
 * @details Installs a fresh recovery buffer so parse_error lands back
 * here; the next statement installs its own, so no stale frame is reused
 */
static Node *try_parse_statement(void) {
  Node *stmt = NULL;
  /* Save the enclosing buffer: without this, an error in outer code after
     a nested recovery longjmps into a returned frame (stack garbage). */
  jmp_buf saved;
  memcpy(saved, stmt_jmp, sizeof(saved));
  if (setjmp(stmt_jmp) == 0) {
    stmt = parse_statement();
  }
  memcpy(stmt_jmp, saved, sizeof(stmt_jmp));
  return stmt;
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
  node->source = source_filename;
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
 * @brief Parses a floating point literal
 * @return AST node for floating point literal
 */
static Node *parse_float_literal() {
  Node *node = create_node(NODE_FLOAT_LITERAL);
  node->float_literal.value = atof(current_token->value);
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
 * @brief Checks if an identifier is reserved and errors if so
 * @param name Identifier to check
 * @param what Kind of declaration (for the error message)
 */
static void check_reserved_ident(const char *name) {
  if (strcmp(name, "this") == 0) {
    parse_error("Identifier 'this' is reserved\n");
  }
}

/**
 * @brief Parses a 'new Type(args)' instantiation expression
 * @return AST node for instantiation (current token is 'new')
 */
static Node *parse_new_expr() {
  Node *node = create_node(NODE_NEW);
  current_token++;
  if (current_token->type != IDENTIFIER) {
    parse_error_expected("Expected struct or class name after 'new'\n");
  }
  size_t len = strlen(current_token->value);
  node->new_expr.type_name = malloc(len + 1);
  memcpy(node->new_expr.type_name, current_token->value, len);
  node->new_expr.type_name[len] = '\0';
  current_token++;
  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, "(") != 0) {
    parse_error_expected("Expected '(' after struct or class name\n");
  }
  current_token++;
  node->new_expr.args = NULL;
  Node *tail = NULL;
  if (current_token->type != END_OF_TOKENS &&
      strcmp(current_token->value, ")") != 0) {
    while (1) {
      Node *arg = parse_expression();
      if (arg == NULL) {
        return NULL;
      }
      if (node->new_expr.args == NULL) {
        node->new_expr.args = arg;
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
    parse_error_expected("Expected ')' after 'new' arguments\n");
  }
  current_token++;
  return node;
}

/**
 * @brief Parses a member access (object.member)
 * @param object Object identifier or arr[i] index node (already parsed)
 * @return AST node for member access
 */
static Node *parse_member_access(Node *object) {
  Node *node = create_node(NODE_MEMBER_ACCESS);
  size_t len;
  if (object->type == NODE_IDENTIFIER) {
    len = strlen(object->identifier.name);
    node->member_access.object = malloc(len + 1);
    memcpy(node->member_access.object, object->identifier.name, len);
    node->member_access.object[len] = '\0';
    node->member_access.object_expr = NULL;
  } else {
    /* any other base (arr[i], a.b) is kept as an expression */
    node->member_access.object = NULL;
    node->member_access.object_expr = object;
  }
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
      if (arg == NULL) {
        return NULL;
      }
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
 * @brief Parses a parenthesized argument list into an args chain
 * @details Current token must be '(', consumed through the matching ')'
 * @return First argument node (linked via right), NULL when empty
 */
static Node *parse_call_arguments() {
  current_token++;

  Node *args = NULL;
  Node *tail = NULL;

  if (current_token->type != END_OF_TOKENS &&
      strcmp(current_token->value, ")") != 0) {
    while (1) {
      Node *arg = parse_expression();
      if (arg == NULL) {
        return NULL;
      }
      if (args == NULL) {
        args = arg;
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
    parse_error_expected("Expected ')' after arguments\n");
  }
  current_token++;

  return args;
}

/**
 * @brief Parses a method call (object.member(args), member parsed already)
 * @param object Object name (borrowed, copied into the node)
 * @param member Member name (borrowed, copied into the node)
 * @return AST node for method call
 */
static Node *parse_method_call_expr(const char *object, const char *member) {
  Node *node = create_node(NODE_METHOD_CALL);
  size_t len = strlen(object);
  node->method_call.object = malloc(len + 1);
  memcpy(node->method_call.object, object, len);
  node->method_call.object[len] = '\0';
  node->method_call.object_expr = NULL;
  len = strlen(member);
  node->method_call.method = malloc(len + 1);
  memcpy(node->method_call.method, member, len);
  node->method_call.method[len] = '\0';
  current_token++;

  node->method_call.args = NULL;
  Node *tail = NULL;

  if (current_token->type != END_OF_TOKENS &&
      strcmp(current_token->value, ")") != 0) {
    while (1) {
      Node *arg = parse_expression();
      if (arg == NULL) {
        return NULL;
      }
      if (node->method_call.args == NULL) {
        node->method_call.args = arg;
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
    parse_error_expected("Expected ')' after method arguments\n");
  }
  current_token++;

  return node;
}

/**
 * @brief Parses a method call on an arr[i] base (object already parsed)
 * @param object_expr Index node for the arr[i] base (adopted by the node)
 * @param member Member name (borrowed, copied into the node)
 * @return AST node for method call
 */
static Node *parse_method_call_expr_idx(Node *object_expr, const char *member) {
  Node *node = create_node(NODE_METHOD_CALL);
  node->method_call.object = NULL;
  node->method_call.object_expr = object_expr;
  size_t len = strlen(member);
  node->method_call.method = malloc(len + 1);
  memcpy(node->method_call.method, member, len);
  node->method_call.method[len] = '\0';
  node->method_call.args = parse_call_arguments();
  return node;
}

/**
 * @brief Parses a primary expression without postfix operators
 * @return AST node for the base expression
 */
static Node *parse_primary_base() {
  if (current_token->type == END_OF_TOKENS) {
    parse_error("Unexpected end of input in expression\n");
  }
  if (current_token->type == KEYWORD &&
      strcmp(current_token->value, "new") == 0) {
    return parse_new_expr();
  }
  if (current_token->type == KEYWORD &&
      strcmp(current_token->value, "null") == 0) {
    Node *node = create_node(NODE_NULL);
    current_token++;
    return node;
  }
  /* Unary minus, so negative literals like -17 parse. Modelled as 0 - x
     so the existing numeric codegen and type inference handle it. */
  if (current_token->type == OPERATOR &&
      strcmp(current_token->value, "-") == 0) {
    Node *minus = create_node(NODE_BINARY_OP);
    minus->binary_op.op = malloc(2);
    memcpy(minus->binary_op.op, "-", 2);
    Node *zero = create_node(NODE_INT_LITERAL);
    zero->int_literal.value = 0;
    current_token++;
    Node *operand = parse_primary_base();
    if (operand == NULL) {
      return NULL;
    }
    minus->binary_op.left = zero;
    minus->binary_op.right = operand;
    return minus;
  }
  /* Bitwise NOT. Modelled as a binary "~" with a dummy 0 left operand so
     every generic AST walk keeps working; sem and IR only read the right
     operand and emit a single NOT. Takes a full primary (unlike unary
     minus) so ~self.flags and ~a[0] work. */
  if (current_token->type == OPERATOR &&
      strcmp(current_token->value, "~") == 0) {
    Node *inv = create_node(NODE_BINARY_OP);
    inv->binary_op.op = malloc(2);
    memcpy(inv->binary_op.op, "~", 2);
    Node *zero = create_node(NODE_INT_LITERAL);
    zero->int_literal.value = 0;
    current_token++;
    Node *operand = parse_primary();
    if (operand == NULL) {
      return NULL;
    }
    inv->binary_op.left = zero;
    inv->binary_op.right = operand;
    return inv;
  }
  /* 'char' is a keyword type, but char(...) is the builtin call. */
  if (current_token->type == KEYWORD &&
      strcmp(current_token->value, "char") == 0 &&
      current_token[1].value != NULL &&
      strcmp(current_token[1].value, "(") == 0) {
    Node *node = create_node(NODE_FUNC_CALL);
    node->func_call.name = malloc(5);
    memcpy(node->func_call.name, "char", 5);
    current_token++;
    node->func_call.args = parse_call_arguments();
    return node;
  }
  if (current_token->type == INT) {
    return parse_int_literal();
  } else if (current_token->type == FLOAT) {
    return parse_float_literal();
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
        if (element == NULL) {
          if (current_token->type != END_OF_TOKENS &&
              current_token->value != NULL &&
              strcmp(current_token->value, ",") == 0) {
            current_token++;
            continue;
          }
          break;
        }
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
 * @brief Parses a primary expression with postfix operators
 * @details Handles indexing (base[index]), member access (obj.field) and
 * method calls (obj.method(args)) on top of parse_primary_base
 * @return AST node for primary expression
 */
static Node *parse_primary() {
  Node *base = parse_primary_base();
  if (base == NULL) {
    return NULL;
  }
  while (1) {
    if (current_token->type != END_OF_TOKENS && current_token->value != NULL &&
        strcmp(current_token->value, "[") == 0) {
      Node *node = create_node(NODE_INDEX);
      node->index.base = base;
      current_token++;
      node->index.index = parse_expression();
      if (current_token->type == END_OF_TOKENS ||
          strcmp(current_token->value, "]") != 0) {
        parse_error_expected("Expected ']' after index\n");
      }
      current_token++;
      base = node;
      continue;
    }
    if (current_token->type != END_OF_TOKENS && current_token->value != NULL &&
        strcmp(current_token->value, ".") == 0) {
      if (base->type != NODE_IDENTIFIER && base->type != NODE_INDEX &&
          base->type != NODE_MEMBER_ACCESS) {
        parse_error("Only variables, arr[i], and field access support member "
                    "access\n");
      }
      Node *member = parse_member_access(base);
      if (current_token->type != END_OF_TOKENS &&
          current_token->value != NULL &&
          strcmp(current_token->value, "(") == 0) {
        if (member->member_access.object_expr != NULL) {
          base = parse_method_call_expr_idx(member->member_access.object_expr,
                                            member->member_access.member);
        } else {
          base = parse_method_call_expr(member->member_access.object,
                                        member->member_access.member);
        }
        continue;
      }
      base = member;
      continue;
    }
    break;
  }
  return base;
}

/**
 * @brief Creates a binary operation node for the current operator
 * @param left Left operand (already parsed)
 * @return New node with operator filled, right operand unset
 */
static Node *take_operator(Node *left) {
  if (left == NULL) {
    return NULL;
  }
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
  if (left == NULL) {
    return NULL;
  }

  while (current_token->type == OPERATOR &&
         (strcmp(current_token->value, "*") == 0 ||
          strcmp(current_token->value, "/") == 0 ||
          strcmp(current_token->value, "%") == 0)) {
    Node *op = take_operator(left);
    Node *right = parse_primary();
    if (right == NULL) {
      return NULL;
    }
    op->binary_op.right = right;
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
  if (left == NULL) {
    return NULL;
  }

  while (current_token->type == OPERATOR &&
         (strcmp(current_token->value, "+") == 0 ||
          strcmp(current_token->value, "-") == 0)) {
    Node *op = take_operator(left);
    Node *right = parse_multiplicative();
    if (right == NULL) {
      return NULL;
    }
    op->binary_op.right = right;
    left = op;
  }

  return left;
}

/**
 * @brief Parses << and >> (left-associative, below additive)
 * @return AST node for expression
 */
static Node *parse_shift() {
  Node *left = parse_additive();
  if (left == NULL) {
    return NULL;
  }

  while (current_token->type == OPERATOR &&
         (strcmp(current_token->value, "<<") == 0 ||
          strcmp(current_token->value, ">>") == 0)) {
    Node *op = take_operator(left);
    Node *right = parse_additive();
    if (right == NULL) {
      return NULL;
    }
    op->binary_op.right = right;
    left = op;
  }

  return left;
}

/**
 * @brief Parses <, >, <= and >= (left-associative)
 * @return AST node for expression
 */
static Node *parse_relational() {
  Node *left = parse_shift();
  if (left == NULL) {
    return NULL;
  }

  while (current_token->type == OPERATOR &&
         (strcmp(current_token->value, "<") == 0 ||
          strcmp(current_token->value, ">") == 0 ||
          strcmp(current_token->value, "<=") == 0 ||
          strcmp(current_token->value, ">=") == 0)) {
    Node *op = take_operator(left);
    Node *right = parse_shift();
    if (right == NULL) {
      return NULL;
    }
    op->binary_op.right = right;
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
  if (left == NULL) {
    return NULL;
  }

  while (current_token->type == OPERATOR &&
         (strcmp(current_token->value, "==") == 0 ||
          strcmp(current_token->value, "!=") == 0)) {
    Node *op = take_operator(left);
    if (op == NULL) {
      return NULL;
    }
    Node *right = parse_relational();
    if (right == NULL) {
      return NULL;
    }
    op->binary_op.right = right;
    left = op;
  }

  return left;
}

/**
 * @brief Parses & (left-associative, below equality)
 * @return AST node for expression
 */
static Node *parse_bitand() {
  Node *left = parse_equality();
  if (left == NULL) {
    return NULL;
  }

  while (current_token->type == OPERATOR &&
         strcmp(current_token->value, "&") == 0) {
    Node *op = take_operator(left);
    Node *right = parse_equality();
    if (right == NULL) {
      return NULL;
    }
    op->binary_op.right = right;
    left = op;
  }

  return left;
}

/**
 * @brief Parses ^ (left-associative, below &)
 * @return AST node for expression
 */
static Node *parse_bitxor() {
  Node *left = parse_bitand();
  if (left == NULL) {
    return NULL;
  }

  while (current_token->type == OPERATOR &&
         strcmp(current_token->value, "^") == 0) {
    Node *op = take_operator(left);
    Node *right = parse_bitand();
    if (right == NULL) {
      return NULL;
    }
    op->binary_op.right = right;
    left = op;
  }

  return left;
}

/**
 * @brief Parses | (left-associative, below ^)
 * @return AST node for expression
 */
static Node *parse_bitor() {
  Node *left = parse_bitxor();
  if (left == NULL) {
    return NULL;
  }

  while (current_token->type == OPERATOR &&
         strcmp(current_token->value, "|") == 0) {
    Node *op = take_operator(left);
    Node *right = parse_bitxor();
    if (right == NULL) {
      return NULL;
    }
    op->binary_op.right = right;
    left = op;
  }

  return left;
}

/**
 * @brief Parses an expression with binary operations
 * @return AST node for expression
 */
static Node *parse_expression() { return parse_bitor(); }

/**
 * @brief Parses a return statement
 * @return AST node for return statement
 */
static Node *parse_return() {
  Node *node = create_node(NODE_RETURN);
  current_token++;

  node->return_stmt.value = NULL;

  /* Bare return; (only valid in functions declared '-> void') */
  if (current_token->type != END_OF_TOKENS &&
      strcmp(current_token->value, ";") == 0) {
    current_token++;
    return node;
  }

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, "(") != 0) {
    parse_error_expected("Expected '(' or ';' after return\n");
  }
  current_token++;

  if (current_token->type != END_OF_TOKENS &&
      strcmp(current_token->value, ")") == 0) {
    parse_warning("Empty 'return()' - use 'return;' instead\n");
    current_token++;
  } else {
    node->return_stmt.value = parse_expression();

    if (current_token->type == END_OF_TOKENS ||
        strcmp(current_token->value, ")") != 0) {
      parse_error_expected("Expected ')' after return value\n");
    }
    current_token++;
  }

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
  check_reserved_ident(node->var_decl.name);
  current_token++;

  node->var_decl.value = NULL;
  node->var_decl.is_global = 0;

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
 * @brief Parses a global variable declaration (global num x = 0;)
 * @return AST node marked as global
 */
static Node *parse_global_decl() {
  current_token++; /* consume 'global' */
  if (current_token->type == KEYWORD &&
      (strcmp(current_token->value, "num") == 0 ||
       strcmp(current_token->value, "bool") == 0 ||
       strcmp(current_token->value, "str") == 0 ||
       strcmp(current_token->value, "char") == 0)) {
    Node *node = parse_var_decl();
    if (node != NULL) {
      node->var_decl.is_global = 1;
    }
    return node;
  }
  parse_error_expected("Expected type (num, bool, str, char) after 'global'\n");
  return NULL;
}

/**
 * @brief Parses an assignment statement (including += and -=)
 * @return AST node for assignment statement
 */
static Node *parse_assignment() {
  size_t len = strlen(current_token->value);
  char *name = malloc(len + 1);
  memcpy(name, current_token->value, len);
  name[len] = '\0';
  check_reserved_ident(name);
  current_token++;

  if (current_token->type == END_OF_TOKENS) {
    free(name);
    parse_error_expected("Expected assignment operator after identifier\n");
  }

  Node *node;
  if (strcmp(current_token->value, "+=") == 0) {
    node = create_node(NODE_ADD_ASSIGN);
    node->add_assign.name = name;
    current_token++;
    node->add_assign.value = parse_expression();
  } else if (strcmp(current_token->value, "-=") == 0) {
    node = create_node(NODE_SUB_ASSIGN);
    node->sub_assign.name = name;
    current_token++;
    node->sub_assign.value = parse_expression();
  } else if (strcmp(current_token->value, "=") == 0) {
    node = create_node(NODE_ASSIGNMENT);
    node->assignment.name = name;
    current_token++;
    node->assignment.value = parse_expression();
  } else {
    free(name);
    parse_error_expected("Expected '=', '+=', or '-=' after identifier\n");
  }

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
    Node *stmt = try_parse_statement();
    if (stmt == NULL) {
      continue;
    }
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
  check_reserved_ident(node->for_stmt.var_name);
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
    parse_error_expected("Expected identifier after arr\n");
  }
  size_t len = strlen(current_token->value);
  node->array_decl.name = malloc(len + 1);
  memcpy(node->array_decl.name, current_token->value, len);
  node->array_decl.name[len] = '\0';
  check_reserved_ident(node->array_decl.name);
  current_token++;

  if (strcmp(current_token->value, "=") != 0) {
    parse_error_expected("Expected '=' after arr name\n");
  }
  current_token++;

  node->array_decl.elements = parse_expression();
  if (node->array_decl.elements == NULL) {
    parse_error_expected("Expected array literal or identifier after '='\n");
  }

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
  copy->source = node->source;
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
    if (node->function.return_type != NULL) {
      len = strlen(node->function.return_type);
      copy->function.return_type = malloc(len + 1);
      memcpy(copy->function.return_type, node->function.return_type, len);
      copy->function.return_type[len] = '\0';
    } else {
      copy->function.return_type = NULL;
    }
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
  case NODE_FLOAT_LITERAL:
    copy->float_literal.value = node->float_literal.value;
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
  case NODE_ADD_ASSIGN:
    len = strlen(node->add_assign.name);
    copy->add_assign.name = malloc(len + 1);
    memcpy(copy->add_assign.name, node->add_assign.name, len);
    copy->add_assign.name[len] = '\0';
    copy->add_assign.value = clone_node(node->add_assign.value);
    break;
  case NODE_SUB_ASSIGN:
    len = strlen(node->sub_assign.name);
    copy->sub_assign.name = malloc(len + 1);
    memcpy(copy->sub_assign.name, node->sub_assign.name, len);
    copy->sub_assign.name[len] = '\0';
    copy->sub_assign.value = clone_node(node->sub_assign.value);
    break;
  case NODE_MEMBER_ACCESS:
    if (node->member_access.object != NULL) {
      len = strlen(node->member_access.object);
      copy->member_access.object = malloc(len + 1);
      memcpy(copy->member_access.object, node->member_access.object, len);
      copy->member_access.object[len] = '\0';
    } else {
      copy->member_access.object = NULL;
    }
    copy->member_access.object_expr =
        clone_node(node->member_access.object_expr);
    len = strlen(node->member_access.member);
    copy->member_access.member = malloc(len + 1);
    memcpy(copy->member_access.member, node->member_access.member, len);
    copy->member_access.member[len] = '\0';
    break;
  case NODE_BREAK:
  case NODE_CONTINUE:
  case NODE_NULL:
    break;
  case NODE_STRUCT_DEF:
    len = strlen(node->struct_def.name);
    copy->struct_def.name = malloc(len + 1);
    memcpy(copy->struct_def.name, node->struct_def.name, len);
    copy->struct_def.name[len] = '\0';
    copy->struct_def.is_public = node->struct_def.is_public;
    copy->struct_def.fields = clone_list(node->struct_def.fields);
    break;
  case NODE_CLASS_DEF:
    len = strlen(node->class_def.name);
    copy->class_def.name = malloc(len + 1);
    memcpy(copy->class_def.name, node->class_def.name, len);
    copy->class_def.name[len] = '\0';
    if (node->class_def.base != NULL) {
      len = strlen(node->class_def.base);
      copy->class_def.base = malloc(len + 1);
      memcpy(copy->class_def.base, node->class_def.base, len);
      copy->class_def.base[len] = '\0';
    } else {
      copy->class_def.base = NULL;
    }
    copy->class_def.is_public = node->class_def.is_public;
    copy->class_def.fields = clone_list(node->class_def.fields);
    copy->class_def.methods = clone_list(node->class_def.methods);
    break;
  case NODE_METHOD_DEF:
    len = strlen(node->method_def.ret_type);
    copy->method_def.ret_type = malloc(len + 1);
    memcpy(copy->method_def.ret_type, node->method_def.ret_type, len);
    copy->method_def.ret_type[len] = '\0';
    len = strlen(node->method_def.name);
    copy->method_def.name = malloc(len + 1);
    memcpy(copy->method_def.name, node->method_def.name, len);
    copy->method_def.name[len] = '\0';
    copy->method_def.is_public = node->method_def.is_public;
    copy->method_def.params = clone_list(node->method_def.params);
    copy->method_def.body = clone_list(node->method_def.body);
    break;
  case NODE_NEW:
    len = strlen(node->new_expr.type_name);
    copy->new_expr.type_name = malloc(len + 1);
    memcpy(copy->new_expr.type_name, node->new_expr.type_name, len);
    copy->new_expr.type_name[len] = '\0';
    copy->new_expr.args = clone_list(node->new_expr.args);
    break;
  case NODE_INDEX:
    copy->index.base = clone_node(node->index.base);
    copy->index.index = clone_node(node->index.index);
    break;
  case NODE_METHOD_CALL:
    if (node->method_call.object != NULL) {
      len = strlen(node->method_call.object);
      copy->method_call.object = malloc(len + 1);
      memcpy(copy->method_call.object, node->method_call.object, len);
      copy->method_call.object[len] = '\0';
    } else {
      copy->method_call.object = NULL;
    }
    copy->method_call.object_expr = clone_node(node->method_call.object_expr);
    len = strlen(node->method_call.method);
    copy->method_call.method = malloc(len + 1);
    memcpy(copy->method_call.method, node->method_call.method, len);
    copy->method_call.method[len] = '\0';
    copy->method_call.args = clone_list(node->method_call.args);
    break;
  case NODE_MEMBER_ASSIGN:
    if (node->member_assign.object != NULL) {
      len = strlen(node->member_assign.object);
      copy->member_assign.object = malloc(len + 1);
      memcpy(copy->member_assign.object, node->member_assign.object, len);
      copy->member_assign.object[len] = '\0';
    } else {
      copy->member_assign.object = NULL;
    }
    copy->member_assign.object_expr =
        clone_node(node->member_assign.object_expr);
    len = strlen(node->member_assign.member);
    copy->member_assign.member = malloc(len + 1);
    memcpy(copy->member_assign.member, node->member_assign.member, len);
    copy->member_assign.member[len] = '\0';
    len = strlen(node->member_assign.op);
    copy->member_assign.op = malloc(len + 1);
    memcpy(copy->member_assign.op, node->member_assign.op, len);
    copy->member_assign.op[len] = '\0';
    copy->member_assign.value = clone_node(node->member_assign.value);
    break;
  case NODE_INDEX_ASSIGN:
    copy->index_assign.base = clone_node(node->index_assign.base);
    copy->index_assign.index = clone_node(node->index_assign.index);
    len = strlen(node->index_assign.op);
    copy->index_assign.op = malloc(len + 1);
    memcpy(copy->index_assign.op, node->index_assign.op, len);
    copy->index_assign.op[len] = '\0';
    copy->index_assign.value = clone_node(node->index_assign.value);
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

  /* AST nodes borrow this name for diagnostics, so it must outlive the
     statement: path is a stack buffer, keep a heap copy forever. */
  char *path_heap = malloc(path_len + 1);
  memcpy(path_heap, path, path_len + 1);

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

  FILE *target = fopen(path, "rb");
  if (!target) {
    char message[640];
    snprintf(message, sizeof(message), "Could not open file '%s'", path);
    parse_error_at(stmt_line, stmt_col, stmt_width, message);
    return NULL;
  }
  if (import_in_progress(path)) {
    char message[640];
    snprintf(message, sizeof(message), "Import cycle detected for '%s'", path);
    parse_error_at(stmt_line, stmt_col, stmt_width, message);
    return NULL;
  }

  Node *sub_root = NULL;
  if (!import_is_done(path)) {
    Token *saved_token = current_token;
    const char *saved_source = source_filename;
    Node *saved_head = program_head;
    Node *saved_tail = program_tail;
    jmp_buf saved_jmp;
    memcpy(saved_jmp, stmt_jmp, sizeof(stmt_jmp));
    LexerSnapshot lexer_state;
    lexer_save(&lexer_state);

    program_head = NULL;
    program_tail = NULL;
    lexer_set_file_name(path_heap);
    Token *sub_tokens = Lexer(target);
    sub_root = Parser(sub_tokens, path_heap);
    lexer_set_file_name(source_filename);

    memcpy(stmt_jmp, saved_jmp, sizeof(stmt_jmp));
    lexer_restore(&lexer_state);
    current_token = saved_token;
    source_filename = saved_source;
    program_head = saved_head;
    program_tail = saved_tail;

    if (import_done_count >= MAX_IMPORT_DEPTH) {
      parse_error_at(stmt_line, stmt_col, stmt_width,
                     "Too many imported files\n");
      return NULL;
    }
    import_done[import_done_count] = path_heap;
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
    if (found != NULL) {
      if (!found->function.is_public) {
        char message[640];
        snprintf(message, sizeof(message), "Function '%s' is private in '%s'",
                 names[i], path);
        parse_error_at(stmt_line, stmt_col, stmt_width, message);
      }
      check_duplicate_fn(names[i], stmt_line, stmt_col, stmt_width);
      check_duplicate_def(names[i], "Function", stmt_line, stmt_col,
                          stmt_width);
      emit_statement(clone_node(found));
      continue;
    }
    Node *found_struct = find_struct_in(sub_root, names[i]);
    if (found_struct != NULL) {
      if (!found_struct->struct_def.is_public) {
        char message[640];
        snprintf(message, sizeof(message), "Struct '%s' is private in '%s'",
                 names[i], path);
        parse_error_at(stmt_line, stmt_col, stmt_width, message);
      }
      check_duplicate_def(names[i], "Struct", stmt_line, stmt_col, stmt_width);
      emit_statement(clone_node(found_struct));
      continue;
    }
    Node *found_class = find_class_in(sub_root, names[i]);
    if (found_class != NULL) {
      if (!found_class->class_def.is_public) {
        char message[640];
        snprintf(message, sizeof(message), "Class '%s' is private in '%s'",
                 names[i], path);
        parse_error_at(stmt_line, stmt_col, stmt_width, message);
      }
      check_duplicate_def(names[i], "Class", stmt_line, stmt_col, stmt_width);
      emit_statement(clone_node(found_class));
      continue;
    }
    Node *owner = find_method_owner(sub_root, names[i]);
    if (owner != NULL) {
      char message[640];
      snprintf(message, sizeof(message),
               "Method '%s' is private to class '%s' in '%s', import the class "
               "instead",
               names[i], owner->class_def.name, path);
      parse_error_at(stmt_line, stmt_col, stmt_width, message);
    }
    {
      char message[640];
      snprintf(message, sizeof(message), "'%s' is not defined in '%s'",
               names[i], path);
      parse_error_at(stmt_line, stmt_col, stmt_width, message);
    }
  }

  if (star) {
    for (Node *s = sub_root; s != NULL; s = s->right) {
      if (s->type == NODE_FUNCTION && s->function.is_public) {
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
        check_duplicate_def(s->function.name, "Function", stmt_line, stmt_col,
                            stmt_width);
        emit_statement(clone_node(s));
      } else if (s->type == NODE_STRUCT_DEF && s->struct_def.is_public) {
        int listed = 0;
        for (int i = 0; i < name_count; i++) {
          if (strcmp(names[i], s->struct_def.name) == 0) {
            listed = 1;
            break;
          }
        }
        if (listed) {
          continue;
        }
        check_duplicate_def(s->struct_def.name, "Struct", stmt_line, stmt_col,
                            stmt_width);
        emit_statement(clone_node(s));
      } else if (s->type == NODE_CLASS_DEF && s->class_def.is_public) {
        int listed = 0;
        for (int i = 0; i < name_count; i++) {
          if (strcmp(names[i], s->class_def.name) == 0) {
            listed = 1;
            break;
          }
        }
        if (listed) {
          continue;
        }
        check_duplicate_def(s->class_def.name, "Class", stmt_line, stmt_col,
                            stmt_width);
        emit_statement(clone_node(s));
      }
    }
  }

  return NULL;
}

/**
 * @brief Errors if a type or function name is already defined
 * @param name Name to check (struct, class or function)
 * @param kind Kind word for the message ("Struct", "Class", "Function")
 * @param line Position line for the error
 * @param col Position column for the error
 * @param width Squiggle width for the error
 */
static void check_duplicate_def(const char *name, const char *kind, int line,
                                int col, int width) {
  for (Node *s = program_head; s != NULL; s = s->right) {
    if (s->type == NODE_FUNCTION && strcmp(s->function.name, name) == 0) {
      char message[96];
      snprintf(message, sizeof(message), "%s '%s' is already defined", kind,
               name);
      parse_error_at(line, col, width, message);
    }
    if (s->type == NODE_STRUCT_DEF && strcmp(s->struct_def.name, name) == 0) {
      char message[96];
      snprintf(message, sizeof(message), "%s '%s' is already defined", kind,
               name);
      parse_error_at(line, col, width, message);
    }
    if (s->type == NODE_CLASS_DEF && strcmp(s->class_def.name, name) == 0) {
      char message[96];
      snprintf(message, sizeof(message), "%s '%s' is already defined", kind,
               name);
      parse_error_at(line, col, width, message);
    }
  }
}

/**
 * @brief Parses an optional visibility modifier (public/private)
 * @param is_public Receives the parsed visibility
 * @return Non-zero if a modifier was present
 */
static int parse_visibility(int *is_public) {
  if (current_token->type == IDENTIFIER &&
      (strcmp(current_token->value, "public") == 0 ||
       strcmp(current_token->value, "private") == 0)) {
    *is_public = strcmp(current_token->value, "public") == 0;
    current_token++;
    return 1;
  }
  return 0;
}

/**
 * @brief Warns that a definition has no visibility and defaults to private
 * @param kind Kind word ("Struct", "Class")
 * @param name Definition name
 * @param def_line Line of the definition keyword
 * @param def_col Column of the definition keyword
 */
static void warn_missing_visibility(const char *kind, const char *name,
                                    int def_line, int def_col) {
  char message[96];
  snprintf(message, sizeof(message),
           "%s '%s' has no visibility, defaulting to private\n", kind, name);
  parse_warning_at(def_line, def_col, (int)strlen(name), message);
}

/**
 * @brief Parses a break or continue statement
 * @return AST node for the jump statement
 */
static Node *parse_break_or_continue(int is_break) {
  Node *node = create_node(is_break ? NODE_BREAK : NODE_CONTINUE);
  current_token++;
  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ";") != 0) {
    parse_error_expected("Expected ';' after statement\n");
  }
  current_token++;
  return node;
}

/**
 * @brief Parses a struct definition
 * @return AST node for struct definition (current token is 'struct')
 */
static Node *parse_struct_def() {
  int def_line = current_token->line;
  int def_col = current_token->col;
  Node *node = create_node(NODE_STRUCT_DEF);
  current_token++;

  int is_public = 0;
  int has_visibility = parse_visibility(&is_public);

  if (current_token->type != IDENTIFIER) {
    parse_error_expected("Expected struct name\n");
  }
  size_t len = strlen(current_token->value);
  node->struct_def.name = malloc(len + 1);
  memcpy(node->struct_def.name, current_token->value, len);
  node->struct_def.name[len] = '\0';
  check_reserved_ident(node->struct_def.name);
  check_duplicate_def(node->struct_def.name, "Struct", current_token->line,
                      current_token->col, token_width());
  current_token++;

  if (!has_visibility) {
    warn_missing_visibility("Struct", node->struct_def.name, def_line, def_col);
  }
  node->struct_def.is_public = is_public;

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, "{") != 0) {
    parse_error_expected("Expected '{' after struct name\n");
  }
  current_token++;

  node->struct_def.fields = NULL;
  Node *tail = NULL;
  while (current_token->type != END_OF_TOKENS &&
         (current_token->value == NULL ||
          strcmp(current_token->value, "}") != 0)) {
    if (current_token->type != KEYWORD && current_token->type != IDENTIFIER) {
      parse_error_expected(
          "Expected field type (num, bool, str, or a struct/class name) in "
          "struct\n");
    }
    if (current_token->type == KEYWORD &&
        strcmp(current_token->value, "num") != 0 &&
        strcmp(current_token->value, "bool") != 0 &&
        strcmp(current_token->value, "str") != 0) {
      parse_error_expected(
          "Expected field type (num, bool, str, or a struct/class name) in "
          "struct\n");
    }
    if (current_token->type == IDENTIFIER && current_token[1].value != NULL &&
        strcmp(current_token[1].value, ".") == 0) {
      parse_error("A field type cannot be a member expression\n");
    }
    Node *field = create_node(NODE_VAR_DECL);
    len = strlen(current_token->value);
    field->var_decl.var_type = malloc(len + 1);
    memcpy(field->var_decl.var_type, current_token->value, len);
    field->var_decl.var_type[len] = '\0';
    current_token++;
    if (current_token->type != IDENTIFIER) {
      parse_error_expected("Expected field name in struct\n");
    }
    len = strlen(current_token->value);
    field->var_decl.name = malloc(len + 1);
    memcpy(field->var_decl.name, current_token->value, len);
    field->var_decl.name[len] = '\0';
    check_reserved_ident(field->var_decl.name);
    current_token++;
    if (current_token->type != END_OF_TOKENS &&
        strcmp(current_token->value, "(") == 0) {
      parse_error("Structs cannot have methods, use a class\n");
    }
    if (current_token->type == END_OF_TOKENS ||
        strcmp(current_token->value, ";") != 0) {
      parse_error_expected("Expected ';' after struct field\n");
    }
    current_token++;
    field->var_decl.value = NULL;
    field->var_decl.is_global = 0;
    {
      int dup = 0;
      for (Node *f = node->struct_def.fields; f != NULL; f = f->right) {
        if (strcmp(f->var_decl.name, field->var_decl.name) == 0) {
          char message[96];
          snprintf(message, sizeof(message), "Duplicate field '%s'",
                   field->var_decl.name);
          parse_error_at(field->line, field->col, field->width, message);
          dup = 1;
          break;
        }
      }
      if (dup) {
        continue;
      }
    }
    if (node->struct_def.fields == NULL) {
      node->struct_def.fields = field;
      tail = field;
    } else {
      tail->right = field;
      tail = field;
    }
  }

  if (current_token->type == END_OF_TOKENS) {
    parse_error_expected("Expected '}' after struct body\n");
  }
  current_token++;
  return node;
}

/**
 * @brief Parses a method definition inside a class body
 * @return AST node for method definition (current token is return type)
 */
static Node *parse_method_def() {
  Node *node = create_node(NODE_METHOD_DEF);
  size_t len = strlen(current_token->value);
  node->method_def.ret_type = malloc(len + 1);
  memcpy(node->method_def.ret_type, current_token->value, len);
  node->method_def.ret_type[len] = '\0';
  current_token++;

  if (current_token->type != IDENTIFIER) {
    parse_error_expected("Expected method name\n");
  }
  len = strlen(current_token->value);
  node->method_def.name = malloc(len + 1);
  memcpy(node->method_def.name, current_token->value, len);
  node->method_def.name[len] = '\0';
  check_reserved_ident(node->method_def.name);
  if (strcmp(node->method_def.name, "input") == 0 ||
      strcmp(node->method_def.name, "len") == 0 ||
      strcmp(node->method_def.name, "tostr") == 0 ||
      strcmp(node->method_def.name, "tonum") == 0 ||
      strcmp(node->method_def.name, "readFile") == 0 ||
      strcmp(node->method_def.name, "writeFile") == 0 ||
      strcmp(node->method_def.name, "char") == 0 ||
      strcmp(node->method_def.name, "args") == 0) {
    parse_error("Method name is reserved\n");
  }
  current_token++;

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, "(") != 0) {
    parse_error_expected("Expected '(' after method name\n");
  }
  current_token++;

  node->method_def.params = NULL;
  Node *param_tail = NULL;
  if (current_token->type != END_OF_TOKENS &&
      strcmp(current_token->value, ")") != 0) {
    while (1) {
      Node *param = NULL;
      if (current_token->type == KEYWORD &&
          (strcmp(current_token->value, "num") == 0 ||
           strcmp(current_token->value, "bool") == 0 ||
           strcmp(current_token->value, "str") == 0 ||
           strcmp(current_token->value, "arr") == 0)) {
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
        check_reserved_ident(param->var_decl.name);
        param->var_decl.value = NULL;
        param->var_decl.is_global = 0;
        current_token++;
      } else if (current_token->type == IDENTIFIER) {
        if (current_token[1].type == IDENTIFIER) {
          param = create_node(NODE_VAR_DECL);
          len = strlen(current_token->value);
          param->var_decl.var_type = malloc(len + 1);
          memcpy(param->var_decl.var_type, current_token->value, len);
          param->var_decl.var_type[len] = '\0';
          current_token++;
          len = strlen(current_token->value);
          param->var_decl.name = malloc(len + 1);
          memcpy(param->var_decl.name, current_token->value, len);
          param->var_decl.name[len] = '\0';
          check_reserved_ident(param->var_decl.name);
          param->var_decl.value = NULL;
          param->var_decl.is_global = 0;
          current_token++;
        } else {
          param = create_node(NODE_IDENTIFIER);
          len = strlen(current_token->value);
          param->identifier.name = malloc(len + 1);
          memcpy(param->identifier.name, current_token->value, len);
          param->identifier.name[len] = '\0';
          check_reserved_ident(param->identifier.name);
          current_token++;
        }
      } else {
        parse_error_expected("Expected parameter in method definition\n");
      }
      const char *pname = param->type == NODE_VAR_DECL ? param->var_decl.name
                                                       : param->identifier.name;
      for (Node *p = node->method_def.params; p != NULL; p = p->right) {
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
      if (node->method_def.params == NULL) {
        node->method_def.params = param;
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

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ")") != 0) {
    parse_error_expected("Expected ')' after method parameters\n");
  }
  current_token++;

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, "{") != 0) {
    parse_error_expected("Expected '{' after method declaration\n");
  }
  current_token++;

  node->method_def.body = parse_block_statements();
  node->method_def.is_public = 0;

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, "}") != 0) {
    parse_error_expected("Expected '}' after method body\n");
  }
  current_token++;
  return node;
}

/**
 * @brief Parses a class definition
 * @return AST node for class definition (current token is 'class')
 */
static Node *parse_class_def() {
  int def_line = current_token->line;
  int def_col = current_token->col;
  Node *node = create_node(NODE_CLASS_DEF);
  current_token++;

  int is_public = 0;
  int has_visibility = parse_visibility(&is_public);

  if (current_token->type != IDENTIFIER) {
    parse_error_expected("Expected class name\n");
  }
  size_t len = strlen(current_token->value);
  node->class_def.name = malloc(len + 1);
  memcpy(node->class_def.name, current_token->value, len);
  node->class_def.name[len] = '\0';
  check_reserved_ident(node->class_def.name);
  check_duplicate_def(node->class_def.name, "Class", current_token->line,
                      current_token->col, token_width());
  current_token++;

  if (!has_visibility) {
    warn_missing_visibility("Class", node->class_def.name, def_line, def_col);
  }
  node->class_def.is_public = is_public;
  node->class_def.base = NULL;

  if (current_token->type == KEYWORD &&
      strcmp(current_token->value, "inherit") == 0) {
    current_token++;
    if (current_token->type != IDENTIFIER) {
      parse_error_expected("Expected base class name after 'inherit'\n");
    }
    len = strlen(current_token->value);
    node->class_def.base = malloc(len + 1);
    memcpy(node->class_def.base, current_token->value, len);
    node->class_def.base[len] = '\0';
    current_token++;
  }

  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, "{") != 0) {
    parse_error_expected("Expected '{' after class name\n");
  }
  current_token++;

  node->class_def.fields = NULL;
  node->class_def.methods = NULL;
  Node *field_tail = NULL;
  Node *method_tail = NULL;
  while (current_token->type != END_OF_TOKENS &&
         (current_token->value == NULL ||
          strcmp(current_token->value, "}") != 0)) {
    if (current_token->type == IDENTIFIER &&
        (strcmp(current_token->value, "public") == 0 ||
         strcmp(current_token->value, "private") == 0)) {
      parse_error("Visibility modifiers are not allowed on class members, "
                  "methods are always private to the class\n");
    }
    if (current_token->type != KEYWORD && current_token->type != IDENTIFIER) {
      parse_error_expected(
          "Expected field or method type (num, bool, str, or a struct/class "
          "name) in class\n");
    }
    if (current_token->type == KEYWORD &&
        strcmp(current_token->value, "num") != 0 &&
        strcmp(current_token->value, "bool") != 0 &&
        strcmp(current_token->value, "str") != 0) {
      parse_error_expected(
          "Expected field or method type (num, bool, str, or a struct/class "
          "name) in class\n");
    }
    if (current_token[1].value == NULL || current_token[1].type != IDENTIFIER) {
      parse_error_expected("Expected field or method name in class\n");
    }
    if (current_token[2].value != NULL &&
        strcmp(current_token[2].value, "(") == 0) {
      Node *method = parse_method_def();
      if (method == NULL) {
        continue;
      }
      {
        int skip = 0;
        for (Node *m = node->class_def.methods; m != NULL; m = m->right) {
          if (strcmp(m->method_def.name, method->method_def.name) == 0) {
            char message[96];
            snprintf(message, sizeof(message), "Duplicate method '%s'",
                     method->method_def.name);
            parse_error_at(method->line, method->col, method->width, message);
            skip = 1;
            break;
          }
        }
        for (Node *f = node->class_def.fields; f != NULL && !skip;
             f = f->right) {
          if (strcmp(f->var_decl.name, method->method_def.name) == 0) {
            char message[96];
            snprintf(message, sizeof(message),
                     "Method '%s' clashes with a field name",
                     method->method_def.name);
            parse_error_at(method->line, method->col, method->width, message);
            skip = 1;
            break;
          }
        }
        if (skip) {
          continue;
        }
      }
      if (node->class_def.methods == NULL) {
        node->class_def.methods = method;
        method_tail = method;
      } else {
        method_tail->right = method;
        method_tail = method;
      }
    } else {
      Node *field = create_node(NODE_VAR_DECL);
      len = strlen(current_token->value);
      field->var_decl.var_type = malloc(len + 1);
      memcpy(field->var_decl.var_type, current_token->value, len);
      field->var_decl.var_type[len] = '\0';
      current_token++;
      if (current_token->type != IDENTIFIER) {
        parse_error_expected("Expected field name in class\n");
      }
      len = strlen(current_token->value);
      field->var_decl.name = malloc(len + 1);
      memcpy(field->var_decl.name, current_token->value, len);
      field->var_decl.name[len] = '\0';
      check_reserved_ident(field->var_decl.name);
      current_token++;
      if (current_token->type == END_OF_TOKENS ||
          strcmp(current_token->value, ";") != 0) {
        parse_error_expected("Expected ';' after class field\n");
      }
      current_token++;
      field->var_decl.value = NULL;
      field->var_decl.is_global = 0;
      {
        int skip = 0;
        for (Node *f = node->class_def.fields; f != NULL; f = f->right) {
          if (strcmp(f->var_decl.name, field->var_decl.name) == 0) {
            char message[96];
            snprintf(message, sizeof(message), "Duplicate field '%s'",
                     field->var_decl.name);
            parse_error_at(field->line, field->col, field->width, message);
            skip = 1;
            break;
          }
        }
        if (!skip && node->class_def.methods != NULL) {
          for (Node *m = node->class_def.methods; m != NULL; m = m->right) {
            if (strcmp(m->method_def.name, field->var_decl.name) == 0) {
              char message[96];
              snprintf(message, sizeof(message),
                       "Field '%s' clashes with a method name",
                       field->var_decl.name);
              parse_error_at(field->line, field->col, field->width, message);
              skip = 1;
              break;
            }
          }
        }
        if (skip) {
          continue;
        }
      }
      if (node->class_def.fields == NULL) {
        node->class_def.fields = field;
        field_tail = field;
      } else {
        field_tail->right = field;
        field_tail = field;
      }
    }
  }

  if (current_token->type == END_OF_TOKENS) {
    parse_error_expected("Expected '}' after class body\n");
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
  if (strcmp(node->function.name, "input") == 0 ||
      strcmp(node->function.name, "len") == 0 ||
      strcmp(node->function.name, "tostr") == 0 ||
      strcmp(node->function.name, "tonum") == 0 ||
      strcmp(node->function.name, "readFile") == 0 ||
      strcmp(node->function.name, "writeFile") == 0 ||
      strcmp(node->function.name, "char") == 0 ||
      strcmp(node->function.name, "args") == 0) {
    parse_error("Function name is reserved, use another name\n");
  }
  check_reserved_ident(node->function.name);
  check_duplicate_fn(node->function.name, current_token->line,
                     current_token->col, token_width());
  check_duplicate_def(node->function.name, "Function", current_token->line,
                      current_token->col, token_width());
  int name_line = current_token->line;
  int name_col = current_token->col;
  int name_width = token_width();
  current_token++;

  /* A desynced stream can end here (e.g. an unterminated string swallowed
     the rest of the file): report instead of crashing on the sentinel. */
  if (current_token->type == END_OF_TOKENS || current_token->value == NULL ||
      strcmp(current_token->value, "(") != 0) {
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
          (strcmp(current_token->value, "num") == 0 ||
           strcmp(current_token->value, "bool") == 0 ||
           strcmp(current_token->value, "str") == 0 ||
           strcmp(current_token->value, "arr") == 0)) {
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
        check_reserved_ident(param->var_decl.name);
        param->var_decl.value = NULL;
        param->var_decl.is_global = 0;
        current_token++;
      } else if (current_token->type == IDENTIFIER) {
        if (current_token[1].type == IDENTIFIER) {
          param = create_node(NODE_VAR_DECL);
          len = strlen(current_token->value);
          param->var_decl.var_type = malloc(len + 1);
          memcpy(param->var_decl.var_type, current_token->value, len);
          param->var_decl.var_type[len] = '\0';
          current_token++;
          len = strlen(current_token->value);
          param->var_decl.name = malloc(len + 1);
          memcpy(param->var_decl.name, current_token->value, len);
          param->var_decl.name[len] = '\0';
          check_reserved_ident(param->var_decl.name);
          param->var_decl.value = NULL;
          param->var_decl.is_global = 0;
          current_token++;
        } else {
          param = create_node(NODE_IDENTIFIER);
          len = strlen(current_token->value);
          param->identifier.name = malloc(len + 1);
          memcpy(param->identifier.name, current_token->value, len);
          param->identifier.name[len] = '\0';
          check_reserved_ident(param->identifier.name);
          current_token++;
        }
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

  /* --- NEW: optional return-type -> type --- */
  node->function.return_type = NULL;
  if (current_token->type == OPERATOR &&
      strcmp(current_token->value, "->") == 0) {
    current_token++;
    if ((current_token->type == KEYWORD &&
         (strcmp(current_token->value, "num") == 0 ||
          strcmp(current_token->value, "str") == 0 ||
          strcmp(current_token->value, "arr") == 0 ||
          strcmp(current_token->value, "char") == 0)) ||
        (current_token->type == IDENTIFIER &&
         strcmp(current_token->value, "void") == 0)) {
      node->function.return_type = strdup(current_token->value);
      current_token++;
    } else {
      parse_error_expected("Expected type after '->' in function return\n");
    }
  }
  /* ----------------------------------------- */

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
 * @brief Parses a member statement (method call or field assignment)
 * @return AST node for the statement (current token is the object name)
 */
/**
 * @brief Parses a member statement from an already parsed postfix base
 * @param base Parsed base expression (field chain or method call)
 * @param start_line Statement start line for diagnostics
 * @param start_col Statement start column for diagnostics
 * @return AST node for the statement
 */
static Node *parse_member_statement_from(Node *base, int start_line,
                                         int start_col) {
  if (base == NULL) {
    return NULL;
  }
  /* arr[i].field / arr[i].field = v: finish the postfix chain first */
  while (current_token->type != END_OF_TOKENS && current_token->value != NULL &&
         strcmp(current_token->value, ".") == 0) {
    if (base->type != NODE_IDENTIFIER && base->type != NODE_INDEX &&
        base->type != NODE_MEMBER_ACCESS) {
      parse_error("Only variables, arr[i], and field access support member "
                  "access\n");
    }
    Node *member = parse_member_access(base);
    if (current_token->type != END_OF_TOKENS && current_token->value != NULL &&
        strcmp(current_token->value, "(") == 0) {
      if (member->member_access.object_expr != NULL) {
        base = parse_method_call_expr_idx(member->member_access.object_expr,
                                          member->member_access.member);
      } else {
        base = parse_method_call_expr(member->member_access.object,
                                      member->member_access.member);
      }
      continue;
    }
    base = member;
  }
  if (base->type == NODE_METHOD_CALL) {
    if (current_token->type == END_OF_TOKENS ||
        strcmp(current_token->value, ";") != 0) {
      parse_error_expected("Expected ';' after method call\n");
    }
    current_token++;
    return base;
  }
  if (base->type != NODE_MEMBER_ACCESS) {
    parse_error("Unexpected member access in statement\n");
  }
  if (current_token->type == END_OF_TOKENS ||
      (strcmp(current_token->value, "=") != 0 &&
       strcmp(current_token->value, "+=") != 0 &&
       strcmp(current_token->value, "-=") != 0)) {
    parse_error("Unexpected member access in statement\n");
  }
  Node *node = create_node(NODE_MEMBER_ASSIGN);
  node->line = start_line;
  node->col = start_col;
  node->width = base->width > 0 ? base->width : 1;
  size_t len = strlen(current_token->value);
  node->member_assign.op = malloc(len + 1);
  memcpy(node->member_assign.op, current_token->value, len);
  node->member_assign.op[len] = '\0';
  current_token++;
  node->member_assign.object = base->member_access.object;
  node->member_assign.member = base->member_access.member;
  node->member_assign.value = parse_expression();
  node->member_assign.object_expr = base->member_access.object_expr;
  if (current_token->type == END_OF_TOKENS ||
      strcmp(current_token->value, ";") != 0) {
    parse_error_expected("Expected ';' after assignment\n");
  }
  current_token++;
  return node;
}

/**
 * @brief Parses a member statement (method call or field assignment)
 * @return AST node for the statement (current token is the object name)
 */
static Node *parse_member_statement() {
  int start_line = current_token->line;
  int start_col = current_token->col;
  /* Parse the whole postfix chain so a.b.c works */
  return parse_member_statement_from(parse_primary(), start_line, start_col);
}

/**
 * @brief Parses a variable declaration with a struct/class type name
 * @return AST node for variable declaration (current token is the type)
 */
static Node *parse_typed_var_decl() {
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
  check_reserved_ident(node->var_decl.name);
  current_token++;

  node->var_decl.value = NULL;
  node->var_decl.is_global = 0;

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
    } else if (strcmp(current_token->value, "num") == 0 ||
               strcmp(current_token->value, "bool") == 0 ||
               strcmp(current_token->value, "str") == 0 ||
               strcmp(current_token->value, "char") == 0) {
      return parse_var_decl();
    } else if (strcmp(current_token->value, "arr") == 0) {
      return parse_array_decl();
    } else if (strcmp(current_token->value, "global") == 0) {
      return parse_global_decl();
    } else if (strcmp(current_token->value, "fn") == 0) {
      return parse_function();
    } else if (strcmp(current_token->value, "struct") == 0) {
      return parse_struct_def();
    } else if (strcmp(current_token->value, "class") == 0) {
      return parse_class_def();
    } else if (strcmp(current_token->value, "break") == 0) {
      return parse_break_or_continue(1);
    } else if (strcmp(current_token->value, "continue") == 0) {
      return parse_break_or_continue(0);
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
      if (call == NULL) {
        return NULL;
      }
      if (current_token->type == END_OF_TOKENS ||
          strcmp(current_token->value, ";") != 0) {
        parse_error_expected("Expected ';' after function call\n");
      }
      current_token++;
      return call;
    } else if (current_token[1].value != NULL &&
               strcmp(current_token[1].value, "[") == 0) {
      int node_start_line = current_token->line;
      int node_start_col = current_token->col;
      char *nm = malloc(strlen(current_token->value) + 1);
      strcpy(nm, current_token->value);
      Node *base = create_node(NODE_IDENTIFIER);
      base->identifier.name = nm;
      base->line = current_token->line;
      base->col = current_token->col;
      current_token++; // name
      current_token++; // '['
      Node *index = parse_expression();
      if (current_token->type == END_OF_TOKENS ||
          strcmp(current_token->value, "]") != 0) {
        parse_error_expected("Expected ']' after index\n");
      }
      current_token++; // ']'
      Node *idx = create_node(NODE_INDEX);
      idx->index.base = base;
      idx->index.index = index;
      /* a[i].field / a[i].method() statements reuse the member parser */
      if (current_token->type != END_OF_TOKENS &&
          strcmp(current_token->value, ".") == 0) {
        return parse_member_statement_from(idx, node_start_line,
                                           node_start_col);
      }
      if (current_token->type == END_OF_TOKENS ||
          (strcmp(current_token->value, "=") != 0 &&
           strcmp(current_token->value, "+=") != 0 &&
           strcmp(current_token->value, "-=") != 0)) {
        parse_error_expected("Expected '=', '+=', or '-=' after index\n");
      }
      Node *node = create_node(NODE_INDEX_ASSIGN);
      size_t ol = strlen(current_token->value);
      node->index_assign.op = malloc(ol + 1);
      memcpy(node->index_assign.op, current_token->value, ol);
      node->index_assign.op[ol] = '\0';
      current_token++;
      node->index_assign.base = base;
      node->index_assign.index = index;
      node->index_assign.value = parse_expression();
      if (current_token->type == END_OF_TOKENS ||
          strcmp(current_token->value, ";") != 0) {
        parse_error_expected("Expected ';' after index assignment\n");
      }
      current_token++;
      return node;
    } else if (current_token[1].value != NULL &&
               (strcmp(current_token[1].value, "=") == 0 ||
                strcmp(current_token[1].value, "+=") == 0 ||
                strcmp(current_token[1].value, "-=") == 0)) {
      return parse_assignment();
    } else if (current_token[1].value != NULL &&
               strcmp(current_token[1].value, ".") == 0) {
      return parse_member_statement();
    } else if (current_token[1].type == IDENTIFIER) {
      return parse_typed_var_decl();
    }
  }
  if (current_token->type == END_OF_TOKENS || current_token->value == NULL) {
    parse_error("Unexpected token in statement\n");
  }
  {
    char message[96];
    snprintf(message, sizeof(message), "Unexpected '%s' in statement",
             current_token->value);
    current_token++;
    parse_error(message);
  }
}

Node *Parser(Token *tokens, const char *filename) {
  source_filename = filename;
  current_token = tokens;
  program_head = NULL;
  program_tail = NULL;
  if (setjmp(stmt_jmp) != 0) {
    /* Import depth overflow before any statement ran (push never took). */
    Node *root = program_head;
    program_head = NULL;
    program_tail = NULL;
    return root;
  }
  push_import(filename);

  Node *current = NULL;

  while (current_token->type != END_OF_TOKENS) {
    if (current != NULL && current->type == NODE_RETURN) {
      parse_warning_line("Unreachable code after return");
    }
    Node *stmt = try_parse_statement();
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
    Node *calls[1024];
    int call_count = 0;
    /* collect_calls follows statement links, so walk the list once. */
    collect_calls(program_head, calls, &call_count, 1024);
    for (int k = 0; k < call_count; k++) {
      if (strcmp(calls[k]->func_call.name, "input") == 0 ||
          strcmp(calls[k]->func_call.name, "len") == 0 ||
          strcmp(calls[k]->func_call.name, "tostr") == 0 ||
          strcmp(calls[k]->func_call.name, "tonum") == 0 ||
          strcmp(calls[k]->func_call.name, "readFile") == 0 ||
          strcmp(calls[k]->func_call.name, "writeFile") == 0 ||
          strcmp(calls[k]->func_call.name, "char") == 0 ||
          strcmp(calls[k]->func_call.name, "args") == 0) {
        continue;
      }
      Node *def = find_function_in(program_head, calls[k]->func_call.name);
      if (def == NULL) {
        Node *owner = find_method_owner(program_head, calls[k]->func_call.name);
        if (owner != NULL) {
          char message[128];
          snprintf(message, sizeof(message),
                   "Method '%s' is private to class '%s', call it as "
                   "instance.%s(...)",
                   calls[k]->func_call.name, owner->class_def.name,
                   calls[k]->func_call.name);
          parse_error_at(calls[k]->line, calls[k]->col, calls[k]->width,
                         message);
          continue;
        }
        char message[96];
        snprintf(message, sizeof(message), "Function '%s' is not defined",
                 calls[k]->func_call.name);
        parse_error_at(calls[k]->line, calls[k]->col, calls[k]->width, message);
        continue;
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
        parse_error_at(calls[k]->line, calls[k]->col, calls[k]->width, message);
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
  case NODE_FLOAT_LITERAL:
    printf("Float(%f)", root->float_literal.value);
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
  case NODE_ADD_ASSIGN:
    printf("AddAssign(%s += ", root->add_assign.name);
    print_tree(root->add_assign.value);
    printf(")");
    break;
  case NODE_SUB_ASSIGN:
    printf("SubAssign(%s -= ", root->sub_assign.name);
    print_tree(root->sub_assign.value);
    printf(")");
    break;
  case NODE_MEMBER_ACCESS:
    printf("Member(");
    if (root->member_access.object != NULL) {
      printf("%s", root->member_access.object);
    } else {
      print_tree(root->member_access.object_expr);
    }
    printf(".%s)", root->member_access.member);
    break;
  case NODE_BREAK:
    printf("Break");
    break;
  case NODE_CONTINUE:
    printf("Continue");
    break;
  case NODE_NULL:
    printf("Null");
    break;
  case NODE_STRUCT_DEF:
    printf("Struct(%s %s, fields: ",
           root->struct_def.is_public ? "public" : "private",
           root->struct_def.name);
    print_tree(root->struct_def.fields);
    printf(")");
    break;
  case NODE_CLASS_DEF:
    printf("Class(%s %s", root->class_def.is_public ? "public" : "private",
           root->class_def.name);
    if (root->class_def.base != NULL) {
      printf(" inherit %s", root->class_def.base);
    }
    printf(", fields: ");
    print_tree(root->class_def.fields);
    printf(", methods: ");
    print_tree(root->class_def.methods);
    printf(")");
    break;
  case NODE_METHOD_DEF:
    printf("Method(%s %s, params: ", root->method_def.ret_type,
           root->method_def.name);
    print_tree(root->method_def.params);
    printf(", body: ");
    print_tree(root->method_def.body);
    printf(")");
    break;
  case NODE_NEW:
    printf("New(%s, args: ", root->new_expr.type_name);
    print_tree(root->new_expr.args);
    printf(")");
    break;
  case NODE_INDEX:
    printf("Index(");
    print_tree(root->index.base);
    printf(", ");
    print_tree(root->index.index);
    printf(")");
    break;
  case NODE_METHOD_CALL:
    printf("MethodCall(");
    if (root->method_call.object != NULL) {
      printf("%s", root->method_call.object);
    } else {
      print_tree(root->method_call.object_expr);
    }
    printf(".%s, args: ", root->method_call.method);
    print_tree(root->method_call.args);
    printf(")");
    break;
  case NODE_MEMBER_ASSIGN:
    printf("MemberAssign(");
    if (root->member_assign.object != NULL) {
      printf("%s", root->member_assign.object);
    } else {
      print_tree(root->member_assign.object_expr);
    }
    printf(".%s %s ", root->member_assign.member, root->member_assign.op);
    print_tree(root->member_assign.value);
    printf(")");
    break;
  case NODE_INDEX_ASSIGN:
    printf("IndexAssign(");
    print_tree(root->index_assign.base);
    printf("[");
    print_tree(root->index_assign.index);
    printf("] %s ", root->index_assign.op);
    print_tree(root->index_assign.value);
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
