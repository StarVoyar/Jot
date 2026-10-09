#ifndef IR_BUILD_H
#define IR_BUILD_H

#include "ir.h"
#include "../sem/sem.h"
#include "../sem/sem_impl.h"

/**
 * @brief IR builder context for one function body
 */
typedef struct {
  IrModule *m;
  IrFunc *f;
  int cur; /**< Current block id (ids stay valid across reallocs) */
  SemVar *vars;
  int nvars;
  SemFunc *sfunc;
  int is_method;
  int cls;
  char *names[SEM_MAX_VARS];
  int slots[SEM_MAX_VARS];
  int nscope;
  int break_blk[SEM_MAX_LOOP_DEPTH];
  int cont_blk[SEM_MAX_LOOP_DEPTH];
  int loop_depth;
  int terminated; /**< Non-zero once the current block ends in a jump */
  int ret_float;  /**< Plain function normalizes int results to float */
  const char *ret_kind; /**< Declared return keyword, NULL for entry */
} BuildCtx;

/**
 * @brief Reports an internal builder error (unreachable on valid programs)
 * @param what Description of the invariant violated
 */
void irb_bug(const char *what);

/**
 * @brief Maps a semantic category to an IR category
 * @param t Semantic category
 * @return IR category
 */
IrType irb_type(SemType t);

/**
 * @brief Allocates a compiler temporary in the frame
 * @param ctx Builder context
 * @param t Temporary category
 * @return Frame slot
 */
int irb_temp(BuildCtx *ctx, IrType t);

/**
 * @brief Emits an instruction into the current block
 * @param ctx Builder context
 * @param ins Instruction
 * @return Emitted instruction
 */
IrInstr *irb_emit(BuildCtx *ctx, IrInstr ins);

/**
 * @brief Returns the current emission block
 * @param ctx Builder context
 * @return Current block (looked up fresh, safe across reallocs)
 */
IrBlock *irb_cur(BuildCtx *ctx);

/**
 * @brief Starts a new block and continues emission there
 * @param ctx Builder context
 * @return New block
 */
IrBlock *irb_block(BuildCtx *ctx);

/**
 * @brief Creates a block without disturbing the current one
 * @param ctx Builder context
 * @return New block id (blocks may move, ids stay valid)
 */
int irb_fresh(BuildCtx *ctx);

/**
 * @brief Continues emission in a block by id
 * @param ctx Builder context
 * @param id Block id
 */
void irb_goto(BuildCtx *ctx, int id);

/**
 * @brief Looks a name up in the builder scope
 * @param ctx Builder context
 * @param name Variable name
 * @return Frame slot, or -1
 */
int irb_scope_find(BuildCtx *ctx, const char *name);

/**
 * @brief Declares a name in the builder scope
 * @param ctx Builder context
 * @param name Variable name (borrowed)
 * @param slot Frame slot
 */
void irb_scope_declare(BuildCtx *ctx, const char *name, int slot);

/**
 * @brief Finds a global index by name
 * @param ctx Builder context
 * @param name Global name
 * @return Global index, or -1
 */
int irb_global_at(BuildCtx *ctx, const char *name);

/**
 * @brief Lowers an expression to a value
 * @param ctx Builder context
 * @param node Expression node (already checked)
 * @return Value holding the result
 */
IrVal irb_expr(BuildCtx *ctx, Node *node);

/**
 * @brief Converts a value to a category with an explicit instruction
 * @param ctx Builder context
 * @param v Value to convert
 * @param want Target category
 * @return Converted value (a temp when conversion runs)
 */
IrVal irb_convert(BuildCtx *ctx, IrVal v, IrType want);

/**
 * @brief Truncates an integer value to its low byte
 * @param ctx Builder context
 * @param v Value to truncate
 * @return Truncated value
 */
IrVal irb_trunc8(BuildCtx *ctx, IrVal v);

/**
 * @brief Converts an index value to a plain integer
 * @param ctx Builder context
 * @param v Index value
 * @return Integer index value
 */
IrVal irb_index(BuildCtx *ctx, IrVal v);

/**
 * @brief Lowers a call to a user function
 * @param ctx Builder context
 * @param node Call node
 * @param fi Function index
 * @return Result value (temp) or an unused void marker
 */
IrVal irb_call(BuildCtx *ctx, Node *node, int fi);

/**
 * @brief Lowers a block of statements
 * @param ctx Builder context
 * @param list First statement (linked via right)
 */
void irb_block_stmts(BuildCtx *ctx, Node *list);

#endif
