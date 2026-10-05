#define _CRT_SECURE_NO_WARNINGS

#include "codegen.h"

/** Output assembly file */
static FILE *out;

/** Counter for jump labels (like curly_count in the video) */
static int label_id;

/** Counter for string literals */
static int string_count;

/** Collected string literal values */
static char *string_table[1024];

/**
 * @brief Value categories tracked by the type checker
 */
typedef enum {
  TYPE_INT,    /**< Integers and integer-like values */
  TYPE_FLOAT,  /**< Floating point values */
  TYPE_BOOL,   /**< Comparison results */
  TYPE_STRING, /**< String pointers */
  TYPE_ARRAY   /**< Arrays (not first-class values) */
} ValueType;

/** Variable names in the current function frame */
static char *var_names[256];

/** Declared type of each variable in the current frame */
static ValueType var_types[256];

/** Whether a variable is a function parameter (1) or a local (0) */
static int var_is_param[256];

/** Whether a variable was loaded at least once */
static int var_used[256];

/** Source line of each variable declaration */
static int var_line[256];

/** Source column of each variable declaration */
static int var_col[256];

/** Number of variables in the current frame */
static int var_count;

/** Array names in the current frame */
static char *array_names[64];

/** First slot of each array in the current frame */
static int array_base[64];

/** Element count of each array in the current frame */
static int array_len[64];

/** Non-zero if the array holds float (doubles) rather than ints */
static int array_is_float[64];

/** Number of arrays in the current frame */
static int array_count;

/** Reserved stack bytes for locals */
static int frame_size;

/** Non-zero while generating a function body (returns use ret) */
static int in_function;

/** Non-zero if the program defines fn main (it lives at jot_main) */
static int has_user_main;

/** Names of defined functions for argument type checking */
static char *sig_names[256];

/** Parameter lists of defined functions, parallel to sig_names */
static Node *sig_params[256];

/** Number of recorded signatures */
static int sig_count;

/** Non-zero if sig param i holds floats (promoted from num) */
static int sig_param_float[256][32];

/** Non-zero if function returns only comparisons (bool), never floats */
static int sig_is_bool_only[256];

/** Forward declaration for recursive generation */
static void gen_expression(Node *node);

/** Forward declaration for statement generation */
static void gen_statement(Node *node);

/** Forward declaration for block generation */
static void gen_block(Node *list);

/** Forward declaration for float param scan */
static void scan_float_calls(Node *node);

/** Source file name for error messages */
static const char *codegen_source;

/**
 * @brief Prints a modern error for an AST node and exits
 * @param node Fault node, may be NULL (uses 1:1 then)
 * @param format printf-style message without the Error: prefix
 */
static NORETURN void codegen_error(Node *node, const char *format, ...) {
  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  int line = 1;
  int col = 1;
  int width = 1;
  if (node != NULL) {
    if (node->line > 0) {
      line = node->line;
    }
    if (node->col > 0) {
      col = node->col;
    }
    if (node->width > 0) {
      width = node->width;
    }
  }
  term_report(TERM_ERROR, codegen_source, line, col, width, message);
  exit(1);
}

/**
 * @brief Warns without exiting, with source context
 * @param node Fault node, may be NULL (uses 1:1 then)
 * @param format printf-style message without the Warning: prefix
 */
static void codegen_warning(Node *node, const char *format, ...) {
  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  int line = 1;
  int col = 1;
  int width = 1;
  if (node != NULL) {
    if (node->line > 0) {
      line = node->line;
    }
    if (node->col > 0) {
      col = node->col;
    }
    if (node->width > 0) {
      width = node->width;
    }
  }
  term_report(TERM_WARNING, codegen_source, line, col, width, message);
}

/**
 * @brief Finds a variable in the current frame (innermost wins)
 * @param name Variable name to look up
 * @return Slot index, or -1 if not found
 */
static int find_var(const char *name) {
  for (int i = var_count - 1; i >= 0; i--) {
    if (var_names[i] != NULL && strcmp(var_names[i], name) == 0) {
      return i;
    }
  }
  return -1;
}

/**
 * @brief Finds an array in the current frame
 * @param name Array name to look up
 * @return Slot index, or -1 if not found
 */
static int find_array(const char *name) {
  for (int i = 0; i < array_count; i++) {
    if (strcmp(array_names[i], name) == 0) {
      return i;
    }
  }
  return -1;
}

/**
 * @brief Adds a variable to the current frame
 * @param node Declaration node at fault if the frame is full
 * @param name Variable name to store
 * @param type Declared value category
 * @param is_param Non-zero if the variable is a function parameter
 * @return Slot index of the new variable
 */
static int add_var(Node *node, const char *name, ValueType type, int is_param) {
  if (var_count >= 256) {
    codegen_error(node, "Too many variables in function");
  }
  size_t len = strlen(name);
  var_names[var_count] = malloc(len + 1);
  memcpy(var_names[var_count], name, len);
  var_names[var_count][len] = '\0';
  var_types[var_count] = type;
  var_is_param[var_count] = is_param;
  var_used[var_count] = 0;
  var_line[var_count] = node != NULL ? node->line : 1;
  var_col[var_count] = node != NULL ? node->col : 1;
  var_count++;
  return var_count - 1;
}

/**
 * @brief Loads a variable slot, warning on bare parameter access
 * @param node Identifier node at fault
 * @param name Variable name from source
 * @return Slot index
 */
static int require_var(Node *node, const char *name) {
  int slot = find_var(name);
  if (slot < 0) {
    codegen_error(node, "Variable '%s' not declared", name);
  }
  if (var_is_param[slot]) {
    codegen_warning(node, "Parameter '%s' should be accessed as self.%s", name,
                    name);
  }
  var_used[slot] = 1;
  return slot;
}

/**
 * @brief Loads a parameter slot for self.member access
 * @param node Member access node at fault
 * @param member Member name from source
 * @return Slot index
 */
static int require_param(Node *node, const char *member) {
  int slot = find_var(member);
  if (slot < 0 || !var_is_param[slot]) {
    codegen_error(node, "'%s' is not a parameter", member);
  }
  var_used[slot] = 1;
  return slot;
}

/**
 * @brief Warns about unused locals declared in a scope
 * @param from First slot of the scope, restores nothing
 */
static void check_unused_vars(int from) {
  for (int i = from; i < var_count; i++) {
    if (var_names[i] != NULL && !var_is_param[i] && !var_used[i]) {
      char message[96];
      snprintf(message, sizeof(message), "Variable '%s' is never used",
               var_names[i]);
      term_report(TERM_WARNING, codegen_source, var_line[i], var_col[i], 1,
                  message);
    }
  }
}

/**
 * @brief Maps a type keyword to a value category
 * @param keyword Type keyword from source (num, bool, string, char, array)
 * @return Matching value category, num for anything else
 */
static ValueType type_keyword(const char *keyword) {
  if (strcmp(keyword, "bool") == 0) {
    return TYPE_BOOL;
  }
  if (strcmp(keyword, "string") == 0) {
    return TYPE_STRING;
  }
  if (strcmp(keyword, "array") == 0) {
    return TYPE_ARRAY;
  }
  return TYPE_INT;
}

/**
 * @brief Names a value category for messages
 * @param type Value category
 * @return Type name (num, bool, string, array)
 */
static const char *type_name(ValueType type) {
  switch (type) {
  case TYPE_BOOL:
    return "bool";
  case TYPE_FLOAT:
    return "num";
  case TYPE_STRING:
    return "string";
  case TYPE_ARRAY:
    return "array";
  default:
    return "num";
  }
}

/**
 * @brief Checks if a value category behaves as a number
 * @param type Value category
 * @return Non-zero for int, float, and bool
 */
static int is_numeric(ValueType type) {
  return type == TYPE_INT || type == TYPE_FLOAT || type == TYPE_BOOL;
}

/**
 * @brief Checks two categories are assignment compatible
 * @param declared Declared type of the target
 * @param given Inferred type of the value
 * @return Non-zero if the value may be stored in the target
 */
static int types_compatible(ValueType declared, ValueType given) {
  if (declared == TYPE_STRING || given == TYPE_STRING) {
    return declared == TYPE_STRING && given == TYPE_STRING;
  }
  if (declared == TYPE_ARRAY || given == TYPE_ARRAY) {
    return 0;
  }
  return is_numeric(declared) && is_numeric(given);
}

/**
 * @brief Infers the value category without warnings or use-marking
 * @param node Expression node
 * @return Inferred category, int for unknown identifiers/calls
 */
static ValueType peek_type(Node *node) {
  if (node == NULL) {
    return TYPE_INT;
  }
  switch (node->type) {
  case NODE_INT_LITERAL:
    return TYPE_INT;
  case NODE_FLOAT_LITERAL:
    return TYPE_FLOAT;
  case NODE_STRING_LITERAL:
    return TYPE_STRING;
  case NODE_IDENTIFIER: {
    int slot = find_var(node->identifier.name);
    if (slot < 0) {
      return TYPE_INT;
    }
    return var_types[slot];
  }
  case NODE_FUNC_CALL: {
    if (strcmp(node->func_call.name, "input") == 0) {
      return TYPE_INT;
    }
    for (int s = 0; s < sig_count; s++) {
      if (strcmp(sig_names[s], node->func_call.name) == 0) {
        if (sig_is_bool_only[s]) {
          return TYPE_INT;
        }
        for (int j = 0; j < 32; j++) {
          if (sig_param_float[s][j]) {
            return TYPE_FLOAT;
          }
        }
        return TYPE_INT;
      }
    }
    return TYPE_INT;
  }
  case NODE_BINARY_OP: {
    const char *op = node->binary_op.op;
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
        strcmp(op, "<") == 0 || strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 ||
        strcmp(op, ">=") == 0) {
      return TYPE_BOOL;
    }
    ValueType lt = peek_type(node->binary_op.left);
    ValueType rt = peek_type(node->binary_op.right);
    if (lt == TYPE_FLOAT || rt == TYPE_FLOAT) {
      return TYPE_FLOAT;
    }
    return TYPE_INT;
  }
  case NODE_MEMBER_ACCESS: {
    int slot = find_var(node->member_access.member);
    if (slot < 0 || !var_is_param[slot]) {
      return TYPE_INT;
    }
    return var_types[slot];
  }
  default:
    return TYPE_INT;
  }
}

/**
 * @brief Infers the value category of an expression
 * @param node Expression node (identifiers must be declared)
 * @return Inferred category, int for calls with unknown signatures
 */
static ValueType expr_type(Node *node) {
  switch (node->type) {
  case NODE_INT_LITERAL:
    return TYPE_INT;
  case NODE_FLOAT_LITERAL:
    return TYPE_FLOAT;
  case NODE_STRING_LITERAL:
    return TYPE_STRING;
  case NODE_IDENTIFIER: {
    int slot = require_var(node, node->identifier.name);
    return var_types[slot];
  }
  case NODE_FUNC_CALL: {
    if (strcmp(node->func_call.name, "input") == 0) {
      return TYPE_INT;
    }
    for (int s = 0; s < sig_count; s++) {
      if (strcmp(sig_names[s], node->func_call.name) == 0) {
        if (sig_is_bool_only[s]) {
          return TYPE_INT;
        }
        for (int j = 0; j < 32; j++) {
          if (sig_param_float[s][j]) {
            return TYPE_FLOAT;
          }
        }
        return TYPE_INT;
      }
    }
    return TYPE_INT;
  }
  case NODE_BINARY_OP: {
    const char *op = node->binary_op.op;
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
        strcmp(op, "<") == 0 || strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 ||
        strcmp(op, ">=") == 0) {
      return TYPE_BOOL;
    }
    ValueType lt = peek_type(node->binary_op.left);
    ValueType rt = peek_type(node->binary_op.right);
    if (lt == TYPE_FLOAT || rt == TYPE_FLOAT) {
      return TYPE_FLOAT;
    }
    return TYPE_INT;
  }
  case NODE_MEMBER_ACCESS: {
    int slot = require_param(node, node->member_access.member);
    return var_types[slot];
  }
  default:
    return TYPE_INT;
  }
}

/**
 * @brief Checks if an operator string is a comparison
 * @param op Operator string
 * @return Non-zero for ==, !=, <, >, <=, >=
 */
static int is_comparison_op(const char *op) {
  return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
         strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
         strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0;
}

/**
 * @brief Scans a statement list for returns, tracking bool-only status
 * @param node List to scan (follows right chain, recurses into blocks)
 * @param found Receives non-zero if any return was seen
 * @param all_bool Receives non-zero if all returns so far are comparisons
 */
static void scan_returns_bool_only(Node *node, int *found, int *all_bool) {
  for (Node *s = node; s != NULL; s = s->right) {
    switch (s->type) {
    case NODE_RETURN: {
      *found = 1;
      Node *v = s->return_stmt.value;
      if (!(v != NULL && v->type == NODE_BINARY_OP &&
            is_comparison_op(v->binary_op.op))) {
        *all_bool = 0;
      }
      break;
    }
    case NODE_IF:
      scan_returns_bool_only(s->if_stmt.body, found, all_bool);
      scan_returns_bool_only(s->if_stmt.else_body, found, all_bool);
      break;
    case NODE_WHILE:
      scan_returns_bool_only(s->while_stmt.body, found, all_bool);
      break;
    case NODE_FOR:
      scan_returns_bool_only(s->for_stmt.body, found, all_bool);
      break;
    default:
      break;
    }
  }
}

/**
 * @brief Scans calls to promote num params receiving floats to TYPE_FLOAT
 * @param node Node to visit (follows right sibling chain)
 */
static void scan_float_calls(Node *node) {
  if (node == NULL) {
    return;
  }
  if (node->type == NODE_FUNC_CALL) {
    if (strcmp(node->func_call.name, "input") != 0) {
      for (int s = 0; s < sig_count; s++) {
        if (strcmp(sig_names[s], node->func_call.name) == 0) {
          int idx = 0;
          for (Node *a = node->func_call.args; a != NULL && idx < 32;
               a = a->right, idx++) {
            if (peek_type(a) == TYPE_FLOAT) {
              sig_param_float[s][idx] = 1;
            }
          }
          break;
        }
      }
    }
    scan_float_calls(node->func_call.args);
  } else {
    switch (node->type) {
    case NODE_VAR_DECL:
      scan_float_calls(node->var_decl.value);
      break;
    case NODE_PRINT:
      scan_float_calls(node->print_stmt.value);
      break;
    case NODE_RETURN:
      scan_float_calls(node->return_stmt.value);
      break;
    case NODE_BINARY_OP:
      scan_float_calls(node->binary_op.left);
      scan_float_calls(node->binary_op.right);
      break;
    case NODE_IF:
      scan_float_calls(node->if_stmt.condition);
      scan_float_calls(node->if_stmt.body);
      scan_float_calls(node->if_stmt.else_body);
      break;
    case NODE_WHILE:
      scan_float_calls(node->while_stmt.condition);
      scan_float_calls(node->while_stmt.body);
      break;
    case NODE_FOR:
      scan_float_calls(node->for_stmt.body);
      break;
    case NODE_FUNCTION:
      scan_float_calls(node->function.params);
      scan_float_calls(node->function.body);
      break;
    case NODE_ASSIGNMENT:
      scan_float_calls(node->assignment.value);
      break;
    case NODE_ADD_ASSIGN:
      scan_float_calls(node->add_assign.value);
      break;
    case NODE_SUB_ASSIGN:
      scan_float_calls(node->sub_assign.value);
      break;
    case NODE_ARRAY_DECL:
      scan_float_calls(node->array_decl.elements);
      break;
    case NODE_ARRAY_LITERAL:
      scan_float_calls(node->array_literal.elements);
      break;
    default:
      break;
    }
  }
  scan_float_calls(node->right);
}

/**
 * @brief Adds a string literal to the string table
 * @param value Raw string value (without quotes)
 * @return Index of the string in the table
 */
static int add_string(const char *value) {
  for (int i = 0; i < string_count; i++) {
    if (strcmp(string_table[i], value) == 0) {
      return i;
    }
  }
  if (string_count >= 1024) {
    codegen_error(NULL, "Too many string literals");
  }
  size_t len = strlen(value);
  string_table[string_count] = malloc(len + 1);
  memcpy(string_table[string_count], value, len);
  string_table[string_count][len] = '\0';
  string_count++;
  return string_count - 1;
}

/**
 * @brief Checks if a character can start an interpolated name
 * @param c Character to check
 * @return Non-zero if it can start a name
 */
static int is_name_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

/**
 * @brief Checks if a character can continue an interpolated name
 * @param c Character to check
 * @return Non-zero if it can continue a name
 */
static int is_name_char(char c) {
  return is_name_start(c) || (c >= '0' && c <= '9');
}

/**
 * @brief Collects literal chunks of an interpolated print string
 * @param value Raw string value (may contain {name} placeholders)
 */
static void collect_print_chunks(const char *value) {
  size_t start = 0;
  size_t i = 0;
  while (value[i] != '\0') {
    if (value[i] == '{') {
      size_t j = i + 1;
      if (is_name_start(value[j])) {
        while (is_name_char(value[j])) {
          j++;
        }
        if (value[j] == '.' && is_name_start(value[j + 1])) {
          j++;
          while (is_name_char(value[j])) {
            j++;
          }
        }
        if (value[j] == '}') {
          if (i > start) {
            size_t len = i - start;
            char *chunk = malloc(len + 1);
            memcpy(chunk, value + start, len);
            chunk[len] = '\0';
            add_string(chunk);
            free(chunk);
          }
          i = j + 1;
          start = i;
          continue;
        }
      }
    }
    i++;
  }
  if (value[i] == '\0' && i > start) {
    size_t len = i - start;
    char *chunk = malloc(len + 1);
    memcpy(chunk, value + start, len);
    chunk[len] = '\0';
    add_string(chunk);
    free(chunk);
  }
}

/**
 * @brief Walks the AST and collects every string literal
 * @param node Node to visit (follows right sibling chain)
 */
static void collect_strings(Node *node) {
  if (node == NULL) {
    return;
  }

  switch (node->type) {
  case NODE_STRING_LITERAL:
    add_string(node->string_literal.value);
    break;
  case NODE_VAR_DECL:
    collect_strings(node->var_decl.value);
    break;
  case NODE_PRINT:
    if (node->print_stmt.value != NULL &&
        node->print_stmt.value->type == NODE_STRING_LITERAL) {
      collect_print_chunks(node->print_stmt.value->string_literal.value);
    } else {
      collect_strings(node->print_stmt.value);
    }
    break;
  case NODE_RETURN:
    collect_strings(node->return_stmt.value);
    break;
  case NODE_BINARY_OP:
    collect_strings(node->binary_op.left);
    collect_strings(node->binary_op.right);
    break;
  case NODE_IF:
    collect_strings(node->if_stmt.condition);
    collect_strings(node->if_stmt.body);
    collect_strings(node->if_stmt.else_body);
    break;
  case NODE_WHILE:
    collect_strings(node->while_stmt.condition);
    collect_strings(node->while_stmt.body);
    break;
  case NODE_FOR:
    collect_strings(node->for_stmt.body);
    break;
  case NODE_FUNCTION:
    collect_strings(node->function.params);
    collect_strings(node->function.body);
    break;
  case NODE_FUNC_CALL:
    collect_strings(node->func_call.args);
    break;
  case NODE_ASSIGNMENT:
    collect_strings(node->assignment.value);
    break;
  case NODE_ARRAY_DECL:
    collect_strings(node->array_decl.elements);
    break;
  case NODE_ARRAY_LITERAL:
    collect_strings(node->array_literal.elements);
    break;
  default:
    break;
  }

  collect_strings(node->right);
}

/**
 * @brief Writes a raw string value as NASM db content
 * @param value Raw string value (may contain backslash-n)
 */
static void write_nasm_string(const char *value) {
  int in_quote = 0;
  int first = 1;
  for (size_t i = 0; value[i] != '\0';) {
    if (value[i] == '\\' && value[i + 1] == 'n') {
      if (in_quote) {
        fprintf(out, "\"");
        in_quote = 0;
      }
      if (!first) {
        fprintf(out, ", ");
      }
      fprintf(out, "10");
      first = 0;
      i += 2;
      continue;
    }
    if (value[i] == '\n') {
      if (in_quote) {
        fprintf(out, "\"");
        in_quote = 0;
      }
      if (!first) {
        fprintf(out, ", ");
      }
      if (!first) {
        fprintf(out, ", ");
      }
      fprintf(out, "10");
      first = 0;
      i++;
      continue;
    }
    if (!in_quote) {
      if (!first) {
        fprintf(out, ", ");
      }
      fprintf(out, "\"");
      in_quote = 1;
      first = 0;
    }
    if (value[i] == '"') {
      fprintf(out, "\\\"");
    } else if (value[i] == '\\') {
      fprintf(out, "\\\\");
    } else {
      fprintf(out, "%c", value[i]);
    }
    i++;
  }
  if (in_quote) {
    fprintf(out, "\"");
  }
  fprintf(out, ", 0");
}

/**
 * @brief Emits the data section (formats and string literals)
 */
static void gen_data_section() {
  fprintf(out, "section .data\n");
  fprintf(out, "  fmt_int db \"%%lld\", 10, 0\n");
  fprintf(out, "  fmt_int_raw db \"%%lld\", 0\n");
  fprintf(out, "  fmt_float db \"%%.15g\", 10, 0\n");
  fprintf(out, "  fmt_float_raw db \"%%.15g\", 0\n");
  fprintf(out, "  fmt_str db \"%%s\", 0\n");
  fprintf(out, "  fmt_input db \"%%d\", 0\n");
  fprintf(out, "  fmt_invalid db \"invalid input: expected integer\", 10, 0\n");
  fprintf(out, "  fmt_overflow db \"integer overflow\", 10, 0\n");
  fprintf(out, "  fmt_divzero db \"division by zero\", 10, 0\n");
  fprintf(out, "  fmt_stack db \"stack overflow\", 10, 0\n");
  fprintf(out, "  stack_floor dq 0\n");
  for (int i = 0; i < string_count; i++) {
    fprintf(out, "  str%d db ", i);
    if (string_table[i][0] == '\0') {
      fprintf(out, "0");
    } else {
      write_nasm_string(string_table[i]);
    }
    fprintf(out, "\n");
  }
}

/**
 * @brief Emits a function prologue with room for locals
 */
static void gen_prologue() {
  fprintf(out, "  push rbp\n");
  fprintf(out, "  mov rbp, rsp\n");
  fprintf(out, "  sub rsp, %d\n", frame_size);
  fprintf(out, "  cmp rsp, [rel stack_floor]\n");
  fprintf(out, "  jb stack_overflow_trap\n");
}

/**
 * @brief Emits a fatal runtime trap (message to stdout, exit 3)
 * @param label Trap label to define
 * @param format Data label holding the message
 */
static void gen_trap(const char *label, const char *format) {
  fprintf(out, "%s:\n", label);
  fprintf(out, "  mov rax, rsp\n");
  fprintf(out, "  and rax, 8\n");
  fprintf(out, "  sub rsp, rax\n");
  fprintf(out, "  lea rcx, [rel %s]\n", format);
  fprintf(out, "  sub rsp, 32\n");
  fprintf(out, "  call printf\n");
  fprintf(out, "  add rsp, 32\n");
  fprintf(out, "  mov ecx, 3\n");
  fprintf(out, "  sub rsp, 32\n");
  fprintf(out, "  call exit\n");
}

/**
 * @brief Maps a Jot function name to its assembly label
 * @param name Function name from source
 * @return Assembly label (fn main lives at jot_main, entry stays main)
 */
static const char *func_label(const char *name) {
  if (has_user_main && strcmp(name, "main") == 0) {
    return "jot_main";
  }
  return name;
}

/**
 * @brief Generates a stdin integer read, result left in rax
 * @details Aligns the stack dynamically since calls inside expressions
 * may run with rsp 8 off 16 byte alignment.
 */
static void gen_input() {
  fprintf(out, "  mov rax, rsp\n");
  fprintf(out, "  and rax, 8\n");
  fprintf(out, "  sub rsp, rax\n");
  fprintf(out, "  sub rsp, 48\n");
  fprintf(out, "  mov [rsp + 32], rax\n");
  fprintf(out, "  mov dword [rsp + 40], 0\n");
  fprintf(out, "  lea rcx, [rel fmt_input]\n");
  fprintf(out, "  lea rdx, [rsp + 40]\n");
  fprintf(out, "  call scanf\n");
  fprintf(out, "  cmp rax, 1\n");
  fprintf(out, "  jne input_error_trap\n");
  fprintf(out, "  movsxd rax, dword [rsp + 40]\n");
  fprintf(out, "  mov r10, [rsp + 32]\n");
  fprintf(out, "  add rsp, 48\n");
  fprintf(out, "  add rsp, r10\n");
}

/**
 * @brief Generates code for a function call, result left in rax
 * @param node Call node to generate
 * @details First four arguments use rcx, rdx, r8, r9. The rest spill
 * onto the stack above the 32 byte shadow space (Windows x64).
 */
static void gen_call(Node *node) {
  const char *regs[4] = {"rcx", "rdx", "r8", "r9"};
  int arg_count = 0;
  for (Node *a = node->func_call.args; a != NULL; a = a->right) {
    arg_count++;
  }
  if (strcmp(node->func_call.name, "input") == 0) {
    if (arg_count != 0) {
      codegen_error(node, "input takes no arguments");
    }
    gen_input();
    return;
  }
  for (int s = 0; s < sig_count; s++) {
    if (strcmp(sig_names[s], node->func_call.name) == 0) {
      Node *a = node->func_call.args;
      Node *p = sig_params[s];
      while (a != NULL && p != NULL) {
        ValueType given = expr_type(a);
        ValueType want = p->type == NODE_VAR_DECL
                             ? type_keyword(p->var_decl.var_type)
                             : TYPE_INT;
        if (!types_compatible(want, given)) {
          codegen_error(a, "Cannot pass %s to %s parameter", type_name(given),
                        type_name(want));
        }
        a = a->right;
        p = p->right;
      }
      break;
    }
  }
  int frame = 64 + 8 * (arg_count > 4 ? arg_count - 4 : 0);
  if (frame % 16 != 0) {
    frame += 8;
  }
  /* Find signature for conversions (NULL if unknown). */
  Node *sig_param_head = NULL;
  int sig_idx_for_call = -1;
  for (int s = 0; s < sig_count; s++) {
    if (strcmp(sig_names[s], node->func_call.name) == 0) {
      sig_param_head = sig_params[s];
      sig_idx_for_call = s;
      break;
    }
  }
  fprintf(out, "  sub rsp, %d\n", frame);
  int i = 0;
  for (Node *a = node->func_call.args; a != NULL; a = a->right) {
    ValueType given = peek_type(a);
    ValueType want = TYPE_INT;
    Node *p = sig_param_head;
    for (int k = 0; k < i && p != NULL; k++) {
      p = p->right;
    }
    if (p != NULL) {
      want = p->type == NODE_VAR_DECL ? type_keyword(p->var_decl.var_type)
                                      : TYPE_INT;
      if (want == TYPE_INT && sig_idx_for_call >= 0 && i < 32 &&
          sig_param_float[sig_idx_for_call][i]) {
        want = TYPE_FLOAT;
      }
    }
    gen_expression(a);
    if ((want == TYPE_INT || want == TYPE_BOOL) && given == TYPE_FLOAT) {
      fprintf(out, "  movq xmm0, rax\n");
      fprintf(out, "  cvttsd2si rax, xmm0\n");
    } else if (want == TYPE_FLOAT &&
               (given == TYPE_INT || given == TYPE_BOOL)) {
      fprintf(out, "  cvtsi2sd xmm0, rax\n");
      fprintf(out, "  movq rax, xmm0\n");
    }
    if (i < 4) {
      fprintf(out, "  mov [rsp + %d], rax\n", frame - 32 + 8 * i);
    } else {
      fprintf(out, "  mov [rsp + %d], rax\n", 32 + 8 * (i - 4));
    }
    i++;
  }
  for (i = 0; i < arg_count && i < 4; i++) {
    fprintf(out, "  mov %s, [rsp + %d]\n", regs[i], frame - 32 + 8 * i);
  }
  fprintf(out, "  call %s\n", func_label(node->func_call.name));
  fprintf(out, "  add rsp, %d\n", frame);
}

/**
 * @brief Generates code for an expression, result left in rax
 * @param node Expression node to generate
 */
static void gen_expression(Node *node) {
  if (node == NULL) {
    codegen_error(NULL, "NULL expression in codegen");
  }

  switch (node->type) {
  case NODE_INT_LITERAL:
    fprintf(out, "  mov rax, %lld\n", node->int_literal.value);
    break;
  case NODE_FLOAT_LITERAL: {
    unsigned long long float_bits;
    memcpy(&float_bits, &node->float_literal.value, sizeof(double));
    fprintf(out, "  mov rax, 0x%llx\n", float_bits);
    break;
  }
  case NODE_IDENTIFIER: {
    int slot = require_var(node, node->identifier.name);
    fprintf(out, "  mov rax, [rbp - %d]\n", (slot + 1) * 8);
    break;
  }
  case NODE_MEMBER_ACCESS: {
    if (strcmp(node->member_access.object, "self") != 0) {
      codegen_error(node, "Only self.member access is supported");
    }
    int slot = require_param(node, node->member_access.member);
    fprintf(out, "  mov rax, [rbp - %d]\n", (slot + 1) * 8);
    break;
  }
  case NODE_STRING_LITERAL: {
    int idx = add_string(node->string_literal.value);
    fprintf(out, "  lea rax, [rel str%d]\n", idx);
    break;
  }
  case NODE_FUNC_CALL:
    gen_call(node);
    break;
  case NODE_ARRAY_LITERAL:
    codegen_error(node, "Array literal not supported in codegen expression");
    break;
  case NODE_BINARY_OP: {
    const char *op = node->binary_op.op;
    ValueType left_type = peek_type(node->binary_op.left);
    ValueType right_type = peek_type(node->binary_op.right);
    if (!is_numeric(left_type) || !is_numeric(right_type)) {
      ValueType bad = !is_numeric(left_type) ? left_type : right_type;
      codegen_error(node, "Operator '%s' cannot be applied to %s", op,
                    type_name(bad));
    }
    int is_float = (left_type == TYPE_FLOAT || right_type == TYPE_FLOAT);
    if (strcmp(op, "%") == 0 && is_float) {
      codegen_error(node, "Operator '%%' cannot be applied to float");
    }
    gen_expression(node->binary_op.left);
    fprintf(out, "  push rax\n");
    gen_expression(node->binary_op.right);
    fprintf(out, "  mov rbx, rax\n");
    fprintf(out, "  pop rax\n");
    if (!is_float) {
      if (strcmp(op, "+") == 0) {
        fprintf(out, "  add rax, rbx\n");
        fprintf(out, "  jo overflow_trap\n");
      } else if (strcmp(op, "-") == 0) {
        fprintf(out, "  sub rax, rbx\n");
        fprintf(out, "  jo overflow_trap\n");
      } else if (strcmp(op, "*") == 0) {
        fprintf(out, "  imul rax, rbx\n");
        fprintf(out, "  jo overflow_trap\n");
      } else if (strcmp(op, "/") == 0) {
        if (node->binary_op.right->type == NODE_INT_LITERAL &&
            node->binary_op.right->int_literal.value == 0) {
          codegen_error(node->binary_op.right, "Division by zero");
        }
        fprintf(out, "  test rbx, rbx\n");
        fprintf(out, "  jz divzero_trap\n");
        fprintf(out, "  cqo\n");
        fprintf(out, "  idiv rbx\n");
      } else if (strcmp(op, "%") == 0) {
        if (node->binary_op.right->type == NODE_INT_LITERAL &&
            node->binary_op.right->int_literal.value == 0) {
          codegen_error(node->binary_op.right, "Division by zero");
        }
        fprintf(out, "  test rbx, rbx\n");
        fprintf(out, "  jz divzero_trap\n");
        fprintf(out, "  cqo\n");
        fprintf(out, "  idiv rbx\n");
        fprintf(out, "  mov rax, rdx\n");
      } else if (strcmp(op, "==") == 0) {
        fprintf(out, "  cmp rax, rbx\n");
        fprintf(out, "  sete al\n");
        fprintf(out, "  movzx rax, al\n");
      } else if (strcmp(op, "!=") == 0) {
        fprintf(out, "  cmp rax, rbx\n");
        fprintf(out, "  setne al\n");
        fprintf(out, "  movzx rax, al\n");
      } else if (strcmp(op, "<") == 0) {
        fprintf(out, "  cmp rax, rbx\n");
        fprintf(out, "  setl al\n");
        fprintf(out, "  movzx rax, al\n");
      } else if (strcmp(op, ">") == 0) {
        fprintf(out, "  cmp rax, rbx\n");
        fprintf(out, "  setg al\n");
        fprintf(out, "  movzx rax, al\n");
      } else if (strcmp(op, "<=") == 0) {
        fprintf(out, "  cmp rax, rbx\n");
        fprintf(out, "  setle al\n");
        fprintf(out, "  movzx rax, al\n");
      } else if (strcmp(op, ">=") == 0) {
        fprintf(out, "  cmp rax, rbx\n");
        fprintf(out, "  setge al\n");
        fprintf(out, "  movzx rax, al\n");
      } else {
        codegen_error(node, "Unsupported operator '%s' in codegen", op);
      }
      break;
    }
    /* Float path: rax holds left bits, rbx holds right bits. */
    if (left_type == TYPE_FLOAT) {
      fprintf(out, "  movq xmm0, rax\n");
    } else {
      fprintf(out, "  cvtsi2sd xmm0, rax\n");
    }
    if (right_type == TYPE_FLOAT) {
      fprintf(out, "  movq xmm1, rbx\n");
    } else {
      fprintf(out, "  cvtsi2sd xmm1, rbx\n");
    }
    if (strcmp(op, "+") == 0) {
      fprintf(out, "  addsd xmm0, xmm1\n");
      fprintf(out, "  movq rax, xmm0\n");
    } else if (strcmp(op, "-") == 0) {
      fprintf(out, "  subsd xmm0, xmm1\n");
      fprintf(out, "  movq rax, xmm0\n");
    } else if (strcmp(op, "*") == 0) {
      fprintf(out, "  mulsd xmm0, xmm1\n");
      fprintf(out, "  movq rax, xmm0\n");
    } else if (strcmp(op, "/") == 0) {
      fprintf(out, "  divsd xmm0, xmm1\n");
      fprintf(out, "  movq rax, xmm0\n");
    } else if (strcmp(op, "==") == 0) {
      fprintf(out, "  ucomisd xmm0, xmm1\n");
      fprintf(out, "  sete al\n");
      fprintf(out, "  setnp bl\n");
      fprintf(out, "  and al, bl\n");
      fprintf(out, "  movzx rax, al\n");
    } else if (strcmp(op, "!=") == 0) {
      fprintf(out, "  ucomisd xmm0, xmm1\n");
      fprintf(out, "  setne al\n");
      fprintf(out, "  setp bl\n");
      fprintf(out, "  or al, bl\n");
      fprintf(out, "  movzx rax, al\n");
    } else if (strcmp(op, "<") == 0) {
      fprintf(out, "  ucomisd xmm0, xmm1\n");
      fprintf(out, "  setb al\n");
      fprintf(out, "  setnp bl\n");
      fprintf(out, "  and al, bl\n");
      fprintf(out, "  movzx rax, al\n");
    } else if (strcmp(op, ">") == 0) {
      fprintf(out, "  ucomisd xmm0, xmm1\n");
      fprintf(out, "  seta al\n");
      fprintf(out, "  movzx rax, al\n");
    } else if (strcmp(op, "<=") == 0) {
      fprintf(out, "  ucomisd xmm0, xmm1\n");
      fprintf(out, "  setbe al\n");
      fprintf(out, "  setnp bl\n");
      fprintf(out, "  and al, bl\n");
      fprintf(out, "  movzx rax, al\n");
    } else if (strcmp(op, ">=") == 0) {
      fprintf(out, "  ucomisd xmm0, xmm1\n");
      fprintf(out, "  setae al\n");
      fprintf(out, "  movzx rax, al\n");
    } else {
      codegen_error(node, "Unsupported operator '%s' in codegen", op);
    }
    break;
  }
  default:
    codegen_error(node, "Unexpected expression in codegen");
    break;
  }
}

/**
 * @brief Emits cmp plus a jump to a false label for a condition
 * @param cond Condition expression node
 * @param false_label Label to jump to when the condition is false
 */
static void gen_condition_jump(Node *cond, int false_label) {
  if (cond != NULL) {
    ValueType cond_type = peek_type(cond);
    /* Ensure variables exist (emits errors/warnings via expr_type path). */
    if (cond_type == TYPE_STRING || cond_type == TYPE_ARRAY) {
      /* Re-run expr_type for accurate error message with location. */
      ValueType checked = expr_type(cond);
      if (!is_numeric(checked)) {
        codegen_error(cond, "Condition must be a number, got %s",
                      type_name(checked));
      }
    } else if (!is_numeric(cond_type)) {
      ValueType checked = expr_type(cond);
      codegen_error(cond, "Condition must be a number, got %s",
                    type_name(checked));
    }
  }
  if (cond != NULL && cond->type == NODE_BINARY_OP) {
    const char *op = cond->binary_op.op;
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
        strcmp(op, "<") == 0 || strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 ||
        strcmp(op, ">=") == 0) {
      ValueType lt = peek_type(cond->binary_op.left);
      ValueType rt = peek_type(cond->binary_op.right);
      int is_float = (lt == TYPE_FLOAT || rt == TYPE_FLOAT);
      if (!is_float) {
        gen_expression(cond->binary_op.left);
        fprintf(out, "  push rax\n");
        gen_expression(cond->binary_op.right);
        fprintf(out, "  mov rbx, rax\n");
        fprintf(out, "  pop rax\n");
        fprintf(out, "  cmp rax, rbx\n");
        if (strcmp(op, "==") == 0) {
          fprintf(out, "  jne label%d\n", false_label);
        } else if (strcmp(op, "!=") == 0) {
          fprintf(out, "  je label%d\n", false_label);
        } else if (strcmp(op, "<") == 0) {
          fprintf(out, "  jge label%d\n", false_label);
        } else if (strcmp(op, ">") == 0) {
          fprintf(out, "  jle label%d\n", false_label);
        } else if (strcmp(op, "<=") == 0) {
          fprintf(out, "  jg label%d\n", false_label);
        } else {
          fprintf(out, "  jl label%d\n", false_label);
        }
        return;
      }
      /* Float comparisons: reuse expression logic (NaN-safe), then test. */
      gen_expression(cond);
      fprintf(out, "  cmp rax, 0\n");
      fprintf(out, "  je label%d\n", false_label);
      return;
    }
  }
  /* Non-comparison condition: float needs -0.0/NaN-safe zero test. */
  if (cond != NULL && peek_type(cond) == TYPE_FLOAT) {
    gen_expression(cond);
    fprintf(out, "  movq xmm0, rax\n");
    fprintf(out, "  xorpd xmm1, xmm1\n");
    fprintf(out, "  ucomisd xmm0, xmm1\n");
    int true_label = label_id++;
    /* NaN (unordered, PF=1) is truthy: skip the je. */
    fprintf(out, "  jp label%d\n", true_label);
    fprintf(out, "  je label%d\n", false_label);
    fprintf(out, "label%d:\n", true_label);
    return;
  }
  gen_expression(cond);
  fprintf(out, "  cmp rax, 0\n");
  fprintf(out, "  je label%d\n", false_label);
}

/**
 * @brief Generates code for an interpolated print string
 * @param strnode String literal node at fault on bad placeholders
 * @param value Raw string value (may contain {name} placeholders)
 */
static void gen_print_string(Node *strnode, const char *value) {
  size_t start = 0;
  size_t i = 0;
  while (value[i] != '\0') {
    if (value[i] == '{') {
      size_t j = i + 1;
      if (is_name_start(value[j])) {
        while (is_name_char(value[j])) {
          j++;
        }
        if (value[j] == '.' && is_name_start(value[j + 1])) {
          j++;
          while (is_name_char(value[j])) {
            j++;
          }
        }
        if (value[j] == '}') {
          if (i > start) {
            size_t len = i - start;
            char *chunk = malloc(len + 1);
            memcpy(chunk, value + start, len);
            chunk[len] = '\0';
            int idx = add_string(chunk);
            free(chunk);
            fprintf(out, "  lea rcx, [rel fmt_str]\n");
            fprintf(out, "  lea rdx, [rel str%d]\n", idx);
            fprintf(out, "  sub rsp, 32\n");
            fprintf(out, "  call printf\n");
            fprintf(out, "  add rsp, 32\n");
          }
          size_t name_len = j - (i + 1);
          char *name = malloc(name_len + 1);
          memcpy(name, value + i + 1, name_len);
          name[name_len] = '\0';
          char *dot = strchr(name, '.');
          int slot;
          if (dot == NULL) {
            slot = require_var(strnode, name);
          } else {
            *dot = '\0';
            if (strcmp(name, "self") != 0) {
              codegen_error(strnode, "Only self.member access is supported");
            }
            slot = require_param(strnode, dot + 1);
          }
          free(name);
          if (var_types[slot] == TYPE_STRING) {
            fprintf(out, "  lea rcx, [rel fmt_str]\n");
            fprintf(out, "  mov rdx, [rbp - %d]\n", (slot + 1) * 8);
            fprintf(out, "  sub rsp, 32\n");
            fprintf(out, "  call printf\n");
            fprintf(out, "  add rsp, 32\n");
          } else if (var_types[slot] == TYPE_FLOAT) {
            fprintf(out, "  movsd xmm0, [rbp - %d]\n", (slot + 1) * 8);
            fprintf(out, "  movq rdx, xmm0\n");
            fprintf(out, "  lea rcx, [rel fmt_float_raw]\n");
            fprintf(out, "  movapd xmm1, xmm0\n");
            fprintf(out, "  sub rsp, 32\n");
            fprintf(out, "  call printf\n");
            fprintf(out, "  add rsp, 32\n");
          } else {
            fprintf(out, "  lea rcx, [rel fmt_int_raw]\n");
            fprintf(out, "  mov rdx, [rbp - %d]\n", (slot + 1) * 8);
            fprintf(out, "  sub rsp, 32\n");
            fprintf(out, "  call printf\n");
            fprintf(out, "  add rsp, 32\n");
          }
          i = j + 1;
          start = i;
          continue;
        }
      }
    }
    i++;
  }
  if (value[i] == '\0' && i > start) {
    size_t len = i - start;
    char *chunk = malloc(len + 1);
    memcpy(chunk, value + start, len);
    chunk[len] = '\0';
    int idx = add_string(chunk);
    free(chunk);
    fprintf(out, "  lea rcx, [rel fmt_str]\n");
    fprintf(out, "  lea rdx, [rel str%d]\n", idx);
    fprintf(out, "  sub rsp, 32\n");
    fprintf(out, "  call printf\n");
    fprintf(out, "  add rsp, 32\n");
  }
}

/**
 * @brief Generates code for a block of statements
 * @param list First statement in the block (linked via right)
 */
static void gen_block(Node *list) {
  int saved = var_count;
  int saved_arrays = array_count;
  for (Node *s = list; s != NULL; s = s->right) {
    gen_statement(s);
  }
  check_unused_vars(saved);
  var_count = saved;
  array_count = saved_arrays;
}

/**
 * @brief Generates code for a single statement
 * @param node Statement node to generate
 */
static void gen_statement(Node *node) {
  switch (node->type) {
  case NODE_VAR_DECL: {
    int existing = find_var(node->var_decl.name);
    if (existing >= 0 && !var_is_param[existing]) {
      codegen_error(node, "Variable '%s' already declared",
                    node->var_decl.name);
    }
    if (find_array(node->var_decl.name) >= 0) {
      codegen_error(node, "Variable '%s' already declared",
                    node->var_decl.name);
    }
    if (existing >= 0) {
      codegen_warning(node, "Shadows parameter '%s'", node->var_decl.name);
    }
    ValueType declared = type_keyword(node->var_decl.var_type);
    ValueType given = TYPE_INT;
    int has_value = (node->var_decl.value != NULL);
    if (has_value) {
      given = expr_type(node->var_decl.value);
      if (!types_compatible(declared, given)) {
        codegen_error(node->var_decl.value, "Cannot assign %s to %s '%s'",
                      type_name(given), type_name(declared),
                      node->var_decl.name);
      }
    }
    int slot = add_var(node, node->var_decl.name, declared, 0);
    /* num holding a float becomes a float slot (monotonic promotion). */
    if (has_value && declared == TYPE_INT && given == TYPE_FLOAT) {
      var_types[slot] = TYPE_FLOAT;
    }
    if (has_value) {
      if (var_types[slot] == TYPE_FLOAT && given == TYPE_INT) {
        gen_expression(node->var_decl.value);
        fprintf(out, "  cvtsi2sd xmm0, rax\n");
        fprintf(out, "  movq rax, xmm0\n");
        fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
      } else if (var_types[slot] == TYPE_BOOL && given == TYPE_FLOAT) {
        gen_expression(node->var_decl.value);
        fprintf(out, "  movq xmm0, rax\n");
        fprintf(out, "  cvttsd2si rax, xmm0\n");
        fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
      } else {
        gen_expression(node->var_decl.value);
        fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
      }
    } else {
      fprintf(out, "  mov QWORD [rbp - %d], 0\n", (slot + 1) * 8);
    }
    break;
  }
  case NODE_ASSIGNMENT: {
    int slot = require_var(node, node->assignment.name);
    ValueType given = expr_type(node->assignment.value);
    if (!types_compatible(var_types[slot], given)) {
      codegen_error(node->assignment.value, "Cannot assign %s to %s '%s'",
                    type_name(given), type_name(var_types[slot]),
                    node->assignment.name);
    }
    /* num holding a float becomes a float slot. */
    if (var_types[slot] == TYPE_INT && given == TYPE_FLOAT) {
      var_types[slot] = TYPE_FLOAT;
    }
    if (var_types[slot] == TYPE_FLOAT && given == TYPE_INT) {
      gen_expression(node->assignment.value);
      fprintf(out, "  cvtsi2sd xmm0, rax\n");
      fprintf(out, "  movq rax, xmm0\n");
      fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
    } else if (var_types[slot] == TYPE_FLOAT && given == TYPE_BOOL) {
      gen_expression(node->assignment.value);
      fprintf(out, "  cvtsi2sd xmm0, rax\n");
      fprintf(out, "  movq rax, xmm0\n");
      fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
    } else if (var_types[slot] == TYPE_BOOL && given == TYPE_FLOAT) {
      gen_expression(node->assignment.value);
      fprintf(out, "  movq xmm0, rax\n");
      fprintf(out, "  cvttsd2si rax, xmm0\n");
      fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
    } else {
      gen_expression(node->assignment.value);
      fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
    }
    break;
  }
  case NODE_ADD_ASSIGN: {
    int slot = require_var(node, node->add_assign.name);
    if (!is_numeric(var_types[slot])) {
      codegen_error(node, "Cannot use += on non-numeric type '%s'",
                    type_name(var_types[slot]));
    }
    ValueType given = peek_type(node->add_assign.value);
    /* Validate (also catches undeclared vars on error path). */
    {
      ValueType checked = expr_type(node->add_assign.value);
      (void)checked;
    }
    int var_is_float = (var_types[slot] == TYPE_FLOAT);
    int given_is_float = (given == TYPE_FLOAT);
    if (var_types[slot] == TYPE_BOOL && given_is_float) {
      /* bool stays int: truncate float operand. */
      gen_expression(node->add_assign.value);
      fprintf(out, "  movq xmm0, rax\n");
      fprintf(out, "  cvttsd2si rax, xmm0\n");
      fprintf(out, "  add [rbp - %d], rax\n", (slot + 1) * 8);
      fprintf(out, "  jo overflow_trap\n");
      break;
    }
    if (!var_is_float && !given_is_float) {
      gen_expression(node->add_assign.value);
      fprintf(out, "  add [rbp - %d], rax\n", (slot + 1) * 8);
      fprintf(out, "  jo overflow_trap\n");
      break;
    }
    /* Float path. */
    int var_before_float = var_is_float;
    if (var_types[slot] == TYPE_INT) {
      var_types[slot] = TYPE_FLOAT;
    }
    gen_expression(node->add_assign.value);
    if (given_is_float) {
      fprintf(out, "  movq xmm1, rax\n");
    } else {
      fprintf(out, "  cvtsi2sd xmm1, rax\n");
    }
    if (var_before_float) {
      fprintf(out, "  movq xmm0, [rbp - %d]\n", (slot + 1) * 8);
    } else {
      fprintf(out, "  mov rax, [rbp - %d]\n", (slot + 1) * 8);
      fprintf(out, "  cvtsi2sd xmm0, rax\n");
    }
    fprintf(out, "  addsd xmm0, xmm1\n");
    fprintf(out, "  movq rax, xmm0\n");
    fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
    break;
  }
  case NODE_SUB_ASSIGN: {
    int slot = require_var(node, node->sub_assign.name);
    if (!is_numeric(var_types[slot])) {
      codegen_error(node, "Cannot use -= on non-numeric type '%s'",
                    type_name(var_types[slot]));
    }
    ValueType given = peek_type(node->sub_assign.value);
    {
      ValueType checked = expr_type(node->sub_assign.value);
      (void)checked;
    }
    int var_is_float = (var_types[slot] == TYPE_FLOAT);
    int given_is_float = (given == TYPE_FLOAT);
    if (var_types[slot] == TYPE_BOOL && given_is_float) {
      gen_expression(node->sub_assign.value);
      fprintf(out, "  movq xmm0, rax\n");
      fprintf(out, "  cvttsd2si rbx, xmm0\n");
      fprintf(out, "  mov rax, [rbp - %d]\n", (slot + 1) * 8);
      fprintf(out, "  sub rax, rbx\n");
      fprintf(out, "  jo overflow_trap\n");
      fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
      break;
    }
    if (!var_is_float && !given_is_float) {
      gen_expression(node->sub_assign.value);
      fprintf(out, "  mov rbx, rax\n");
      fprintf(out, "  mov rax, [rbp - %d]\n", (slot + 1) * 8);
      fprintf(out, "  sub rax, rbx\n");
      fprintf(out, "  jo overflow_trap\n");
      fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
      break;
    }
    int var_before_float = var_is_float;
    if (var_types[slot] == TYPE_INT) {
      var_types[slot] = TYPE_FLOAT;
    }
    gen_expression(node->sub_assign.value);
    if (given_is_float) {
      fprintf(out, "  movq xmm1, rax\n");
    } else {
      fprintf(out, "  cvtsi2sd xmm1, rax\n");
    }
    if (var_before_float) {
      fprintf(out, "  movq xmm0, [rbp - %d]\n", (slot + 1) * 8);
    } else {
      fprintf(out, "  mov rax, [rbp - %d]\n", (slot + 1) * 8);
      fprintf(out, "  cvtsi2sd xmm0, rax\n");
    }
    fprintf(out, "  subsd xmm0, xmm1\n");
    fprintf(out, "  movq rax, xmm0\n");
    fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
    break;
  }
  case NODE_PRINT: {
    Node *value = node->print_stmt.value;
    if (value != NULL && value->type == NODE_STRING_LITERAL) {
      gen_print_string(value, value->string_literal.value);
    } else if (value != NULL && value->type == NODE_IDENTIFIER) {
      int slot = require_var(value, value->identifier.name);
      if (var_types[slot] == TYPE_STRING) {
        fprintf(out, "  lea rcx, [rel fmt_str]\n");
        fprintf(out, "  mov rdx, [rbp - %d]\n", (slot + 1) * 8);
        fprintf(out, "  sub rsp, 32\n");
        fprintf(out, "  call printf\n");
        fprintf(out, "  add rsp, 32\n");
      } else if (var_types[slot] == TYPE_FLOAT) {
        fprintf(out, "  movsd xmm0, [rbp - %d]\n", (slot + 1) * 8);
        fprintf(out, "  movq rdx, xmm0\n");
        fprintf(out, "  lea rcx, [rel fmt_float]\n");
        fprintf(out, "  movapd xmm1, xmm0\n");
        fprintf(out, "  sub rsp, 32\n");
        fprintf(out, "  call printf\n");
        fprintf(out, "  add rsp, 32\n");
      } else {
        fprintf(out, "  lea rcx, [rel fmt_int]\n");
        fprintf(out, "  mov rdx, [rbp - %d]\n", (slot + 1) * 8);
        fprintf(out, "  sub rsp, 32\n");
        fprintf(out, "  call printf\n");
        fprintf(out, "  add rsp, 32\n");
      }
    } else {
      ValueType etype = peek_type(value);
      if (etype == TYPE_STRING) {
        gen_expression(value);
        fprintf(out, "  lea rcx, [rel fmt_str]\n");
        fprintf(out, "  mov rdx, rax\n");
        fprintf(out, "  sub rsp, 32\n");
        fprintf(out, "  call printf\n");
        fprintf(out, "  add rsp, 32\n");
      } else if (etype == TYPE_FLOAT) {
        gen_expression(value);
        fprintf(out, "  movq xmm0, rax\n");
        fprintf(out, "  movq rdx, xmm0\n");
        fprintf(out, "  lea rcx, [rel fmt_float]\n");
        fprintf(out, "  movapd xmm1, xmm0\n");
        fprintf(out, "  sub rsp, 32\n");
        fprintf(out, "  call printf\n");
        fprintf(out, "  add rsp, 32\n");
      } else {
        gen_expression(value);
        fprintf(out, "  lea rcx, [rel fmt_int]\n");
        fprintf(out, "  mov rdx, rax\n");
        fprintf(out, "  sub rsp, 32\n");
        fprintf(out, "  call printf\n");
        fprintf(out, "  add rsp, 32\n");
      }
    }
    break;
  }
  case NODE_RETURN: {
    gen_expression(node->return_stmt.value);
    if (in_function) {
      fprintf(out, "  mov rsp, rbp\n");
      fprintf(out, "  pop rbp\n");
      fprintf(out, "  ret\n");
    } else {
      fprintf(out, "  mov rcx, rax\n");
      fprintf(out, "  sub rsp, 32\n");
      fprintf(out, "  call exit\n");
    }
    break;
  }
  case NODE_IF: {
    int else_label = label_id++;
    int end_label = label_id++;
    gen_condition_jump(node->if_stmt.condition, else_label);
    gen_block(node->if_stmt.body);
    fprintf(out, "  jmp label%d\n", end_label);
    fprintf(out, "label%d:\n", else_label);
    if (node->if_stmt.else_body != NULL) {
      if (node->if_stmt.else_body->type == NODE_IF) {
        gen_statement(node->if_stmt.else_body);
      } else {
        gen_block(node->if_stmt.else_body);
      }
    }
    fprintf(out, "label%d:\n", end_label);
    break;
  }
  case NODE_WHILE: {
    int loop_label = label_id++;
    int end_label = label_id++;
    fprintf(out, "loop%d:\n", loop_label);
    gen_condition_jump(node->while_stmt.condition, end_label);
    gen_block(node->while_stmt.body);
    fprintf(out, "  jmp loop%d\n", loop_label);
    fprintf(out, "label%d:\n", end_label);
    break;
  }
  case NODE_FUNC_CALL:
    gen_call(node);
    break;
  case NODE_FUNCTION:
    codegen_error(node, "Nested functions not supported in codegen");
    break;
  case NODE_ARRAY_DECL: {
    int existing = find_var(node->array_decl.name);
    if (existing >= 0 && !var_is_param[existing]) {
      codegen_error(node, "Variable '%s' already declared",
                    node->array_decl.name);
    }
    if (find_array(node->array_decl.name) >= 0) {
      codegen_error(node, "Variable '%s' already declared",
                    node->array_decl.name);
    }
    if (existing >= 0) {
      codegen_warning(node, "Shadows parameter '%s'", node->array_decl.name);
    }
    int base = var_count;
    int count = 0;
    for (Node *e = node->array_decl.elements->array_literal.elements; e != NULL;
         e = e->right) {
      count++;
    }
    if (var_count + count > 256) {
      codegen_error(node, "Too many variables in function");
    }
    int arr_is_float = 0;
    for (Node *e = node->array_decl.elements->array_literal.elements; e != NULL;
         e = e->right) {
      if (peek_type(e) == TYPE_FLOAT) {
        arr_is_float = 1;
        break;
      }
    }
    for (Node *e = node->array_decl.elements->array_literal.elements; e != NULL;
         e = e->right) {
      ValueType given = expr_type(e);
      if (!is_numeric(given)) {
        codegen_error(e, "Array element must be a number, got %s",
                      type_name(given));
      }
      ValueType ptype = peek_type(e);
      gen_expression(e);
      if (arr_is_float && ptype != TYPE_FLOAT) {
        fprintf(out, "  cvtsi2sd xmm0, rax\n");
        fprintf(out, "  movq rax, xmm0\n");
      } else if (!arr_is_float && ptype == TYPE_FLOAT) {
        /* Unreachable (arr_is_float would be true), kept for safety. */
        fprintf(out, "  movq xmm0, rax\n");
        fprintf(out, "  cvttsd2si rax, xmm0\n");
      }
      fprintf(out, "  push rax\n");
    }
    for (int i = 0; i < count; i++) {
      fprintf(out, "  pop rax\n");
      fprintf(out, "  mov [rbp - %d], rax\n", (base + i + 1) * 8);
    }
    var_count += count;
    if (array_count >= 64) {
      codegen_error(node, "Too many arrays in function");
    }
    size_t len = strlen(node->array_decl.name);
    array_names[array_count] = malloc(len + 1);
    memcpy(array_names[array_count], node->array_decl.name, len);
    array_names[array_count][len] = '\0';
    array_base[array_count] = base;
    array_len[array_count] = count;
    array_is_float[array_count] = arr_is_float;
    array_count++;
    break;
  }
  case NODE_FOR: {
    int arr = find_array(node->for_stmt.array_name);
    if (arr < 0) {
      codegen_error(node, "Array '%s' is not declared",
                    node->for_stmt.array_name);
    }
    int saved = var_count;
    int saved_arrays = array_count;
    int existing = find_var(node->for_stmt.var_name);
    if (existing >= 0 && !var_is_param[existing]) {
      codegen_error(node, "Variable '%s' already declared",
                    node->for_stmt.var_name);
    }
    if (find_array(node->for_stmt.var_name) >= 0) {
      codegen_error(node, "Variable '%s' already declared",
                    node->for_stmt.var_name);
    }
    if (existing >= 0) {
      codegen_warning(node, "Shadows parameter '%s'", node->for_stmt.var_name);
    }
    int var_slot = add_var(node, node->for_stmt.var_name,
                           array_is_float[arr] ? TYPE_FLOAT : TYPE_INT, 0);
    int base = array_base[arr];
    int count = array_len[arr];
    int loop_label = label_id++;
    int end_label = label_id++;
    fprintf(out, "  push r12\n");
    fprintf(out, "  xor r12d, r12d\n");
    fprintf(out, "loop%d:\n", loop_label);
    fprintf(out, "  cmp r12, %d\n", count);
    fprintf(out, "  jge label%d\n", end_label);
    fprintf(out, "  mov rax, [rbp - %d + r12*8]\n", (base + count) * 8);
    fprintf(out, "  mov [rbp - %d], rax\n", (var_slot + 1) * 8);
    gen_block(node->for_stmt.body);
    fprintf(out, "  inc r12\n");
    fprintf(out, "  jmp loop%d\n", loop_label);
    fprintf(out, "label%d:\n", end_label);
    fprintf(out, "  pop r12\n");
    var_count = saved;
    array_count = saved_arrays;
    break;
  }
  default:
    codegen_error(node, "Unexpected statement in codegen");
    break;
  }
}

/**
 * @brief Generates code for a function body
 * @param node Function definition node
 */
static void gen_function(Node *node) {
  const char *name = node->function.name;
  int saved_count = var_count;
  int saved_arrays = array_count;
  int saved_in_function = in_function;
  var_count = 0;
  array_count = 0;
  in_function = 1;

  fprintf(out, "%s:\n", func_label(name));
  gen_prologue();

  int param_index = 0;
  const char *param_regs[4] = {"rcx", "rdx", "r8", "r9"};
  int sig_idx = -1;
  for (int s = 0; s < sig_count; s++) {
    if (strcmp(sig_names[s], name) == 0) {
      sig_idx = s;
      break;
    }
  }
  for (Node *p = node->function.params; p != NULL; p = p->right) {
    const char *param_name = NULL;
    ValueType param_type = TYPE_INT;
    if (p->type == NODE_VAR_DECL) {
      param_name = p->var_decl.name;
      param_type = type_keyword(p->var_decl.var_type);
      if (param_type == TYPE_INT && sig_idx >= 0 && param_index < 32 &&
          sig_param_float[sig_idx][param_index]) {
        param_type = TYPE_FLOAT;
      }
    } else if (p->type == NODE_IDENTIFIER) {
      param_name = p->identifier.name;
      if (sig_idx >= 0 && param_index < 32 &&
          sig_param_float[sig_idx][param_index]) {
        param_type = TYPE_FLOAT;
      }
    } else {
      codegen_error(p, "Unexpected parameter in codegen");
    }
    int slot = add_var(p, param_name, param_type, 1);
    if (param_index < 4) {
      fprintf(out, "  mov [rbp - %d], %s\n", (slot + 1) * 8,
              param_regs[param_index]);
    } else {
      fprintf(out, "  mov rax, [rbp + %d]\n", 48 + 8 * (param_index - 4));
      fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
    }
    param_index++;
  }

  gen_block(node->function.body);

  fprintf(out, "  mov rsp, rbp\n");
  fprintf(out, "  pop rbp\n");
  fprintf(out, "  ret\n");

  var_count = saved_count;
  array_count = saved_arrays;
  in_function = saved_in_function;
}

/**
 * @brief Generates NASM x86-64 assembly for a Jot program
 * @param root Root of the AST (linked list of top level statements)
 * @param filename Output assembly file path
 * @details Entry point main runs the top level statements only.
 * Functions (including fn main, emitted as jot_main) run when called.
 */
void GenerateAssembly(Node *root, const char *source, const char *output) {
  codegen_source = source;
  label_id = 0;
  string_count = 0;
  var_count = 0;
  frame_size = 2048;
  in_function = 0;
  has_user_main = 0;

  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_FUNCTION && strcmp(s->function.name, "main") == 0) {
      has_user_main = 1;
      break;
    }
  }

  sig_count = 0;
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_FUNCTION && sig_count < 256) {
      size_t len = strlen(s->function.name);
      sig_names[sig_count] = malloc(len + 1);
      memcpy(sig_names[sig_count], s->function.name, len);
      sig_names[sig_count][len] = '\0';
      sig_params[sig_count] = s->function.params;
      sig_count++;
    }
  }
  for (int i = 0; i < sig_count; i++) {
    for (int j = 0; j < 32; j++) {
      sig_param_float[i][j] = 0;
    }
    sig_is_bool_only[i] = 0;
  }
  /* Bool-only functions return comparisons, never floats. */
  for (int i = 0; i < sig_count; i++) {
    for (Node *s = root; s != NULL; s = s->right) {
      if (s->type == NODE_FUNCTION &&
          strcmp(s->function.name, sig_names[i]) == 0) {
        int found = 0;
        int all_bool = 1;
        scan_returns_bool_only(s->function.body, &found, &all_bool);
        if (found && all_bool) {
          sig_is_bool_only[i] = 1;
        }
        break;
      }
    }
  }
  scan_float_calls(root);

  collect_strings(root);

  out = fopen(output, "w");
  if (!out) {
    codegen_error(NULL, "Could not open output file '%s'", output);
  }

  fprintf(out, "global main\n");
  fprintf(out, "extern printf\n");
  fprintf(out, "extern scanf\n");
  fprintf(out, "extern exit\n");
  gen_data_section();
  fprintf(out, "section .text\n");

  fprintf(out, "main:\n");
  fprintf(out, "  mov rax, rsp\n");
  fprintf(out, "  sub rax, 262144\n");
  fprintf(out, "  mov [rel stack_floor], rax\n");
  gen_prologue();
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type != NODE_FUNCTION) {
      gen_statement(s);
    }
  }
  check_unused_vars(0);
  fprintf(out, "  xor ecx, ecx\n");
  fprintf(out, "  sub rsp, 32\n");
  fprintf(out, "  call exit\n");

  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_FUNCTION) {
      gen_function(s);
    }
  }

  gen_trap("overflow_trap", "fmt_overflow");
  gen_trap("divzero_trap", "fmt_divzero");
  gen_trap("stack_overflow_trap", "fmt_stack");
  gen_trap("input_error_trap", "fmt_invalid");

  fclose(out);
}
