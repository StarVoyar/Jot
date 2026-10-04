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

/** Number of variables in the current frame */
static int var_count;

/** Reserved stack bytes for locals */
static int frame_size;

/** Non-zero while generating fn main (returns exit the process) */
static int in_main;

/** Forward declaration for recursive generation */
static void gen_expression(Node *node);

/** Forward declaration for statement generation */
static void gen_statement(Node *node);

/** Forward declaration for block generation */
static void gen_block(Node *list);

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
 * @param name Variable name to store
 * @param is_str Non-zero if the variable holds a string pointer
 * @return Slot index of the new variable
 */
static int add_var(const char *name, int is_str) {
  if (var_count >= 32) {
    printf("Error: Too many variables in function\n");
    exit(1);
  }
  size_t len = strlen(name);
  var_names[var_count] = malloc(len + 1);
  memcpy(var_names[var_count], name, len);
  var_names[var_count][len] = '\0';
  var_is_string[var_count] = is_str;
  var_count++;
  return var_count - 1;
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
    printf("Error: Too many string literals\n");
    exit(1);
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
 * @brief Generates code for a function call, result left in rax
 * @param node Call node to generate
 */
static void gen_call(Node *node) {
  const char *regs[4] = {"rcx", "rdx", "r8", "r9"};
  int arg_count = 0;
  for (Node *a = node->func_call.args; a != NULL; a = a->right) {
    arg_count++;
  }
  if (arg_count > 4) {
    printf("Error: Only up to 4 call arguments supported in codegen\n");
    exit(1);
  }
  int i = 0;
  for (Node *a = node->func_call.args; a != NULL; a = a->right) {
    gen_expression(a);
    fprintf(out, "  mov %s, rax\n", regs[i]);
    i++;
  }
  fprintf(out, "  sub rsp, 32\n");
  fprintf(out, "  call %s\n", node->func_call.name);
  fprintf(out, "  add rsp, 32\n");
}

/**
 * @brief Generates code for an expression, result left in rax
 * @param node Expression node to generate
 */
static void gen_expression(Node *node) {
  if (node == NULL) {
    printf("Error: NULL expression in codegen\n");
    exit(1);
  }

  switch (node->type) {
  case NODE_INT_LITERAL:
    fprintf(out, "  mov rax, %d\n", node->int_literal.value);
    break;
  case NODE_IDENTIFIER: {
    int slot = find_var(node->identifier.name);
    if (slot < 0) {
      printf("Error: Variable '%s' not declared\n", node->identifier.name);
      exit(1);
    }
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
    printf("Error: Array literal not supported in codegen expression\n");
    exit(1);
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
      printf("Error: Unsupported operator '%s' in codegen\n", op);
      exit(1);
    }
    break;
  }
  default:
    printf("Error: Unexpected expression in codegen\n");
    exit(1);
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
 * @param value Raw string value (may contain {name} placeholders)
 */
static void gen_print_string(const char *value) {
  size_t start = 0;
  size_t i = 0;
  while (value[i] != '\0') {
    if (value[i] == '{') {
      size_t j = i + 1;
      if (is_name_start(value[j])) {
        while (is_name_char(value[j])) {
          j++;
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
          int slot = find_var(name);
          if (slot < 0) {
            printf("Error: Variable '%s' not declared\n", name);
            exit(1);
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
      printf("Error: Variable '%s' already declared\n", node->var_decl.name);
      exit(1);
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
    int slot = add_var(node->var_decl.name, is_str);
    if (node->var_decl.value != NULL) {
      gen_expression(node->var_decl.value);
      fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
    } else {
      fprintf(out, "  mov QWORD [rbp - %d], 0\n", (slot + 1) * 8);
    }
    break;
  }
  case NODE_ASSIGNMENT: {
    int slot = find_var(node->assignment.name);
    if (slot < 0) {
      printf("Error: Variable '%s' not declared\n", node->assignment.name);
      exit(1);
    }
    gen_expression(node->assignment.value);
    fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
    break;
  }
  case NODE_PRINT: {
    Node *value = node->print_stmt.value;
    if (value != NULL && value->type == NODE_STRING_LITERAL) {
      gen_print_string(value->string_literal.value);
    } else if (value != NULL && value->type == NODE_IDENTIFIER) {
      int slot = find_var(value->identifier.name);
      if (slot < 0) {
        printf("Error: Variable '%s' not declared\n", value->identifier.name);
        exit(1);
      }
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
    if (in_main) {
      fprintf(out, "  mov rcx, rax\n");
      fprintf(out, "  sub rsp, 32\n");
      fprintf(out, "  call exit\n");
    } else {
      fprintf(out, "  mov rsp, rbp\n");
      fprintf(out, "  pop rbp\n");
      fprintf(out, "  ret\n");
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
    printf("Error: Nested functions not supported in codegen\n");
    exit(1);
    break;
  case NODE_ARRAY_DECL:
    printf("Error: Arrays not supported in codegen yet\n");
    exit(1);
    break;
  case NODE_FOR:
    printf("Error: For loops not supported in codegen yet\n");
    exit(1);
    break;
  default:
    printf("Error: Unexpected statement in codegen\n");
    exit(1);
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
  int saved_main = in_main;
  var_count = 0;

  if (strcmp(name, "main") == 0) {
    in_main = 1;
  } else {
    in_main = 0;
  }

  fprintf(out, "%s:\n", name);
  gen_prologue();

  int param_index = 0;
  const char *param_regs[4] = {"rcx", "rdx", "r8", "r9"};
  for (Node *p = node->function.params; p != NULL; p = p->right) {
    if (param_index >= 4) {
      printf("Error: Only up to 4 function parameters supported\n");
      exit(1);
    }
    if (p->type == NODE_VAR_DECL) {
      int slot = add_var(p->var_decl.name, 0);
      fprintf(out, "  mov [rbp - %d], %s\n", (slot + 1) * 8,
              param_regs[param_index]);
    } else if (p->type == NODE_IDENTIFIER) {
      int slot = add_var(p->identifier.name, 0);
      fprintf(out, "  mov [rbp - %d], %s\n", (slot + 1) * 8,
              param_regs[param_index]);
    } else {
      printf("Error: Unexpected parameter in codegen\n");
      exit(1);
    }
    param_index++;
  }

  gen_block(node->function.body);

  if (strcmp(name, "main") == 0) {
    fprintf(out, "  xor ecx, ecx\n");
    fprintf(out, "  sub rsp, 32\n");
    fprintf(out, "  call exit\n");
  } else {
    fprintf(out, "  mov rsp, rbp\n");
    fprintf(out, "  pop rbp\n");
    fprintf(out, "  ret\n");
  }

  var_count = saved_count;
  in_main = saved_main;
}

/**
 * @brief Generates NASM x86-64 assembly for a Jot program
 * @param root Root of the AST (linked list of top level statements)
 * @param filename Output assembly file path
 */
void GenerateAssembly(Node *root, const char *filename) {
  label_id = 0;
  string_count = 0;
  var_count = 0;
  frame_size = 256;
  in_main = 1;

  collect_strings(root);

  out = fopen(filename, "w");
  if (!out) {
    printf("Error: Could not open output file '%s'\n", filename);
    exit(1);
  }

  fprintf(out, "global main\n");
  fprintf(out, "extern printf\n");
  fprintf(out, "extern exit\n");
  gen_data_section();
  fprintf(out, "section .text\n");

  Node *main_fn = NULL;
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_FUNCTION && strcmp(s->function.name, "main") == 0) {
      main_fn = s;
      break;
    }
  }

  if (main_fn != NULL) {
    gen_function(main_fn);
    for (Node *s = root; s != NULL; s = s->right) {
      if (s->type == NODE_FUNCTION) {
        if (s != main_fn) {
          gen_function(s);
        }
      } else if (s->type == NODE_FUNC_CALL) {
        if (strcmp(s->func_call.name, "main") != 0) {
          fprintf(out, "global _top_call%d\n", label_id);
        }
      } else {
        printf("Error: Top level statements outside fn main not supported\n");
        exit(1);
      }
    }
  } else {
    fprintf(out, "main:\n");
    gen_prologue();
    gen_block(root);
    fprintf(out, "  xor ecx, ecx\n");
    fprintf(out, "  sub rsp, 32\n");
    fprintf(out, "  call exit\n");
  }

  fclose(out);
}
