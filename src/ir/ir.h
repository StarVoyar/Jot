#ifndef IR_H
#define IR_H

#include "../parser/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Value categories in IR
 * @details Every value carries its category; conversions between int and
 * float are always explicit instructions, never implied.
 */
typedef enum {
  IR_INT,    /**< 64-bit integers, bools, and character codes */
  IR_FLOAT,  /**< IEEE doubles (num values are normalized to these) */
  IR_BOOL,   /**< Comparison results (0 or 1, stored like ints) */
  IR_STR,    /**< Heap string pointers */
  IR_ARR,    /**< Array block pointers */
  IR_STRUCT, /**< Struct instance pointers */
  IR_CLASS,  /**< Class instance pointers */
  IR_NULL,   /**< Null pointers */
  IR_VOID    /**< Absence of a value */
} IrType;

/**
 * @brief IR operation codes (enum, never strings)
 */
typedef enum {
  IR_NOP,        /**< Removed instruction (kept as a tombstone) */
  IR_CONST_I,    /**< dst = 64-bit immediate */
  IR_CONST_F,    /**< dst = double immediate */
  IR_CONST_S,    /**< dst = string literal */
  IR_CONST_NULL, /**< dst = null */
  IR_COPY,       /**< dst = src */
  IR_ADD,        /**< Integer or float addition (traps on int overflow) */
  IR_SUB,        /**< Integer or float subtraction (traps on int overflow) */
  IR_MUL,        /**< Integer or float multiplication (traps on overflow) */
  IR_DIV,        /**< Integer (traps on zero) or float division */
  IR_MOD,        /**< Integer remainder (traps on zero) */
  IR_AND,        /**< Integer bitwise AND (wraps, never traps) */
  IR_OR,         /**< Integer bitwise OR (wraps, never traps) */
  IR_XOR,        /**< Integer bitwise XOR (wraps, never traps) */
  IR_SHL,        /**< Integer shift left (count masked to 0-63, wraps) */
  IR_SHR,        /**< Arithmetic shift right (count masked to 0-63) */
  IR_NOT,        /**< Integer bitwise NOT (single operand in v[0]) */
  IR_CMP,        /**< Comparison, result is 0 or 1 */
  IR_I2F,        /**< Integer bits to double */
  IR_F2I,        /**< Double to integer by truncation */
  IR_TRUNC8,     /**< Integer to its low byte (bool array/index use) */
  IR_LOAD,       /**< dst = frame variable */
  IR_STORE,      /**< Frame variable = src */
  IR_LOADG,      /**< dst = global */
  IR_STOREG,     /**< Global = src */
  IR_JUMP,       /**< Unconditional jump */
  IR_BR,         /**< Conditional branch on an int condition */
  IR_CALL,       /**< Call a plain function */
  IR_CALLM,      /**< Call a method through the vtable */
  IR_RET,        /**< Return from a function (value already converted) */
  IR_EXIT,       /**< Exit the process with a code */
  IR_PRINT_S,    /**< Print a literal string chunk */
  IR_PRINT_V,    /**< Print a value with its type format */
  IR_INPUT,      /**< Read an integer (optional prompt) */
  IR_LEN,        /**< String length */
  IR_TOSTR,      /**< Number to heap string */
  IR_TONUM,      /**< String to double */
  IR_READFILE,   /**< Whole file to heap string */
  IR_WRITEFILE,  /**< Text to file, result is bytes written */
  IR_CHR,        /**< Character code to 1-byte heap string (traps out of range) */
  IR_ARGS,       /**< Command-line arguments to heap string array */
  IR_NEWARR,     /**< Allocate and fill an array block */
  IR_ARR_LOAD,   /**< Bounds-checked element load */
  IR_ARR_STORE,  /**< Bounds-checked element store (op in aux) */
  IR_ARR_LEN,    /**< Array length */
  IR_STR_IDX,    /**< Character code load from a string */
  IR_CONCAT,     /**< String concatenation */
  IR_NEWINST,    /**< Allocate and fill a struct/class instance */
  IR_COPYINST,   /**< Copy an instance into a fresh heap object */
  IR_FIELD_LOAD, /**< Field load with null check */
  IR_FIELD_STORE /**< Field store with null check */
} IrOp;

/**
 * @brief Comparison conditions
 */
typedef enum {
  IR_CEQ, /**< == */
  IR_CNE, /**< != */
  IR_CLT, /**< < */
  IR_CGT, /**< > */
  IR_CLE, /**< <= */
  IR_CGE /**< >= */
} IrCond;

/**
 * @brief Operand kinds
 */
typedef enum {
  IRV_TEMP,   /**< Compiler temporary (frame slot) */
  IRV_VAR,    /**< Declared variable or parameter (frame slot) */
  IRV_GLOBAL, /**< Global variable (module index) */
  IRV_IMM_I,  /**< 64-bit immediate */
  IRV_IMM_F,  /**< Double immediate */
  IRV_STR,    /**< String literal (module index) */
  IRV_NULL    /**< Null constant */
} IrValKind;

/**
 * @brief One IR operand (frame slots address [rbp - 8*(slot+1)])
 */
typedef struct {
  IrValKind kind;
  int idx;            /**< Slot, global, string, or function index */
  long long imm;      /**< Integer immediate or double bits */
  IrType type;        /**< Operand category */
} IrVal;

/**
 * @brief One three-address instruction
 * @details Fixed operands cover scalar code; list holds variadic operands
 * (call arguments, print values, array elements, instance fields).
 */
typedef struct {
  IrOp op;
  IrType type;   /**< Operation/result category */
  int dst;       /**< Destination temp slot, -1 when none */
  IrVal v[3];    /**< Fixed operands */
  int nv;        /**< Fixed operand count */
  IrVal *list;   /**< Variadic operands (owned) */
  int nlist;     /**< Variadic operand count */
  int cond;      /**< IR_C* for IR_CMP */
  int t;         /**< Jump target block (IR_JUMP, IR_BR true) */
  int f;         /**< IR_BR false block */
  int callee;    /**< Function index, builtin code, or method vtable slot */
  int aux;       /**< Misc: string/global/field index, class id, store op */
  int aux2;      /**< Second misc int: size flag, field count, arg count */
  const char *name; /**< Method label for dumps (borrowed) */
} IrInstr;

/**
 * @brief One basic block (straight-line instructions, ends in a jump)
 */
typedef struct {
  IrInstr *ins;
  int nins;
  int cap;
  int id;
} IrBlock;

/**
 * @brief One lowered function, method, or the entry program
 */
typedef struct {
  char *name;      /**< Source name (entry is "main") */
  char *label;     /**< Assembly label */
  IrType ret;      /**< Return category */
  int is_method;   /**< Non-zero for methods (slot 0 is the receiver) */
  char *this_kind; /**< Receiver class for methods, else NULL */
  char **var_names;
  IrType *var_types;
  char **var_kinds;
  int *var_is_param;
  IrType *var_elems;
  char **var_elem_kinds;
  int nvars;       /**< Declared variables (slots 0..nvars-1) */
  IrType *temp_types;
  int ntemps;      /**< Compiler temps (slots nvars..) */
  IrBlock *blocks;
  int nblocks;
  int capblocks;
} IrFunc;

/**
 * @brief One global variable
 */
typedef struct {
  char *name;
  IrType type;
  char *kind;
  int has_init;
} IrGlobal;

/**
 * @brief One class with its vtable layout resolved
 */
typedef struct {
  char *name;
  int class_id;
  int *vtable; /**< Per slot: owner class index, or -1 when empty */
  int *vtable_mth; /**< Per slot: method index in the owner, or -1 */
  int nslots;
  char **method_names; /**< Owned method names for vtable labels */
  int nmethods;
} IrClass;

/**
 * @brief Whole-program IR module feeding the backend and optimizer
 */
typedef struct {
  IrFunc *funcs;
  int nfuncs;
  IrGlobal *globals;
  int nglobals;
  char **strings;
  int nstrings;
  IrClass *classes;
  int nclasses;
  int has_user_main;
} IrModule;

/**
 * @brief Makes an operand
 */
IrVal ir_temp(int slot, IrType type);
IrVal ir_var(int slot, IrType type);
IrVal ir_global(int gidx, IrType type);
IrVal ir_imm_i(long long value, IrType type);
IrVal ir_imm_f(double value);
IrVal ir_str(int sidx);
IrVal ir_null(void);

/**
 * @brief Creates an empty module
 * @return Owned module
 */
IrModule *ir_module_new(void);

/**
 * @brief Adds a function to the module
 * @param m Module
 * @param name Source name (copied)
 * @param label Assembly label (copied)
 * @param ret Return category
 * @return New function
 */
IrFunc *ir_add_func(IrModule *m, const char *name, const char *label,
                    IrType ret);

/**
 * @brief Adds a block to a function
 * @param f Function
 * @return New block
 */
IrBlock *ir_add_block(IrFunc *f);

/**
 * @brief Appends an instruction to a block
 * @param b Block
 * @param ins Instruction (copied)
 * @return Appended instruction
 */
IrInstr *ir_emit(IrBlock *b, IrInstr ins);

/**
 * @brief Starts a blank instruction of an opcode
 * @param op Operation code
 * @return Zeroed instruction with dst set to -1
 */
IrInstr ir_instr(IrOp op);

/**
 * @brief Adds a string literal to the module, deduplicating
 * @param m Module
 * @param value String value (copied)
 * @return String index
 */
int ir_add_string(IrModule *m, const char *value);

/**
 * @brief Returns the category of a frame slot
 * @param f Function
 * @param slot Frame slot
 * @return Slot category
 */
IrType ir_slot_type(IrFunc *f, int slot);

/**
 * @brief Returns the category of an operand
 * @param f Function owning temp slots
 * @param v Operand
 * @return Operand category
 */
IrType ir_val_type(IrFunc *f, IrVal v);

/**
 * @brief Dumps the module in human-readable form
 * @param m Module
 * @param f Output stream
 */
void ir_dump(IrModule *m, FILE *f);

/**
 * @brief Builds IR for the checked program
 * @param root Top level statements (linked via right)
 * @return Owned module (valid only when semantic analysis passed)
 */
IrModule *ir_build(Node *root);

#endif
