#ifndef TARGET_H
#define TARGET_H

#include <stdint.h>

/**
 * @brief x86-64 registers by number (bit position in the masks below)
 */
typedef enum {
  R_RAX,
  R_RCX,
  R_RDX,
  R_RBX,
  R_RSP,
  R_RBP,
  R_RSI,
  R_RDI,
  R_R8,
  R_R9,
  R_R10,
  R_R11,
  R_R12,
  R_R13,
  R_R14,
  R_R15,
  R_XMM0,
  R_XMM1,
  R_XMM2,
  R_XMM3,
  R_XMM4,
  R_XMM5,
  R_XMM6,
  R_XMM7,
  R_XMM8,
  R_XMM9,
  R_XMM10,
  R_XMM11,
  R_XMM12,
  R_XMM13,
  R_XMM14,
  R_XMM15
} Reg;

#define REG_BIT(r) (1u << (r))

/**
 * @brief Calling convention and register classes of one x86-64 target
 * @details Everything the backend needs to differ between Windows and
 * System V lives here; the emitter only reads this descriptor.
 */
typedef struct {
  const char *name;      /**< "win64" or "elf64" */
  int arg_regs[6];       /**< GP argument registers in order */
  int nargs_regs;        /**< Register arguments (4 win, 6 unix) */
  int shadow;            /**< Callee shadow space (32 win, 0 unix) */
  uint32_t volatile_gp;  /**< Caller-saved GP regs (clobbered by calls) */
  uint32_t saved_gp;     /**< Callee-saved allocatable GP regs */
  uint32_t volatile_xmm; /**< Caller-saved XMM regs */
  uint32_t saved_xmm;    /**< Callee-saved allocatable XMM regs */
  int vararg_eax_int;    /**< AL for varargs without vector regs */
  int vararg_eax_float;  /**< AL for varargs with vector regs */
  int need_plt;          /**< Non-zero when extern calls need wrt ..plt */
} Target;

/** Windows x86-64 descriptor */
extern const Target target_win64;

/** System V x86-64 descriptor */
extern const Target target_unix64;

/**
 * @brief Looks a target up by name
 * @param name "win64" or "elf64"
 * @return Descriptor, or NULL when unknown
 */
const Target *target_by_name(const char *name);

/**
 * @brief Returns the host default target
 * @return win64 on Windows, elf64 elsewhere (single selection point)
 */
const Target *target_host(void);

#endif
