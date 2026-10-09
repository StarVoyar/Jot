#ifndef SEM_H
#define SEM_H

#include "../parser/parser.h"
#include "../terminal/terminal.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Value categories resolved by semantic analysis
 * @details Mirrors the categories code generation needs: num splits into
 * int and float because promotion is decided here, not in the backend.
 */
typedef enum {
  ST_INT,    /**< Integers and integer-like values */
  ST_FLOAT,  /**< Floating point values */
  ST_BOOL,   /**< Comparison results */
  ST_STR,    /**< String pointers */
  ST_ARR,    /**< Array pointers (element type tracked separately) */
  ST_STRUCT, /**< Struct instance (heap pointer) */
  ST_CLASS,  /**< Class instance (heap pointer) */
  ST_NULL,   /**< null (no reference) */
  ST_VOID,   /**< Absence of a value (void calls, bare return) */
  ST_ERR     /**< Poison after an error, suppresses cascades */
} SemType;

/** Maximum fields per struct/class */
#define SEM_MAX_FIELDS 32

/** Maximum methods per class */
#define SEM_MAX_METHODS 32

/** Maximum struct definitions per program */
#define SEM_MAX_STRUCTS 64

/** Maximum class definitions per program */
#define SEM_MAX_CLASSES 64

/** Maximum globals per program */
#define SEM_MAX_GLOBALS 256

/** Maximum vtable slots (distinct method names) */
#define SEM_MAX_METH_SLOTS 128

/** Maximum variables per function frame */
#define SEM_MAX_VARS 256

/** Maximum arrays per function frame */
#define SEM_MAX_ARRAYS 64

/** Maximum parameter count tracked for promotion */
#define SEM_MAX_PARAMS 32

/** Maximum loop nesting depth */
#define SEM_MAX_LOOP_DEPTH 32

/**
 * @brief One variable in a function frame
 */
typedef struct {
  char *name;      /**< Variable name */
  SemType type;    /**< Resolved category (num locals end as INT or FLOAT) */
  char *kind;      /**< Struct/class name for instances, else NULL */
  int is_param;    /**< Non-zero for parameters (read as self.name) */
  int line;        /**< Declaration line for unused warnings */
  int col;         /**< Declaration column */
  int used;        /**< Non-zero once loaded */
  int slot;        /**< Frame slot, also the backend address */
  SemType elem;    /**< Array element category for ST_ARR */
  char *elem_kind; /**< Instance name for struct/class arrays, else NULL */
} SemVar;

/**
 * @brief One function or method body awaiting IR construction
 */
typedef struct {
  char *name;        /**< Function name (methods: plain method name) */
  int is_public;     /**< Non-zero if importable */
  char *ret;         /**< Declared return keyword, NULL when untyped */
  Node *params;      /**< Parameter list (linked via right) */
  Node *body;        /**< Body (linked via right) */
  int nparams;       /**< Parameter count */
  SemType *ptypes;   /**< Final parameter types (promotion applied) */
  char **pkinds;     /**< Instance names for struct/class params */
  char **pnames;     /**< Parameter names in order */
  int line;          /**< Definition line */
  int col;           /**< Definition column */
  const char *source;/**< File of definition (borrowed) */
  Node *decl;        /**< Definition node for diagnostics (may be NULL) */
  int ret_float;     /**< Callers read num results as float */
  int bool_only;     /**< Returns only comparisons, never floats */
  int pfloat[SEM_MAX_PARAMS]; /**< Param slots promoted to float */
  int nvars;         /**< Frame variable count after checking */
  SemVar *vars;      /**< Frame variables in slot order (owned) */
} SemFunc;

/**
 * @brief One global variable
 */
typedef struct {
  char *name;       /**< Global name */
  SemType type;     /**< Resolved category */
  char *kind;       /**< Struct/class name for instances, else NULL */
  Node *init;       /**< Initializer, may be NULL */
  int line;         /**< Declaration line */
  int col;          /**< Declaration column */
  const char *source; /**< File of declaration (borrowed) */
} SemGlobal;

/**
 * @brief One struct/class field (every field is one 8-byte slot)
 */
typedef struct {
  char *name; /**< Field name */
  SemType type; /**< Slot category (num fields hold doubles) */
  char *kind; /**< Struct/class name for instance fields, else NULL */
} SemField;

/**
 * @brief One struct definition
 */
typedef struct {
  char *name;                          /**< Struct name */
  int is_public;                       /**< Non-zero if importable */
  SemField fields[SEM_MAX_FIELDS];     /**< Field list */
  int nfields;                         /**< Field count */
} SemStruct;

/**
 * @brief One class definition (fields flattened, base first)
 */
typedef struct {
  char *name;                          /**< Class name */
  int is_public;                       /**< Non-zero if importable */
  char *base;                          /**< Base class name, or NULL */
  int base_idx;                        /**< Base class index, -1 when none */
  int flattened;                       /**< Non-zero once fields are merged */
  int visiting;                        /**< Inheritance cycle detection flag */
  int class_id;                        /**< Runtime id in instance headers */
  SemField fields[SEM_MAX_FIELDS];      /**< Fields, base fields first */
  int nfields;                         /**< Field count including base fields */
  SemFunc methods[SEM_MAX_METHODS];     /**< Owned methods */
  int nmethods;                        /**< Owned method count */
  Node *decl;                          /**< Class definition node */
} SemClass;

/**
 * @brief Whole-program semantic model feeding IR construction
 */
typedef struct {
  SemFunc *funcs;                       /**< Plain functions (grown) */
  int nfuncs;                           /**< Plain function count */
  int capfuncs;                         /**< Allocated function capacity */
  SemGlobal globals[SEM_MAX_GLOBALS];   /**< Global variables */
  int nglobals;                         /**< Global count */
  SemStruct structs[SEM_MAX_STRUCTS];    /**< Struct definitions */
  int nstructs;                         /**< Struct count */
  SemClass classes[SEM_MAX_CLASSES];     /**< Class definitions */
  int nclasses;                         /**< Class count */
  char *meth_slots[SEM_MAX_METH_SLOTS]; /**< Method names by vtable slot */
  int nmeth_slots;                      /**< Assigned vtable slots */
  int has_user_main;                    /**< Program defines fn main */
  SemVar *entry_vars;                   /**< Top-level frame vars (owned) */
  int nentry_vars;                      /**< Top-level frame var count */
} SemProg;

/**
 * @brief Runs semantic analysis over the program
 * @param root Top level statements (linked via right)
 * @param source Main source file for diagnostics
 * @return Non-zero when errors were reported (backend must not run)
 */
int SemAnalyze(Node *root, const char *source);

/**
 * @brief Returns the whole-program model (valid after SemAnalyze)
 * @return Program model with functions, globals, structs, classes
 */
SemProg *sem_prog(void);

/**
 * @brief Looks up a plain function by name
 * @param name Function name
 * @return Function index, or -1
 */
int sem_func_at(const char *name);

/**
 * @brief Returns the recorded type of a checked expression
 * @param node Expression node visited during checking
 * @return Recorded category, ST_ERR when unknown
 */
SemType sem_expr_type(Node *node);

/**
 * @brief Returns the recorded instance kind of a checked expression
 * @param node Expression node visited during checking
 * @return Struct/class name, array element kind, or NULL
 */
const char *sem_expr_kind(Node *node);

/**
 * @brief Returns the frame slot of a declaration
 * @param decl VAR_DECL, ARRAY_DECL, parameter, or FOR node
 * @return Frame slot, or -1 when not recorded
 */
int sem_decl_slot(Node *decl);

/**
 * @brief Names a category for diagnostics (matches historic messages)
 * @param type Category to name
 * @return Type word (num, bool, str, arr, struct, class)
 */
const char *sem_type_name(SemType type);

/**
 * @brief Checks if a category behaves as a number
 * @param type Category to test
 * @return Non-zero for int, float, and bool
 */
int sem_is_numeric(SemType type);

/**
 * @brief Checks assignment compatibility (historic rules)
 * @param declared Declared type of the target
 * @param given Inferred type of the value
 * @return Non-zero if the value may be stored in the target
 */
int sem_compatible(SemType declared, SemType given);

/**
 * @brief Returns the vtable slot of a method name
 * @param name Method name
 * @return Slot index, or -1 when unknown
 */
int sem_method_slot(const char *name);

#endif
