#ifndef SEM_IMPL_H
#define SEM_IMPL_H

#include "sem.h"

/**
 * @brief Active function scope while checking bodies
 * @details Slots allocate monotonically (never reused) so every declaration
 * keeps a stable frame slot for the IR builder; visibility is tracked by a
 * separate binding stack that blocks push and pop (mirrors old scoping).
 */
typedef struct {
  SemVar *vars;      /**< Frame slots in declaration order (append-only) */
  int nvars;         /**< Declared slot count */
  int capvars;       /**< Allocated slot capacity */
  const char *bind_name[SEM_MAX_VARS]; /**< Visible names (block-scoped) */
  int bind_slot[SEM_MAX_VARS];         /**< Slots parallel to bind_name */
  int nbind;                           /**< Visible binding count */
  int func;      /**< Plain function index, -2 for entry, -1 for none */
  int cls;       /**< Class index inside methods, -1 otherwise */
  int mth;       /**< Method index inside methods, -1 otherwise */
  int in_method; /**< Non-zero inside a method body */
  int loop_depth;
  int no_param_warn;
  const char *ret; /**< Declared return keyword, NULL at top level */
} SemScope;

/** Active scope (defined in sem.c) */
extern SemScope sem_scope;

/** Recovery buffer for one statement (defined in sem.c) */
extern jmp_buf sem_jmp;

/** Non-zero while checking statements (defined in sem.c) */
extern int sem_in_stmt;

/** Main source file for diagnostics (defined in sem.c) */
extern const char *sem_source_file;

char *sem_dup(const char *s);
void sem_error(Node *node, const char *format, ...);
void sem_warn(Node *node, const char *format, ...);

int sem_find_global(const char *name);
int sem_find_struct(const char *name);
int sem_find_class(const char *name);
int sem_find_method(int class_idx, const char *method, int *owner_idx);
int sem_is_subclass(const char *kind, const char *base);
int sem_find_struct_field(int struct_idx, const char *field);
int sem_find_class_field(int class_idx, const char *field);
int sem_find_instance_field(const char *kind, const char *field,
                            SemField **out_def);
int sem_method_slot_assign(const char *name);
SemType sem_resolve_decl(const char *keyword, Node *node, const char **out_kind);
void sem_resolve_field(SemField *field, const char *declared, Node *fault);
int sem_count_params(Node *params);
void sem_flatten_class(int c);

void sem_record_type(Node *node, SemType type, const char *kind);
void sem_record_slot(Node *decl, int slot);

int sem_scope_find(const char *name);
int sem_scope_declare(Node *node, const char *name, SemType type, int is_param);
int sem_scope_require(Node *node, const char *name);
int sem_ref_is_global(int ref);
int sem_ref_to_global(int ref);
int sem_scope_require_param(Node *node, const char *member);
void sem_check_unused(int from);
void sem_snapshot_vars(SemVar **dst, int *n);

/**
 * @brief Checks an expression, recording its type for the IR builder
 * @param node Expression node
 * @return Resolved category (ST_ERR after an error, which abandons the
 * statement)
 * @details Mirrors the historic type query: identifiers resolve, calls
 * check voidness, member access resolves fields, but children are never
 * validated here (the emission mirror below does that).
 */
SemType sem_expr(Node *node);

/**
 * @brief Validates an expression like the historic type query
 * @param node Expression node
 * @return Resolved category
 */
SemType sem_validate(Node *node);

/**
 * @brief Mirrors expression emission: checks, warns, marks, in order
 * @param node Expression node
 * @return Resolved category
 */
SemType sem_touch(Node *node);

/**
 * @brief Silent expression category without warnings or use-marking
 * @param node Expression node
 * @return Best-effort category
 */
SemType sem_peek(Node *node);

/**
 * @brief Resolves the static kind of an expression from scope state
 * @param node Expression node
 * @return Struct/class/array-element name, or NULL
 */
const char *sem_peek_kind(Node *node);

/**
 * @brief Returns the statically known instance kind of an expression
 * @param node Expression node (already checked)
 * @return Struct/class/array-element name, or NULL
 */
const char *sem_inst_kind(Node *node);

/**
 * @brief Runs the float/bool fixpoint over signatures (defined in sem_scan.c)
 * @param root Top level statements
 */
void sem_scan_fix(Node *root);

/**
 * @brief One resolved piece of an interpolated print string
 */
typedef struct {
  int is_text;   /**< Non-zero for a literal text chunk */
  char *text;    /**< Chunk text (owned) when is_text */
  int nsegs;     /**< Placeholder path segment count */
  char **segs;   /**< Path segments (owned) */
  SemType type;  /**< Resolved value type */
  const char *kind; /**< Instance kind, may be NULL */
  int base;      /**< 0 frame/global name, 1 self param, 2 this field */
} SemPrintPart;

/**
 * @brief Resolved parts of one interpolated print string
 */
typedef struct {
  SemPrintPart *parts;
  int nparts;
  int capparts;
} SemPrintInfo;

/**
 * @brief Returns recorded placeholder parts for a print statement
 * @param printNode Print statement node
 * @return Parts, or NULL when not an interpolated string print
 */
SemPrintInfo *sem_print_info(Node *printNode);

/**
 * @brief Checks a call expression or statement
 * @param node Call node
 * @param allow_void Non-zero for statement-position calls
 * @return Result category
 */
SemType sem_check_call(Node *node, int allow_void);

/**
 * @brief Checks one top-level statement with statement recovery
 * @param node Statement node (defined in sem_check.c)
 */
void sem_check_stmt(Node *node);

/**
 * @brief Checks a function or method body (defined in sem_check.c)
 * @param func Function record with params, body, and return info
 * @param func_idx Plain function index, or -2 for the entry program
 * @param cls Class index for methods, -1 otherwise
 * @param mth Method index for methods, -1 otherwise
 */
void sem_check_body(SemFunc *func, int func_idx, int cls, int mth);

#endif
