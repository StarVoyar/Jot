#define _CRT_SECURE_NO_WARNINGS
#undef strdup

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
  TYPE_ARRAY,  /**< Arrays (not first-class values) */
  TYPE_STRUCT, /**< Struct instance (heap pointer) */
  TYPE_CLASS,  /**< Class instance (heap pointer) */
  TYPE_NULL    /**< null (no reference) */
} ValueType;

/** Maximum fields per struct/class */
#define MAX_FIELDS 32

/** Maximum methods per class */
#define MAX_METHODS 32

/** Maximum struct definitions per program */
#define MAX_STRUCTS 64

/** Maximum class definitions per program */
#define MAX_CLASSES 64

/** A struct/class field (every field occupies one 8-byte slot) */
typedef struct {
  char *name;      /**< Field name */
  ValueType type;  /**< Slot category (num is stored as a double) */
  char *kind;      /**< Struct/class name for instance fields, else NULL */
  char *decl_type; /**< Raw declared type text, resolved in a later pass */
} FieldDef;

/** A class method (stored AST, emitted with mangled label) */
typedef struct {
  char *name;     /**< Method name */
  char *ret_type; /**< Declared return type keyword */
  Node *params;   /**< Parameter list (linked via right) */
  Node *body;     /**< Method body (linked via right) */
  int line;       /**< Definition line for diagnostics */
  int col;        /**< Definition column for diagnostics */
  Node *decl;     /**< Method definition node (diagnostics) */
} MethodDef;

/** A struct definition */
typedef struct {
  char *name;                  /**< Struct name */
  int is_public;               /**< Non-zero if importable */
  int nfields;                 /**< Field count */
  FieldDef fields[MAX_FIELDS]; /**< Field list */
} StructDef;

/** A class definition */
typedef struct {
  char *name;                     /**< Class name */
  char *base;                     /**< Base class name, or NULL */
  int base_idx;                   /**< Base class index, -1 when none */
  int flattened;                  /**< Non-zero once fields are merged */
  int visiting;                   /**< Inheritance cycle detection flag */
  int class_id;                   /**< Runtime id stored in instance header */
  int is_public;                  /**< Non-zero if importable */
  int nfields;                    /**< Field count (includes base fields) */
  FieldDef fields[MAX_FIELDS];    /**< Field list (base fields first) */
  int nmethods;                   /**< Owned method count */
  MethodDef methods[MAX_METHODS]; /**< Method list (own methods only) */
  struct Node *decl;              /**< Class definition node (diagnostics) */
} ClassDef;

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

/** Struct/class name for TYPE_STRUCT/TYPE_CLASS variables, else NULL */
static char *var_type_name[256];

/** Non-zero suppresses bare-parameter warnings (for self.p writes) */
static int no_param_warn;

/** Array names in the current frame */
static char *array_names[64];

/** Slot of the local holding the array pointer (0-based into frame) */
static int array_slot[64];

/** Element count of each array in the current frame */
static int array_len[64];

/** Element category of each array in the current frame */
static ValueType array_elem[64];

/** For struct/class arrays, the instance type name (else NULL) */
static char *array_kind[64];

/** Number of arrays in the current frame */
static int array_count;

/** Reserved stack bytes for locals */
static int frame_size;

/** Non-zero while generating a function body (returns use ret) */
static int in_function;

/** Declared return type of the enclosing function ("void" enables bare return)
 */
static const char *cur_return_type;

/** Sig index of the enclosing function, -1 outside functions */
static int cur_sig_idx;

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

/** Declared return type of defined functions ("num","str","arr","char" or NULL)
 */
static char *sig_return_type[256];

/** Non-zero if function returns a float (callers must type the call as float)
 */
static int sig_return_float[256];

/** Names of global variables (data-section storage, accessible everywhere) */
static char *global_names[256];

/** Types of global variables, parallel to global_names */
static ValueType global_types[256];

/** Initial value strings of global variables (NULL = zero-init) */
static char *global_init[256];

/** Number of recorded globals */
static int global_count;

/** Struct definitions for this program */
static StructDef struct_defs[MAX_STRUCTS];

/** Number of struct definitions */
static int struct_count;

/** Class definitions for this program */
static ClassDef class_defs[MAX_CLASSES];

/** Number of class definitions */
static int class_count;

/** Non-zero while generating a class method body */
static int in_method;

/** Class index of the method being generated, -1 when not in a method */
static int cur_class;

/** Method index of the method being generated, -1 when not in a method */
static int cur_method;

/** Maximum nesting depth of loops */
#define MAX_LOOP_DEPTH 32

/** Label to jump to for break, innermost loop last */
static int loop_break_label[MAX_LOOP_DEPTH];

/** Label to jump to for continue, innermost loop last */
static int loop_continue_label[MAX_LOOP_DEPTH];

/** Number of loops currently open */
static int loop_depth;

/** Forward declaration for recursive generation */
static void gen_expression(Node *node);

/** Forward declaration for statement generation */
static void gen_statement(Node *node);

/** Forward declaration: side-effect classification used by call/assign opts */
static int expr_is_regsafe(Node *node);

/** Forward declaration for block generation */
static void gen_block(Node *list);

/** Forward declaration for float param scan */
static void scan_float_calls(Node *node);

/** Forward declaration for call argument checking */
static void check_call_arg(Node *arg, ValueType want, const char *want_kind);

/** Forward declaration for method calls (result left in rax) */
static void gen_method_call(Node *node);

/** Forward declaration for the tostr builtin (heap string in rax) */
static void gen_to_str(Node *node);

/** Forward declaration for the tonum builtin (double bits in rax) */
static void gen_to_num(Node *node);

/** Forward declaration for the readFile builtin (heap string in rax) */
static void gen_read_file(Node *node);

/** Forward declaration for the writeFile builtin (byte count in rax) */
static void gen_write_file(Node *node);

/** Jump target for abandoning the current statement after an error */
static jmp_buf gen_jmp;

/** Source file name for error messages */
static const char *codegen_source;

/**
 * @brief Prints a modern error for an AST node and abandons the statement
 * @param node Fault node, may be NULL (uses 1:1 then)
 * @param format printf-style message without the Error: prefix
 * @details Counted via term_report; generation continues with the next
 * statement so all diagnostics print, and the driver refuses to compile
 * when any error was reported
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
  const char *source = codegen_source;
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
    if (node->source != NULL) {
      source = node->source;
    }
  }
  term_report(TERM_ERROR, source, line, col, width, message);
  longjmp(gen_jmp, 1);
}

/**
 * @brief Generates one statement, abandoning it (no code) on error
 * @param node Statement node to generate
 * @details Installs a fresh recovery buffer so codegen_error lands back
 * here; the next statement installs its own, so no stale frame is reused
 */
static void try_gen_statement(Node *node) {
  if (setjmp(gen_jmp) == 0) {
    gen_statement(node);
  }
}

/**
 * @brief Reports a definition-collection error and continues
 * @param node Fault node for location info
 * @param format printf-style message without the Error: prefix
 * @details Used before any statement recovery buffer is active, so it
 * must not longjmp; generation is skipped later when errors exist
 */
static void collect_error(Node *node, const char *format, ...) {
  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  int line = node != NULL && node->line > 0 ? node->line : 1;
  int col = node != NULL && node->col > 0 ? node->col : 1;
  int width = node != NULL && node->width > 0 ? node->width : 1;
  term_report(TERM_ERROR, codegen_source, line, col, width, message);
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
 * @brief Finds a global variable by name
 * @param name Variable name to look up
 * @return Global index, or -1 if not found
 */
static int find_global(const char *name) {
  for (int i = 0; i < global_count; i++) {
    if (strcmp(global_names[i], name) == 0) {
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
  var_type_name[var_count] = NULL;
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
    int g = find_global(name);
    if (g >= 0) {
      return -(g + 2); /* globals encode as -(index+2), -1 = not found */
    }
    codegen_error(node, "Variable '%s' not declared", name);
  }
  if (var_is_param[slot] && !no_param_warn && var_types[slot] != TYPE_ARRAY) {
    codegen_warning(node, "Parameter '%s' should be accessed as self.%s", name,
                    name);
  }
  var_used[slot] = 1;
  return slot;
}

/**
 * @brief Returns non-zero if a slot from require_var is a global reference
 * @param slot Slot value from require_var
 */
static int slot_is_global(int slot) { return slot < -1; }

/**
 * @brief Converts a global-encoded slot to a global index
 * @param slot Slot value from require_var (negative)
 */
static int slot_to_global(int slot) { return (-slot) - 2; }

/**
 * @brief Non-zero if the node is a side-effect-free leaf usable in rbx
 * @details Literals and variable reads never call runtime functions, so they
 *          can be evaluated directly into rbx after the left operand is in
 *          rax, avoiding the push/pop round trip of the generic path.
 * @param node Expression node to classify
 */
static int is_leaf_expr(Node *node) {
  return node != NULL &&
         (node->type == NODE_INT_LITERAL || node->type == NODE_FLOAT_LITERAL ||
          node->type == NODE_IDENTIFIER);
}

/**
 * @brief Emits a leaf expression into rbx without clobbering rax
 * @details Runs the same diagnostics as the generic path (undeclared name,
 *          parameter warnings, unused tracking) via require_var.
 * @param node Literal or identifier node to emit
 */
static void gen_leaf_into_rbx(Node *node) {
  if (node->type == NODE_IDENTIFIER) {
    int slot = require_var(node, node->identifier.name);
    if (slot_is_global(slot)) {
      fprintf(out, "  mov rbx, [rel global_%s]\n", node->identifier.name);
    } else {
      fprintf(out, "  mov rbx, [rbp - %d]\n", (slot + 1) * 8);
    }
  } else if (node->type == NODE_INT_LITERAL) {
    fprintf(out, "  mov rbx, %lld\n", node->int_literal.value);
  } else {
    unsigned long long float_bits;
    memcpy(&float_bits, &node->float_literal.value, sizeof(double));
    fprintf(out, "  mov rbx, 0x%llx\n", float_bits);
  }
}

/**
 * @brief Emits `target = target op expr` as a memory-destination operation
 * @details Matches `v = v + expr`, `v = v - expr`, `v = v * expr` for int
 *          targets and emits `op [addr], rax`, skipping the load and store.
 *          Operand validation must already have run. For globals the right
 *          side must be call-free (a callee could modify the global between
 *          the load and the store in the generic path order).
 * @param target_name Assignment target identifier
 * @param value Value node of the assignment
 * @param addr Already-formatted address operand (e.g. "[rbp - 8]")
 * @param right_must_be_safe Non-zero to reject calls in the right side
 * @return Non-zero if the pattern matched and was emitted
 */
static int try_assign_rmw(const char *target_name, Node *value,
                          const char *addr, int right_must_be_safe) {
  if (value == NULL || value->type != NODE_BINARY_OP) {
    return 0;
  }
  const char *op = value->binary_op.op;
  if (strcmp(op, "+") != 0 && strcmp(op, "-") != 0 && strcmp(op, "*") != 0) {
    return 0;
  }
  Node *lhs = value->binary_op.left;
  if (lhs->type != NODE_IDENTIFIER ||
      strcmp(lhs->identifier.name, target_name) != 0) {
    return 0;
  }
  if (right_must_be_safe && !expr_is_regsafe(value->binary_op.right)) {
    return 0;
  }
  Node *rhs = value->binary_op.right;
  int rhs_imm32 = 0;
  if (rhs->type == NODE_INT_LITERAL) {
    long long rv = rhs->int_literal.value;
    if (rv >= -2147483647LL - 1 && rv <= 2147483647LL) {
      rhs_imm32 = 1;
    }
  }
  if (!rhs_imm32) {
    gen_expression(rhs);
  }
  if (strcmp(op, "+") == 0) {
    if (rhs_imm32) {
      fprintf(out, "  add qword %s, %lld\n", addr, rhs->int_literal.value);
    } else {
      fprintf(out, "  add qword %s, rax\n", addr);
    }
  } else if (strcmp(op, "-") == 0) {
    if (rhs_imm32) {
      fprintf(out, "  sub qword %s, %lld\n", addr, rhs->int_literal.value);
    } else {
      fprintf(out, "  sub qword %s, rax\n", addr);
    }
  } else if (rhs_imm32) {
    fprintf(out, "  imul qword %s, %lld\n", addr, rhs->int_literal.value);
  } else {
    fprintf(out, "  imul qword %s, rax\n", addr);
  }
  fprintf(out, "  jo overflow_trap\n");
  return 1;
}

/**
 * @brief Emits the right operand into rbx with the left operand in rax
 * @details Leaf operands (literals/variables) load straight into rbx;
 *          anything else preserves rax with a push/pop around evaluation.
 * @param right Right-hand operand node
 */
static void gen_right_operand_rbx(Node *right) {
  if (is_leaf_expr(right)) {
    gen_leaf_into_rbx(right);
  } else {
    fprintf(out, "  push rax\n");
    gen_expression(right);
    fprintf(out, "  mov rbx, rax\n");
    fprintf(out, "  pop rax\n");
  }
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
 * @brief Finds a struct definition by name
 * @param name Struct name to look up
 * @return Struct index, or -1 if not found
 */
static int find_struct(const char *name) {
  for (int i = 0; i < struct_count; i++) {
    if (strcmp(struct_defs[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

/**
 * @brief Finds a class definition by name
 * @param name Class name to look up
 * @return Class index, or -1 if not found
 */
static int find_class(const char *name) {
  for (int i = 0; i < class_count; i++) {
    if (strcmp(class_defs[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

/**
 * @brief Finds a method inside a class or its bases
 * @param class_idx Class index from find_class
 * @param method Method name to look up
 * @param owner_idx Receives the class that defines the method (may be NULL)
 * @return Method index inside the owning class, or -1 if not found
 */
static int find_method(int class_idx, const char *method, int *owner_idx) {
  if (owner_idx != NULL) {
    *owner_idx = class_idx;
  }
  for (int c = class_idx; c >= 0; c = class_defs[c].base_idx) {
    for (int i = 0; i < class_defs[c].nmethods; i++) {
      if (strcmp(class_defs[c].methods[i].name, method) == 0) {
        if (owner_idx != NULL) {
          *owner_idx = c;
        }
        return i;
      }
    }
  }
  return -1;
}

#define MAX_METH_SLOTS 128

static char *meth_slots[MAX_METH_SLOTS]; /**< Method names by vtable slot */
static int meth_slot_count;              /**< Assigned vtable slots */

/**
 * @brief Returns the vtable slot for a method name, assigning one if new
 * @param name Method name
 * @return Slot index used in every class vtable
 */
static int method_slot(const char *name) {
  for (int i = 0; i < meth_slot_count; i++) {
    if (strcmp(meth_slots[i], name) == 0) {
      return i;
    }
  }
  if (meth_slot_count >= MAX_METH_SLOTS) {
    collect_error(NULL, "Too many distinct method names");
    return 0;
  }
  size_t len = strlen(name);
  meth_slots[meth_slot_count] = malloc(len + 1);
  memcpy(meth_slots[meth_slot_count], name, len);
  meth_slots[meth_slot_count][len] = '\0';
  return meth_slot_count++;
}

/**
 * @brief Tells whether kind is base or derives from base
 * @param kind Instance kind name (may be NULL)
 * @param base Base kind name (may be NULL)
 * @return Non-zero when kind is base itself or a subclass of it
 */
static int is_subclass(const char *kind, const char *base) {
  if (kind == NULL || base == NULL) {
    return 0;
  }
  if (strcmp(kind, base) == 0) {
    return 1;
  }
  for (int c = find_class(kind); c >= 0; c = class_defs[c].base_idx) {
    if (strcmp(class_defs[c].name, base) == 0) {
      return 1;
    }
  }
  return 0;
}

/**
 * @brief Finds a field inside a struct
 * @param struct_idx Struct index from find_struct
 * @param field Field name to look up
 * @return Field index, or -1 if not found
 */
static int find_struct_field(int struct_idx, const char *field) {
  if (struct_idx < 0) {
    return -1;
  }
  for (int i = 0; i < struct_defs[struct_idx].nfields; i++) {
    if (strcmp(struct_defs[struct_idx].fields[i].name, field) == 0) {
      return i;
    }
  }
  return -1;
}

/**
 * @brief Finds a field inside a class
 * @param class_idx Class index from find_class
 * @param field Field name to look up
 * @return Field index, or -1 if not found
 */
static int find_class_field(int class_idx, const char *field) {
  if (class_idx < 0) {
    return -1;
  }
  for (int i = 0; i < class_defs[class_idx].nfields; i++) {
    if (strcmp(class_defs[class_idx].fields[i].name, field) == 0) {
      return i;
    }
  }
  return -1;
}

/**
 * @brief Looks a field up on a struct or class
 * @param kind Struct or class name
 * @param field Field name
 * @param out_def Receives the field definition (may be NULL)
 * @return Field index, or -1 if not found
 */
static int find_instance_field(const char *kind, const char *field,
                               FieldDef **out_def) {
  if (out_def != NULL) {
    *out_def = NULL;
  }
  int s = find_struct(kind);
  if (s >= 0) {
    int f = find_struct_field(s, field);
    if (f >= 0 && out_def != NULL) {
      *out_def = &struct_defs[s].fields[f];
    }
    return f;
  }
  int c = find_class(kind);
  if (c >= 0) {
    int f = find_class_field(c, field);
    if (f >= 0 && out_def != NULL) {
      *out_def = &class_defs[c].fields[f];
    }
    return f;
  }
  return -1;
}

/**
 * @brief Returns the statically known instance type of an expression
 * @param node Expression node
 * @return Struct/class name for identifiers and new expressions, else NULL
 */
static const char *instance_type_of(Node *node) {
  if (node == NULL) {
    return NULL;
  }
  if (node->type == NODE_IDENTIFIER) {
    int slot = find_var(node->identifier.name);
    if (slot >= 0 &&
        (var_types[slot] == TYPE_STRUCT || var_types[slot] == TYPE_CLASS)) {
      return var_type_name[slot];
    }
    return NULL;
  }
  if (node->type == NODE_NEW) {
    return node->new_expr.type_name;
  }
  if (node->type == NODE_INDEX && node->index.base->type == NODE_IDENTIFIER) {
    int a = find_array(node->index.base->identifier.name);
    if (a >= 0) {
      return array_kind[a];
    }
  }
  if (node->type == NODE_MEMBER_ACCESS) {
    const char *owner = node->member_access.object;
    const char *field = node->member_access.member;
    if (owner != NULL && strcmp(owner, "self") == 0) {
      if (in_method && cur_class >= 0) {
        int f = find_class_field(cur_class, field);
        if (f >= 0) {
          return class_defs[cur_class].fields[f].kind;
        }
      }
      int pslot = find_var(field);
      if (pslot >= 0 && var_is_param[pslot]) {
        return var_type_name[pslot];
      }
      return NULL;
    }
    if (node->member_access.object_expr != NULL) {
      /* chained access: resolve the base, then look the field up on it */
      const char *base_kind = instance_type_of(node->member_access.object_expr);
      if (base_kind == NULL) {
        return NULL;
      }
      FieldDef *def = NULL;
      if (find_instance_field(base_kind, field, &def) >= 0 && def != NULL) {
        return def->kind;
      }
      return NULL;
    }
    const char *owner_kind = instance_type_of(&(Node){
        .type = NODE_IDENTIFIER, .identifier = {.name = (char *)owner}});
    if (owner_kind == NULL) {
      return NULL;
    }
    int s = find_struct(owner_kind);
    if (s >= 0) {
      int f = find_struct_field(s, field);
      if (f >= 0) {
        return struct_defs[s].fields[f].kind;
      }
      return NULL;
    }
    int c = find_class(owner_kind);
    if (c >= 0) {
      int f = find_class_field(c, field);
      if (f >= 0) {
        return class_defs[c].fields[f].kind;
      }
      return NULL;
    }
    return NULL;
  }
  return NULL;
}

/**
 * @brief Resolves a declaration type keyword to a value category
 * @param keyword Type keyword (num, bool, string, struct/class name)
 * @param node Fault node for unknown types
 * @param out_kind Receives struct/class name for instances, NULL otherwise
 * @return Value category (unknown struct/class names are compile errors)
 */
static ValueType resolve_decl_type(const char *keyword, Node *node,
                                   const char **out_kind) {
  if (out_kind != NULL) {
    *out_kind = NULL;
  }
  if (strcmp(keyword, "bool") == 0) {
    return TYPE_BOOL;
  }
  if (strcmp(keyword, "str") == 0) {
    return TYPE_STRING;
  }
  if (strcmp(keyword, "arr") == 0) {
    return TYPE_ARRAY;
  }
  if (strcmp(keyword, "num") == 0) {
    return TYPE_INT;
  }
  if (strcmp(keyword, "char") == 0) {
    return TYPE_INT;
  }
  int s = find_struct(keyword);
  if (s >= 0) {
    if (out_kind != NULL) {
      *out_kind = struct_defs[s].name;
    }
    return TYPE_STRUCT;
  }
  int c = find_class(keyword);
  if (c >= 0) {
    if (out_kind != NULL) {
      *out_kind = class_defs[c].name;
    }
    return TYPE_CLASS;
  }
  codegen_error(node, "Unknown type '%s'", keyword);
}

/**
 * @brief Builds the assembly label for a class method
 * @param class_name Class name
 * @param method Method name
 * @param buf Receives the mangled label (at least 128 bytes)
 */
static void method_label(const char *class_name, const char *method,
                         char *buf) {
  snprintf(buf, 128, "%s__%s", class_name, method);
}

/**
 * @brief Emits a dynamically-aligned prologue for a C runtime call
 * @details Saves the 0/8 byte misalignment above the shadow space so
 * calls inside expressions (rsp 8 off) stay 16 byte aligned
 */
static void gen_runtime_prologue(void) {
  fprintf(out, "  mov rax, rsp\n");
  fprintf(out, "  and rax, 8\n");
  fprintf(out, "  sub rsp, rax\n");
  fprintf(out, "  sub rsp, 48\n");
  fprintf(out, "  mov [rsp + 32], rax\n");
}

/**
 * @brief Emits the epilogue matching gen_runtime_prologue (result in rax)
 */
static void gen_runtime_epilogue(void) {
  fprintf(out, "  mov r10, [rsp + 32]\n");
  fprintf(out, "  add rsp, 48\n");
  fprintf(out, "  add rsp, r10\n");
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
  if (strcmp(keyword, "str") == 0) {
    return TYPE_STRING;
  }
  if (strcmp(keyword, "arr") == 0) {
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
    return "str";
  case TYPE_ARRAY:
    return "arr";
  case TYPE_STRUCT:
    return "struct";
  case TYPE_CLASS:
    return "class";
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
  if (given == TYPE_NULL) {
    /* null fits any reference-like slot, never a number */
    return declared == TYPE_STRING || declared == TYPE_STRUCT ||
           declared == TYPE_CLASS || declared == TYPE_ARRAY;
  }
  if (declared == TYPE_NULL) {
    return 0;
  }
  if (declared == TYPE_STRING || given == TYPE_STRING) {
    return declared == TYPE_STRING && given == TYPE_STRING;
  }
  if (declared == TYPE_ARRAY || given == TYPE_ARRAY) {
    return declared == TYPE_ARRAY && given == TYPE_ARRAY;
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
  case NODE_NULL:
    return TYPE_NULL;
  case NODE_IDENTIFIER: {
    int slot = find_var(node->identifier.name);
    if (slot >= 0) {
      return var_types[slot];
    }
    int g = find_global(node->identifier.name);
    if (g >= 0) {
      return global_types[g];
    }
    return TYPE_INT;
  }
  case NODE_FUNC_CALL: {
    if (strcmp(node->func_call.name, "input") == 0) {
      return TYPE_INT;
    }
    if (strcmp(node->func_call.name, "tostr") == 0) {
      return TYPE_STRING;
    }
    if (strcmp(node->func_call.name, "tonum") == 0) {
      return TYPE_FLOAT;
    }
    if (strcmp(node->func_call.name, "readFile") == 0) {
      return TYPE_STRING;
    }
    for (int s = 0; s < sig_count; s++) {
      if (strcmp(sig_names[s], node->func_call.name) == 0) {
        if (sig_return_type[s] != NULL) {
          if (strcmp(sig_return_type[s], "str") == 0) {
            return TYPE_STRING;
          }
          if (strcmp(sig_return_type[s], "arr") == 0) {
            return TYPE_ARRAY;
          }
          if (strcmp(sig_return_type[s], "void") == 0) {
            return TYPE_NULL;
          }
        }
        if (sig_return_float[s]) {
          return TYPE_FLOAT;
        }
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
    if (strcmp(op, "+") == 0 && (lt == TYPE_STRING || rt == TYPE_STRING)) {
      return TYPE_STRING;
    }
    if (lt == TYPE_FLOAT || rt == TYPE_FLOAT) {
      return TYPE_FLOAT;
    }
    return TYPE_INT;
  }
  case NODE_MEMBER_ACCESS: {
    if (node->member_access.object_expr != NULL) {
      const char *kind = instance_type_of(node->member_access.object_expr);
      if (kind == NULL) {
        return TYPE_INT;
      }
      FieldDef *def = NULL;
      int f = find_instance_field(kind, node->member_access.member, &def);
      if (f >= 0 && def != NULL) {
        return def->type;
      }
      return TYPE_INT;
    }
    if (strcmp(node->member_access.object, "self") == 0) {
      int slot = find_var(node->member_access.member);
      if (slot >= 0 && var_is_param[slot]) {
        return var_types[slot];
      }
      if (in_method && cur_class >= 0) {
        int f = find_class_field(cur_class, node->member_access.member);
        if (f >= 0) {
          return class_defs[cur_class].fields[f].type;
        }
      }
      return TYPE_INT;
    }
    int slot = find_var(node->member_access.object);
    if (slot < 0) {
      return TYPE_INT;
    }
    if (var_types[slot] == TYPE_STRUCT) {
      int s = find_struct(var_type_name[slot]);
      int f = find_struct_field(s, node->member_access.member);
      if (f >= 0) {
        return struct_defs[s].fields[f].type;
      }
      return TYPE_INT;
    }
    if (var_types[slot] == TYPE_CLASS) {
      int c = find_class(var_type_name[slot]);
      int f = find_class_field(c, node->member_access.member);
      if (f >= 0) {
        return class_defs[c].fields[f].type;
      }
      return TYPE_INT;
    }
    return TYPE_INT;
  }
  case NODE_NEW: {
    int s = find_struct(node->new_expr.type_name);
    if (s >= 0) {
      return TYPE_STRUCT;
    }
    int c = find_class(node->new_expr.type_name);
    if (c >= 0) {
      return TYPE_CLASS;
    }
    return TYPE_INT;
  }
  case NODE_INDEX: {
    ValueType btype = peek_type(node->index.base);
    if (btype == TYPE_STRING) {
      return TYPE_INT;
    }
    if (btype == TYPE_ARRAY && node->index.base->type == NODE_IDENTIFIER) {
      int a = find_array(node->index.base->identifier.name);
      if (a >= 0) {
        return array_elem[a];
      }
    }
    if (btype == TYPE_ARRAY) {
      return TYPE_INT;
    }
    return TYPE_INT;
  }
  case NODE_METHOD_CALL: {
    int c = -1;
    if (node->method_call.object_expr != NULL) {
      /* resolves identifier, arr[i], and a.b.c receivers alike */
      const char *kind = instance_type_of(node->method_call.object_expr);
      if (kind != NULL) {
        c = find_class(kind);
      }
    } else if (strcmp(node->method_call.object, "self") == 0) {
      if (in_method) {
        c = cur_class;
      }
    } else {
      int slot = find_var(node->method_call.object);
      if (slot >= 0 &&
          (var_types[slot] == TYPE_STRUCT || var_types[slot] == TYPE_CLASS)) {
        c = find_class(var_type_name[slot]);
      }
    }
    if (c >= 0) {
      int owner = c;
      int m = find_method(c, node->method_call.method, &owner);
      if (m >= 0) {
        const char *rt = class_defs[owner].methods[m].ret_type;
        if (strcmp(rt, "str") == 0) {
          return TYPE_STRING;
        }
        if (strcmp(rt, "bool") == 0) {
          return TYPE_BOOL;
        }
        /* num methods normalize results to doubles (see gen return). */
        return TYPE_FLOAT;
      }
    }
    return TYPE_INT;
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
  case NODE_NULL:
    return TYPE_NULL;
  case NODE_IDENTIFIER: {
    int slot = require_var(node, node->identifier.name);
    if (slot_is_global(slot)) {
      return global_types[slot_to_global(slot)];
    }
    return var_types[slot];
  }
  case NODE_FUNC_CALL: {
    if (strcmp(node->func_call.name, "input") == 0) {
      return TYPE_INT;
    }
    if (strcmp(node->func_call.name, "tostr") == 0) {
      return TYPE_STRING;
    }
    if (strcmp(node->func_call.name, "tonum") == 0) {
      return TYPE_FLOAT;
    }
    if (strcmp(node->func_call.name, "readFile") == 0) {
      return TYPE_STRING;
    }
    for (int s = 0; s < sig_count; s++) {
      if (strcmp(sig_names[s], node->func_call.name) == 0) {
        if (sig_return_type[s] != NULL) {
          if (strcmp(sig_return_type[s], "str") == 0) {
            return TYPE_STRING;
          }
          if (strcmp(sig_return_type[s], "arr") == 0) {
            return TYPE_ARRAY;
          }
          if (strcmp(sig_return_type[s], "void") == 0) {
            codegen_error(node, "Cannot use result of void function '%s'",
                          node->func_call.name);
          }
        }
        if (sig_return_float[s]) {
          return TYPE_FLOAT;
        }
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
    if (strcmp(op, "+") == 0 && (lt == TYPE_STRING || rt == TYPE_STRING)) {
      return TYPE_STRING;
    }
    if (lt == TYPE_FLOAT || rt == TYPE_FLOAT) {
      return TYPE_FLOAT;
    }
    return TYPE_INT;
  }
  case NODE_MEMBER_ACCESS: {
    if (node->member_access.object_expr != NULL) {
      const char *kind = instance_type_of(node->member_access.object_expr);
      if (kind == NULL) {
        return TYPE_INT;
      }
      FieldDef *def = NULL;
      int f = find_instance_field(kind, node->member_access.member, &def);
      if (f >= 0 && def != NULL) {
        return def->type;
      }
      return TYPE_INT;
    }
    if (strcmp(node->member_access.object, "self") == 0) {
      int slot = find_var(node->member_access.member);
      if (slot >= 0 && var_is_param[slot]) {
        var_used[slot] = 1;
        return var_types[slot];
      }
      if (in_method && cur_class >= 0) {
        int f = find_class_field(cur_class, node->member_access.member);
        if (f >= 0) {
          int tslot = find_var("this ");
          if (tslot >= 0) {
            var_used[tslot] = 1;
          }
          return class_defs[cur_class].fields[f].type;
        }
        codegen_error(node, "'%s' is not a parameter or field of class '%s'",
                      node->member_access.member, class_defs[cur_class].name);
      }
      slot = require_param(node, node->member_access.member);
      return var_types[slot];
    }
    int slot = find_var(node->member_access.object);
    if (slot < 0) {
      codegen_error(node, "Variable '%s' not declared",
                    node->member_access.object);
    }
    var_used[slot] = 1;
    if (var_types[slot] == TYPE_STRUCT) {
      int s = find_struct(var_type_name[slot]);
      int f = find_struct_field(s, node->member_access.member);
      if (f < 0) {
        codegen_error(node, "Struct '%s' has no field '%s'",
                      var_type_name[slot], node->member_access.member);
      }
      return struct_defs[s].fields[f].type;
    }
    if (var_types[slot] == TYPE_CLASS) {
      int c = find_class(var_type_name[slot]);
      int f = find_class_field(c, node->member_access.member);
      if (f < 0) {
        codegen_error(node, "Class '%s' has no field '%s'", var_type_name[slot],
                      node->member_access.member);
      }
      return class_defs[c].fields[f].type;
    }
    codegen_error(node, "'%s' is not a struct or class instance",
                  node->member_access.object);
  }
  case NODE_NEW: {
    int s = find_struct(node->new_expr.type_name);
    if (s >= 0) {
      return TYPE_STRUCT;
    }
    int c = find_class(node->new_expr.type_name);
    if (c >= 0) {
      return TYPE_CLASS;
    }
    return TYPE_INT;
  }
  case NODE_INDEX: {
    ValueType btype = peek_type(node->index.base);
    if (btype == TYPE_STRING) {
      return TYPE_INT;
    }
    if (btype == TYPE_ARRAY && node->index.base->type == NODE_IDENTIFIER) {
      int a = find_array(node->index.base->identifier.name);
      if (a >= 0) {
        return array_elem[a];
      }
    }
    if (btype == TYPE_ARRAY) {
      return TYPE_INT;
    }
    return TYPE_INT;
  }
  case NODE_METHOD_CALL: {
    int c = -1;
    if (node->method_call.object_expr != NULL) {
      /* resolves identifier, arr[i], and a.b.c receivers alike */
      const char *kind = instance_type_of(node->method_call.object_expr);
      if (kind != NULL) {
        c = find_class(kind);
      }
    } else if (strcmp(node->method_call.object, "self") == 0) {
      if (in_method) {
        c = cur_class;
      }
    } else {
      int slot = find_var(node->method_call.object);
      if (slot >= 0 &&
          (var_types[slot] == TYPE_STRUCT || var_types[slot] == TYPE_CLASS)) {
        c = find_class(var_type_name[slot]);
      }
    }
    if (c >= 0) {
      int owner = c;
      int m = find_method(c, node->method_call.method, &owner);
      if (m >= 0) {
        const char *rt = class_defs[owner].methods[m].ret_type;
        if (strcmp(rt, "str") == 0) {
          return TYPE_STRING;
        }
        if (strcmp(rt, "bool") == 0) {
          return TYPE_BOOL;
        }
        /* num methods normalize results to doubles (see gen return). */
        return TYPE_FLOAT;
      }
    }
    return TYPE_INT;
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

/* ---- Float-return detection (mirrors runtime promotion rules) ---- */

/** Scan-time value state: int-ish, float, or string */
#define RF_INT 0
#define RF_FLOAT 1
#define RF_STR 2

typedef struct {
  char name[64];
  int state;
} RFVar;

typedef struct {
  RFVar vars[128];
  int nvars;
  RFVar arrays[64];
  int narrays;
  Node *params;
  int sig;
  int saw_float;
} RFCtx;

static int rf_expr_state(Node *node, RFCtx *ctx);

static int rf_get_state(RFCtx *ctx, const char *name) {
  for (int i = ctx->nvars - 1; i >= 0; i--) {
    if (strcmp(ctx->vars[i].name, name) == 0) {
      return ctx->vars[i].state;
    }
  }
  int idx = 0;
  for (Node *p = ctx->params; p != NULL; p = p->right, idx++) {
    const char *pn =
        p->type == NODE_VAR_DECL ? p->var_decl.name : p->identifier.name;
    if (strcmp(pn, name) == 0) {
      if (ctx->sig >= 0 && idx < 32 && sig_param_float[ctx->sig][idx]) {
        return RF_FLOAT;
      }
      return RF_INT;
    }
  }
  int g = find_global(name);
  if (g >= 0) {
    if (global_types[g] == TYPE_FLOAT)
      return RF_FLOAT;
    if (global_types[g] == TYPE_STRING)
      return RF_STR;
    return RF_INT;
  }
  return RF_INT;
}

static void rf_update_var(RFCtx *ctx, const char *name, int st, int exact) {
  int old = rf_get_state(ctx, name);
  int merged = exact ? st : (old > st ? old : st);
  for (int i = ctx->nvars - 1; i >= 0; i--) {
    if (strcmp(ctx->vars[i].name, name) == 0) {
      ctx->vars[i].state = merged;
      return;
    }
  }
  if (ctx->nvars >= 128)
    return;
  snprintf(ctx->vars[ctx->nvars].name, sizeof(ctx->vars[0].name), "%s", name);
  ctx->vars[ctx->nvars].state = merged;
  ctx->nvars++;
}

static int rf_array_state(RFCtx *ctx, const char *name) {
  for (int i = ctx->narrays - 1; i >= 0; i--) {
    if (strcmp(ctx->arrays[i].name, name) == 0) {
      return ctx->arrays[i].state;
    }
  }
  return RF_INT;
}

static void rf_set_array(RFCtx *ctx, const char *name, int st) {
  for (int i = ctx->narrays - 1; i >= 0; i--) {
    if (strcmp(ctx->arrays[i].name, name) == 0) {
      ctx->arrays[i].state = st;
      return;
    }
  }
  if (ctx->narrays >= 64)
    return;
  snprintf(ctx->arrays[ctx->narrays].name, sizeof(ctx->arrays[0].name), "%s",
           name);
  ctx->arrays[ctx->narrays].state = st;
  ctx->narrays++;
}

/**
 * @brief Infers the scan-time float/string state of an expression
 * @param node Expression to inspect
 * @param ctx Function context (vars, params, arrays)
 * @return RF_INT, RF_FLOAT or RF_STR
 */
static int rf_expr_state(Node *node, RFCtx *ctx) {
  if (node == NULL) {
    return RF_INT;
  }
  switch (node->type) {
  case NODE_FLOAT_LITERAL:
    return RF_FLOAT;
  case NODE_STRING_LITERAL:
    return RF_STR;
  case NODE_INT_LITERAL:
  case NODE_NULL:
    return RF_INT;
  case NODE_IDENTIFIER:
    return rf_get_state(ctx, node->identifier.name);
  case NODE_BINARY_OP: {
    const char *op = node->binary_op.op;
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
        strcmp(op, "<") == 0 || strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 ||
        strcmp(op, ">=") == 0) {
      return RF_INT;
    }
    int l = rf_expr_state(node->binary_op.left, ctx);
    int r = rf_expr_state(node->binary_op.right, ctx);
    if (strcmp(op, "+") == 0 && (l == RF_STR || r == RF_STR)) {
      return RF_STR;
    }
    if (l == RF_FLOAT || r == RF_FLOAT) {
      return RF_FLOAT;
    }
    return RF_INT;
  }
  case NODE_FUNC_CALL: {
    const char *nm = node->func_call.name;
    if (strcmp(nm, "tonum") == 0)
      return RF_FLOAT;
    if (strcmp(nm, "tostr") == 0 || strcmp(nm, "readFile") == 0) {
      return RF_STR;
    }
    if (strcmp(nm, "input") == 0)
      return RF_INT;
    for (int s = 0; s < sig_count; s++) {
      if (strcmp(sig_names[s], nm) == 0) {
        if (sig_return_type[s] != NULL) {
          if (strcmp(sig_return_type[s], "str") == 0)
            return RF_STR;
          if (strcmp(sig_return_type[s], "void") == 0)
            return RF_INT;
        }
        if (sig_return_float[s])
          return RF_FLOAT;
        for (int j = 0; j < 32; j++) {
          if (sig_param_float[s][j])
            return RF_FLOAT;
        }
        return RF_INT;
      }
    }
    return RF_INT;
  }
  case NODE_METHOD_CALL: {
    /* Unresolvable at scan time: class methods normalize num to float,
       but the object's class is not known here, so stay conservative. */
    return RF_INT;
  }
  case NODE_INDEX: {
    if (node->index.base != NULL && node->index.base->type == NODE_IDENTIFIER) {
      return rf_array_state(ctx, node->index.base->identifier.name);
    }
    return RF_INT;
  }
  case NODE_ARRAY_LITERAL: {
    int st = RF_INT;
    for (Node *e = node->array_literal.elements; e != NULL; e = e->right) {
      int es = rf_expr_state(e, ctx);
      if (es > st)
        st = es;
    }
    return st;
  }
  case NODE_MEMBER_ACCESS:
  case NODE_NEW:
    return RF_INT;
  default:
    return RF_INT;
  }
}

/**
 * @brief Walks statements, tracking float flow into returns
 * @param list Statement list (follows right chain, recurses into blocks)
 * @param ctx Function context, sees_float set when a float return is found
 */
static void rf_walk(Node *list, RFCtx *ctx) {
  for (Node *s = list; s != NULL; s = s->right) {
    switch (s->type) {
    case NODE_VAR_DECL: {
      int st = RF_INT;
      if (s->var_decl.var_type != NULL &&
          strcmp(s->var_decl.var_type, "str") == 0) {
        st = RF_STR;
      } else if (s->var_decl.value != NULL) {
        st = rf_expr_state(s->var_decl.value, ctx);
      }
      rf_update_var(ctx, s->var_decl.name, st, 1);
      break;
    }
    case NODE_ARRAY_DECL: {
      int st = RF_INT;
      Node *rhs = s->array_decl.elements;
      if (rhs != NULL && rhs->type == NODE_IDENTIFIER) {
        st = rf_array_state(ctx, rhs->identifier.name);
      } else if (rhs != NULL && rhs->type == NODE_ARRAY_LITERAL) {
        for (Node *e = rhs->array_literal.elements; e != NULL; e = e->right) {
          int es = rf_expr_state(e, ctx);
          if (es > st)
            st = es;
        }
      }
      rf_set_array(ctx, s->array_decl.name, st);
      rf_update_var(ctx, s->array_decl.name, RF_INT, 1);
      break;
    }
    case NODE_ASSIGNMENT: {
      int st = rf_expr_state(s->assignment.value, ctx);
      rf_update_var(ctx, s->assignment.name, st, 0);
      break;
    }
    case NODE_ADD_ASSIGN:
    case NODE_SUB_ASSIGN: {
      const char *nm =
          s->type == NODE_ADD_ASSIGN ? s->add_assign.name : s->sub_assign.name;
      Node *val = s->type == NODE_ADD_ASSIGN ? s->add_assign.value
                                             : s->sub_assign.value;
      int st = rf_expr_state(val, ctx);
      rf_update_var(ctx, nm, st, 0);
      break;
    }
    case NODE_RETURN: {
      if (s->return_stmt.value != NULL &&
          rf_expr_state(s->return_stmt.value, ctx) == RF_FLOAT) {
        ctx->saw_float = 1;
      }
      break;
    }
    case NODE_IF:
      rf_walk(s->if_stmt.body, ctx);
      rf_walk(s->if_stmt.else_body, ctx);
      break;
    case NODE_WHILE:
      rf_walk(s->while_stmt.body, ctx);
      break;
    case NODE_FOR: {
      rf_update_var(ctx, s->for_stmt.var_name,
                    rf_array_state(ctx, s->for_stmt.array_name), 1);
      rf_walk(s->for_stmt.body, ctx);
      break;
    }
    default:
      break;
    }
  }
}

/**
 * @brief Marks functions whose returns yield floats (callers type them float)
 * @param root Root statement list
 * @details Monotone: existing flags are kept, so it can be re-run as the
 * param float scan refines parameters that flow into returns
 */
static void scan_return_floats(Node *root) {
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type != NODE_FUNCTION) {
      continue;
    }
    int sig = -1;
    for (int i = 0; i < sig_count; i++) {
      if (strcmp(sig_names[i], s->function.name) == 0) {
        sig = i;
        break;
      }
    }
    if (sig < 0 || sig_return_float[sig]) {
      continue;
    }
    RFCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = sig_params[sig];
    ctx.sig = sig;
    rf_walk(s->function.body, &ctx);
    if (ctx.saw_float) {
      sig_return_float[sig] = 1;
    }
  }
}

/**
 * @brief Promotes num globals initialized with a float to TYPE_FLOAT
 * @param root Statement list (follows right chain)
 */
static void promote_global_inits(Node *root) {
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_VAR_DECL && s->var_decl.is_global &&
        s->var_decl.value != NULL) {
      int g = find_global(s->var_decl.name);
      if (g >= 0 && global_types[g] == TYPE_INT &&
          peek_type(s->var_decl.value) == TYPE_FLOAT) {
        global_types[g] = TYPE_FLOAT;
      }
    }
  }
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
    case NODE_CLASS_DEF:
      scan_returns_bool_only(s->class_def.methods, found, all_bool);
      break;
    case NODE_METHOD_DEF:
      scan_returns_bool_only(s->method_def.body, found, all_bool);
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
    case NODE_INDEX_ASSIGN:
      scan_float_calls(node->index_assign.value);
      scan_float_calls(node->index_assign.index);
      break;
    case NODE_ARRAY_DECL:
      scan_float_calls(node->array_decl.elements);
      break;
    case NODE_ARRAY_LITERAL:
      scan_float_calls(node->array_literal.elements);
      break;
    case NODE_NEW:
      scan_float_calls(node->new_expr.args);
      break;
    case NODE_INDEX:
      scan_float_calls(node->index.base);
      scan_float_calls(node->index.index);
      break;
    case NODE_METHOD_CALL:
      scan_float_calls(node->method_call.args);
      scan_float_calls(node->method_call.object_expr);
      break;
    case NODE_MEMBER_ACCESS:
      scan_float_calls(node->member_access.object_expr);
      break;
    case NODE_MEMBER_ASSIGN:
      scan_float_calls(node->member_assign.value);
      scan_float_calls(node->member_assign.object_expr);
      break;
    case NODE_CLASS_DEF:
      scan_float_calls(node->class_def.methods);
      break;
    case NODE_METHOD_DEF:
      scan_float_calls(node->method_def.body);
      break;
    default:
      break;
    }
  }
  scan_float_calls(node->right);
}

/**
 * @brief Promotes method params receiving floats (matched by method name)
 * @param node Node to visit (follows right sibling chain)
 * @details Method calls resolve their class at codegen time, but float
 * promotion is scanned beforehand, so promotion matches by method name
 * across all classes. Boundary conversions keep this sound.
 */
static void scan_method_float_calls(Node *node) {
  if (node == NULL) {
    return;
  }
  if (node->type == NODE_METHOD_CALL) {
    const char *method = node->method_call.method;
    size_t mlen = strlen(method);
    int idx = 0;
    for (Node *a = node->method_call.args; a != NULL && idx < 32;
         a = a->right, idx++) {
      if (peek_type(a) == TYPE_FLOAT) {
        for (int s = 0; s < sig_count; s++) {
          size_t slen = strlen(sig_names[s]);
          if (slen > mlen + 2 &&
              strcmp(sig_names[s] + slen - mlen, method) == 0 &&
              sig_names[s][slen - mlen - 2] == '_' &&
              sig_names[s][slen - mlen - 1] == '_') {
            sig_param_float[s][idx] = 1;
          }
        }
      }
    }
    scan_method_float_calls(node->method_call.args);
  } else {
    switch (node->type) {
    case NODE_VAR_DECL:
      scan_method_float_calls(node->var_decl.value);
      break;
    case NODE_PRINT:
      scan_method_float_calls(node->print_stmt.value);
      break;
    case NODE_RETURN:
      scan_method_float_calls(node->return_stmt.value);
      break;
    case NODE_BINARY_OP:
      scan_method_float_calls(node->binary_op.left);
      scan_method_float_calls(node->binary_op.right);
      break;
    case NODE_IF:
      scan_method_float_calls(node->if_stmt.condition);
      scan_method_float_calls(node->if_stmt.body);
      scan_method_float_calls(node->if_stmt.else_body);
      break;
    case NODE_WHILE:
      scan_method_float_calls(node->while_stmt.condition);
      scan_method_float_calls(node->while_stmt.body);
      break;
    case NODE_FOR:
      scan_method_float_calls(node->for_stmt.body);
      break;
    case NODE_FUNCTION:
      scan_method_float_calls(node->function.body);
      break;
    case NODE_ASSIGNMENT:
      scan_method_float_calls(node->assignment.value);
      break;
    case NODE_ADD_ASSIGN:
      scan_method_float_calls(node->add_assign.value);
      break;
    case NODE_SUB_ASSIGN:
      scan_method_float_calls(node->sub_assign.value);
      break;
    case NODE_INDEX_ASSIGN:
      scan_method_float_calls(node->index_assign.value);
      scan_method_float_calls(node->index_assign.index);
      break;
    case NODE_ARRAY_DECL:
      scan_method_float_calls(node->array_decl.elements);
      break;
    case NODE_ARRAY_LITERAL:
      scan_method_float_calls(node->array_literal.elements);
      break;
    case NODE_NEW:
      scan_method_float_calls(node->new_expr.args);
      break;
    case NODE_INDEX:
      scan_method_float_calls(node->index.base);
      scan_method_float_calls(node->index.index);
      break;
    case NODE_MEMBER_ACCESS:
      scan_method_float_calls(node->member_access.object_expr);
      break;
    case NODE_MEMBER_ASSIGN:
      scan_method_float_calls(node->member_assign.value);
      scan_method_float_calls(node->member_assign.object_expr);
      break;
    case NODE_CLASS_DEF:
      scan_method_float_calls(node->class_def.methods);
      break;
    case NODE_METHOD_DEF:
      scan_method_float_calls(node->method_def.body);
      break;
    default:
      break;
    }
  }
  scan_method_float_calls(node->right);
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
    term_report(TERM_ERROR, codegen_source, 1, 1, 1,
                "Too many string literals");
    return 0;
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
        while (value[j] == '.' && is_name_start(value[j + 1])) {
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
  case NODE_ADD_ASSIGN:
    collect_strings(node->add_assign.value);
    break;
  case NODE_SUB_ASSIGN:
    collect_strings(node->sub_assign.value);
    break;
  case NODE_INDEX_ASSIGN:
    collect_strings(node->index_assign.value);
    collect_strings(node->index_assign.index);
    break;
  case NODE_ARRAY_DECL:
    collect_strings(node->array_decl.elements);
    break;
  case NODE_ARRAY_LITERAL:
    collect_strings(node->array_literal.elements);
    break;
  case NODE_NEW:
    collect_strings(node->new_expr.args);
    break;
  case NODE_INDEX:
    collect_strings(node->index.base);
    collect_strings(node->index.index);
    break;
  case NODE_METHOD_CALL:
    collect_strings(node->method_call.args);
    collect_strings(node->method_call.object_expr);
    break;
  case NODE_MEMBER_ACCESS:
    collect_strings(node->member_access.object_expr);
    break;
  case NODE_MEMBER_ASSIGN:
    collect_strings(node->member_assign.value);
    collect_strings(node->member_assign.object_expr);
    break;
  case NODE_CLASS_DEF:
    collect_strings(node->class_def.methods);
    break;
  case NODE_METHOD_DEF:
    collect_strings(node->method_def.body);
    break;
  default:
    break;
  }

  collect_strings(node->right);
}

/**
 * @brief Checks if a byte can appear verbatim inside a NASM quoted string
 * @param c Byte to test
 * @return Non-zero for printable ASCII except the quote character
 * @details NASM treats the contents of "..." and '...' as verbatim, so
 * backslashes need no escaping, but ' closes a single-quoted string
 */
static int nasm_verbatim_byte(unsigned char c) {
  return c >= 0x20 && c <= 0x7E && c != '\'';
}

/**
 * @brief Writes a string value as NASM db content
 * @param value Raw string value (escapes already decoded by the lexer)
 * @details Runs of printable bytes are emitted in a single-quoted NASM
 * string; everything else (newline, tab, CR, quote, high bytes) is
 * emitted as a numeric byte. No assembler-level escaping is used, so
 * decoded escapes round-trip exactly.
 */
static void write_nasm_string(const char *value) {
  size_t len = strlen(value);
  size_t i = 0;
  int first = 1;
  while (i < len) {
    size_t run = i;
    while (run < len && nasm_verbatim_byte((unsigned char)value[run])) {
      run++;
    }
    if (run > i) {
      if (!first) {
        fprintf(out, ", ");
      }
      fprintf(out, "'%.*s'", (int)(run - i), value + i);
      first = 0;
      i = run;
      continue;
    }
    if (!first) {
      fprintf(out, ", ");
    }
    fprintf(out, "%u", (unsigned)(unsigned char)value[i]);
    first = 0;
    i++;
  }
  if (first) {
    fprintf(out, "0");
    return;
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
  fprintf(out, "  fmt_index db \"index out of bounds\", 10, 0\n");
  fprintf(out, "  fmt_alloc db \"out of memory\", 10, 0\n");
  fprintf(out, "  fmt_null db \"null instance access\", 10, 0\n");
  fprintf(out, "  fmt_open_r db \"rb\", 0\n");
  fprintf(out, "  fmt_open_w db \"wb\", 0\n");
  fprintf(out, "  fmt_openfail db \"could not open file\", 10, 0\n");
  fprintf(out, "  stack_floor dq 0\n");
  /* Global variables: one dq per global, initialized to 0 at startup */
  for (int i = 0; i < global_count; i++) {
    fprintf(out, "  global_%s dq 0\n", global_names[i]);
  }
  for (int i = 0; i < string_count; i++) {
    fprintf(out, "  str%d db ", i);
    if (string_table[i][0] == '\0') {
      fprintf(out, "0");
    } else {
      write_nasm_string(string_table[i]);
    }
    fprintf(out, "\n");
  }
  /* Class vtables: one row per class, one entry per method slot. */
  for (int c = 0; c < class_count; c++) {
    fprintf(out, "  jotvt_%d dq", c);
    int first = 1;
    for (int s = 0; s < meth_slot_count; s++) {
      int owner = -1;
      int m = find_method(c, meth_slots[s], &owner);
      const char *lab = NULL;
      char buf[128];
      if (m >= 0) {
        method_label(class_defs[owner].name, meth_slots[s], buf);
        lab = buf;
      }
      fprintf(out, "%s %s", first ? "" : ",", lab != NULL ? lab : "0");
      first = 0;
    }
    if (first) {
      fprintf(out, " 0");
    }
    fprintf(out, "\n");
  }
  if (class_count > 0) {
    fprintf(out, "  jot_vtables dq");
    for (int c = 0; c < class_count; c++) {
      fprintf(out, "%s jotvt_%d", c == 0 ? "" : ",", c);
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
 * @param node Call node (0 args, or 1 string prompt like Python)
 * @details Aligns the stack dynamically since calls inside expressions
 * may run with rsp 8 off 16 byte alignment. A prompt is printed with
 * no trailing newline and stdout is flushed before blocking on scanf.
 */
static void gen_input(Node *node) {
  Node *prompt = node->func_call.args;
  int arg_count = 0;
  for (Node *a = prompt; a != NULL; a = a->right) {
    arg_count++;
  }
  if (arg_count > 1) {
    codegen_error(node, "input takes at most 1 argument");
  }
  if (prompt != NULL) {
    ValueType pt = expr_type(prompt);
    if (pt != TYPE_STRING) {
      codegen_error(prompt, "input prompt must be a str, got %s",
                    type_name(pt));
    }
    gen_expression(prompt);
    fprintf(out, "  mov rdx, rax\n");
    fprintf(out, "  lea rcx, [rel fmt_str]\n");
    gen_runtime_prologue();
    fprintf(out, "  call printf\n");
    gen_runtime_epilogue();
    fprintf(out, "  xor ecx, ecx\n");
    gen_runtime_prologue();
    fprintf(out, "  call fflush\n");
    gen_runtime_epilogue();
  }
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
 * @brief Generates a string length read, result left in rax
 * @param node Call node (exactly 1 string argument)
 */
static void gen_len(Node *node) {
  Node *arg = node->func_call.args;
  int arg_count = 0;
  for (Node *a = arg; a != NULL; a = a->right) {
    arg_count++;
  }
  if (arg_count != 1) {
    codegen_error(node, "len takes exactly 1 argument");
  }
  ValueType given = expr_type(arg);
  if (given != TYPE_STRING) {
    codegen_error(arg, "len expects a str, got %s", type_name(given));
  }
  gen_expression(arg);
  fprintf(out, "  mov rcx, rax\n");
  gen_runtime_prologue();
  fprintf(out, "  call strlen\n");
  gen_runtime_epilogue();
}

/**
 * @brief Generates tostr(num), result is a heap string pointer in rax
 * @param node Call node (exactly 1 num argument)
 * @details Formats into a malloc'd buffer with snprintf and returns the
 * buffer, so the result concatenates like any other string
 */
static void gen_to_str(Node *node) {
  Node *arg = node->func_call.args;
  int arg_count = 0;
  for (Node *a = arg; a != NULL; a = a->right) {
    arg_count++;
  }
  if (arg_count != 1) {
    codegen_error(node, "tostr takes exactly 1 argument");
  }
  ValueType given = expr_type(arg);
  if (!is_numeric(given)) {
    codegen_error(arg, "tostr expects a num, got %s", type_name(given));
  }
  int is_float = (given == TYPE_FLOAT);
  gen_expression(arg);
  /* num value arrives in rax (double bits for float); keep it safe across
     the malloc call by spilling to a private slot first. */
  fprintf(out, "  sub rsp, 64\n");
  fprintf(out, "  mov [rsp + 0], rax\n");
  fprintf(out, "  mov rcx, 64\n");
  gen_runtime_prologue();
  fprintf(out, "  call malloc\n");
  gen_runtime_epilogue();
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz alloc_trap\n");
  fprintf(out, "  mov [rsp + 8], rax\n");
  fprintf(out, "  mov rcx, [rsp + 8]\n");
  fprintf(out, "  mov rdx, 64\n");
  fprintf(out, "  lea r8, [rel %s]\n",
          is_float ? "fmt_float_raw" : "fmt_int_raw");
  if (is_float) {
    fprintf(out, "  mov r9, [rsp + 0]\n");
    fprintf(out, "  movq xmm3, [rsp + 0]\n");
    fprintf(out, "  mov eax, 4\n");
  } else {
    fprintf(out, "  mov r9, [rsp + 0]\n");
    fprintf(out, "  xor eax, eax\n");
  }
  fprintf(out, "  mov [rsp + 16], rax\n");
  gen_runtime_prologue();
  fprintf(out, "  call snprintf\n");
  gen_runtime_epilogue();
  fprintf(out, "  mov rax, [rsp + 8]\n");
  fprintf(out, "  add rsp, 64\n");
}

/**
 * @brief Generates tonum(str), result left in rax as double bits
 * @param node Call node (exactly 1 str argument)
 */
static void gen_to_num(Node *node) {
  Node *arg = node->func_call.args;
  int arg_count = 0;
  for (Node *a = arg; a != NULL; a = a->right) {
    arg_count++;
  }
  if (arg_count != 1) {
    codegen_error(node, "tonum takes exactly 1 argument");
  }
  ValueType given = expr_type(arg);
  if (given != TYPE_STRING) {
    codegen_error(arg, "tonum expects a str, got %s", type_name(given));
  }
  gen_expression(arg);
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz null_trap\n");
  fprintf(out, "  mov rcx, rax\n");
  fprintf(out, "  xor edx, edx\n");
  gen_runtime_prologue();
  fprintf(out, "  call strtod\n");
  gen_runtime_epilogue();
  fprintf(out, "  movq rax, xmm0\n");
}

/**
 * @brief Generates readFile(path), result is a heap string pointer in rax
 * @param node Call node (exactly 1 str argument)
 * @details Opens in binary mode, reads the whole file, NUL terminates.
 * Values live in a private spill area because the runtime prologue
 * clobbers rax.
 */
static void gen_read_file(Node *node) {
  Node *arg = node->func_call.args;
  int arg_count = 0;
  for (Node *a = arg; a != NULL; a = a->right) {
    arg_count++;
  }
  if (arg_count != 1) {
    codegen_error(node, "readFile takes exactly 1 argument");
  }
  ValueType given = expr_type(arg);
  if (given != TYPE_STRING) {
    codegen_error(arg, "readFile expects a str path, got %s", type_name(given));
  }
  gen_expression(arg);
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz null_trap\n");
  fprintf(out, "  sub rsp, 64\n");
  fprintf(out, "  mov [rsp + 0], rax\n");
  /* fopen(path, "rb") */
  fprintf(out, "  mov rcx, [rsp + 0]\n");
  fprintf(out, "  lea rdx, [rel fmt_open_r]\n");
  gen_runtime_prologue();
  fprintf(out, "  call fopen\n");
  gen_runtime_epilogue();
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz open_trap\n");
  fprintf(out, "  mov [rsp + 8], rax\n");
  /* fseek(fp, 0, SEEK_END) */
  fprintf(out, "  mov rcx, [rsp + 8]\n");
  fprintf(out, "  xor edx, edx\n");
  fprintf(out, "  mov r8d, 2\n");
  gen_runtime_prologue();
  fprintf(out, "  call fseek\n");
  gen_runtime_epilogue();
  /* size = (long)ftell(fp) */
  fprintf(out, "  mov rcx, [rsp + 8]\n");
  gen_runtime_prologue();
  fprintf(out, "  call ftell\n");
  gen_runtime_epilogue();
  fprintf(out, "  movsxd rax, eax\n");
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  js open_trap\n");
  fprintf(out, "  mov [rsp + 16], rax\n");
  /* buf = malloc(size + 1) */
  fprintf(out, "  mov rcx, [rsp + 16]\n");
  fprintf(out, "  add rcx, 1\n");
  fprintf(out, "  jo overflow_trap\n");
  gen_runtime_prologue();
  fprintf(out, "  call malloc\n");
  gen_runtime_epilogue();
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz alloc_trap\n");
  fprintf(out, "  mov [rsp + 24], rax\n");
  /* fread(buf, 1, size, fp) after rewinding to the start */
  fprintf(out, "  mov rcx, [rsp + 8]\n");
  fprintf(out, "  xor edx, edx\n");
  fprintf(out, "  xor r8d, r8d\n");
  gen_runtime_prologue();
  fprintf(out, "  call fseek\n");
  gen_runtime_epilogue();
  fprintf(out, "  mov rcx, [rsp + 24]\n");
  fprintf(out, "  mov rdx, 1\n");
  fprintf(out, "  mov r8, [rsp + 16]\n");
  fprintf(out, "  mov r9, [rsp + 8]\n");
  gen_runtime_prologue();
  fprintf(out, "  call fread\n");
  gen_runtime_epilogue();
  /* fclose(fp) */
  fprintf(out, "  mov rcx, [rsp + 8]\n");
  gen_runtime_prologue();
  fprintf(out, "  call fclose\n");
  gen_runtime_epilogue();
  /* buf[size] = 0 */
  fprintf(out, "  mov rcx, [rsp + 24]\n");
  fprintf(out, "  mov rdx, [rsp + 16]\n");
  fprintf(out, "  mov byte [rcx + rdx], 0\n");
  fprintf(out, "  mov rax, [rsp + 24]\n");
  fprintf(out, "  add rsp, 64\n");
}

/**
 * @brief Generates writeFile(path, text), result is bytes written in rax
 * @param node Call node (exactly 2 str arguments)
 */
static void gen_write_file(Node *node) {
  Node *path = node->func_call.args;
  int arg_count = 0;
  for (Node *a = path; a != NULL; a = a->right) {
    arg_count++;
  }
  if (arg_count != 2) {
    codegen_error(node, "writeFile takes exactly 2 arguments");
  }
  ValueType ptype = expr_type(path);
  if (ptype != TYPE_STRING) {
    codegen_error(path, "writeFile expects a str path, got %s",
                  type_name(ptype));
  }
  Node *text = path->right;
  ValueType ttype = expr_type(text);
  if (ttype != TYPE_STRING) {
    codegen_error(text, "writeFile expects str text, got %s", type_name(ttype));
  }
  gen_expression(path);
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz null_trap\n");
  fprintf(out, "  sub rsp, 64\n");
  fprintf(out, "  mov [rsp + 0], rax\n");
  gen_expression(text);
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz null_trap\n");
  fprintf(out, "  mov [rsp + 8], rax\n");
  /* len = strlen(text) */
  fprintf(out, "  mov rcx, [rsp + 8]\n");
  gen_runtime_prologue();
  fprintf(out, "  call strlen\n");
  gen_runtime_epilogue();
  fprintf(out, "  mov [rsp + 16], rax\n");
  /* fp = fopen(path, "wb") */
  fprintf(out, "  mov rcx, [rsp + 0]\n");
  fprintf(out, "  lea rdx, [rel fmt_open_w]\n");
  gen_runtime_prologue();
  fprintf(out, "  call fopen\n");
  gen_runtime_epilogue();
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz open_trap\n");
  fprintf(out, "  mov [rsp + 24], rax\n");
  /* fwrite(text, 1, len, fp) */
  fprintf(out, "  mov rcx, [rsp + 8]\n");
  fprintf(out, "  mov rdx, 1\n");
  fprintf(out, "  mov r8, [rsp + 16]\n");
  fprintf(out, "  mov r9, [rsp + 24]\n");
  gen_runtime_prologue();
  fprintf(out, "  call fwrite\n");
  gen_runtime_epilogue();
  fprintf(out, "  mov [rsp + 32], rax\n");
  /* fclose(fp) */
  fprintf(out, "  mov rcx, [rsp + 24]\n");
  gen_runtime_prologue();
  fprintf(out, "  call fclose\n");
  gen_runtime_epilogue();
  fprintf(out, "  mov rax, [rsp + 32]\n");
  fprintf(out, "  add rsp, 64\n");
}

/**
 * @brief Generates code for a function call, result left in rax
 * @param node Call node to generate
 * @details First four arguments use rcx, rdx, r8, r9. The rest spill
 * onto the stack above the 32 byte shadow space (Windows x64).
 */
/**
 * @brief Non-zero if evaluating the expression never writes arg registers
 * @details Used to pick the lean call path: every node it accepts only
 *          touches rax/rbx/xmm and balanced stack slots, so earlier
 *          arguments already loaded into rcx/rdx/r8/r9 stay intact.
 *          Anything that calls the runtime (user calls, concat, strcmp,
 *          malloc) is rejected.
 * @param node Expression to classify (NULL is safe)
 */
static int expr_is_regsafe(Node *node) {
  if (node == NULL) {
    return 1;
  }
  switch (node->type) {
  case NODE_INT_LITERAL:
  case NODE_FLOAT_LITERAL:
  case NODE_STRING_LITERAL:
  case NODE_IDENTIFIER:
  case NODE_NULL:
    return 1;
  case NODE_BINARY_OP: {
    ValueType lt = peek_type(node->binary_op.left);
    ValueType rt = peek_type(node->binary_op.right);
    if (lt == TYPE_STRING || rt == TYPE_STRING) {
      return 0;
    }
    return expr_is_regsafe(node->binary_op.left) &&
           expr_is_regsafe(node->binary_op.right);
  }
  default:
    return 0;
  }
}

static void gen_call(Node *node) {
  const char *regs[4] = {"rcx", "rdx", "r8", "r9"};
  int arg_count = 0;
  for (Node *a = node->func_call.args; a != NULL; a = a->right) {
    arg_count++;
  }
  if (strcmp(node->func_call.name, "input") == 0) {
    gen_input(node);
    return;
  }
  if (strcmp(node->func_call.name, "len") == 0) {
    gen_len(node);
    return;
  }
  if (strcmp(node->func_call.name, "tostr") == 0) {
    gen_to_str(node);
    return;
  }
  if (strcmp(node->func_call.name, "tonum") == 0) {
    gen_to_num(node);
    return;
  }
  if (strcmp(node->func_call.name, "readFile") == 0) {
    gen_read_file(node);
    return;
  }
  if (strcmp(node->func_call.name, "writeFile") == 0) {
    gen_write_file(node);
    return;
  }
  for (int s = 0; s < sig_count; s++) {
    if (strcmp(sig_names[s], node->func_call.name) == 0) {
      Node *a = node->func_call.args;
      Node *p = sig_params[s];
      while (a != NULL && p != NULL) {
        const char *want_kind = NULL;
        ValueType want = TYPE_INT;
        if (p->type == NODE_VAR_DECL) {
          want = resolve_decl_type(p->var_decl.var_type, p, &want_kind);
        }
        check_call_arg(a, want, want_kind);
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
  /* Lean path: a single argument, or up to four side-effect-free
     arguments, go straight from rax into the argument registers with no
     spill/reload round trip. Only 32 bytes of shadow space are needed. */
  int lean = (arg_count <= 1);
  if (!lean && arg_count <= 4) {
    lean = 1;
    for (Node *a = node->func_call.args; a != NULL; a = a->right) {
      if (!expr_is_regsafe(a)) {
        lean = 0;
        break;
      }
    }
  }
  if (lean) {
    fprintf(out, "  sub rsp, 32\n");
    int li = 0;
    for (Node *a = node->func_call.args; a != NULL; a = a->right, li++) {
      ValueType given = peek_type(a);
      ValueType want = TYPE_INT;
      Node *p = sig_param_head;
      for (int k = 0; k < li && p != NULL; k++) {
        p = p->right;
      }
      if (p != NULL) {
        want = p->type == NODE_VAR_DECL ? type_keyword(p->var_decl.var_type)
                                        : TYPE_INT;
        if (want == TYPE_INT && sig_idx_for_call >= 0 && li < 32 &&
            sig_param_float[sig_idx_for_call][li]) {
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
      fprintf(out, "  mov %s, rax\n", regs[li]);
    }
    fprintf(out, "  call %s\n", func_label(node->func_call.name));
    fprintf(out, "  add rsp, 32\n");
    return;
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
 * @brief Concatenates two strings, result left in rax
 * @details IN: rax holds left pointer, rbx holds right pointer, rsp aligned.
 * Allocates len1+len2+1 bytes and copies both parts including the NUL.
 */
static void gen_string_concat(void) {
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz null_trap\n");
  fprintf(out, "  test rbx, rbx\n");
  fprintf(out, "  jz null_trap\n");
  fprintf(out, "  sub rsp, 64\n");
  fprintf(out, "  mov [rsp + 0], rax\n");
  fprintf(out, "  mov [rsp + 8], rbx\n");
  fprintf(out, "  mov rcx, [rsp + 0]\n");
  gen_runtime_prologue();
  fprintf(out, "  call strlen\n");
  gen_runtime_epilogue();
  fprintf(out, "  mov [rsp + 16], rax\n");
  fprintf(out, "  mov rcx, [rsp + 8]\n");
  gen_runtime_prologue();
  fprintf(out, "  call strlen\n");
  gen_runtime_epilogue();
  fprintf(out, "  mov [rsp + 24], rax\n");
  fprintf(out, "  mov rax, [rsp + 16]\n");
  fprintf(out, "  add rax, [rsp + 24]\n");
  fprintf(out, "  jo overflow_trap\n");
  fprintf(out, "  add rax, 1\n");
  fprintf(out, "  jo overflow_trap\n");
  fprintf(out, "  mov rcx, rax\n");
  gen_runtime_prologue();
  fprintf(out, "  call malloc\n");
  gen_runtime_epilogue();
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz alloc_trap\n");
  fprintf(out, "  mov [rsp + 32], rax\n");
  fprintf(out, "  mov rcx, [rsp + 32]\n");
  fprintf(out, "  mov rdx, [rsp + 0]\n");
  fprintf(out, "  mov r8, [rsp + 16]\n");
  gen_runtime_prologue();
  fprintf(out, "  call memcpy\n");
  gen_runtime_epilogue();
  fprintf(out, "  mov rax, [rsp + 32]\n");
  fprintf(out, "  add rax, [rsp + 16]\n");
  fprintf(out, "  mov rcx, rax\n");
  fprintf(out, "  mov rdx, [rsp + 8]\n");
  fprintf(out, "  mov r8, [rsp + 24]\n");
  fprintf(out, "  add r8, 1\n");
  gen_runtime_prologue();
  fprintf(out, "  call memcpy\n");
  gen_runtime_epilogue();
  fprintf(out, "  mov rax, [rsp + 32]\n");
  fprintf(out, "  add rsp, 64\n");
}

/**
 * @brief Checks one call argument against its parameter type
 * @param arg Argument expression node
 * @param want Declared parameter category
 * @param want_kind Struct/class name when want is an instance, else NULL
 */
static void check_call_arg(Node *arg, ValueType want, const char *want_kind) {
  ValueType given = expr_type(arg);
  if (want == TYPE_STRUCT || want == TYPE_CLASS) {
    if (given == want) {
      const char *given_kind = instance_type_of(arg);
      if (given_kind != NULL && want_kind != NULL &&
          !is_subclass(given_kind, want_kind)) {
        codegen_error(arg, "Cannot pass %s to %s parameter", given_kind,
                      want_kind);
      }
    } else if (given == TYPE_INT && arg->type == NODE_FUNC_CALL) {
      /* Optimistic: assume the call returns a matching instance. */
    } else {
      codegen_error(arg, "Cannot pass %s to %s parameter", type_name(given),
                    want_kind);
    }
  } else if (!types_compatible(want, given)) {
    codegen_error(arg, "Cannot pass %s to %s parameter", type_name(given),
                  type_name(want));
  }
}

/**
 * @brief Generates code for a method call, result left in rax
 * @param node METHOD_CALL node to generate
 * @details The instance pointer travels as hidden first argument (rcx)
 */
/**
 * @brief Byte size of an instance (every field is one 8-byte slot)
 * @param kind Struct or class name
 * @return Byte size, at least 8
 */
static long long instance_size(const char *kind) {
  int nfields = 0;
  int s = kind != NULL ? find_struct(kind) : -1;
  if (s >= 0) {
    nfields = struct_defs[s].nfields;
  } else {
    int c = kind != NULL ? find_class(kind) : -1;
    nfields = c >= 0 ? class_defs[c].nfields : 0;
  }
  return nfields == 0 ? 8 : (long long)nfields * 8;
}

/**
 * @brief Copies the instance pointer in rax into a fresh heap instance
 * @param kind Struct or class kind name (NULL is treated as a struct)
 * @details Class instances copy an extra header word holding the class id,
 * so the fresh pointer keeps working with virtual dispatch. Null sources
 * trap (matches struct copy assignment). Leaves the fresh pointer in rax.
 */
static void gen_instance_copy(const char *kind) {
  long long size = instance_size(kind);
  int cls_idx = kind != NULL ? find_class(kind) : -1;
  int is_cls = cls_idx >= 0;
  long long alloc = size + (is_cls ? 8 : 0);
  fprintf(out, "  push rax\n");
  fprintf(out, "  mov rcx, %lld\n", alloc);
  gen_runtime_prologue();
  fprintf(out, "  call malloc\n");
  gen_runtime_epilogue();
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz alloc_trap\n");
  fprintf(out, "  mov rbx, rax\n");
  fprintf(out, "  pop rdx\n");
  fprintf(out, "  test rdx, rdx\n");
  fprintf(out, "  jz null_trap\n");
  if (is_cls) {
    fprintf(out, "  mov QWORD [rbx], %d\n", class_defs[cls_idx].class_id);
    fprintf(out, "  lea rcx, [rbx + 8]\n");
  } else {
    fprintf(out, "  mov rcx, rbx\n");
  }
  fprintf(out, "  mov r8, %lld\n", size);
  gen_runtime_prologue();
  fprintf(out, "  call memcpy\n");
  gen_runtime_epilogue();
  fprintf(out, "  mov rax, rbx\n");
  if (is_cls) {
    fprintf(out, "  add rax, 8\n");
  }
}

static void gen_method_call(Node *node) {
  const char *object = node->method_call.object;
  const char *method = node->method_call.method;
  int cls = -1;
  int self_this = 0;
  int obj_slot = -1;
  int expr_this = (node->method_call.object_expr != NULL);
  if (expr_this) {
    const char *kind = instance_type_of(node->method_call.object_expr);
    if (kind == NULL || find_class(kind) < 0) {
      codegen_error(node->method_call.object_expr,
                    "Can only call methods on a class instance");
    }
    cls = find_class(kind);
  } else if (strcmp(object, "self") == 0) {
    if (!in_method) {
      codegen_error(node, "Only self.member access is supported");
    }
    cls = cur_class;
    self_this = 1;
  } else {
    if (find_struct(object) >= 0 || find_class(object) >= 0) {
      codegen_error(node, "Cannot call method on type '%s', use an instance",
                    object);
    }
    obj_slot = find_var(object);
    if (obj_slot < 0) {
      codegen_error(node, "Variable '%s' not declared", object);
    }
    if (var_types[obj_slot] == TYPE_STRUCT) {
      codegen_error(node, "Struct '%s' has no methods",
                    var_type_name[obj_slot]);
    }
    if (var_types[obj_slot] != TYPE_CLASS) {
      codegen_error(node, "'%s' is not a class instance", object);
    }
    var_used[obj_slot] = 1;
    cls = find_class(var_type_name[obj_slot]);
  }
  int owner = cls;
  int m = find_method(cls, method, &owner);
  if (m < 0) {
    codegen_error(node, "Class '%s' has no method '%s'", class_defs[cls].name,
                  method);
  }
  MethodDef *md = &class_defs[owner].methods[m];
  char label[128];
  method_label(class_defs[owner].name, method, label);
  int vslot = method_slot(method);
  int sig_idx = -1;
  for (int s = 0; s < sig_count; s++) {
    if (strcmp(sig_names[s], label) == 0) {
      sig_idx = s;
      break;
    }
  }
  int want_n = 0;
  for (Node *p = md->params; p != NULL; p = p->right) {
    want_n++;
  }
  int got_n = 0;
  for (Node *a = node->method_call.args; a != NULL; a = a->right) {
    got_n++;
  }
  if (want_n != got_n) {
    codegen_error(node, "Expected %d arguments for method '%s', got %d", want_n,
                  method, got_n);
  }
  ValueType wants[32];
  Node *p = md->params;
  Node *a = node->method_call.args;
  int i = 0;
  for (; p != NULL && a != NULL; p = p->right, a = a->right, i++) {
    const char *kind = NULL;
    ValueType want = TYPE_INT;
    if (p->type == NODE_VAR_DECL) {
      want = resolve_decl_type(p->var_decl.var_type, p, &kind);
      if (want == TYPE_INT && sig_idx >= 0 && i < 32 &&
          sig_param_float[sig_idx][i]) {
        want = TYPE_FLOAT;
      }
    } else {
      if (sig_idx >= 0 && i < 32 && sig_param_float[sig_idx][i]) {
        want = TYPE_FLOAT;
      }
    }
    wants[i] = want;
    check_call_arg(a, want, kind);
  }
  const char *regs[4] = {"rcx", "rdx", "r8", "r9"};
  int total = 1 + got_n;
  int frame = 64 + 8 * (total > 4 ? total - 4 : 0);
  if (frame % 16 != 0) {
    frame += 8;
  }
  fprintf(out, "  sub rsp, %d\n", frame);
  if (expr_this) {
    gen_expression(node->method_call.object_expr);
  } else if (self_this) {
    int tslot = find_var("this ");
    fprintf(out, "  mov rax, [rbp - %d]\n", (tslot + 1) * 8);
  } else {
    fprintf(out, "  mov rax, [rbp - %d]\n", (obj_slot + 1) * 8);
  }
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz null_trap\n");
  fprintf(out, "  mov [rsp + %d], rax\n", frame - 32);
  i = 1;
  for (a = node->method_call.args; a != NULL; a = a->right, i++) {
    ValueType given = peek_type(a);
    ValueType want = wants[i - 1];
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
  }
  for (i = 0; i < total && i < 4; i++) {
    fprintf(out, "  mov %s, [rsp + %d]\n", regs[i], frame - 32 + 8 * i);
  }
  /* Virtual dispatch: class id header -> class vtable -> method label. */
  fprintf(out, "  mov rax, [rsp + %d]\n", frame - 32);
  fprintf(out, "  mov rax, [rax - 8]\n");
  fprintf(out, "  lea r11, [rel jot_vtables]\n");
  fprintf(out, "  mov r11, [r11 + rax*8]\n");
  fprintf(out, "  mov rax, [r11 + %d]\n", vslot * 8);
  fprintf(out, "  call rax\n");
  fprintf(out, "  add rsp, %d\n", frame);
}

/**
 * @brief Generates code for a 'new Type(args)' instantiation, ptr in rax
 * @param node NEW node to generate
 */
static void gen_new(Node *node) {
  const char *type = node->new_expr.type_name;
  int s = find_struct(type);
  int c = find_class(type);
  if (s < 0 && c < 0) {
    if (strcmp(type, "num") == 0 || strcmp(type, "bool") == 0 ||
        strcmp(type, "str") == 0 || strcmp(type, "arr") == 0) {
      codegen_error(node, "Only structs and classes can be created with 'new'");
    }
    codegen_error(node, "Unknown struct or class '%s'", type);
  }
  int nfields = (s >= 0) ? struct_defs[s].nfields : class_defs[c].nfields;
  FieldDef *fields = (s >= 0) ? struct_defs[s].fields : class_defs[c].fields;
  const char *kind = (s >= 0) ? struct_defs[s].name : class_defs[c].name;
  int nargs = 0;
  for (Node *a = node->new_expr.args; a != NULL; a = a->right) {
    nargs++;
  }
  if (nargs != nfields) {
    codegen_error(node, "Expected %d arguments for '%s', got %d", nfields, kind,
                  nargs);
  }
  ValueType given_types[MAX_FIELDS];
  int i = 0;
  for (Node *a = node->new_expr.args; a != NULL; a = a->right, i++) {
    ValueType given = expr_type(a);
    given_types[i] = given;
    ValueType want = fields[i].type;
    if (want == TYPE_STRING) {
      if (given != TYPE_STRING && given != TYPE_NULL) {
        codegen_error(a, "Cannot assign %s to str field '%s'", type_name(given),
                      fields[i].name);
      }
    } else if (want == TYPE_STRUCT || want == TYPE_CLASS) {
      if (given == TYPE_NULL) {
        given_types[i] = TYPE_NULL;
      } else if (given == want) {
        if (fields[i].kind != NULL) {
          const char *ak = instance_type_of(a);
          if (ak != NULL && !is_subclass(ak, fields[i].kind)) {
            codegen_error(a, "Cannot assign %s to %s field '%s'", ak,
                          fields[i].kind, fields[i].name);
          }
        }
      } else {
        codegen_error(a, "Cannot assign %s to %s field '%s'", type_name(given),
                      fields[i].kind != NULL ? fields[i].kind : "instance",
                      fields[i].name);
      }
    } else if (!is_numeric(given)) {
      codegen_error(a, "Cannot assign %s to num field '%s'", type_name(given),
                    fields[i].name);
    }
  }
  for (Node *a = node->new_expr.args; a != NULL; a = a->right) {
    ValueType vt = expr_type(a);
    const char *ak =
        (vt == TYPE_STRUCT || vt == TYPE_CLASS) ? instance_type_of(a) : NULL;
    if (ak != NULL && (a->type == NODE_IDENTIFIER || a->type == NODE_INDEX)) {
      /* instance constructor args bind by copy, like assignment */
      gen_expression(a);
      gen_instance_copy(ak);
    } else {
      gen_expression(a);
    }
    fprintf(out, "  push rax\n");
  }
  long long size = nfields == 0 ? 8 : (long long)nfields * 8;
  int is_cls = (c >= 0);
  fprintf(out, "  mov rcx, %lld\n", size + (is_cls ? 8 : 0));
  gen_runtime_prologue();
  fprintf(out, "  call malloc\n");
  gen_runtime_epilogue();
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz alloc_trap\n");
  fprintf(out, "  mov r11, rax\n");
  if (is_cls) {
    /* class id header keeps virtual dispatch working on copies */
    fprintf(out, "  mov QWORD [r11], %d\n", class_defs[c].class_id);
    fprintf(out, "  add r11, 8\n");
  }
  for (i = nfields - 1; i >= 0; i--) {
    fprintf(out, "  pop rdx\n");
    if (fields[i].type == TYPE_FLOAT &&
        (given_types[i] == TYPE_INT || given_types[i] == TYPE_BOOL)) {
      fprintf(out, "  cvtsi2sd xmm0, rdx\n");
      fprintf(out, "  movq rdx, xmm0\n");
    } else if (fields[i].type == TYPE_BOOL && given_types[i] == TYPE_FLOAT) {
      fprintf(out, "  movq xmm0, rdx\n");
      fprintf(out, "  cvttsd2si rdx, xmm0\n");
    }
    fprintf(out, "  mov [r11 + %d], rdx\n", i * 8);
  }
  fprintf(out, "  mov rax, r11\n");
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
    if (slot_is_global(slot)) {
      fprintf(out, "  mov rax, [rel global_%s]\n", node->identifier.name);
    } else {
      fprintf(out, "  mov rax, [rbp - %d]\n", (slot + 1) * 8);
    }
    break;
  }
  case NODE_MEMBER_ACCESS: {
    const char *object = node->member_access.object;
    const char *member = node->member_access.member;
    if (node->member_access.object_expr != NULL) {
      const char *kind = instance_type_of(node->member_access.object_expr);
      if (kind == NULL) {
        codegen_error(node->member_access.object_expr,
                      "Can only access fields on a struct or class instance");
      }
      FieldDef *def = NULL;
      int f = find_instance_field(kind, member, &def);
      if (f < 0) {
        codegen_error(node, "'%s' has no field '%s'", kind, member);
      }
      gen_expression(node->member_access.object_expr);
      fprintf(out, "  test rax, rax\n");
      fprintf(out, "  jz null_trap\n");
      fprintf(out, "  mov rax, [rax + %d]\n", f * 8);
      break;
    }
    if (strcmp(object, "self") == 0) {
      int pslot = find_var(member);
      if (pslot >= 0 && var_is_param[pslot]) {
        var_used[pslot] = 1;
        fprintf(out, "  mov rax, [rbp - %d]\n", (pslot + 1) * 8);
        break;
      }
      if (in_method && cur_class >= 0) {
        int f = find_class_field(cur_class, member);
        if (f >= 0) {
          int tslot = find_var("this ");
          var_used[tslot] = 1;
          fprintf(out, "  mov rax, [rbp - %d]\n", (tslot + 1) * 8);
          fprintf(out, "  test rax, rax\n");
          fprintf(out, "  jz null_trap\n");
          fprintf(out, "  mov rax, [rax + %d]\n", f * 8);
          break;
        }
        codegen_error(node, "'%s' is not a parameter or field of class '%s'",
                      member, class_defs[cur_class].name);
      }
      int slot = require_param(node, member);
      fprintf(out, "  mov rax, [rbp - %d]\n", (slot + 1) * 8);
      break;
    }
    if (find_struct(object) >= 0 || find_class(object) >= 0) {
      codegen_error(node, "Cannot use type '%s' as a value, use an instance",
                    object);
    }
    int slot = find_var(object);
    if (slot < 0) {
      codegen_error(node, "Variable '%s' not declared", object);
    }
    var_used[slot] = 1;
    if (var_types[slot] == TYPE_STRUCT) {
      int s = find_struct(var_type_name[slot]);
      int f = find_struct_field(s, member);
      if (f < 0) {
        codegen_error(node, "Struct '%s' has no field '%s'",
                      var_type_name[slot], member);
      }
      fprintf(out, "  mov rax, [rbp - %d]\n", (slot + 1) * 8);
      fprintf(out, "  test rax, rax\n");
      fprintf(out, "  jz null_trap\n");
      fprintf(out, "  mov rax, [rax + %d]\n", f * 8);
      break;
    }
    if (var_types[slot] == TYPE_CLASS) {
      int c = find_class(var_type_name[slot]);
      int f = find_class_field(c, member);
      if (f < 0) {
        codegen_error(node, "Class '%s' has no field '%s'", var_type_name[slot],
                      member);
      }
      fprintf(out, "  mov rax, [rbp - %d]\n", (slot + 1) * 8);
      fprintf(out, "  test rax, rax\n");
      fprintf(out, "  jz null_trap\n");
      fprintf(out, "  mov rax, [rax + %d]\n", f * 8);
      break;
    }
    codegen_error(node, "'%s' is not a struct or class instance", object);
    break;
  }
  case NODE_STRING_LITERAL: {
    int idx = add_string(node->string_literal.value);
    fprintf(out, "  lea rax, [rel str%d]\n", idx);
    break;
  }
  case NODE_FUNC_CALL: {
    for (int s = 0; s < sig_count; s++) {
      if (strcmp(sig_names[s], node->func_call.name) == 0) {
        if (sig_return_type[s] != NULL &&
            strcmp(sig_return_type[s], "void") == 0) {
          codegen_error(node, "Cannot use result of void function '%s'",
                        node->func_call.name);
        }
        break;
      }
    }
    gen_call(node);
    break;
  }
  case NODE_NEW: {
    gen_new(node);
    break;
  }
  case NODE_INDEX: {
    ValueType base_type = peek_type(node->index.base);
    ValueType index_type = peek_type(node->index.index);
    if (!is_numeric(index_type)) {
      ValueType checked = expr_type(node->index.index);
      codegen_error(node->index.index, "Index must be a number, got %s",
                    type_name(checked));
    }
    if (base_type == TYPE_ARRAY) {
      if (node->index.base->type != NODE_IDENTIFIER) {
        codegen_error(node->index.base,
                      "Cannot index a non-variable array base");
      }
      int arr = find_array(node->index.base->identifier.name);
      if (arr < 0) {
        codegen_error(node->index.base, "Array '%s' is not declared",
                      node->index.base->identifier.name);
      }
      gen_expression(node->index.base);
      fprintf(out, "  push rax\n");
      gen_expression(node->index.index);
      if (index_type == TYPE_FLOAT) {
        fprintf(out, "  movq xmm0, rax\n");
        fprintf(out, "  cvttsd2si rax, xmm0\n");
      } else if (index_type == TYPE_BOOL) {
        fprintf(out, "  movzx rax, al\n");
      }
      fprintf(out, "  mov rdx, [rsp]\n");
      fprintf(out, "  test rdx, rdx\n");
      fprintf(out, "  jz null_trap\n");
      fprintf(out, "  mov rcx, [rdx]\n");
      fprintf(out, "  cmp rax, 0\n");
      fprintf(out, "  jl index_trap\n");
      fprintf(out, "  cmp rax, rcx\n");
      fprintf(out, "  jae index_trap\n");
      fprintf(out, "  mov rdx, [rsp]\n");
      fprintf(out, "  mov rax, [rdx + rax*8 + 8]\n");
      fprintf(out, "  add rsp, 8\n");
      break;
    }
    if (base_type == TYPE_STRING) {
      ValueType checked = expr_type(node->index.base);
      (void)checked;
    } else {
      ValueType checked = expr_type(node->index.base);
      if (checked != TYPE_STRING) {
        codegen_error(node->index.base, "Only strings can be indexed, got %s",
                      type_name(checked));
      }
    }
    gen_expression(node->index.base);
    fprintf(out, "  push rax\n");
    gen_expression(node->index.index);
    if (index_type == TYPE_FLOAT) {
      fprintf(out, "  movq xmm0, rax\n");
      fprintf(out, "  cvttsd2si rax, xmm0\n");
    } else if (index_type == TYPE_BOOL) {
      fprintf(out, "  movzx rax, al\n");
    }
    fprintf(out, "  mov rbx, rax\n");
    fprintf(out, "  pop rax\n");
    fprintf(out, "  test rax, rax\n");
    fprintf(out, "  jz null_trap\n");
    fprintf(out, "  cmp rbx, 0\n");
    fprintf(out, "  jl index_trap\n");
    fprintf(out, "  sub rsp, 64\n");
    fprintf(out, "  mov [rsp + 0], rax\n");
    fprintf(out, "  mov [rsp + 8], rbx\n");
    fprintf(out, "  mov rcx, [rsp + 0]\n");
    gen_runtime_prologue();
    fprintf(out, "  call strlen\n");
    gen_runtime_epilogue();
    fprintf(out, "  cmp [rsp + 8], rax\n");
    fprintf(out, "  jae index_trap\n");
    fprintf(out, "  mov rax, [rsp + 0]\n");
    fprintf(out, "  add rax, [rsp + 8]\n");
    fprintf(out, "  movzx eax, byte [rax]\n");
    fprintf(out, "  add rsp, 64\n");
    break;
  }
  case NODE_METHOD_CALL:
    gen_method_call(node);
    break;
  case NODE_NULL:
    fprintf(out, "  xor eax, eax\n");
    break;
  case NODE_ARRAY_LITERAL:
    codegen_error(node, "Array literal not supported in codegen expression");
    break;
  case NODE_BINARY_OP: {
    const char *op = node->binary_op.op;
    ValueType left_type = peek_type(node->binary_op.left);
    ValueType right_type = peek_type(node->binary_op.right);
    int left_is_str = (left_type == TYPE_STRING);
    int right_is_str = (right_type == TYPE_STRING);
    /* null comparisons are plain 64-bit pointer compares */
    if (left_type == TYPE_NULL || right_type == TYPE_NULL) {
      if (strcmp(op, "==") != 0 && strcmp(op, "!=") != 0) {
        codegen_error(node, "Operator '%s' cannot be applied to null", op);
      }
      gen_expression(node->binary_op.left);
      fprintf(out, "  push rax\n");
      gen_expression(node->binary_op.right);
      fprintf(out, "  mov rbx, rax\n");
      fprintf(out, "  pop rax\n");
      fprintf(out, "  cmp rax, rbx\n");
      if (strcmp(op, "==") == 0) {
        fprintf(out, "  sete al\n");
      } else {
        fprintf(out, "  setne al\n");
      }
      fprintf(out, "  movzx rax, al\n");
      break;
    }
    if (left_is_str || right_is_str) {
      if (strcmp(op, "+") == 0) {
        if (!left_is_str || !right_is_str) {
          ValueType bad = left_is_str ? right_type : left_type;
          codegen_error(node, "Cannot concatenate string with %s",
                        type_name(bad));
        }
        gen_expression(node->binary_op.left);
        fprintf(out, "  push rax\n");
        gen_expression(node->binary_op.right);
        fprintf(out, "  mov rbx, rax\n");
        fprintf(out, "  pop rax\n");
        gen_string_concat();
        break;
      }
      if (is_comparison_op(op)) {
        if (!left_is_str || !right_is_str) {
          ValueType bad = left_is_str ? right_type : left_type;
          codegen_error(node, "Operator '%s' cannot compare string with %s", op,
                        type_name(bad));
        }
        gen_expression(node->binary_op.left);
        fprintf(out, "  push rax\n");
        gen_expression(node->binary_op.right);
        fprintf(out, "  mov rbx, rax\n");
        fprintf(out, "  pop rax\n");
        fprintf(out, "  mov rcx, rax\n");
        fprintf(out, "  mov rdx, rbx\n");
        gen_runtime_prologue();
        fprintf(out, "  call strcmp\n");
        gen_runtime_epilogue();
        fprintf(out, "  test eax, eax\n");
        if (strcmp(op, "==") == 0) {
          fprintf(out, "  sete al\n");
        } else if (strcmp(op, "!=") == 0) {
          fprintf(out, "  setne al\n");
        } else if (strcmp(op, "<") == 0) {
          fprintf(out, "  setl al\n");
        } else if (strcmp(op, ">") == 0) {
          fprintf(out, "  setg al\n");
        } else if (strcmp(op, "<=") == 0) {
          fprintf(out, "  setle al\n");
        } else {
          fprintf(out, "  setge al\n");
        }
        fprintf(out, "  movzx rax, al\n");
        break;
      }
      ValueType bad = left_is_str ? left_type : right_type;
      codegen_error(node, "Operator '%s' cannot be applied to %s", op,
                    type_name(bad));
    }
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
    /* Fast path: a leaf right operand (literal/variable) loads straight into
       rbx with no push/pop round trip, and small int literals are used as
       immediates directly. */
    int right_imm32 = 0;
    if (!is_float && node->binary_op.right->type == NODE_INT_LITERAL) {
      long long imm_val = node->binary_op.right->int_literal.value;
      if (imm_val >= -2147483647LL - 1 && imm_val <= 2147483647LL) {
        right_imm32 = 1;
      }
    }
    if (is_leaf_expr(node->binary_op.right)) {
      if (!right_imm32) {
        gen_leaf_into_rbx(node->binary_op.right);
      }
    } else {
      fprintf(out, "  push rax\n");
      gen_expression(node->binary_op.right);
      fprintf(out, "  mov rbx, rax\n");
      fprintf(out, "  pop rax\n");
    }
    if (!is_float) {
      long long imm_val =
          right_imm32 ? node->binary_op.right->int_literal.value : 0;
      if (right_imm32 && (strcmp(op, "/") == 0 || strcmp(op, "%") == 0)) {
        fprintf(out, "  mov rbx, %lld\n", imm_val);
      }
      if (strcmp(op, "+") == 0) {
        if (right_imm32) {
          fprintf(out, "  add rax, %lld\n", imm_val);
        } else {
          fprintf(out, "  add rax, rbx\n");
        }
        fprintf(out, "  jo overflow_trap\n");
      } else if (strcmp(op, "-") == 0) {
        if (right_imm32) {
          fprintf(out, "  sub rax, %lld\n", imm_val);
        } else {
          fprintf(out, "  sub rax, rbx\n");
        }
        fprintf(out, "  jo overflow_trap\n");
      } else if (strcmp(op, "*") == 0) {
        if (right_imm32) {
          fprintf(out, "  imul rax, %lld\n", imm_val);
        } else {
          fprintf(out, "  imul rax, rbx\n");
        }
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
      } else if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                 strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
                 strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) {
        if (right_imm32) {
          fprintf(out, "  cmp rax, %lld\n", imm_val);
        } else {
          fprintf(out, "  cmp rax, rbx\n");
        }
        if (strcmp(op, "==") == 0) {
          fprintf(out, "  sete al\n");
        } else if (strcmp(op, "!=") == 0) {
          fprintf(out, "  setne al\n");
        } else if (strcmp(op, "<") == 0) {
          fprintf(out, "  setl al\n");
        } else if (strcmp(op, ">") == 0) {
          fprintf(out, "  setg al\n");
        } else if (strcmp(op, "<=") == 0) {
          fprintf(out, "  setle al\n");
        } else {
          fprintf(out, "  setge al\n");
        }
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
        Node *cond_right = cond->binary_op.right;
        Node *cond_left = cond->binary_op.left;
        int right_imm32 = 0;
        if (cond_right->type == NODE_INT_LITERAL) {
          long long cv = cond_right->int_literal.value;
          if (cv >= -2147483647LL - 1 && cv <= 2147483647LL) {
            right_imm32 = 1;
          }
        }
        if (right_imm32 && cond_left->type == NODE_IDENTIFIER) {
          /* Best case: compare the variable memory operand directly. */
          int slot = require_var(cond_left, cond_left->identifier.name);
          if (slot_is_global(slot)) {
            fprintf(out, "  cmp qword [rel global_%s], %lld\n",
                    cond_left->identifier.name, cond_right->int_literal.value);
          } else {
            fprintf(out, "  cmp qword [rbp - %d], %lld\n", (slot + 1) * 8,
                    cond_right->int_literal.value);
          }
        } else {
          gen_expression(cond_left);
          if (right_imm32) {
            fprintf(out, "  cmp rax, %lld\n", cond_right->int_literal.value);
          } else if (is_leaf_expr(cond_right)) {
            gen_leaf_into_rbx(cond_right);
            fprintf(out, "  cmp rax, rbx\n");
          } else {
            fprintf(out, "  push rax\n");
            gen_expression(cond_right);
            fprintf(out, "  mov rbx, rax\n");
            fprintf(out, "  pop rax\n");
            fprintf(out, "  cmp rax, rbx\n");
          }
        }
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
        while (value[j] == '.' && is_name_start(value[j + 1])) {
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
          /* Split the path into segments, then build a synthetic path
             expression so a.b.c chains resolve through member access. */
          Node root_node;
          memset(&root_node, 0, sizeof(root_node));
          Node link_nodes[8];
          memset(link_nodes, 0, sizeof(link_nodes));
          char *seg_parts[9];
          int seg_total = 0;
          for (const char *rest = name;;) {
            const char *dot = strchr(rest, '.');
            size_t seg_len = dot != NULL ? (size_t)(dot - rest) : strlen(rest);
            if (seg_total >= 9) {
              codegen_error(strnode, "Placeholder path too deep");
            }
            char *part = malloc(seg_len + 1);
            memcpy(part, rest, seg_len);
            part[seg_len] = '\0';
            seg_parts[seg_total++] = part;
            if (dot == NULL) {
              break;
            }
            rest = dot + 1;
          }
          Node *head;
          int idx;
          if (seg_total > 0 && strcmp(seg_parts[0], "self") == 0) {
            if (seg_total < 2) {
              codegen_error(strnode, "'self' is not a value by itself");
            }
            root_node.type = NODE_MEMBER_ACCESS;
            root_node.member_access.object = "self";
            root_node.member_access.object_expr = NULL;
            root_node.member_access.member = seg_parts[1];
            head = &root_node;
            idx = 2;
          } else {
            root_node.type = NODE_IDENTIFIER;
            root_node.identifier.name = seg_parts[0];
            head = &root_node;
            idx = 1;
          }
          for (; idx < seg_total; idx++) {
            Node *cur = &link_nodes[idx - 1];
            cur->type = NODE_MEMBER_ACCESS;
            cur->member_access.object = NULL;
            cur->member_access.object_expr = head;
            cur->member_access.member = seg_parts[idx];
            head = cur;
          }
          head->line = strnode->line;
          head->col = strnode->col;
          head->width = strnode->width;
          ValueType ptype = expr_type(head);
          if (ptype == TYPE_STRUCT || ptype == TYPE_CLASS ||
              ptype == TYPE_ARRAY) {
            const char *shown = instance_type_of(head);
            codegen_error(strnode, "Cannot print %s '%s' directly",
                          shown != NULL ? shown : type_name(ptype), name);
          }
          gen_expression(head);
          if (ptype == TYPE_FLOAT) {
            fprintf(out, "  movq xmm0, rax\n");
          }
          fprintf(out, "  mov rdx, rax\n");
          for (int k = 0; k < seg_total; k++) {
            free(seg_parts[k]);
          }
          free(name);
          if (ptype == TYPE_STRING) {
            fprintf(out, "  lea rcx, [rel fmt_str]\n");
            fprintf(out, "  sub rsp, 32\n");
            fprintf(out, "  call printf\n");
            fprintf(out, "  add rsp, 32\n");
          } else if (ptype == TYPE_FLOAT) {
            fprintf(out, "  lea rcx, [rel fmt_float_raw]\n");
            fprintf(out, "  movapd xmm1, xmm0\n");
            fprintf(out, "  sub rsp, 32\n");
            fprintf(out, "  call printf\n");
            fprintf(out, "  add rsp, 32\n");
          } else {
            fprintf(out, "  lea rcx, [rel fmt_int_raw]\n");
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
    try_gen_statement(s);
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
    if (node->var_decl.is_global) {
      /* Registered by the pre-scan; claim it and emit the initializer. */
      int g = find_global(node->var_decl.name);
      if (g < 0) {
        codegen_error(node, "Global '%s' was not registered",
                      node->var_decl.name);
      }
      if (global_init[g] != NULL) {
        codegen_error(node, "Duplicate global variable '%s'",
                      node->var_decl.name);
      }
      global_init[g] = "";
      if (node->var_decl.value != NULL) {
        ValueType igiven = peek_type(node->var_decl.value);
        gen_expression(node->var_decl.value);
        if (global_types[g] == TYPE_INT && igiven == TYPE_FLOAT) {
          global_types[g] = TYPE_FLOAT;
        } else if (global_types[g] == TYPE_FLOAT &&
                   (igiven == TYPE_INT || igiven == TYPE_BOOL)) {
          fprintf(out, "  cvtsi2sd xmm0, rax\n");
          fprintf(out, "  movq rax, xmm0\n");
        } else if (global_types[g] == TYPE_BOOL && igiven == TYPE_FLOAT) {
          fprintf(out, "  movq xmm0, rax\n");
          fprintf(out, "  cvttsd2si rax, xmm0\n");
        }
        fprintf(out, "  mov [rel global_%s], rax\n", node->var_decl.name);
      }
      break;
    }
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
    const char *declared_kind = NULL;
    ValueType declared =
        resolve_decl_type(node->var_decl.var_type, node, &declared_kind);
    ValueType given = TYPE_INT;
    int has_value = (node->var_decl.value != NULL);
    if (has_value) {
      given = expr_type(node->var_decl.value);
      if (declared == TYPE_STRUCT || declared == TYPE_CLASS) {
        if (node->var_decl.value->type == NODE_NEW) {
          if (!is_subclass(node->var_decl.value->new_expr.type_name,
                           declared_kind)) {
            codegen_error(node->var_decl.value, "Cannot assign %s to %s '%s'",
                          node->var_decl.value->new_expr.type_name,
                          declared_kind, node->var_decl.name);
          }
          given = declared;
        } else if (given == declared) {
          const char *given_kind = instance_type_of(node->var_decl.value);
          if (given_kind != NULL && !is_subclass(given_kind, declared_kind)) {
            codegen_error(node->var_decl.value, "Cannot assign %s to %s '%s'",
                          given_kind, declared_kind, node->var_decl.name);
          }
        } else if (given == TYPE_INT &&
                   node->var_decl.value->type == NODE_FUNC_CALL) {
          given = declared;
        } else if (given == TYPE_NULL) {
          given = declared;
        } else {
          codegen_error(node->var_decl.value, "Cannot assign %s to %s '%s'",
                        type_name(given), declared_kind, node->var_decl.name);
        }
      } else if (!types_compatible(declared, given)) {
        codegen_error(node->var_decl.value, "Cannot assign %s to %s '%s'",
                      type_name(given), type_name(declared),
                      node->var_decl.name);
      }
    }
    int slot = add_var(node, node->var_decl.name, declared, 0);
    if (declared_kind != NULL) {
      size_t kn = strlen(declared_kind);
      var_type_name[slot] = malloc(kn + 1);
      memcpy(var_type_name[slot], declared_kind, kn);
      var_type_name[slot][kn] = '\0';
    }
    /* num holding a float becomes a float slot (monotonic promotion). */
    if (has_value && declared == TYPE_INT && given == TYPE_FLOAT) {
      var_types[slot] = TYPE_FLOAT;
    }
    if (has_value) {
      if (declared == TYPE_STRUCT || declared == TYPE_CLASS) {
        if (given == TYPE_NULL) {
          fprintf(out, "  mov QWORD [rbp - %d], 0\n", (slot + 1) * 8);
        } else if (node->var_decl.value->type == NODE_NEW ||
                   node->var_decl.value->type == NODE_FUNC_CALL) {
          gen_expression(node->var_decl.value);
          fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
        } else if (node->var_decl.value->type == NODE_IDENTIFIER ||
                   node->var_decl.value->type == NODE_INDEX) {
          /* instance sources bind by copy (class id header preserved) */
          const char *skind = instance_type_of(node->var_decl.value);
          gen_expression(node->var_decl.value);
          gen_instance_copy(skind != NULL ? skind : declared_kind);
          fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
        }
      } else if (var_types[slot] == TYPE_FLOAT && given == TYPE_INT) {
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
    int slot = find_var(node->assignment.name);
    int gslot = -1;
    if (slot < 0) {
      gslot = find_global(node->assignment.name);
      if (gslot >= 0) {
        /* global assignment */
        ValueType ggiven = expr_type(node->assignment.value);
        if (!types_compatible(global_types[gslot], ggiven)) {
          codegen_error(node->assignment.value, "Cannot assign %s to %s '%s'",
                        type_name(ggiven), type_name(global_types[gslot]),
                        node->assignment.name);
        }
        if (global_types[gslot] == TYPE_INT && ggiven == TYPE_FLOAT) {
          gen_expression(node->assignment.value);
          global_types[gslot] = TYPE_FLOAT;
          fprintf(out, "  mov [rel global_%s], rax\n", node->assignment.name);
          break;
        }
        if (global_types[gslot] == TYPE_BOOL && ggiven == TYPE_FLOAT) {
          gen_expression(node->assignment.value);
          fprintf(out, "  movq xmm0, rax\n");
          fprintf(out, "  cvttsd2si rax, xmm0\n");
          fprintf(out, "  mov [rel global_%s], rax\n", node->assignment.name);
          break;
        }
        char rmw_addr[96];
        snprintf(rmw_addr, sizeof(rmw_addr), "[rel global_%s]",
                 node->assignment.name);
        if ((global_types[gslot] == TYPE_INT ||
             global_types[gslot] == TYPE_BOOL) &&
            (ggiven == TYPE_INT || ggiven == TYPE_BOOL) &&
            try_assign_rmw(node->assignment.name, node->assignment.value,
                           rmw_addr, 1)) {
          break;
        }
        gen_expression(node->assignment.value);
        if (global_types[gslot] == TYPE_FLOAT &&
            (ggiven == TYPE_INT || ggiven == TYPE_BOOL)) {
          fprintf(out, "  cvtsi2sd xmm0, rax\n");
          fprintf(out, "  movq rax, xmm0\n");
        }
        fprintf(out, "  mov [rel global_%s], rax\n", node->assignment.name);
        break;
      }
      codegen_error(node, "Variable '%s' not declared", node->assignment.name);
    }
    if (var_is_param[slot] && !no_param_warn &&
        var_types[slot] != TYPE_STRUCT && var_types[slot] != TYPE_CLASS) {
      codegen_warning(node, "Parameter '%s' should be accessed as self.%s",
                      node->assignment.name, node->assignment.name);
    }
    var_used[slot] = 1;
    ValueType given = expr_type(node->assignment.value);
    if (var_types[slot] == TYPE_STRUCT || var_types[slot] == TYPE_CLASS) {
      const char *kind = var_type_name[slot];
      if (given == TYPE_NULL) {
        fprintf(out, "  mov QWORD [rbp - %d], 0\n", (slot + 1) * 8);
      } else if (node->assignment.value->type == NODE_NEW) {
        if (!is_subclass(node->assignment.value->new_expr.type_name, kind)) {
          codegen_error(node->assignment.value, "Cannot assign %s to %s '%s'",
                        node->assignment.value->new_expr.type_name, kind,
                        node->assignment.name);
        }
        gen_expression(node->assignment.value);
        fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
      } else if (given == var_types[slot] &&
                 (node->assignment.value->type == NODE_IDENTIFIER ||
                  node->assignment.value->type == NODE_INDEX)) {
        /* instance assignment rebinds to a fresh copy of the source */
        const char *given_kind = instance_type_of(node->assignment.value);
        if (given_kind != NULL && !is_subclass(given_kind, kind)) {
          codegen_error(node->assignment.value, "Cannot assign %s to %s '%s'",
                        given_kind, kind, node->assignment.name);
        }
        gen_expression(node->assignment.value);
        gen_instance_copy(given_kind != NULL ? given_kind : kind);
        fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
      } else if (given == TYPE_INT &&
                 node->assignment.value->type == NODE_FUNC_CALL) {
        gen_expression(node->assignment.value);
        fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
      } else {
        codegen_error(node->assignment.value, "Cannot assign %s to %s '%s'",
                      type_name(given), kind, node->assignment.name);
      }
      break;
    }
    if (!types_compatible(var_types[slot], given)) {
      codegen_error(node->assignment.value, "Cannot assign %s to %s '%s'",
                    type_name(given), type_name(var_types[slot]),
                    node->assignment.name);
    }
    /* Generate the RHS while the slot has its old type so self-references
       (n = n * 1.5) still read the int-typed slot, then promote. */
    if (var_types[slot] == TYPE_INT && given == TYPE_FLOAT) {
      gen_expression(node->assignment.value);
      var_types[slot] = TYPE_FLOAT;
      fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
      break;
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
      char rmw_addr[64];
      snprintf(rmw_addr, sizeof(rmw_addr), "[rbp - %d]", (slot + 1) * 8);
      if (var_types[slot] == TYPE_FLOAT || given == TYPE_FLOAT ||
          !try_assign_rmw(node->assignment.name, node->assignment.value,
                          rmw_addr, 0)) {
        gen_expression(node->assignment.value);
        fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
      }
    }
    break;
  }
  case NODE_ADD_ASSIGN: {
    int slot = require_var(node, node->add_assign.name);
    if (slot_is_global(slot)) {
      int gi = slot_to_global(slot);
      ValueType ggiven = peek_type(node->add_assign.value);
      /* Validate (also catches undeclared vars on error path). */
      {
        ValueType checked = expr_type(node->add_assign.value);
        (void)checked;
      }
      if (global_types[gi] == TYPE_STRING) {
        fprintf(out, "  mov rax, [rel global_%s]\n", node->add_assign.name);
        gen_right_operand_rbx(node->add_assign.value);
        gen_string_concat();
        fprintf(out, "  mov [rel global_%s], rax\n", node->add_assign.name);
        break;
      }
      if (!is_numeric(global_types[gi])) {
        codegen_error(node, "Cannot use += on non-numeric type '%s'",
                      type_name(global_types[gi]));
      }
      int g_is_float = (global_types[gi] == TYPE_FLOAT);
      int gv_is_float = (ggiven == TYPE_FLOAT);
      if (global_types[gi] == TYPE_BOOL && gv_is_float) {
        gen_expression(node->add_assign.value);
        fprintf(out, "  movq xmm0, rax\n");
        fprintf(out, "  cvttsd2si rax, xmm0\n");
        fprintf(out, "  add [rel global_%s], rax\n", node->add_assign.name);
        fprintf(out, "  jo overflow_trap\n");
        break;
      }
      if (!g_is_float && !gv_is_float) {
        fprintf(out, "  mov rax, [rel global_%s]\n", node->add_assign.name);
        gen_right_operand_rbx(node->add_assign.value);
        fprintf(out, "  add rax, rbx\n");
        fprintf(out, "  jo overflow_trap\n");
        fprintf(out, "  mov [rel global_%s], rax\n", node->add_assign.name);
        break;
      }
      /* Float path. */
      gen_expression(node->add_assign.value);
      if (global_types[gi] == TYPE_INT) {
        global_types[gi] = TYPE_FLOAT;
      }
      if (gv_is_float) {
        fprintf(out, "  movq xmm1, rax\n");
      } else {
        fprintf(out, "  cvtsi2sd xmm1, rax\n");
      }
      if (g_is_float) {
        fprintf(out, "  movq xmm0, [rel global_%s]\n", node->add_assign.name);
      } else {
        fprintf(out, "  mov rax, [rel global_%s]\n", node->add_assign.name);
        fprintf(out, "  cvtsi2sd xmm0, rax\n");
      }
      fprintf(out, "  addsd xmm0, xmm1\n");
      fprintf(out, "  movq rax, xmm0\n");
      fprintf(out, "  mov [rel global_%s], rax\n", node->add_assign.name);
      break;
    }
    if (var_types[slot] == TYPE_STRING) {
      ValueType sgiven = expr_type(node->add_assign.value);
      if (sgiven != TYPE_STRING) {
        codegen_error(node->add_assign.value,
                      "Cannot concatenate string with %s", type_name(sgiven));
      }
      fprintf(out, "  mov rax, [rbp - %d]\n", (slot + 1) * 8);
      gen_right_operand_rbx(node->add_assign.value);
      gen_string_concat();
      fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
      break;
    }
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
    gen_expression(node->add_assign.value);
    if (var_types[slot] == TYPE_INT) {
      var_types[slot] = TYPE_FLOAT;
    }
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
    if (slot_is_global(slot)) {
      int gi = slot_to_global(slot);
      ValueType ggiven = peek_type(node->sub_assign.value);
      {
        ValueType checked = expr_type(node->sub_assign.value);
        (void)checked;
      }
      if (global_types[gi] == TYPE_STRING) {
        codegen_error(node, "Operator '-=' cannot be applied to str");
      }
      if (!is_numeric(global_types[gi])) {
        codegen_error(node, "Cannot use -= on non-numeric type '%s'",
                      type_name(global_types[gi]));
      }
      int g_is_float = (global_types[gi] == TYPE_FLOAT);
      int gv_is_float = (ggiven == TYPE_FLOAT);
      if (global_types[gi] == TYPE_BOOL && gv_is_float) {
        gen_expression(node->sub_assign.value);
        fprintf(out, "  movq xmm0, rax\n");
        fprintf(out, "  cvttsd2si rbx, xmm0\n");
        fprintf(out, "  mov rax, [rel global_%s]\n", node->sub_assign.name);
        fprintf(out, "  sub rax, rbx\n");
        fprintf(out, "  jo overflow_trap\n");
        fprintf(out, "  mov [rel global_%s], rax\n", node->sub_assign.name);
        break;
      }
      if (!g_is_float && !gv_is_float) {
        gen_expression(node->sub_assign.value);
        fprintf(out, "  mov rbx, rax\n");
        fprintf(out, "  mov rax, [rel global_%s]\n", node->sub_assign.name);
        fprintf(out, "  sub rax, rbx\n");
        fprintf(out, "  jo overflow_trap\n");
        fprintf(out, "  mov [rel global_%s], rax\n", node->sub_assign.name);
        break;
      }
      /* Float path. */
      gen_expression(node->sub_assign.value);
      if (global_types[gi] == TYPE_INT) {
        global_types[gi] = TYPE_FLOAT;
      }
      if (gv_is_float) {
        fprintf(out, "  movq xmm1, rax\n");
      } else {
        fprintf(out, "  cvtsi2sd xmm1, rax\n");
      }
      if (g_is_float) {
        fprintf(out, "  movq xmm0, [rel global_%s]\n", node->sub_assign.name);
      } else {
        fprintf(out, "  mov rax, [rel global_%s]\n", node->sub_assign.name);
        fprintf(out, "  cvtsi2sd xmm0, rax\n");
      }
      fprintf(out, "  subsd xmm0, xmm1\n");
      fprintf(out, "  movq rax, xmm0\n");
      fprintf(out, "  mov [rel global_%s], rax\n", node->sub_assign.name);
      break;
    }
    if (var_types[slot] == TYPE_STRING) {
      codegen_error(node, "Operator '-=' cannot be applied to str");
    }
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
    gen_expression(node->sub_assign.value);
    if (var_types[slot] == TYPE_INT) {
      var_types[slot] = TYPE_FLOAT;
    }
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
  case NODE_INDEX_ASSIGN: {
    Node *base = node->index_assign.base;
    Node *index = node->index_assign.index;
    Node *value = node->index_assign.value;
    const char *op = node->index_assign.op;
    if (base->type != NODE_IDENTIFIER) {
      codegen_error(base, "Cannot index-assign a non-variable base");
    }
    int slot = require_var(node, base->identifier.name);
    int arr = find_array(base->identifier.name);
    if (arr < 0 || var_types[slot] != TYPE_ARRAY) {
      codegen_error(node, "Can only assign into an array");
    }
    ValueType elem = array_elem[arr];
    const char *ekind = array_kind[arr];
    if (elem == TYPE_NULL) {
      elem = TYPE_INT;
    }
    var_used[slot] = 1;
    ValueType idx_t = peek_type(index);
    if (!is_numeric(idx_t)) {
      ValueType checked = expr_type(index);
      codegen_error(index, "Index must be a number, got %s",
                    type_name(checked));
    }
    if (strcmp(op, "+=") == 0 || strcmp(op, "-=") == 0) {
      if (elem == TYPE_STRUCT || elem == TYPE_CLASS) {
        codegen_error(node, "Can only use '=' on struct/class array elements");
      }
    }
    /* compute the slot address, keep it above the saved pointer */
    gen_expression(base);
    fprintf(out, "  push rax\n");
    gen_expression(index);
    if (idx_t == TYPE_FLOAT) {
      fprintf(out, "  movq xmm0, rax\n");
      fprintf(out, "  cvttsd2si rax, xmm0\n");
    } else if (idx_t == TYPE_BOOL) {
      fprintf(out, "  movzx rax, al\n");
    }
    fprintf(out, "  mov rdx, [rsp]\n");
    fprintf(out, "  test rdx, rdx\n");
    fprintf(out, "  jz null_trap\n");
    fprintf(out, "  mov rcx, [rdx]\n");
    fprintf(out, "  cmp rax, 0\n");
    fprintf(out, "  jl index_trap\n");
    fprintf(out, "  cmp rax, rcx\n");
    fprintf(out, "  jae index_trap\n");
    fprintf(out, "  lea rcx, [rdx + rax*8 + 8]\n");
    fprintf(out, "  mov [rsp], rcx\n");
    if (strcmp(op, "=") == 0) {
      ValueType given = expr_type(value);
      if (elem == TYPE_FLOAT) {
        if (!is_numeric(given)) {
          codegen_error(value, "Cannot assign %s to array element",
                        type_name(given));
        }
        gen_expression(value);
        if (given == TYPE_INT || given == TYPE_BOOL) {
          fprintf(out, "  cvtsi2sd xmm0, rax\n");
          fprintf(out, "  movq rax, xmm0\n");
        }
      } else if (elem == TYPE_INT) {
        if (given != TYPE_INT && given != TYPE_BOOL) {
          codegen_error(value, "Cannot assign %s to num array element",
                        type_name(given));
        }
        gen_expression(value);
        if (given == TYPE_BOOL) {
          fprintf(out, "  movzx rax, al\n");
        }
      } else if (elem == TYPE_STRING) {
        if (given == TYPE_NULL) {
          fprintf(out, "  xor eax, eax\n");
        } else {
          if (given != TYPE_STRING) {
            codegen_error(value, "Cannot assign %s to str array element",
                          type_name(given));
          }
          gen_expression(value);
        }
      } else if (elem == TYPE_STRUCT || elem == TYPE_CLASS) {
        if (given == TYPE_NULL) {
          fprintf(out, "  xor eax, eax\n");
        } else if (given == elem) {
          if (ekind != NULL) {
            const char *vk = instance_type_of(value);
            if (vk != NULL && strcmp(vk, ekind) != 0) {
              codegen_error(value, "Cannot assign %s to %s array element", vk,
                            ekind);
            }
          }
          gen_expression(value);
          if (ekind != NULL &&
              (value->type == NODE_IDENTIFIER || value->type == NODE_INDEX)) {
            /* arr[i] = q binds by copy (like struct assignment) */
            gen_instance_copy(ekind);
          }
        } else {
          codegen_error(value, "Cannot assign %s to %s array element",
                        type_name(given),
                        ekind != NULL ? ekind : type_name(elem));
        }
      }
      fprintf(out, "  mov rdx, [rsp]\n");
      fprintf(out, "  mov [rdx], rax\n");
      fprintf(out, "  add rsp, 8\n");
    } else if (elem == TYPE_STRING) {
      /* arr[i] += suffix: element concat (matches str +=) */
      if (strcmp(op, "-=") == 0) {
        codegen_error(node, "Operator '-=' cannot be applied to str");
      }
      ValueType given = expr_type(value);
      if (given != TYPE_STRING) {
        codegen_error(value, "Cannot concatenate string with %s",
                      type_name(given));
      }
      fprintf(out, "  mov rdx, [rsp]\n");
      fprintf(out, "  mov rax, [rdx]\n");
      fprintf(out, "  push rax\n");
      gen_expression(value);
      fprintf(out, "  mov rbx, rax\n");
      fprintf(out, "  pop rax\n");
      gen_string_concat();
      fprintf(out, "  mov rdx, [rsp]\n");
      fprintf(out, "  mov [rdx], rax\n");
      fprintf(out, "  add rsp, 8\n");
    } else {
      /* numeric += / -= */
      if (!is_numeric(elem)) {
        codegen_error(node, "Operator '%s' needs a numeric array element", op);
      }
      ValueType given = expr_type(value);
      if (!is_numeric(given)) {
        codegen_error(value, "Operator '%s' needs a numeric value, got %s", op,
                      type_name(given));
      }
      if (elem == TYPE_FLOAT) {
        gen_expression(value);
        if (given == TYPE_FLOAT) {
          fprintf(out, "  movq xmm1, rax\n");
        } else {
          fprintf(out, "  cvtsi2sd xmm1, rax\n");
        }
        fprintf(out, "  mov rdx, [rsp]\n");
        fprintf(out, "  movq xmm0, [rdx]\n");
        if (strcmp(op, "+=") == 0) {
          fprintf(out, "  addsd xmm0, xmm1\n");
        } else {
          fprintf(out, "  subsd xmm0, xmm1\n");
        }
        fprintf(out, "  movq rax, xmm0\n");
        fprintf(out, "  mov [rdx], rax\n");
        fprintf(out, "  add rsp, 8\n");
      } else {
        if (given == TYPE_FLOAT) {
          codegen_error(value, "Cannot apply '%s' with float to int element",
                        op);
        }
        gen_expression(value);
        if (given == TYPE_BOOL) {
          fprintf(out, "  movzx rax, al\n");
        }
        fprintf(out, "  mov rdx, [rsp]\n");
        fprintf(out, "  mov rcx, [rdx]\n");
        if (strcmp(op, "+=") == 0) {
          fprintf(out, "  add rax, rcx\n");
        } else {
          fprintf(out, "  sub rcx, rax\n");
          fprintf(out, "  mov rax, rcx\n");
        }
        fprintf(out, "  jo overflow_trap\n");
        fprintf(out, "  mov [rdx], rax\n");
        fprintf(out, "  add rsp, 8\n");
      }
    }
    break;
  }
  case NODE_MEMBER_ASSIGN: {
    const char *object = node->member_assign.object;
    const char *member = node->member_assign.member;
    const char *op = node->member_assign.op;
    Node *value = node->member_assign.value;
    int is_param_target = 0;
    int target_slot = -1;
    int field_off = -1;
    ValueType field_type = TYPE_INT;
    const char *field_kind = NULL;
    int this_field = 0;
    int expr_target = (node->member_assign.object_expr != NULL);
    Node *index_base = node->member_assign.object_expr;
    if (expr_target) {
      const char *kind = instance_type_of(index_base);
      if (kind == NULL) {
        codegen_error(index_base,
                      "Can only assign fields on a struct or class instance");
      }
      FieldDef *def = NULL;
      int f = find_instance_field(kind, member, &def);
      if (f < 0) {
        codegen_error(node, "'%s' has no field '%s'", kind, member);
      }
      field_off = f * 8;
      field_type = def != NULL ? def->type : TYPE_INT;
      field_kind = def != NULL ? def->kind : NULL;
      if (index_base->type == NODE_IDENTIFIER) {
        int aslot = find_var(index_base->identifier.name);
        if (aslot >= 0) {
          var_used[aslot] = 1;
        }
      }
    } else if (strcmp(object, "self") == 0) {
      int pslot = find_var(member);
      if (pslot >= 0 && var_is_param[pslot]) {
        is_param_target = 1;
        target_slot = pslot;
      } else if (in_method && cur_class >= 0 &&
                 find_class_field(cur_class, member) >= 0) {
        int f = find_class_field(cur_class, member);
        this_field = 1;
        field_off = f * 8;
        field_type = class_defs[cur_class].fields[f].type;
        field_kind = class_defs[cur_class].fields[f].kind;
        var_used[find_var("this ")] = 1;
      } else if (in_method && cur_class >= 0) {
        codegen_error(node, "'%s' is not a parameter or field of class '%s'",
                      member, class_defs[cur_class].name);
      } else {
        int pslot2 = find_var(member);
        if (pslot2 >= 0 && var_is_param[pslot2]) {
          is_param_target = 1;
          target_slot = pslot2;
        } else {
          require_param(node, member);
        }
      }
    } else {
      if (find_struct(object) >= 0 || find_class(object) >= 0) {
        codegen_error(node, "Cannot use type '%s' as a value, use an instance",
                      object);
      }
      int slot = find_var(object);
      if (slot < 0) {
        codegen_error(node, "Variable '%s' not declared", object);
      }
      var_used[slot] = 1;
      if (var_types[slot] == TYPE_STRUCT) {
        int s = find_struct(var_type_name[slot]);
        int f = find_struct_field(s, member);
        if (f < 0) {
          codegen_error(node, "Struct '%s' has no field '%s'",
                        var_type_name[slot], member);
        }
        target_slot = slot;
        field_off = f * 8;
        field_type = struct_defs[s].fields[f].type;
        field_kind = struct_defs[s].fields[f].kind;
      } else if (var_types[slot] == TYPE_CLASS) {
        int c = find_class(var_type_name[slot]);
        int f = find_class_field(c, member);
        if (f < 0) {
          codegen_error(node, "Class '%s' has no field '%s'",
                        var_type_name[slot], member);
        }
        target_slot = slot;
        field_off = f * 8;
        field_type = class_defs[c].fields[f].type;
        field_kind = class_defs[c].fields[f].kind;
      } else {
        codegen_error(node, "'%s' is not a struct or class instance", object);
      }
    }
    if (is_param_target) {
      Node tmp;
      memset(&tmp, 0, sizeof(tmp));
      tmp.line = node->line;
      tmp.col = node->col;
      tmp.width = node->width;
      if (strcmp(op, "=") == 0) {
        tmp.type = NODE_ASSIGNMENT;
        tmp.assignment.name = (char *)member;
        tmp.assignment.value = value;
      } else if (strcmp(op, "+=") == 0) {
        tmp.type = NODE_ADD_ASSIGN;
        tmp.add_assign.name = (char *)member;
        tmp.add_assign.value = value;
      } else {
        tmp.type = NODE_SUB_ASSIGN;
        tmp.sub_assign.name = (char *)member;
        tmp.sub_assign.value = value;
      }
      int saved = no_param_warn;
      no_param_warn = 1;
      gen_statement(&tmp);
      no_param_warn = saved;
      break;
    }
    ValueType given = peek_type(value);
    {
      ValueType checked = expr_type(value);
      (void)checked;
    }
    if (field_type == TYPE_STRING && strcmp(op, "-=") == 0) {
      codegen_error(node, "Operator '-=' cannot be applied to str");
    }
    if (field_type == TYPE_STRING && given != TYPE_STRING) {
      if (strcmp(op, "=") == 0) {
        codegen_error(value, "Cannot assign %s to str field '%s'",
                      type_name(given), member);
      }
      codegen_error(value, "Cannot concatenate string with %s",
                    type_name(given));
    }
    if (field_type == TYPE_STRUCT || field_type == TYPE_CLASS) {
      if (strcmp(op, "=") != 0) {
        codegen_error(node,
                      "Operator '%s' cannot be applied to instance fields", op);
      }
      if (given != TYPE_NULL) {
        if (given != field_type) {
          codegen_error(value, "Cannot assign %s to %s field '%s'",
                        type_name(given),
                        field_kind != NULL ? field_kind : "instance", member);
        } else if (field_kind != NULL) {
          const char *ak = instance_type_of(value);
          if (ak != NULL && !is_subclass(ak, field_kind)) {
            codegen_error(value, "Cannot assign %s to %s field '%s'", ak,
                          field_kind, member);
          }
        }
      }
    } else if (field_type != TYPE_STRING && !is_numeric(given)) {
      codegen_error(value, "Cannot assign %s to %s field '%s'",
                    type_name(given), field_type == TYPE_FLOAT ? "num" : "bool",
                    member);
    }
    if (strcmp(op, "=") == 0) {
      if ((field_type == TYPE_STRUCT || field_type == TYPE_CLASS) &&
          (given == TYPE_STRUCT || given == TYPE_CLASS) &&
          (value->type == NODE_IDENTIFIER || value->type == NODE_INDEX) &&
          field_kind != NULL) {
        /* instance fields bind by copy, like struct assignment */
        const char *skind = instance_type_of(value);
        gen_expression(value);
        gen_instance_copy(skind != NULL ? skind : field_kind);
      } else {
        gen_expression(value);
      }
      fprintf(out, "  push rax\n");
      if (expr_target) {
        gen_expression(index_base);
      } else if (this_field) {
        int tslot = find_var("this ");
        fprintf(out, "  mov rax, [rbp - %d]\n", (tslot + 1) * 8);
      } else {
        fprintf(out, "  mov rax, [rbp - %d]\n", (target_slot + 1) * 8);
      }
      fprintf(out, "  test rax, rax\n");
      fprintf(out, "  jz null_trap\n");
      fprintf(out, "  pop rdx\n");
      if (field_type == TYPE_FLOAT &&
          (given == TYPE_INT || given == TYPE_BOOL)) {
        fprintf(out, "  cvtsi2sd xmm0, rdx\n");
        fprintf(out, "  movq rdx, xmm0\n");
      } else if (field_type == TYPE_BOOL && given == TYPE_FLOAT) {
        fprintf(out, "  movq xmm0, rdx\n");
        fprintf(out, "  cvttsd2si rdx, xmm0\n");
      }
      fprintf(out, "  mov [rax + %d], rdx\n", field_off);
      break;
    }
    if (field_type == TYPE_STRING) {
      if (strcmp(op, "-=") == 0) {
        codegen_error(node, "Operator '-=' cannot be applied to str");
      }
      if (expr_target) {
        gen_expression(index_base);
      } else if (this_field) {
        int tslot = find_var("this ");
        fprintf(out, "  mov rax, [rbp - %d]\n", (tslot + 1) * 8);
      } else {
        fprintf(out, "  mov rax, [rbp - %d]\n", (target_slot + 1) * 8);
      }
      fprintf(out, "  test rax, rax\n");
      fprintf(out, "  jz null_trap\n");
      fprintf(out, "  push rax\n");
      fprintf(out, "  mov rax, [rsp]\n");
      fprintf(out, "  mov rcx, [rax + %d]\n", field_off);
      fprintf(out, "  push rcx\n");
      gen_expression(value);
      fprintf(out, "  mov rbx, rax\n");
      fprintf(out, "  pop rax\n");
      gen_string_concat();
      fprintf(out, "  mov r11, rax\n");
      fprintf(out, "  pop rax\n");
      fprintf(out, "  test rax, rax\n");
      fprintf(out, "  jz null_trap\n");
      fprintf(out, "  mov [rax + %d], r11\n", field_off);
      break;
    }
    if (expr_target) {
      gen_expression(index_base);
    } else if (this_field) {
      int tslot = find_var("this ");
      fprintf(out, "  mov rax, [rbp - %d]\n", (tslot + 1) * 8);
    } else {
      fprintf(out, "  mov rax, [rbp - %d]\n", (target_slot + 1) * 8);
    }
    fprintf(out, "  test rax, rax\n");
    fprintf(out, "  jz null_trap\n");
    fprintf(out, "  push rax\n");
    fprintf(out, "  mov rax, [rsp]\n");
    fprintf(out, "  mov rcx, [rax + %d]\n", field_off);
    fprintf(out, "  push rcx\n");
    gen_expression(value);
    if (field_type == TYPE_FLOAT) {
      if (given == TYPE_FLOAT) {
        fprintf(out, "  movq xmm1, rax\n");
      } else {
        fprintf(out, "  cvtsi2sd xmm1, rax\n");
      }
      fprintf(out, "  pop rcx\n");
      fprintf(out, "  movq xmm0, rcx\n");
      if (strcmp(op, "+=") == 0) {
        fprintf(out, "  addsd xmm0, xmm1\n");
      } else {
        fprintf(out, "  subsd xmm0, xmm1\n");
      }
      fprintf(out, "  movq rcx, xmm0\n");
    } else {
      if (given == TYPE_FLOAT) {
        fprintf(out, "  movq xmm0, rax\n");
        fprintf(out, "  cvttsd2si rbx, xmm0\n");
      } else {
        fprintf(out, "  mov rbx, rax\n");
      }
      fprintf(out, "  pop rcx\n");
      fprintf(out, "  mov rax, rcx\n");
      if (strcmp(op, "+=") == 0) {
        fprintf(out, "  add rax, rbx\n");
      } else {
        fprintf(out, "  sub rax, rbx\n");
      }
      fprintf(out, "  jo overflow_trap\n");
      fprintf(out, "  mov rcx, rax\n");
    }
    fprintf(out, "  pop rax\n");
    fprintf(out, "  test rax, rax\n");
    fprintf(out, "  jz null_trap\n");
    fprintf(out, "  mov [rax + %d], rcx\n", field_off);
    break;
  }
  case NODE_METHOD_CALL:
    gen_method_call(node);
    break;
  case NODE_PRINT: {
    Node *value = node->print_stmt.value;
    if (value != NULL && value->type == NODE_STRING_LITERAL) {
      gen_print_string(value, value->string_literal.value);
    } else if (value != NULL && value->type == NODE_IDENTIFIER) {
      int slot = require_var(value, value->identifier.name);
      if (slot_is_global(slot)) {
        ValueType gt = global_types[slot_to_global(slot)];
        if (gt == TYPE_STRING) {
          fprintf(out, "  lea rcx, [rel fmt_str]\n");
          fprintf(out, "  mov rdx, [rel global_%s]\n", value->identifier.name);
        } else if (gt == TYPE_FLOAT) {
          fprintf(out, "  movsd xmm0, [rel global_%s]\n",
                  value->identifier.name);
          fprintf(out, "  movq rdx, xmm0\n");
          fprintf(out, "  lea rcx, [rel fmt_float]\n");
          fprintf(out, "  movapd xmm1, xmm0\n");
        } else {
          fprintf(out, "  lea rcx, [rel fmt_int]\n");
          fprintf(out, "  mov rdx, [rel global_%s]\n", value->identifier.name);
        }
        fprintf(out, "  sub rsp, 32\n");
        fprintf(out, "  call printf\n");
        fprintf(out, "  add rsp, 32\n");
        break;
      }
      if (var_types[slot] == TYPE_STRUCT || var_types[slot] == TYPE_CLASS) {
        codegen_error(value, "Cannot print %s '%s' directly",
                      var_type_name[slot], value->identifier.name);
      }
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
      if (etype == TYPE_STRUCT || etype == TYPE_CLASS) {
        const char *k = instance_type_of(value);
        if (value->type == NODE_NEW) {
          k = value->new_expr.type_name;
        }
        if (k != NULL) {
          codegen_error(value, "Cannot print %s directly", k);
        }
        codegen_error(value, "Cannot print struct or class values directly");
      }
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
    if (node->return_stmt.value == NULL) {
      if (cur_return_type == NULL || strcmp(cur_return_type, "void") != 0) {
        codegen_error(node,
                      "Bare 'return;' is only allowed in functions declared "
                      "'-> void'");
      }
    } else if (cur_return_type != NULL &&
               strcmp(cur_return_type, "void") == 0) {
      codegen_error(node->return_stmt.value,
                    "Function declared '-> void' cannot return a value");
    }
    if (in_method && cur_class >= 0 && cur_method >= 0) {
      const char *rt = class_defs[cur_class].methods[cur_method].ret_type;
      const char *mname = class_defs[cur_class].methods[cur_method].name;
      ValueType given = peek_type(node->return_stmt.value);
      {
        ValueType checked = expr_type(node->return_stmt.value);
        (void)checked;
      }
      if (strcmp(rt, "str") == 0) {
        if (given != TYPE_STRING) {
          codegen_error(node->return_stmt.value,
                        "Method '%s' must return string, got %s", mname,
                        type_name(given));
        }
        gen_expression(node->return_stmt.value);
      } else if (strcmp(rt, "bool") == 0) {
        if (!is_numeric(given)) {
          codegen_error(node->return_stmt.value,
                        "Method '%s' must return bool, got %s", mname,
                        type_name(given));
        }
        gen_expression(node->return_stmt.value);
        if (given == TYPE_FLOAT) {
          fprintf(out, "  movq xmm0, rax\n");
          fprintf(out, "  cvttsd2si rax, xmm0\n");
        }
      } else {
        if (!is_numeric(given)) {
          codegen_error(node->return_stmt.value,
                        "Method '%s' must return num, got %s", mname,
                        type_name(given));
        }
        gen_expression(node->return_stmt.value);
        if (given == TYPE_INT || given == TYPE_BOOL) {
          fprintf(out, "  cvtsi2sd xmm0, rax\n");
          fprintf(out, "  movq rax, xmm0\n");
        }
      }
    } else if (node->return_stmt.value != NULL) {
      ValueType rgiven = peek_type(node->return_stmt.value);
      gen_expression(node->return_stmt.value);
      /* Float-returning functions normalize int results so every
         caller can read the return slot as a double. */
      if (cur_sig_idx >= 0 && sig_return_float[cur_sig_idx] &&
          (rgiven == TYPE_INT || rgiven == TYPE_BOOL)) {
        fprintf(out, "  cvtsi2sd xmm0, rax\n");
        fprintf(out, "  movq rax, xmm0\n");
      }
    }
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
    if (loop_depth >= MAX_LOOP_DEPTH) {
      codegen_error(node, "Loops nested too deeply");
    }
    loop_break_label[loop_depth] = end_label;
    /* continue re-runs the condition, so it targets the loop head */
    loop_continue_label[loop_depth] = loop_label;
    loop_depth++;
    fprintf(out, "label%d:\n", loop_label);
    gen_condition_jump(node->while_stmt.condition, end_label);
    gen_block(node->while_stmt.body);
    fprintf(out, "  jmp label%d\n", loop_label);
    fprintf(out, "label%d:\n", end_label);
    loop_depth--;
    break;
  }
  case NODE_BREAK: {
    if (loop_depth <= 0) {
      codegen_error(node, "break outside of a loop");
    }
    fprintf(out, "  jmp label%d\n", loop_break_label[loop_depth - 1]);
    break;
  }
  case NODE_CONTINUE: {
    if (loop_depth <= 0) {
      codegen_error(node, "continue outside of a loop");
    }
    fprintf(out, "  jmp label%d\n", loop_continue_label[loop_depth - 1]);
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
    int slot = add_var(node, node->array_decl.name, TYPE_ARRAY, 0);
    if (array_count >= 64) {
      codegen_error(node, "Too many arrays in function");
    }
    size_t nl = strlen(node->array_decl.name);
    array_names[array_count] = malloc(nl + 1);
    memcpy(array_names[array_count], node->array_decl.name, nl);
    array_names[array_count][nl] = '\0';
    array_count++;

    Node *rhs = node->array_decl.elements;
    if (rhs != NULL && rhs->type == NODE_IDENTIFIER) {
      int src = find_array(rhs->identifier.name);
      if (src < 0) {
        codegen_error(rhs, "Cannot initialize 'array' from non-array '%s'",
                      rhs->identifier.name);
      }
      gen_expression(rhs);
      fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
      array_slot[array_count - 1] = slot;
      array_elem[array_count - 1] = array_elem[src];
      array_len[array_count - 1] = array_len[src];
      array_kind[array_count - 1] =
          array_kind[src] ? strdup(array_kind[src]) : NULL;
      break;
    }
    if (rhs == NULL || rhs->type != NODE_ARRAY_LITERAL) {
      codegen_error(node,
                    "Array must be initialized with [...] or another array");
      break;
    }
    int count = 0;
    for (Node *e = rhs->array_literal.elements; e != NULL; e = e->right) {
      count++;
    }
    /* determine a common element category */
    ValueType elem = TYPE_NULL;
    const char *elem_name_kind = NULL;
    int seen = 0, any_float = 0, elem_struct_like = 0;
    for (Node *e = rhs->array_literal.elements; e != NULL; e = e->right) {
      ValueType v = expr_type(e);
      const char *k =
          (v == TYPE_STRUCT || v == TYPE_CLASS) ? instance_type_of(e) : NULL;
      if (!seen) {
        elem = v;
        elem_name_kind = k;
        seen = 1;
        elem_struct_like = (v == TYPE_STRUCT || v == TYPE_CLASS);
        any_float = (v == TYPE_FLOAT);
        continue;
      }
      if (elem_struct_like) {
        if (v != elem || (v != TYPE_STRUCT && v != TYPE_CLASS) ||
            (elem_name_kind != NULL && k != NULL &&
             strcmp(elem_name_kind, k) != 0)) {
          codegen_error(
              e, "Array elements must all have the same struct/class type");
        }
        if (elem_name_kind == NULL && k != NULL) {
          elem_name_kind = k;
        }
      } else if (elem == TYPE_STRING) {
        if (v != TYPE_STRING) {
          codegen_error(e, "Array elements must all be strings");
        }
      } else if (is_numeric(elem) && is_numeric(v)) {
        if (v == TYPE_FLOAT) {
          any_float = 1;
        }
      } else {
        codegen_error(e, "Array elements must all be numbers, all strings, or "
                         "instances of one struct/class");
      }
    }
    if (!seen) {
      /* empty literal: default to generic numeric slot */
      elem = TYPE_INT;
      elem_name_kind = NULL;
    } else if (is_numeric(elem) && any_float) {
      elem = TYPE_FLOAT;
    }

    array_slot[array_count - 1] = slot;
    array_elem[array_count - 1] = elem;
    array_len[array_count - 1] = count;
    array_kind[array_count - 1] =
        elem_name_kind != NULL ? strdup(elem_name_kind) : NULL;

    /* allocate the array block: 8 bytes length + count*8 payload */
    fprintf(out, "  mov rcx, %d\n", (count + 1) * 8);
    gen_runtime_prologue();
    fprintf(out, "  call malloc\n");
    gen_runtime_epilogue();
    fprintf(out, "  test rax, rax\n");
    fprintf(out, "  jz alloc_trap\n");
    fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
    fprintf(out, "  mov rbx, rax\n");
    fprintf(out, "  mov QWORD [rbx], %d\n", count);
    /* struct/class literal elements bind by copy (like struct assignment) */
    long long elem_size = 0;
    if ((elem == TYPE_STRUCT || elem == TYPE_CLASS) &&
        array_kind[array_count - 1] != NULL) {
      elem_size = instance_size(array_kind[array_count - 1]);
    }
    for (Node *e = rhs->array_literal.elements; e != NULL; e = e->right) {
      ValueType vt = peek_type(e);
      if (elem == TYPE_FLOAT) {
        gen_expression(e);
        if (vt == TYPE_INT || vt == TYPE_BOOL) {
          fprintf(out, "  cvtsi2sd xmm0, rax\n");
          fprintf(out, "  movq rax, xmm0\n");
        }
      } else if (elem == TYPE_INT) {
        gen_expression(e);
        if (vt == TYPE_FLOAT) {
          fprintf(out, "  movq xmm0, rax\n");
          fprintf(out, "  cvttsd2si rax, xmm0\n");
        }
      } else {
        gen_expression(e);
        if ((elem == TYPE_STRUCT || elem == TYPE_CLASS) && elem_size > 0 &&
            (e->type == NODE_IDENTIFIER || e->type == NODE_INDEX)) {
          gen_instance_copy(array_kind[array_count - 1]);
        }
      }
      fprintf(out, "  push rax\n");
    }
    fprintf(out, "  mov rbx, [rbp - %d]\n", (slot + 1) * 8);
    for (int i = count - 1; i >= 0; i--) {
      fprintf(out, "  pop rax\n");
      fprintf(out, "  mov [rbx + %d], rax\n", i * 8 + 8);
    }
    break;
  }
  case NODE_FOR: {
    int arr = find_array(node->for_stmt.array_name);
    if (arr < 0) {
      int vslot = find_var(node->for_stmt.array_name);
      if (vslot >= 0) {
        const char *kind =
            (var_types[vslot] == TYPE_STRUCT || var_types[vslot] == TYPE_CLASS)
                ? var_type_name[vslot]
                : type_name(var_types[vslot]);
        codegen_error(node, "Cannot iterate over %s '%s', only arrays can",
                      kind, node->for_stmt.array_name);
      }
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
    ValueType elem_vtype = array_elem[arr];
    if (elem_vtype == TYPE_NULL) {
      elem_vtype = TYPE_INT;
    }
    if (array_slot[arr] >= 0) {
      var_used[array_slot[arr]] = 1;
    }
    int var_slot = add_var(node, node->for_stmt.var_name, elem_vtype, 0);
    if ((elem_vtype == TYPE_STRUCT || elem_vtype == TYPE_CLASS) &&
        array_kind[arr] != NULL) {
      var_type_name[var_slot] = strdup(array_kind[arr]);
    }
    int asl = array_slot[arr];
    int loop_label = label_id++;
    int end_label = label_id++;
    int cont_label = label_id++;
    /* struct/class loop vars bind by copy (null elements stay null) */
    long long loop_copy_size = 0;
    if ((elem_vtype == TYPE_STRUCT || elem_vtype == TYPE_CLASS) &&
        array_kind[arr] != NULL) {
      loop_copy_size = instance_size(array_kind[arr]);
    }
    if (loop_depth >= MAX_LOOP_DEPTH) {
      codegen_error(node, "Loops nested too deeply");
    }
    loop_break_label[loop_depth] = end_label;
    loop_continue_label[loop_depth] = cont_label;
    loop_depth++;
    fprintf(out, "  push r12\n");
    fprintf(out, "  xor r12d, r12d\n");
    fprintf(out, "loop%d:\n", loop_label);
    fprintf(out, "  mov rax, [rbp - %d]\n", (asl + 1) * 8);
    fprintf(out, "  test rax, rax\n");
    fprintf(out, "  jz null_trap\n");
    fprintf(out, "  mov r11, rax\n");
    fprintf(out, "  mov rcx, [r11]\n");
    fprintf(out, "  cmp r12, rcx\n");
    fprintf(out, "  jge label%d\n", end_label);
    fprintf(out, "  mov rax, [r11 + r12*8 + 8]\n");
    if (loop_copy_size > 0) {
      int copy_done = label_id++;
      fprintf(out, "  test rax, rax\n");
      fprintf(out, "  jz label%d\n", copy_done);
      gen_instance_copy(array_kind[arr]);
      fprintf(out, "label%d:\n", copy_done);
    }
    fprintf(out, "  mov [rbp - %d], rax\n", (var_slot + 1) * 8);
    gen_block(node->for_stmt.body);
    fprintf(out, "label%d:\n", cont_label);
    fprintf(out, "  inc r12\n");
    fprintf(out, "  jmp loop%d\n", loop_label);
    fprintf(out, "label%d:\n", end_label);
    fprintf(out, "  pop r12\n");
    loop_depth--;
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
  const char *saved_return_type = cur_return_type;
  int saved_sig_idx = cur_sig_idx;
  var_count = 0;
  array_count = 0;
  in_function = 1;
  cur_return_type = node->function.return_type;

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
  cur_sig_idx = sig_idx;
  for (Node *p = node->function.params; p != NULL; p = p->right) {
    const char *param_name = NULL;
    ValueType param_type = TYPE_INT;
    const char *param_kind = NULL;
    if (p->type == NODE_VAR_DECL) {
      param_name = p->var_decl.name;
      param_type = resolve_decl_type(p->var_decl.var_type, p, &param_kind);
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
    if (param_kind != NULL) {
      size_t kn = strlen(param_kind);
      var_type_name[slot] = malloc(kn + 1);
      memcpy(var_type_name[slot], param_kind, kn);
      var_type_name[slot][kn] = '\0';
    }
    if (param_type == TYPE_ARRAY && array_count < 64) {
      /* array params carry whatever backs them; inside a body,
         for-in and a[i] treat them as variable numeric values */
      size_t pn = strlen(param_name);
      array_names[array_count] = malloc(pn + 1);
      memcpy(array_names[array_count], param_name, pn);
      array_names[array_count][pn] = '\0';
      array_slot[array_count] = slot;
      array_elem[array_count] = TYPE_INT;
      array_len[array_count] = 0;
      array_kind[array_count] = NULL;
      array_count++;
    }
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
  cur_return_type = saved_return_type;
  cur_sig_idx = saved_sig_idx;
}

/**
 * @brief Generates code for a class method body
 * @param class_idx Class index from find_class
 * @param method_idx Method index from find_method
 * @details The instance pointer travels as hidden first argument (rcx)
 * and is spilled to a hidden "this " slot. Inside the body, self.X
 * resolves to a parameter first, then to a class field.
 */
static void gen_method(int class_idx, int method_idx) {
  ClassDef *cd = &class_defs[class_idx];
  MethodDef *md = &cd->methods[method_idx];
  char label[128];
  method_label(cd->name, md->name, label);
  int saved_count = var_count;
  int saved_arrays = array_count;
  int saved_in_function = in_function;
  int saved_in_method = in_method;
  int saved_class = cur_class;
  int saved_method = cur_method;
  const char *saved_return_type = cur_return_type;
  int saved_sig_idx = cur_sig_idx;
  var_count = 0;
  array_count = 0;
  in_function = 1;
  in_method = 1;
  cur_class = class_idx;
  cur_method = method_idx;
  cur_return_type = md->ret_type;

  fprintf(out, "%s:\n", label);
  gen_prologue();

  int this_slot = add_var(NULL, "this ", TYPE_CLASS, 1);
  {
    size_t kn = strlen(cd->name);
    var_type_name[this_slot] = malloc(kn + 1);
    memcpy(var_type_name[this_slot], cd->name, kn);
    var_type_name[this_slot][kn] = '\0';
  }
  fprintf(out, "  mov [rbp - %d], rcx\n", (this_slot + 1) * 8);

  int sig_idx = -1;
  for (int s = 0; s < sig_count; s++) {
    if (strcmp(sig_names[s], label) == 0) {
      sig_idx = s;
      break;
    }
  }
  cur_sig_idx = sig_idx;
  const char *param_regs[4] = {"rcx", "rdx", "r8", "r9"};
  int param_index = 0;
  for (Node *p = md->params; p != NULL; p = p->right) {
    const char *param_name = NULL;
    ValueType param_type = TYPE_INT;
    const char *param_kind = NULL;
    if (p->type == NODE_VAR_DECL) {
      param_name = p->var_decl.name;
      param_type = resolve_decl_type(p->var_decl.var_type, p, &param_kind);
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
    if (find_class_field(class_idx, param_name) >= 0) {
      codegen_warning(p, "Parameter '%s' shadows a field of class '%s'",
                      param_name, cd->name);
    }
    int slot = add_var(p, param_name, param_type, 1);
    if (param_kind != NULL) {
      size_t kn = strlen(param_kind);
      var_type_name[slot] = malloc(kn + 1);
      memcpy(var_type_name[slot], param_kind, kn);
      var_type_name[slot][kn] = '\0';
    }
    int abs_idx = param_index + 1;
    if (abs_idx < 4) {
      fprintf(out, "  mov [rbp - %d], %s\n", (slot + 1) * 8,
              param_regs[abs_idx]);
    } else {
      fprintf(out, "  mov rax, [rbp + %d]\n", 48 + 8 * (abs_idx - 4));
      fprintf(out, "  mov [rbp - %d], rax\n", (slot + 1) * 8);
    }
    param_index++;
  }

  gen_block(md->body);

  fprintf(out, "  mov rsp, rbp\n");
  fprintf(out, "  pop rbp\n");
  fprintf(out, "  ret\n");

  var_count = saved_count;
  array_count = saved_arrays;
  in_function = saved_in_function;
  in_method = saved_in_method;
  cur_class = saved_class;
  cur_method = saved_method;
  cur_return_type = saved_return_type;
  cur_sig_idx = saved_sig_idx;
}

/**
 * @brief Generates NASM x86-64 assembly for a Jot program
 * @param root Root of the AST (linked list of top level statements)
 * @param filename Output assembly file path
 * @details Entry point main runs the top level statements only.
 * Functions (including fn main, emitted as jot_main) run when called.
 */
/**
 * @brief Resolves one declared field type to a slot category
 * @param field Field slot to fill in
 * @param declared Declared type text (num, bool, str, or a type name)
 * @param fault Field declaration node for diagnostics
 * @details Runs after every struct and class name is registered, so nested
 * fields may be declared before their type. Instance fields hold a pointer,
 * which keeps a self-referential field legal (no infinite size).
 */
static void resolve_field_type(FieldDef *field, const char *declared,
                               Node *fault) {
  if (strcmp(declared, "num") == 0) {
    field->type = TYPE_FLOAT;
    field->kind = NULL;
    return;
  }
  if (strcmp(declared, "bool") == 0) {
    field->type = TYPE_BOOL;
    field->kind = NULL;
    return;
  }
  if (strcmp(declared, "str") == 0) {
    field->type = TYPE_STRING;
    field->kind = NULL;
    return;
  }
  int s = find_struct(declared);
  if (s >= 0) {
    field->type = TYPE_STRUCT;
    field->kind = struct_defs[s].name;
    return;
  }
  int c = find_class(declared);
  if (c >= 0) {
    field->type = TYPE_CLASS;
    field->kind = class_defs[c].name;
    return;
  }
  collect_error(fault, "Unknown field type '%s'", declared);
}

/**
 * @brief Counts a method's parameters
 * @param params Parameter list head (may be NULL)
 * @return Parameter count
 */
static int count_params(Node *params) {
  int n = 0;
  for (Node *p = params; p != NULL; p = p->right) {
    n++;
  }
  return n;
}

/**
 * @brief Merges a class's fields with its base class (base fields first)
 * @param c Class index to flatten
 * @details Base fields keep their offsets, so base-typed references stay
 * layout-compatible with derived instances. Method overrides are checked
 * here; method lookup walks the chain instead of merging lists.
 */
static void flatten_class(int c) {
  ClassDef *d = &class_defs[c];
  if (d->flattened) {
    return;
  }
  if (d->base_idx < 0) {
    d->flattened = 1;
    return;
  }
  if (d->visiting) {
    collect_error(d->decl, "Inheritance cycle through class '%s'", d->name);
    d->base_idx = -1;
    d->flattened = 1;
    return;
  }
  d->visiting = 1;
  flatten_class(d->base_idx);
  d->visiting = 0;
  if (d->flattened) {
    return;
  }
  ClassDef *b = &class_defs[d->base_idx];

  for (int m = 0; m < d->nmethods; m++) {
    int owner = -1;
    int bm = find_method(d->base_idx, d->methods[m].name, &owner);
    if (bm >= 0 && count_params(d->methods[m].params) !=
                       count_params(class_defs[owner].methods[bm].params)) {
      codegen_warning(d->methods[m].decl,
                      "Method '%s' overrides with different parameters",
                      d->methods[m].name);
    }
  }

  for (int f = 0; f < d->nfields; f++) {
    for (int bf = 0; bf < b->nfields; bf++) {
      if (strcmp(d->fields[f].name, b->fields[bf].name) == 0) {
        collect_error(d->decl,
                      "Field '%s' in class '%s' hides a base class field",
                      d->fields[f].name, d->name);
      }
    }
  }
  if (d->nfields + b->nfields > MAX_FIELDS) {
    collect_error(d->decl, "Too many fields in class '%s' with base '%s'",
                  d->name, b->name);
    d->flattened = 1;
    return;
  }
  memmove(&d->fields[b->nfields], &d->fields[0],
          (size_t)d->nfields * sizeof(FieldDef));
  memcpy(&d->fields[0], b->fields, (size_t)b->nfields * sizeof(FieldDef));
  d->nfields += b->nfields;
  d->flattened = 1;
}

void GenerateAssembly(Node *root, const char *source, const char *output) {
  codegen_source = source;
  label_id = 0;
  string_count = 0;
  var_count = 0;
  frame_size = 2048;
  in_function = 0;
  in_method = 0;
  cur_return_type = NULL;
  cur_sig_idx = -1;
  loop_depth = 0;
  cur_class = -1;
  cur_method = -1;
  no_param_warn = 0;
  has_user_main = 0;

  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_FUNCTION && strcmp(s->function.name, "main") == 0) {
      has_user_main = 1;
      break;
    }
  }

  global_count = 0;
  sig_count = 0;
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_FUNCTION && sig_count < 256) {
      size_t len = strlen(s->function.name);
      sig_names[sig_count] = malloc(len + 1);
      memcpy(sig_names[sig_count], s->function.name, len);
      sig_names[sig_count][len] = '\0';
      sig_params[sig_count] = s->function.params;
      sig_return_type[sig_count] = s->function.return_type;
      sig_count++;
    }
  }
  struct_count = 0;
  class_count = 0;
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_STRUCT_DEF) {
      for (int i = 0; i < sig_count; i++) {
        if (strcmp(sig_names[i], s->struct_def.name) == 0) {
          collect_error(s, "Struct '%s' is already defined",
                        s->struct_def.name);
        }
      }
      for (int i = 0; i < struct_count; i++) {
        if (strcmp(struct_defs[i].name, s->struct_def.name) == 0) {
          collect_error(s, "Struct '%s' is already defined",
                        s->struct_def.name);
        }
      }
      for (int i = 0; i < class_count; i++) {
        if (strcmp(class_defs[i].name, s->struct_def.name) == 0) {
          collect_error(s, "Struct '%s' is already defined",
                        s->struct_def.name);
        }
      }
      if (struct_count >= MAX_STRUCTS) {
        collect_error(s, "Too many structs defined");
      }
      StructDef *d = &struct_defs[struct_count];
      size_t len = strlen(s->struct_def.name);
      d->name = malloc(len + 1);
      memcpy(d->name, s->struct_def.name, len);
      d->name[len] = '\0';
      d->is_public = s->struct_def.is_public;
      d->nfields = 0;
      for (Node *f = s->struct_def.fields; f != NULL; f = f->right) {
        if (d->nfields >= MAX_FIELDS) {
          collect_error(f, "Too many fields in struct '%s'", d->name);
        }
        size_t fl = strlen(f->var_decl.name);
        d->fields[d->nfields].name = malloc(fl + 1);
        memcpy(d->fields[d->nfields].name, f->var_decl.name, fl);
        d->fields[d->nfields].name[fl] = '\0';
        if (strcmp(f->var_decl.var_type, "str") == 0) {
          d->fields[d->nfields].type = TYPE_STRING;
        } else if (strcmp(f->var_decl.var_type, "bool") == 0) {
          d->fields[d->nfields].type = TYPE_BOOL;
        } else if (strcmp(f->var_decl.var_type, "num") == 0) {
          d->fields[d->nfields].type = TYPE_FLOAT;
        } else {
          d->fields[d->nfields].type = TYPE_FLOAT;
        }
        d->fields[d->nfields].kind = NULL;
        {
          size_t tl = strlen(f->var_decl.var_type);
          d->fields[d->nfields].decl_type = malloc(tl + 1);
          memcpy(d->fields[d->nfields].decl_type, f->var_decl.var_type, tl);
          d->fields[d->nfields].decl_type[tl] = '\0';
        }
        d->nfields++;
      }
      struct_count++;
    } else if (s->type == NODE_CLASS_DEF) {
      for (int i = 0; i < sig_count; i++) {
        if (strcmp(sig_names[i], s->class_def.name) == 0) {
          collect_error(s, "Class '%s' is already defined", s->class_def.name);
        }
      }
      for (int i = 0; i < struct_count; i++) {
        if (strcmp(struct_defs[i].name, s->class_def.name) == 0) {
          collect_error(s, "Class '%s' is already defined", s->class_def.name);
        }
      }
      for (int i = 0; i < class_count; i++) {
        if (strcmp(class_defs[i].name, s->class_def.name) == 0) {
          collect_error(s, "Class '%s' is already defined", s->class_def.name);
        }
      }
      if (class_count >= MAX_CLASSES) {
        collect_error(s, "Too many classes defined");
      }
      ClassDef *d = &class_defs[class_count];
      size_t len = strlen(s->class_def.name);
      d->name = malloc(len + 1);
      memcpy(d->name, s->class_def.name, len);
      d->name[len] = '\0';
      d->decl = s;
      if (s->class_def.base != NULL) {
        len = strlen(s->class_def.base);
        d->base = malloc(len + 1);
        memcpy(d->base, s->class_def.base, len);
        d->base[len] = '\0';
      } else {
        d->base = NULL;
      }
      d->base_idx = -1;
      d->flattened = 0;
      d->visiting = 0;
      d->class_id = class_count;
      d->is_public = s->class_def.is_public;
      d->nfields = 0;
      for (Node *f = s->class_def.fields; f != NULL; f = f->right) {
        if (d->nfields >= MAX_FIELDS) {
          collect_error(f, "Too many fields in class '%s'", d->name);
        }
        size_t fl = strlen(f->var_decl.name);
        d->fields[d->nfields].name = malloc(fl + 1);
        memcpy(d->fields[d->nfields].name, f->var_decl.name, fl);
        d->fields[d->nfields].name[fl] = '\0';
        if (strcmp(f->var_decl.var_type, "str") == 0) {
          d->fields[d->nfields].type = TYPE_STRING;
        } else if (strcmp(f->var_decl.var_type, "bool") == 0) {
          d->fields[d->nfields].type = TYPE_BOOL;
        } else if (strcmp(f->var_decl.var_type, "num") == 0) {
          d->fields[d->nfields].type = TYPE_FLOAT;
        } else {
          d->fields[d->nfields].type = TYPE_FLOAT;
        }
        d->fields[d->nfields].kind = NULL;
        {
          size_t tl = strlen(f->var_decl.var_type);
          d->fields[d->nfields].decl_type = malloc(tl + 1);
          memcpy(d->fields[d->nfields].decl_type, f->var_decl.var_type, tl);
          d->fields[d->nfields].decl_type[tl] = '\0';
        }
        d->nfields++;
      }
      d->nmethods = 0;
      for (Node *m = s->class_def.methods; m != NULL; m = m->right) {
        if (d->nmethods >= MAX_METHODS) {
          collect_error(m, "Too many methods in class '%s'", d->name);
        }
        if (strcmp(m->method_def.ret_type, "num") != 0 &&
            strcmp(m->method_def.ret_type, "bool") != 0 &&
            strcmp(m->method_def.ret_type, "str") != 0) {
          collect_error(m, "Method '%s' must return num, bool or str",
                        m->method_def.name);
        }
        size_t ml = strlen(m->method_def.name);
        d->methods[d->nmethods].name = malloc(ml + 1);
        memcpy(d->methods[d->nmethods].name, m->method_def.name, ml);
        d->methods[d->nmethods].name[ml] = '\0';
        size_t rl = strlen(m->method_def.ret_type);
        d->methods[d->nmethods].ret_type = malloc(rl + 1);
        memcpy(d->methods[d->nmethods].ret_type, m->method_def.ret_type, rl);
        d->methods[d->nmethods].ret_type[rl] = '\0';
        d->methods[d->nmethods].params = m->method_def.params;
        d->methods[d->nmethods].body = m->method_def.body;
        d->methods[d->nmethods].line = m->line;
        d->methods[d->nmethods].col = m->col;
        d->methods[d->nmethods].decl = m;
        d->nmethods++;
        if (sig_count < 256) {
          char label[128];
          method_label(d->name, m->method_def.name, label);
          size_t ll = strlen(label);
          sig_names[sig_count] = malloc(ll + 1);
          memcpy(sig_names[sig_count], label, ll);
          sig_names[sig_count][ll] = '\0';
          sig_params[sig_count] = m->method_def.params;
          sig_count++;
        }
      }
      class_count++;
    }
  }
  /* Second pass: resolve field types now that every struct/class name is
     known (nested fields may be declared before their type). */
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_STRUCT_DEF) {
      int si = -1;
      for (int i = 0; i < struct_count; i++) {
        if (strcmp(struct_defs[i].name, s->struct_def.name) == 0) {
          si = i;
          break;
        }
      }
      if (si < 0) {
        continue;
      }
      int fi = 0;
      for (Node *f = s->struct_def.fields;
           f != NULL && fi < struct_defs[si].nfields; f = f->right, fi++) {
        resolve_field_type(&struct_defs[si].fields[fi], f->var_decl.var_type,
                           f);
      }
    } else if (s->type == NODE_CLASS_DEF) {
      int ci = -1;
      for (int i = 0; i < class_count; i++) {
        if (strcmp(class_defs[i].name, s->class_def.name) == 0) {
          ci = i;
          break;
        }
      }
      if (ci < 0) {
        continue;
      }
      int fi = 0;
      for (Node *f = s->class_def.fields;
           f != NULL && fi < class_defs[ci].nfields; f = f->right, fi++) {
        resolve_field_type(&class_defs[ci].fields[fi], f->var_decl.var_type, f);
      }
    }
  }
  /* Inheritance: resolve bases, merge fields, assign vtable slots. */
  for (int c = 0; c < class_count; c++) {
    if (class_defs[c].base != NULL) {
      class_defs[c].base_idx = find_class(class_defs[c].base);
      if (class_defs[c].base_idx < 0) {
        collect_error(class_defs[c].decl, "Unknown base class '%s'",
                      class_defs[c].base);
        class_defs[c].base_idx = -1;
      } else if (class_defs[c].base_idx == c) {
        collect_error(class_defs[c].decl, "Class '%s' cannot inherit itself",
                      class_defs[c].name);
        class_defs[c].base_idx = -1;
      }
    }
  }
  for (int c = 0; c < class_count; c++) {
    flatten_class(c);
  }
  for (int c = 0; c < class_count; c++) {
    for (int m = 0; m < class_defs[c].nmethods; m++) {
      method_slot(class_defs[c].methods[m].name);
    }
  }
  /* Register globals before the type scans so find_global resolves. */
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_VAR_DECL && s->var_decl.is_global) {
      const char *dk = NULL;
      ValueType dt = resolve_decl_type(s->var_decl.var_type, s, &dk);
      int found = -1;
      for (int i = 0; i < global_count; i++) {
        if (strcmp(global_names[i], s->var_decl.name) == 0) {
          found = i;
          break;
        }
      }
      if (found >= 0) {
        collect_error(s, "Duplicate global variable '%s'", s->var_decl.name);
      } else if (global_count < 256) {
        size_t nlen = strlen(s->var_decl.name);
        global_names[global_count] = malloc(nlen + 1);
        memcpy(global_names[global_count], s->var_decl.name, nlen);
        global_names[global_count][nlen] = '\0';
        global_types[global_count] = dt;
        global_init[global_count] = NULL;
        global_count++;
      }
    }
  }
  for (int i = 0; i < sig_count; i++) {
    for (int j = 0; j < 32; j++) {
      sig_param_float[i][j] = 0;
    }
    sig_is_bool_only[i] = 0;
    sig_return_float[i] = 0;
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
    for (int c = 0; c < class_count && !sig_is_bool_only[i]; c++) {
      for (int m = 0; m < class_defs[c].nmethods; m++) {
        char label[128];
        method_label(class_defs[c].name, class_defs[c].methods[m].name, label);
        if (strcmp(label, sig_names[i]) == 0) {
          int found = 0;
          int all_bool = 1;
          scan_returns_bool_only(class_defs[c].methods[m].body, &found,
                                 &all_bool);
          if (found && all_bool) {
            sig_is_bool_only[i] = 1;
          }
          break;
        }
      }
    }
  }
  scan_method_float_calls(root);
  /* Param and return float promotion refine each other; iterate to a
     fixed point (both scans are monotone, so this converges). */
  for (int pass = 0; pass < 3; pass++) {
    scan_float_calls(root);
    scan_return_floats(root);
    promote_global_inits(root);
  }

  collect_strings(root);

  out = fopen(output, "w");
  if (!out) {
    char message[256];
    snprintf(message, sizeof(message), "Could not open output file '%s'",
             output);
    term_report(TERM_ERROR, codegen_source, 1, 1, 1, message);
    return;
  }

  fprintf(out, "global main\n");
  fprintf(out, "extern printf\n");
  fprintf(out, "extern scanf\n");
  fprintf(out, "extern exit\n");
  fprintf(out, "extern malloc\n");
  fprintf(out, "extern strlen\n");
  fprintf(out, "extern strcmp\n");
  fprintf(out, "extern memcpy\n");
  fprintf(out, "extern fflush\n");
  fprintf(out, "extern snprintf\n");
  fprintf(out, "extern strtod\n");
  fprintf(out, "extern fopen\n");
  fprintf(out, "extern fseek\n");
  fprintf(out, "extern ftell\n");
  fprintf(out, "extern fread\n");
  fprintf(out, "extern fwrite\n");
  fprintf(out, "extern fclose\n");
  gen_data_section();
  fprintf(out, "section .text\n");

  fprintf(out, "main:\n");
  fprintf(out, "  mov rax, rsp\n");
  fprintf(out, "  sub rax, 262144\n");
  fprintf(out, "  mov [rel stack_floor], rax\n");
  gen_prologue();
  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type != NODE_FUNCTION && s->type != NODE_STRUCT_DEF &&
        s->type != NODE_CLASS_DEF) {
      try_gen_statement(s);
    }
  }
  check_unused_vars(0);
  fprintf(out, "  xor ecx, ecx\n");
  fprintf(out, "  sub rsp, 32\n");
  fprintf(out, "  call exit\n");

  for (Node *s = root; s != NULL; s = s->right) {
    if (s->type == NODE_FUNCTION) {
      jmp_buf saved;
      memcpy(saved, gen_jmp, sizeof(gen_jmp));
      if (setjmp(gen_jmp) == 0) {
        gen_function(s);
      }
      memcpy(gen_jmp, saved, sizeof(gen_jmp));
    }
  }

  for (int c = 0; c < class_count; c++) {
    for (int m = 0; m < class_defs[c].nmethods; m++) {
      jmp_buf saved;
      memcpy(saved, gen_jmp, sizeof(gen_jmp));
      if (setjmp(gen_jmp) == 0) {
        gen_method(c, m);
      }
      memcpy(gen_jmp, saved, sizeof(gen_jmp));
    }
  }

  gen_trap("overflow_trap", "fmt_overflow");
  gen_trap("divzero_trap", "fmt_divzero");
  gen_trap("stack_overflow_trap", "fmt_stack");
  gen_trap("input_error_trap", "fmt_invalid");
  gen_trap("index_trap", "fmt_index");
  gen_trap("alloc_trap", "fmt_alloc");
  gen_trap("null_trap", "fmt_null");
  gen_trap("open_trap", "fmt_openfail");

  fclose(out);
}
