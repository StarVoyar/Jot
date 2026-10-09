#include "target.h"

#include <string.h>

/**
 * @brief Windows x86-64: rcx/rdx/r8/r9 args, 32-byte shadow, xmm6-15 saved
 * @details Scratch regs owned by the emitter (rax, rbx, r10, r11, xmm0-3)
 * are excluded from every mask; rsp/rbp are reserved.
 */
const Target target_win64 = {
    "win64",
    {R_RCX, R_RDX, R_R8, R_R9, -1, -1},
    4,
    32,
    REG_BIT(R_RCX) | REG_BIT(R_RDX) | REG_BIT(R_R8) | REG_BIT(R_R9),
    REG_BIT(R_RSI) | REG_BIT(R_RDI) | REG_BIT(R_R12) | REG_BIT(R_R13) |
        REG_BIT(R_R14) | REG_BIT(R_R15),
    REG_BIT(R_XMM4) | REG_BIT(R_XMM5),
    REG_BIT(R_XMM6) | REG_BIT(R_XMM7) | REG_BIT(R_XMM8) | REG_BIT(R_XMM9) |
        REG_BIT(R_XMM10) | REG_BIT(R_XMM11) | REG_BIT(R_XMM12) |
        REG_BIT(R_XMM13) | REG_BIT(R_XMM14) | REG_BIT(R_XMM15),
    0,
    4,
    0,
};

/**
 * @brief System V x86-64: rdi/rsi/rdx/rcx/r8/r9 args, no shadow
 * @details All xmm regs are caller-saved, so float values live in
 * volatile regs or spill; rsi/rdi differ from Windows (caller-saved).
 */
const Target target_unix64 = {
    "elf64",
    {R_RDI, R_RSI, R_RDX, R_RCX, R_R8, R_R9},
    6,
    0,
    REG_BIT(R_RCX) | REG_BIT(R_RDX) | REG_BIT(R_RSI) | REG_BIT(R_RDI) |
        REG_BIT(R_R8) | REG_BIT(R_R9),
    REG_BIT(R_R12) | REG_BIT(R_R13) | REG_BIT(R_R14) | REG_BIT(R_R15),
    REG_BIT(R_XMM4) | REG_BIT(R_XMM5) | REG_BIT(R_XMM6) | REG_BIT(R_XMM7) |
        REG_BIT(R_XMM8) | REG_BIT(R_XMM9) | REG_BIT(R_XMM10) |
        REG_BIT(R_XMM11) | REG_BIT(R_XMM12) | REG_BIT(R_XMM13) |
        REG_BIT(R_XMM14) | REG_BIT(R_XMM15),
    0,
    0,
    1,
    1,
};

const Target *target_by_name(const char *name) {
  if (name == NULL) {
    return NULL;
  }
  if (strcmp(name, "win64") == 0) {
    return &target_win64;
  }
  if (strcmp(name, "elf64") == 0) {
    return &target_unix64;
  }
  return NULL;
}

const Target *target_host(void) {
#ifdef _WIN32
  return &target_win64;
#else
  return &target_unix64;
#endif
}
