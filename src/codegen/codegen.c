#define _CRT_SECURE_NO_WARNINGS

#include "codegen.h"

/** Output assembly file */
static FILE *out;

/** Counter for jump labels (like curly_count in the video) */
static int label_id;

/** Counter for string literals */
static int string_count;

/** Collected string literal values */
static char *string_table[256];

/** Variable names in the current function frame */
static char *var_names[64];

/** Whether a variable holds a string pointer (1) or an integer (0) */
static int var_is_string[64];

/** Whether a variable is a function parameter (1) or a local (0) */
static int var_is_param[64];

/** Number of variables in the current frame */
static int var_count;

/** Reserved stack bytes for locals */
static int frame_size;

/** Non-zero while generating a function body (returns use ret) */
static int in_function;

/** Non-zero if the program defines fn main (it lives at jot_main) */
static int has_user_main;

/** Forward declaration for recursive generation */
static void gen_expression(Node *node);

/** Forward declaration for statement generation */
static void gen_statement(Node *node);

/** Forward declaration for block generation */
static void gen_block(Node *list);

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
 * @brief Finds a variable in the current frame
 * @param name Variable name to look up
 * @return Slot index, or -1 if not found
 */
static int find_var(const char *name) {
  for (int i = 0; i < var_count; i++) {
    if (strcmp(var_names[i], name) == 0) {
      return i;
    }
  }
  return -1;
}

/**
 * @brief Adds a variable to the current frame
 * @param node Declaration node at fault if the frame is full
 * @param name Variable name to store
 * @param is_str Non-zero if the variable holds a string pointer
 * @param is_param Non-zero if the variable is a function parameter
 * @return Slot index of the new variable
 */
static int add_var(Node *node, const char *name, int is_str, int is_param) {
  if (var_count >= 32) {
    codegen_error(node, "Too many variables in function");
  }
  size_t len = strlen(name);
  var_names[var_count] = malloc(len + 1);
  memcpy(var_names[var_count], name, len);
  var_names[var_count][len] = '\0';
  var_is_string[var_count] = is_str;
  var_is_param[var_count] = is_param;
  var_count++;
  return var_count - 1;
}

/**
 * @brief Loads a variable slot, rejecting bare parameter access
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
    codegen_error(node, "Parameter '%s' must be accessed as self.%s", name,
                  name);
  }
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
  return slot;
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
  if (string_count >= 256) {
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
  fprintf(out, "  fmt_int db \"%%d\", 10, 0\n");
  fprintf(out, "  fmt_int_raw db \"%%d\", 0\n");
  fprintf(out, "  fmt_str db \"%%s\", 0\n");
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
  if (arg_count <= 4) {
    int i = 0;
    for (Node *a = node->func_call.args; a != NULL; a = a->right) {
      gen_expression(a);
      fprintf(out, "  mov %s, rax\n", regs[i]);
      i++;
    }
    fprintf(out, "  sub rsp, 32\n");
    fprintf(out, "  call %s\n", func_label(node->func_call.name));
    fprintf(out, "  add rsp, 32\n");
    return;
  }

  int frame = 64 + 8 * (arg_count - 4);
  if (frame % 16 != 0) {
    frame += 8;
  }
  fprintf(out, "  sub rsp, %d\n", frame);
  int i = 0;
  for (Node *a = node->func_call.args; a != NULL; a = a->right) {
    gen_expression(a);
    if (i < 4) {
      fprintf(out, "  mov [rsp + %d], rax\n", frame - 32 + 8 * i);
    } else {
      fprintf(out, "  mov [rsp + %d], rax\n", 32 + 8 * (i - 4));
    }
    i++;
  }
  for (i = 0; i < 4; i++) {
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
    fprintf(out, "  mov rax, %d\n", node->int_literal.value);
    break;
  case NODE_IDENTIFIER: {
    int slot = require_var(node, node->identifier.name);
    fprintf(out, "  mov rax, [rbp - %d]\n", (slot + 1) * 8);
    break;
  }
  case NODE_MEMBER_ACCESS: {
    if (strcmp(node->member_access.object, "self") != 0) {
      printf("Error: Only self.member access is supported\n");
      exit(1);
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
    gen_expression(node->binary_op.left);
    fprintf(out, "  push rax\n");
    gen_expression(node->binary_op.right);
    fprintf(out, "  mov rbx, rax\n");
    fprintf(out, "  pop rax\n");
    if (strcmp(op, "+") == 0) {
      fprintf(out, "  add rax, rbx\n");
    } else if (strcmp(op, "-") == 0) {
      fprintf(out, "  sub rax, rbx\n");
    } else if (strcmp(op, "*") == 0) {
      fprintf(out, "  imul rax, rbx\n");
    } else if (strcmp(op, "/") == 0) {
      fprintf(out, "  xor rdx, rdx\n");
      fprintf(out, "  div rbx\n");
    } else if (strcmp(op, "%") == 0) {
      fprintf(out, "  xor rdx, rdx\n");
      fprintf(out, "  div rbx\n");
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
  if (cond != NULL && cond->type == NODE_BINARY_OP) {
    const char *op = cond->binary_op.op;
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
        strcmp(op, "<") == 0 || strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 ||
        strcmp(op, ">=") == 0) {
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
          if (var_is_string[slot]) {
            fprintf(out, "  lea rcx, [rel fmt_str]\n");
            fprintf(out, "  mov rdx, [rbp - %d]\n", (slot + 1) * 8);
          } else {
            fprintf(out, "  lea rcx, [rel fmt_int_raw]\n");
            fprintf(out, "  mov rdx, [rbp - %d]\n", (slot + 1) * 8);
          }
          fprintf(out, "  sub rsp, 32\n");
          fprintf(out, "  call printf\n");
          fprintf(out, "  add rsp, 32\n");
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
  for (Node *s = list; s != NULL; s = s->right) {
    gen_statement(s);
  }
  var_count = saved;
}

/**
 * @brief Generates code for a single statement
 * @param node Statement node to generate
 */
static void gen_statement(Node *node) {
  switch (node->type) {
  case NODE_VAR_DECL: {
    if (find_var(node->var_decl.name) >= 0) {
      codegen_error(node, "Variable '%s' already declared",
                    node->var_decl.name);
    }
    int is_str = 0;
    if (node->var_decl.value != NULL &&
        node->var_decl.value->type == NODE_STRING_LITERAL) {
      is_str = 1;
    }
    if (node->var_decl.value != NULL &&
        strcmp(node->var_decl.var_type, "string") == 0) {
      is_str = 1;
    }
    int slot = add_var(node, node->var_decl.name, is_str, 0);
    if (node->var_decl.value != NULL) {
      gen_expression(node->var_decl.value);
      fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
    } else {
      fprintf(out, "  mov QWORD [rbp - %d], 0\n", (slot + 1) * 8);
    }
    break;
  }
  case NODE_ASSIGNMENT: {
    int slot = require_var(node, node->assignment.name);
    gen_expression(node->assignment.value);
    fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
    break;
  }
  case NODE_PRINT: {
    Node *value = node->print_stmt.value;
    if (value != NULL && value->type == NODE_STRING_LITERAL) {
      gen_print_string(value, value->string_literal.value);
    } else if (value != NULL && value->type == NODE_IDENTIFIER) {
      int slot = require_var(value, value->identifier.name);
      if (var_is_string[slot]) {
        fprintf(out, "  lea rcx, [rel fmt_str]\n");
        fprintf(out, "  mov rdx, [rbp - %d]\n", (slot + 1) * 8);
      } else {
        fprintf(out, "  lea rcx, [rel fmt_int]\n");
        fprintf(out, "  mov rdx, [rbp - %d]\n", (slot + 1) * 8);
      }
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
  case NODE_ARRAY_DECL:
    codegen_error(node, "Arrays not supported in codegen yet");
    break;
  case NODE_FOR:
    codegen_error(node, "For loops not supported in codegen yet");
    break;
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
  int saved_in_function = in_function;
  var_count = 0;
  in_function = 1;

  fprintf(out, "%s:\n", func_label(name));
  gen_prologue();

  int param_index = 0;
  const char *param_regs[4] = {"rcx", "rdx", "r8", "r9"};
  for (Node *p = node->function.params; p != NULL; p = p->right) {
    const char *param_name = NULL;
    if (p->type == NODE_VAR_DECL) {
      param_name = p->var_decl.name;
    } else if (p->type == NODE_IDENTIFIER) {
      param_name = p->identifier.name;
    } else {
      codegen_error(p, "Unexpected parameter in codegen");
    }
    int slot = add_var(p, param_name, 0, 1);
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
  frame_size = 256;
  in_function = 0;
  has_user_main = 0;

  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_FUNCTION && strcmp(s->function.name, "main") == 0) {
      has_user_main = 1;
      break;
    }
  }

  collect_strings(root);

  out = fopen(output, "w");
  if (!out) {
    codegen_error(NULL, "Could not open output file '%s'", output);
  }

  fprintf(out, "global main\n");
  fprintf(out, "extern printf\n");
  fprintf(out, "extern exit\n");
  gen_data_section();
  fprintf(out, "section .text\n");

  fprintf(out, "main:\n");
  gen_prologue();
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type != NODE_FUNCTION) {
      gen_statement(s);
    }
  }
  fprintf(out, "  xor ecx, ecx\n");
  fprintf(out, "  sub rsp, 32\n");
  fprintf(out, "  call exit\n");

  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_FUNCTION) {
      gen_function(s);
    }
  }

  fclose(out);
}
