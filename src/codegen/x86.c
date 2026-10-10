#include "x86.h"

#include <string.h>

/** Module being emitted */
static IrModule *xm;

/** Assembly stream */
static FILE *xo;

/** Target descriptor (convention, register classes) */
static const Target *xt;

/** Function being emitted */
static IrFunc *xf;

/** Allocation result for the function */
static const RegAlloc *xa;

/** Frame size for save-area offsets */
static int x_frame;

/** Saved GP count for XMM save offsets */
static int x_npush;

/** Frame slot address operand offset */
static int slot_off(int slot) { return (slot + 1) * 8; }

static const char *gp64[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                             "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                             "r12", "r13", "r14", "r15"};

static const char *gp32[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp",
                             "esi", "edi", "r8d", "r9d", "r10d", "r11d",
                             "r12d", "r13d", "r14d", "r15d"};

static const char *gp8[] = {"al", "cl", "dl", "bl", "spl", "bpl",
                            "sil", "dil", "r8b", "r9b", "r10b", "r11b",
                            "r12b", "r13b", "r14b", "r15b"};

static const char *xmm[] = {"xmm0",  "xmm1",  "xmm2",  "xmm3",
                            "xmm4",  "xmm5",  "xmm6",  "xmm7",
                            "xmm8",  "xmm9",  "xmm10", "xmm11",
                            "xmm12", "xmm13", "xmm14", "xmm15"};

const char *reg_name(int reg) {
  if (reg >= R_XMM0) {
    return xmm[reg - R_XMM0];
  }
  return gp64[reg];
}

const char *reg_name8(int reg) { return gp8[reg]; }

/**
 * @brief 32-bit name of a GP register (for zeroing)
 * @param reg GP register number
 * @return 32-bit name
 */
static const char *reg_name32(int reg) { return gp32[reg]; }

/**
 * @brief Returns an argument register name
 * @param i Argument position
 * @return Register name from the target descriptor
 */
static const char *areg(int i) { return reg_name(xt->arg_regs[i]); }

/**
 * @brief Located operand: register, frame slot, global, string, immediate
 */
typedef struct {
  int is_reg;
  int reg;
  int is_mem;
  int slot;
  int is_global;
  int gidx;
  int is_str;
  int sidx;
  int is_imm;
  long long imm;
  IrType type;
} Op;

/**
 * @brief Resolves an IR value to a machine operand location
 * @param v Value to locate
 * @return Operand location (temps use their allocation)
 */
static Op op_of(IrVal v) {
  Op o;
  memset(&o, 0, sizeof(o));
  o.type = ir_val_type(xf, v);
  switch (v.kind) {
  case IRV_TEMP: {
    int ti = v.idx - xf->nvars;
    if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
      o.is_reg = 1;
      o.reg = xa->temps[ti].reg;
    } else {
      o.is_mem = 1;
      o.slot = v.idx;
    }
    break;
  }
  case IRV_VAR:
    o.is_mem = 1;
    o.slot = v.idx;
    break;
  case IRV_GLOBAL:
    o.is_global = 1;
    o.gidx = v.idx;
    break;
  case IRV_IMM_I:
    o.is_imm = 1;
    o.imm = v.imm;
    break;
  case IRV_IMM_F:
    o.is_imm = 1;
    o.imm = v.imm;
    break;
  case IRV_STR:
    o.is_str = 1;
    o.sidx = v.idx;
    break;
  case IRV_NULL:
    o.is_imm = 1;
    o.imm = 0;
    break;
  }
  return o;
}

/**
 * @brief Tells whether an immediate fits a sign-extended 32-bit operand
 * @param v Value to test
 * @return Non-zero when imm32 works
 */
static int imm32(long long v) { return v >= -2147483648LL && v <= 2147483647LL; }

/**
 * @brief Materializes an operand into rax (raw bits)
 * @param o Operand to load
 */
static void load_rax(Op o) {
  if (o.is_reg && o.reg >= R_XMM0) {
    fprintf(xo, "  movq rax, %s\n", reg_name(o.reg));
  } else if (o.is_reg) {
    if (o.reg != R_RAX) {
      fprintf(xo, "  mov rax, %s\n", reg_name(o.reg));
    }
  } else if (o.is_mem) {
    fprintf(xo, "  mov rax, [rbp - %d]\n", slot_off(o.slot));
  } else if (o.is_global) {
    fprintf(xo, "  mov rax, [rel global_%s]\n", xm->globals[o.gidx].name);
  } else if (o.is_str) {
    fprintf(xo, "  lea rax, [rel str%d]\n", o.sidx);
  } else if (o.type == IR_FLOAT) {
    fprintf(xo, "  mov rax, 0x%llx\n", (unsigned long long)o.imm);
  } else {
    fprintf(xo, "  mov rax, %lld\n", o.imm);
  }
}

/**
 * @brief Materializes an operand into a GP register (raw bits)
 * @param o Operand to load
 * @param reg Destination register
 */
static void load_reg(Op o, int reg) {
  const char *dst = reg_name(reg);
  if (o.is_reg && o.reg >= R_XMM0) {
    fprintf(xo, "  movq %s, %s\n", dst, reg_name(o.reg));
  } else if (o.is_reg) {
    if (o.reg != reg) {
      fprintf(xo, "  mov %s, %s\n", dst, reg_name(o.reg));
    }
  } else if (o.is_mem) {
    fprintf(xo, "  mov %s, [rbp - %d]\n", dst, slot_off(o.slot));
  } else if (o.is_global) {
    fprintf(xo, "  mov %s, [rel global_%s]\n", dst,
            xm->globals[o.gidx].name);
  } else if (o.is_str) {
    fprintf(xo, "  lea %s, [rel str%d]\n", dst, o.sidx);
  } else if (o.type == IR_FLOAT) {
    fprintf(xo, "  mov %s, 0x%llx\n", dst, (unsigned long long)o.imm);
  } else {
    fprintf(xo, "  mov %s, %lld\n", dst, o.imm);
  }
}

/**
 * @brief Materializes operand bits into an XMM register
 * @param o Operand to load
 * @param xr Destination XMM register
 */
static void load_xmm(Op o, int xr) {
  const char *dst = reg_name(xr);
  if (o.is_reg && o.reg >= R_XMM0) {
    if (o.reg != xr) {
      fprintf(xo, "  movapd %s, %s\n", dst, reg_name(o.reg));
    }
  } else if (o.is_reg) {
    fprintf(xo, "  movq %s, %s\n", dst, reg_name(o.reg));
  } else if (o.is_mem) {
    fprintf(xo, "  movq %s, [rbp - %d]\n", dst, slot_off(o.slot));
  } else if (o.is_global) {
    fprintf(xo, "  movq %s, [rel global_%s]\n", dst,
            xm->globals[o.gidx].name);
  } else {
    fprintf(xo, "  mov rax, 0x%llx\n", (unsigned long long)o.imm);
    fprintf(xo, "  movq %s, rax\n", dst);
  }
}

/**
 * @brief Stores rax into a frame slot
 * @param slot Destination slot
 */
static void store_slot(int slot) {
  fprintf(xo, "  mov [rbp - %d], rax\n", slot_off(slot));
}

/**
 * @brief Emits a dynamically-aligned prologue for a C runtime call
 * @details Saves the 0/8 byte misalignment above the shadow space so
 * calls inside expressions (rsp 8 off) stay 16 byte aligned
 */
static void gen_runtime_prologue(void) {
  fprintf(xo, "  mov rax, rsp\n");
  fprintf(xo, "  and rax, 8\n");
  fprintf(xo, "  sub rsp, rax\n");
  fprintf(xo, "  sub rsp, 48\n");
  fprintf(xo, "  mov [rsp + 32], rax\n");
}

/**
 * @brief Emits the epilogue matching gen_runtime_prologue (result in rax)
 */
static void gen_runtime_epilogue(void) {
  fprintf(xo, "  mov r10, [rsp + 32]\n");
  fprintf(xo, "  add rsp, 48\n");
  fprintf(xo, "  add rsp, r10\n");
}

/**
 * @brief Calls an extern C function (PLT-relative on ELF targets)
 * @param name Function to call
 */
static void extcall(const char *name) {
  if (xt->need_plt) {
    fprintf(xo, "  call %s wrt ..plt\n", name);
  } else {
    fprintf(xo, "  call %s\n", name);
  }
}

/**
 * @brief Calls a C function with a balanced stack (rsp%16==0 at entry)
 * @param name Function to call
 */
static void ccall(const char *name) {
  if (xt->shadow != 0) {
    fprintf(xo, "  sub rsp, %d\n", xt->shadow);
  }
  extcall(name);
  if (xt->shadow != 0) {
    fprintf(xo, "  add rsp, %d\n", xt->shadow);
  }
}

/**
 * @brief Sets AL for a variadic call with or without vector registers
 * @param uses_vector Non-zero when an xmm register carries an argument
 */
static void vararg_eax(int uses_vector) {
  fprintf(xo, "  mov eax, %d\n",
          uses_vector ? xt->vararg_eax_float : xt->vararg_eax_int);
}

/*__X862__*/

/**
 * @brief Emits a value-connection instruction (const/copy/conversion/load)
 * @param ins Instruction with a temp destination
 */
static void sel_move(IrInstr *ins) {
  int ti = ins->dst - xf->nvars;
  int dst_reg =
      (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg)
          ? xa->temps[ti].reg
          : -1;
  Op o = op_of(ins->v[0]);
  switch (ins->op) {
  case IR_CONST_I:
    if (dst_reg >= 0) {
      fprintf(xo, "  mov %s, %lld\n", reg_name(dst_reg), o.imm);
    } else if (imm32(o.imm)) {
      fprintf(xo, "  mov qword [rbp - %d], %lld\n", slot_off(ins->dst),
              o.imm);
    } else {
      fprintf(xo, "  mov rax, %lld\n", o.imm);
      store_slot(ins->dst);
    }
    break;
  case IR_CONST_F:
    if (dst_reg >= 0 && dst_reg < R_XMM0) {
      fprintf(xo, "  mov %s, 0x%llx\n", reg_name(dst_reg),
              (unsigned long long)o.imm);
    } else if (dst_reg >= R_XMM0) {
      fprintf(xo, "  mov rax, 0x%llx\n", (unsigned long long)o.imm);
      fprintf(xo, "  movq %s, rax\n", reg_name(dst_reg));
    } else {
      fprintf(xo, "  mov rax, 0x%llx\n", (unsigned long long)o.imm);
      store_slot(ins->dst);
    }
    break;
  case IR_CONST_S:
    if (dst_reg >= 0) {
      fprintf(xo, "  lea %s, [rel str%d]\n", reg_name(dst_reg), o.sidx);
    } else {
      fprintf(xo, "  lea rax, [rel str%d]\n", o.sidx);
      store_slot(ins->dst);
    }
    break;
  case IR_CONST_NULL:
    if (dst_reg >= 0 && dst_reg < R_XMM0) {
      fprintf(xo, "  xor %s, %s\n", reg_name32(dst_reg),
              reg_name32(dst_reg));
    } else if (dst_reg >= R_XMM0) {
      fprintf(xo, "  xorpd %s, %s\n", reg_name(dst_reg),
              reg_name(dst_reg));
    } else {
      fprintf(xo, "  mov qword [rbp - %d], 0\n", slot_off(ins->dst));
    }
    break;
  case IR_COPY:
    if (dst_reg >= 0 && dst_reg < R_XMM0) {
      load_reg(o, dst_reg);
    } else if (dst_reg >= R_XMM0) {
      if (o.is_reg && o.reg >= R_XMM0) {
        if (o.reg != dst_reg) {
          fprintf(xo, "  movapd %s, %s\n", reg_name(dst_reg),
                  reg_name(o.reg));
        }
      } else {
        load_xmm(o, dst_reg);
      }
    } else {
      load_rax(o);
      store_slot(ins->dst);
    }
    break;
  case IR_I2F:
    if (dst_reg >= R_XMM0) {
      if (o.is_mem) {
        fprintf(xo, "  cvtsi2sd %s, [rbp - %d]\n", reg_name(dst_reg),
                slot_off(o.slot));
      } else if (o.is_global) {
        fprintf(xo, "  cvtsi2sd %s, [rel global_%s]\n", reg_name(dst_reg),
                xm->globals[o.gidx].name);
      } else {
        load_rax(o);
        fprintf(xo, "  cvtsi2sd %s, rax\n", reg_name(dst_reg));
      }
    } else {
      load_rax(o);
      fprintf(xo, "  cvtsi2sd xmm0, rax\n");
      fprintf(xo, "  movq rax, xmm0\n");
      if (dst_reg >= 0) {
        fprintf(xo, "  mov %s, rax\n", reg_name(dst_reg));
      } else {
        store_slot(ins->dst);
      }
    }
    break;
  case IR_F2I:
    if (o.is_reg && o.reg >= R_XMM0) {
      fprintf(xo, "  cvttsd2si rax, %s\n", reg_name(o.reg));
    } else {
      load_xmm(o, R_XMM0);
      fprintf(xo, "  cvttsd2si rax, xmm0\n");
    }
    if (dst_reg >= 0) {
      fprintf(xo, "  mov %s, rax\n", reg_name(dst_reg));
    } else {
      store_slot(ins->dst);
    }
    break;
  case IR_TRUNC8:
    if (o.is_imm) {
      long long v = o.imm & 0xFF;
      if (dst_reg >= 0) {
        fprintf(xo, "  mov %s, %lld\n", reg_name(dst_reg), v);
      } else {
        fprintf(xo, "  mov qword [rbp - %d], %lld\n", slot_off(ins->dst),
                v);
      }
      break;
    }
    if (o.is_reg) {
      if (dst_reg >= 0) {
        fprintf(xo, "  movzx %s, %s\n", reg_name32(dst_reg),
                reg_name8(o.reg));
      } else {
        fprintf(xo, "  movzx eax, %s\n", reg_name8(o.reg));
        store_slot(ins->dst);
      }
    } else if (o.is_mem) {
      if (dst_reg >= 0) {
        fprintf(xo, "  movzx %s, byte [rbp - %d]\n", reg_name32(dst_reg),
                slot_off(o.slot));
      } else {
        fprintf(xo, "  movzx eax, byte [rbp - %d]\n", slot_off(o.slot));
        store_slot(ins->dst);
      }
    } else if (o.is_global) {
      if (dst_reg >= 0) {
        fprintf(xo, "  movzx %s, byte [rel global_%s]\n",
                reg_name32(dst_reg), xm->globals[o.gidx].name);
      } else {
        fprintf(xo, "  movzx eax, byte [rel global_%s]\n",
                xm->globals[o.gidx].name);
        store_slot(ins->dst);
      }
    }
    break;
  case IR_LOAD:
  case IR_LOADG: {
    const char *src;
    char buf[96];
    if (ins->op == IR_LOAD) {
      snprintf(buf, sizeof(buf), "[rbp - %d]", slot_off(ins->v[0].idx));
      src = buf;
    } else {
      snprintf(buf, sizeof(buf), "[rel global_%s]",
               xm->globals[ins->v[0].idx].name);
      src = buf;
    }
    if (dst_reg >= 0 && dst_reg < R_XMM0) {
      fprintf(xo, "  mov %s, %s\n", reg_name(dst_reg), src);
    } else if (dst_reg >= R_XMM0) {
      fprintf(xo, "  movq %s, %s\n", reg_name(dst_reg), src);
    } else {
      fprintf(xo, "  mov rax, %s\n", src);
      store_slot(ins->dst);
    }
    break;
  }
  default:
    break;
  }
}

/**
 * @brief Emits a store into a variable or global
 * @param ins Store instruction (v[0] is the destination)
 */
static void sel_store(IrInstr *ins) {
  Op v = op_of(ins->v[1]);
  if (ins->op == IR_STORE) {
    if (v.is_reg && v.reg >= R_XMM0) {
      fprintf(xo, "  movq [rbp - %d], %s\n", slot_off(ins->v[0].idx),
              reg_name(v.reg));
    } else if (v.is_reg) {
      fprintf(xo, "  mov [rbp - %d], %s\n", slot_off(ins->v[0].idx),
              reg_name(v.reg));
    } else if (v.is_imm && v.type != IR_FLOAT && imm32(v.imm)) {
      fprintf(xo, "  mov qword [rbp - %d], %lld\n", slot_off(ins->v[0].idx),
              v.imm);
    } else {
      load_rax(v);
      fprintf(xo, "  mov [rbp - %d], rax\n", slot_off(ins->v[0].idx));
    }
  } else {
    const char *name = xm->globals[ins->v[0].idx].name;
    if (v.is_reg && v.reg >= R_XMM0) {
      fprintf(xo, "  movq [rel global_%s], %s\n", name, reg_name(v.reg));
    } else if (v.is_reg) {
      fprintf(xo, "  mov [rel global_%s], %s\n", name, reg_name(v.reg));
    } else if (v.is_imm && v.type != IR_FLOAT && imm32(v.imm)) {
      fprintf(xo, "  mov qword [rel global_%s], %lld\n", name, v.imm);
    } else {
      load_rax(v);
      fprintf(xo, "  mov [rel global_%s], rax\n", name);
    }
  }
}

/**
 * @brief Emits an operand into rax/rbx for integer arithmetic
 * @param o Operand
 * @param to_rbx Non-zero to load rbx instead of rax
 */
static void arith_operand(Op o, int to_rbx) {
  const char *dst = to_rbx ? "rbx" : "rax";
  if (o.is_reg) {
    if ((to_rbx ? R_RBX : R_RAX) != o.reg) {
      fprintf(xo, "  mov %s, %s\n", dst, reg_name(o.reg));
    }
  } else if (o.is_mem) {
    fprintf(xo, "  mov %s, [rbp - %d]\n", dst, slot_off(o.slot));
  } else if (o.is_global) {
    fprintf(xo, "  mov %s, [rel global_%s]\n", dst,
            xm->globals[o.gidx].name);
  } else {
    fprintf(xo, "  mov %s, %lld\n", dst, o.imm);
  }
}

/**
 * @brief Emits integer arithmetic with overflow and divide traps
 * @param ins Arithmetic instruction (integer-typed)
 */
static void sel_arith_int(IrInstr *ins) {
  int ti = ins->dst - xf->nvars;
  int dst_reg =
      (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg)
          ? xa->temps[ti].reg
          : -1;
  Op l = op_of(ins->v[0]);
  Op r = op_of(ins->v[1]);
  const char *opname = "add";
  if (ins->op == IR_SUB) {
    opname = "sub";
  } else if (ins->op == IR_MUL) {
    opname = "imul";
  }
  if (ins->op == IR_DIV || ins->op == IR_MOD) {
    arith_operand(r, 1);
    arith_operand(l, 0);
    fprintf(xo, "  test rbx, rbx\n");
    fprintf(xo, "  jz divzero_trap\n");
    fprintf(xo, "  cqo\n");
    fprintf(xo, "  idiv rbx\n");
    if (ins->op == IR_MOD) {
      fprintf(xo, "  mov rax, rdx\n");
    }
    if (dst_reg >= 0) {
      fprintf(xo, "  mov %s, rax\n", reg_name(dst_reg));
    } else {
      store_slot(ins->dst);
    }
    return;
  }
  /* Destination-register form computes in place. */
  if (dst_reg >= 0) {
    if (r.is_reg && r.reg == dst_reg) {
      load_reg(l, R_RAX);
      fprintf(xo, "  %s rax, %s\n", opname, reg_name(dst_reg));
      fprintf(xo, "  mov %s, rax\n", reg_name(dst_reg));
    } else {
      if (!(l.is_reg && l.reg == dst_reg)) {
        load_reg(l, dst_reg);
      }
      if (r.is_imm && imm32(r.imm)) {
        fprintf(xo, "  %s %s, %lld\n", opname, reg_name(dst_reg), r.imm);
      } else if (r.is_mem) {
        fprintf(xo, "  %s %s, [rbp - %d]\n", opname, reg_name(dst_reg),
                slot_off(r.slot));
      } else if (r.is_global) {
        fprintf(xo, "  %s %s, [rel global_%s]\n", opname,
                reg_name(dst_reg), xm->globals[r.gidx].name);
      } else {
        load_reg(r, R_RBX);
        fprintf(xo, "  %s %s, rbx\n", opname, reg_name(dst_reg));
      }
    }
    fprintf(xo, "  jo overflow_trap\n");
    return;
  }
  /* Memory-destination form for same-address stores (no load/store). */
  if (l.is_mem && ins->dst == l.slot &&
      (ins->op == IR_ADD || ins->op == IR_SUB)) {
    if (r.is_imm && imm32(r.imm)) {
      fprintf(xo, "  %s qword [rbp - %d], %lld\n", opname, slot_off(l.slot),
              r.imm);
      fprintf(xo, "  jo overflow_trap\n");
      return;
    }
    if (r.is_reg) {
      fprintf(xo, "  %s qword [rbp - %d], %s\n", opname, slot_off(l.slot),
              reg_name(r.reg));
      fprintf(xo, "  jo overflow_trap\n");
      return;
    }
  }
  arith_operand(r, 1);
  arith_operand(l, 0);
  fprintf(xo, "  %s rax, rbx\n", opname);
  fprintf(xo, "  jo overflow_trap\n");
  store_slot(ins->dst);
}

/**
 * @brief Emits float arithmetic (double bits)
 * @param ins Arithmetic instruction (float-typed)
 */
static void sel_arith_float(IrInstr *ins) {
  int ti = ins->dst - xf->nvars;
  int dst_reg =
      (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg)
          ? xa->temps[ti].reg
          : -1;
  Op l = op_of(ins->v[0]);
  Op r = op_of(ins->v[1]);
  const char *opname = "addsd";
  if (ins->op == IR_SUB) {
    opname = "subsd";
  } else if (ins->op == IR_MUL) {
    opname = "mulsd";
  } else if (ins->op == IR_DIV) {
    opname = "divsd";
  }
  int xd = (dst_reg >= R_XMM0) ? dst_reg : R_XMM0;
  load_xmm(r, R_XMM1);
  load_xmm(l, xd);
  fprintf(xo, "  %s %s, xmm1\n", opname, reg_name(xd));
  if (dst_reg >= R_XMM0) {
    if (dst_reg != xd) {
      fprintf(xo, "  movapd %s, %s\n", reg_name(dst_reg), reg_name(xd));
    }
  } else if (dst_reg >= 0) {
    fprintf(xo, "  movq %s, %s\n", reg_name(dst_reg), reg_name(xd));
  } else {
    fprintf(xo, "  movq rax, %s\n", reg_name(xd));
    store_slot(ins->dst);
  }
}

/**
 * @brief Emits bitwise integer ops (no overflow traps: raw 64-bit patterns)
 * @param ins Bitwise instruction (integer-typed, shifts mask counts to 0-63)
 * @details Variable shift counts use cl with rcx saved/restored around the
 * sequence (rcx may hold a live temp); no calls occur between push and pop.
 */
static void sel_bits_int(IrInstr *ins) {
  int ti = ins->dst - xf->nvars;
  int dst_reg =
      (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg)
          ? xa->temps[ti].reg
          : -1;
  Op l = op_of(ins->v[0]);
  if (ins->op == IR_NOT) {
    if (dst_reg >= 0) {
      load_reg(l, dst_reg);
      fprintf(xo, "  not %s\n", reg_name(dst_reg));
    } else if (l.is_mem && l.slot == ins->dst) {
      fprintf(xo, "  not qword [rbp - %d]\n", slot_off(l.slot));
    } else {
      load_rax(l);
      fprintf(xo, "  not rax\n");
      store_slot(ins->dst);
    }
    return;
  }
  Op r = op_of(ins->v[1]);
  const char *opname = "and";
  if (ins->op == IR_OR) {
    opname = "or";
  } else if (ins->op == IR_XOR) {
    opname = "xor";
  } else if (ins->op == IR_SHL) {
    opname = "shl";
  } else if (ins->op == IR_SHR) {
    opname = "sar";
  }
  int is_shift = ins->op == IR_SHL || ins->op == IR_SHR;
  long long count = r.is_imm ? (r.imm & 63) : 0;
  /* Destination-register form computes in place. */
  if (dst_reg >= 0) {
    if (is_shift) {
      fprintf(xo, "  push rcx\n");
      if (dst_reg == R_RCX) {
        load_rax(l);
        load_reg(r, R_RCX);
        fprintf(xo, "  %s rax, cl\n", opname);
        fprintf(xo, "  pop rcx\n");
        fprintf(xo, "  mov rcx, rax\n");
      } else {
        if (!(l.is_reg && l.reg == dst_reg)) {
          load_reg(l, dst_reg);
        }
        load_reg(r, R_RCX);
        fprintf(xo, "  %s %s, cl\n", opname, reg_name(dst_reg));
        fprintf(xo, "  pop rcx\n");
      }
      return;
    }
    if (r.is_reg && r.reg == dst_reg) {
      load_reg(l, R_RAX);
      fprintf(xo, "  %s rax, %s\n", opname, reg_name(dst_reg));
      fprintf(xo, "  mov %s, rax\n", reg_name(dst_reg));
    } else {
      if (!(l.is_reg && l.reg == dst_reg)) {
        load_reg(l, dst_reg);
      }
      if (r.is_imm && imm32(r.imm)) {
        fprintf(xo, "  %s %s, %lld\n", opname, reg_name(dst_reg), r.imm);
      } else if (r.is_mem) {
        fprintf(xo, "  %s %s, [rbp - %d]\n", opname, reg_name(dst_reg),
                slot_off(r.slot));
      } else if (r.is_global) {
        fprintf(xo, "  %s %s, [rel global_%s]\n", opname,
                reg_name(dst_reg), xm->globals[r.gidx].name);
      } else {
        load_reg(r, R_RBX);
        fprintf(xo, "  %s %s, rbx\n", opname, reg_name(dst_reg));
      }
    }
    return;
  }
  /* Memory-destination form for same-address stores (no load/store). */
  if (l.is_mem && ins->dst == l.slot) {
    if (r.is_imm && (is_shift || imm32(r.imm))) {
      fprintf(xo, "  %s qword [rbp - %d], %lld\n", opname, slot_off(l.slot),
              is_shift ? count : r.imm);
      return;
    }
    if (!is_shift && r.is_reg) {
      fprintf(xo, "  %s qword [rbp - %d], %s\n", opname, slot_off(l.slot),
              reg_name(r.reg));
      return;
    }
  }
  if (is_shift && !r.is_imm) {
    /* Variable count through cl; rcx may hold a live temp. */
    fprintf(xo, "  push rcx\n");
    load_rax(l);
    load_reg(r, R_RCX);
    fprintf(xo, "  %s rax, cl\n", opname);
    fprintf(xo, "  pop rcx\n");
    store_slot(ins->dst);
    return;
  }
  arith_operand(r, 1);
  arith_operand(l, 0);
  if (is_shift) {
    fprintf(xo, "  mov rcx, rbx\n");
    fprintf(xo, "  %s rax, cl\n", opname);
  } else {
    fprintf(xo, "  %s rax, rbx\n", opname);
  }
  store_slot(ins->dst);
}

/**
 * @brief Emits an integer setcc sequence with 0/1 result
 * @param cond Comparison condition
 * @param dst_reg Destination register, or -1 for a frame slot
 * @param dst_slot Frame slot when dst_reg is -1
 */
static void emit_setcc(int cond, int dst_reg, int dst_slot) {
  if (dst_reg >= 0) {
    if (cond == IR_CEQ) {
      fprintf(xo, "  sete %s\n", reg_name8(dst_reg));
    } else if (cond == IR_CNE) {
      fprintf(xo, "  setne %s\n", reg_name8(dst_reg));
    } else if (cond == IR_CLT) {
      fprintf(xo, "  setl %s\n", reg_name8(dst_reg));
    } else if (cond == IR_CGT) {
      fprintf(xo, "  setg %s\n", reg_name8(dst_reg));
    } else if (cond == IR_CLE) {
      fprintf(xo, "  setle %s\n", reg_name8(dst_reg));
    } else {
      fprintf(xo, "  setge %s\n", reg_name8(dst_reg));
    }
    fprintf(xo, "  movzx %s, %s\n", reg_name(dst_reg),
            reg_name8(dst_reg));
  } else {
    if (cond == IR_CEQ) {
      fprintf(xo, "  sete al\n");
    } else if (cond == IR_CNE) {
      fprintf(xo, "  setne al\n");
    } else if (cond == IR_CLT) {
      fprintf(xo, "  setl al\n");
    } else if (cond == IR_CGT) {
      fprintf(xo, "  setg al\n");
    } else if (cond == IR_CLE) {
      fprintf(xo, "  setle al\n");
    } else {
      fprintf(xo, "  setge al\n");
    }
    fprintf(xo, "  movzx eax, al\n");
    fprintf(xo, "  mov [rbp - %d], rax\n", slot_off(dst_slot));
  }
}

/**
 * @brief Emits a comparison with 0/1 result
 * @param ins Comparison instruction
 */
/**
 * @brief Emits a string comparison, flags set from strcmp
 * @param l Left operand
 * @param r Right operand
 */
static void cmp_str_operands(Op l, Op r) {
  load_rax(l);
  fprintf(xo, "  push rax\n");
  load_rax(r);
  fprintf(xo, "  mov rbx, rax\n");
  fprintf(xo, "  pop rax\n");
  fprintf(xo, "  mov %s, rax\n", areg(0));
  fprintf(xo, "  mov %s, rbx\n", areg(1));
  gen_runtime_prologue();
  extcall("strcmp");
  gen_runtime_epilogue();
  fprintf(xo, "  test eax, eax\n");
}

/**
 * @brief Emits a float comparison, flags set from ucomisd
 * @param l Left operand
 * @param r Right operand
 */
static void cmp_float_operands(Op l, Op r) {
  load_xmm(l, R_XMM0);
  load_xmm(r, R_XMM1);
  fprintf(xo, "  ucomisd xmm0, xmm1\n");
}

/**
 * @brief Emits an integer comparison, flags set from cmp
 * @param l Left operand
 * @param r Right operand
 */
static void cmp_int_operands(Op l, Op r) {
  if (l.is_reg) {
    if (r.is_imm && imm32(r.imm)) {
      fprintf(xo, "  cmp %s, %lld\n", reg_name(l.reg), r.imm);
    } else if (r.is_mem) {
      fprintf(xo, "  cmp %s, [rbp - %d]\n", reg_name(l.reg),
              slot_off(r.slot));
    } else if (r.is_global) {
      fprintf(xo, "  cmp %s, [rel global_%s]\n", reg_name(l.reg),
              xm->globals[r.gidx].name);
    } else {
      load_reg(r, R_RBX);
      fprintf(xo, "  cmp %s, rbx\n", reg_name(l.reg));
    }
  } else {
    load_rax(l);
    if (r.is_imm && imm32(r.imm)) {
      fprintf(xo, "  cmp rax, %lld\n", r.imm);
    } else {
      load_reg(r, R_RBX);
      fprintf(xo, "  cmp rax, rbx\n");
    }
  }
}

static void sel_cmp(IrInstr *ins) {
  int ti = ins->dst - xf->nvars;
  int dst_reg =
      (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg)
          ? xa->temps[ti].reg
          : -1;
  Op l = op_of(ins->v[0]);
  Op r = op_of(ins->v[1]);
  if (ins->type == IR_STR) {
    cmp_str_operands(l, r);
    if (dst_reg >= 0) {
      emit_setcc(ins->cond, dst_reg, 0);
    } else {
      emit_setcc(ins->cond, -1, ins->dst);
    }
    return;
  }
  if (ins->type == IR_FLOAT) {
    cmp_float_operands(l, r);
    if (ins->cond == IR_CEQ) {
      fprintf(xo, "  sete al\n");
      fprintf(xo, "  setnp bl\n");
      fprintf(xo, "  and al, bl\n");
    } else if (ins->cond == IR_CNE) {
      fprintf(xo, "  setne al\n");
      fprintf(xo, "  setp bl\n");
      fprintf(xo, "  or al, bl\n");
    } else if (ins->cond == IR_CLT) {
      fprintf(xo, "  setb al\n");
      fprintf(xo, "  setnp bl\n");
      fprintf(xo, "  and al, bl\n");
    } else if (ins->cond == IR_CGT) {
      fprintf(xo, "  seta al\n");
    } else if (ins->cond == IR_CLE) {
      fprintf(xo, "  setbe al\n");
      fprintf(xo, "  setnp bl\n");
      fprintf(xo, "  and al, bl\n");
    } else {
      fprintf(xo, "  setae al\n");
    }
    fprintf(xo, "  movzx eax, al\n");
    if (dst_reg >= 0) {
      fprintf(xo, "  mov %s, rax\n", reg_name(dst_reg));
    } else {
      store_slot(ins->dst);
    }
    return;
  }
  cmp_int_operands(l, r);
  if (dst_reg >= 0) {
    emit_setcc(ins->cond, dst_reg, 0);
  } else {
    emit_setcc(ins->cond, -1, ins->dst);
  }
}

/**
 * @brief Emits a fused compare-and-branch (no materialized boolean)
 * @param fi Function index for labels
 * @param cmp Comparison instruction
 * @param br Branch instruction consuming the comparison
 * @param next Layout-next block id (-1 when none)
 */
static void sense_int_jcc(int fi, int cond, int t, int f, int next);
static void sense_float_jcc(int fi, int cond, int t, int f, int next);
static int sel_fused(int fi, IrInstr *cmp, IrInstr *br, int next) {
  Op l = op_of(cmp->v[0]);
  Op r = op_of(cmp->v[1]);
  if (cmp->type == IR_STR) {
    cmp_str_operands(l, r);
    sense_int_jcc(fi, cmp->cond, br->t, br->f, next);
  } else if (cmp->type == IR_FLOAT) {
    cmp_float_operands(l, r);
    sense_float_jcc(fi, cmp->cond, br->t, br->f, next);
  } else {
    cmp_int_operands(l, r);
    sense_int_jcc(fi, cmp->cond, br->t, br->f, next);
  }
  return 1;
}

/**
 * @brief Maps a function name to its assembly label
 * @param name Function name from source
 * @return Assembly label (fn main lives at jot_main, entry stays main)
 */
static const char *func_label(const char *name) {
  if (xm->has_user_main && strcmp(name, "main") == 0) {
    return "jot_main";
  }
  return name;
}

/**
 * @brief Emits a call frame: spill operands, load registers, call
 * @param ins Call instruction (v[0] is the receiver for methods)
 * @param is_method Non-zero for vtable dispatch
 */
static void sel_call(IrInstr *ins, int is_method) {
  int first = is_method ? 1 : 0;
  int total = first + ins->nlist;
  int nregs = total < xt->nargs_regs ? total : xt->nargs_regs;
  /* Lean path: no operand lives in an argument register, so loading
     registers directly cannot clobber a not-yet-read value. */
  int lean = 1;
  if (is_method) {
    Op recv = op_of(ins->v[0]);
    if (recv.is_reg) {
      for (int r = 0; r < nregs; r++) {
        if (recv.reg == xt->arg_regs[r]) {
          lean = 0;
          break;
        }
      }
    }
  }
  for (int k = 0; k < ins->nlist && lean; k++) {
    Op a = op_of(ins->list[k]);
    if (a.is_reg) {
      for (int r = 0; r < nregs; r++) {
        if (a.reg == xt->arg_regs[r]) {
          lean = 0;
          break;
        }
      }
    }
  }
  int frame, i, rhome;
  if (lean) {
    int nstack = total > xt->nargs_regs ? total - xt->nargs_regs : 0;
    if (xt->shadow != 0) {
      frame = 32 + 8 * nstack;
      if (frame % 16 != 0) {
        frame += 8;
      }
      rhome = -1;
      fprintf(xo, "  sub rsp, %d\n", frame);
      if (is_method) {
        load_reg(op_of(ins->v[0]), xt->arg_regs[0]);
        fprintf(xo, "  test %s, %s\n", areg(0), areg(0));
        fprintf(xo, "  jz null_trap\n");
        i = 1;
      } else {
        i = 0;
      }
      for (int k = 0; k < ins->nlist; k++, i++) {
        if (i < xt->nargs_regs) {
          load_reg(op_of(ins->list[k]), xt->arg_regs[i]);
        } else {
          load_rax(op_of(ins->list[k]));
          fprintf(xo, "  mov [rsp + %d], rax\n", 32 + 8 * (i - 4));
        }
      }
    } else {
      frame = 8 * nstack;
      if (frame % 16 != 0) {
        frame += 8;
      }
      rhome = -1;
      if (frame > 0) {
        fprintf(xo, "  sub rsp, %d\n", frame);
      }
      if (is_method) {
        load_reg(op_of(ins->v[0]), xt->arg_regs[0]);
        fprintf(xo, "  test %s, %s\n", areg(0), areg(0));
        fprintf(xo, "  jz null_trap\n");
        i = 1;
      } else {
        i = 0;
      }
      for (int k = 0; k < ins->nlist; k++, i++) {
        if (i < xt->nargs_regs) {
          load_reg(op_of(ins->list[k]), xt->arg_regs[i]);
        } else {
          load_rax(op_of(ins->list[k]));
          fprintf(xo, "  mov [rsp + %d], rax\n", 8 * (i - 6));
        }
      }
    }
    if (is_method) {
      fprintf(xo, "  mov rax, %s\n", areg(0));
      fprintf(xo, "  mov rax, [rax - 8]\n");
      fprintf(xo, "  lea r11, [rel jot_vtables]\n");
      fprintf(xo, "  mov r11, [r11 + rax*8]\n");
      fprintf(xo, "  mov rax, [r11 + %d]\n", ins->callee * 8);
      fprintf(xo, "  call rax\n");
    } else {
      fprintf(xo, "  call %s\n", func_label(ins->name));
    }
    if (frame > 0) {
      fprintf(xo, "  add rsp, %d\n", frame);
    }
    if (ins->dst >= 0) {
      int ti = ins->dst - xf->nvars;
      if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
        int reg = xa->temps[ti].reg;
        if (reg >= R_XMM0) {
          fprintf(xo, "  movq %s, rax\n", reg_name(reg));
        } else {
          fprintf(xo, "  mov %s, rax\n", reg_name(reg));
        }
      } else {
        store_slot(ins->dst);
      }
    }
    return;
  }
  if (xt->shadow != 0) {
    frame = 64 + 8 * (total > 4 ? total - 4 : 0);
    if (frame % 16 != 0) {
      frame += 8;
    }
    rhome = frame - 32;
    fprintf(xo, "  sub rsp, %d\n", frame);
    if (is_method) {
      load_rax(op_of(ins->v[0]));
      fprintf(xo, "  test rax, rax\n");
      fprintf(xo, "  jz null_trap\n");
      fprintf(xo, "  mov [rsp + %d], rax\n", frame - 32);
      i = 1;
    } else {
      i = 0;
    }
    for (int k = 0; k < ins->nlist; k++, i++) {
      load_rax(op_of(ins->list[k]));
      if (i < 4) {
        fprintf(xo, "  mov [rsp + %d], rax\n", frame - 32 + 8 * i);
      } else {
        fprintf(xo, "  mov [rsp + %d], rax\n", 32 + 8 * (i - 4));
      }
    }
    for (i = 0; i < total && i < 4; i++) {
      fprintf(xo, "  mov %s, [rsp + %d]\n", areg(i), frame - 32 + 8 * i);
    }
  } else {
    int nstack = total > 6 ? total - 6 : 0;
    int nhome = total < 6 ? total : 6;
    frame = nhome * 8 + nstack * 8;
    if (frame % 16 != 0) {
      frame += 8;
    }
    rhome = nstack * 8;
    if (frame > 0) {
      fprintf(xo, "  sub rsp, %d\n", frame);
    }
    if (is_method) {
      load_rax(op_of(ins->v[0]));
      fprintf(xo, "  test rax, rax\n");
      fprintf(xo, "  jz null_trap\n");
      fprintf(xo, "  mov [rsp + %d], rax\n", nstack * 8);
      i = 1;
    } else {
      i = 0;
    }
    for (int k = 0; k < ins->nlist; k++, i++) {
      load_rax(op_of(ins->list[k]));
      if (i < 6) {
        fprintf(xo, "  mov [rsp + %d], rax\n", nstack * 8 + 8 * i);
      } else {
        fprintf(xo, "  mov [rsp + %d], rax\n", 8 * (i - 6));
      }
    }
    for (i = 0; i < total && i < 6; i++) {
      fprintf(xo, "  mov %s, [rsp + %d]\n", areg(i), nstack * 8 + 8 * i);
    }
  }
  if (is_method) {
    /* Virtual dispatch: class id header -> class vtable -> method. */
    fprintf(xo, "  mov rax, [rsp + %d]\n", rhome);
    fprintf(xo, "  mov rax, [rax - 8]\n");
    fprintf(xo, "  lea r11, [rel jot_vtables]\n");
    fprintf(xo, "  mov r11, [r11 + rax*8]\n");
    fprintf(xo, "  mov rax, [r11 + %d]\n", ins->callee * 8);
    fprintf(xo, "  call rax\n");
  } else {
    fprintf(xo, "  call %s\n", func_label(ins->name));
  }
  if (frame > 0) {
    fprintf(xo, "  add rsp, %d\n", frame);
  }
  if (ins->dst >= 0) {
    int ti = ins->dst - xf->nvars;
    if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
      int reg = xa->temps[ti].reg;
      if (reg >= R_XMM0) {
        fprintf(xo, "  movq %s, rax\n", reg_name(reg));
      } else {
        fprintf(xo, "  mov %s, rax\n", reg_name(reg));
      }
    } else {
      store_slot(ins->dst);
    }
  }
}

/**
 * @brief Emits an input call with an optional prompt
 * @param ins Input instruction (list[0] is the prompt when present)
 */
static void sel_input(IrInstr *ins) {
  if (ins->nlist > 0) {
    load_rax(op_of(ins->list[0]));
    fprintf(xo, "  mov %s, rax\n", areg(1));
    fprintf(xo, "  lea %s, [rel fmt_str]\n", areg(0));
    gen_runtime_prologue();
    extcall("printf");
    gen_runtime_epilogue();
    fprintf(xo, "  xor %s, %s\n", reg_name32(xt->arg_regs[0]),
            reg_name32(xt->arg_regs[0]));
    gen_runtime_prologue();
    extcall("fflush");
    gen_runtime_epilogue();
  }
  fprintf(xo, "  mov rax, rsp\n");
  fprintf(xo, "  and rax, 8\n");
  fprintf(xo, "  sub rsp, rax\n");
  fprintf(xo, "  sub rsp, 48\n");
  fprintf(xo, "  mov [rsp + 32], rax\n");
  fprintf(xo, "  mov dword [rsp + 40], 0\n");
  fprintf(xo, "  lea %s, [rel fmt_input]\n", areg(0));
  fprintf(xo, "  lea %s, [rsp + 40]\n", areg(1));
  extcall("scanf");
  fprintf(xo, "  cmp rax, 1\n");
  fprintf(xo, "  jne input_error_trap\n");
  fprintf(xo, "  movsxd rax, dword [rsp + 40]\n");
  fprintf(xo, "  mov r10, [rsp + 32]\n");
  fprintf(xo, "  add rsp, 48\n");
  fprintf(xo, "  add rsp, r10\n");
  int ti = ins->dst - xf->nvars;
  if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
    fprintf(xo, "  mov %s, rax\n", reg_name(xa->temps[ti].reg));
  } else {
    store_slot(ins->dst);
  }
}

/**
 * @brief Emits a string length read
 * @param ins Length instruction
 */
static void sel_len(IrInstr *ins) {
  load_rax(op_of(ins->list[0]));
  fprintf(xo, "  mov %s, rax\n", areg(0));
  gen_runtime_prologue();
  extcall("strlen");
  gen_runtime_epilogue();
  int ti = ins->dst - xf->nvars;
  if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
    fprintf(xo, "  mov %s, rax\n", reg_name(xa->temps[ti].reg));
  } else {
    store_slot(ins->dst);
  }
}

/**
 * @brief Emits tostr (formats into a malloc'd buffer)
 * @param ins Tostr instruction
 */
static void sel_to_str(IrInstr *ins) {
  Op a = op_of(ins->list[0]);
  int is_float = a.type == IR_FLOAT;
  load_rax(a);
  fprintf(xo, "  sub rsp, 64\n");
  fprintf(xo, "  mov [rsp + 0], rax\n");
  fprintf(xo, "  mov %s, 64\n", areg(0));
  gen_runtime_prologue();
  extcall("malloc");
  gen_runtime_epilogue();
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz alloc_trap\n");
  fprintf(xo, "  mov [rsp + 8], rax\n");
  fprintf(xo, "  mov %s, [rsp + 8]\n", areg(0));
  fprintf(xo, "  mov %s, 64\n", areg(1));
  fprintf(xo, "  lea %s, [rel %s]\n", areg(2),
          is_float ? "fmt_float_raw" : "fmt_int_raw");
  if (is_float) {
    fprintf(xo, "  mov %s, [rsp + 0]\n", areg(3));
    fprintf(xo, "  movq xmm3, [rsp + 0]\n");
    vararg_eax(1);
  } else {
    fprintf(xo, "  mov %s, [rsp + 0]\n", areg(3));
    vararg_eax(0);
  }
  fprintf(xo, "  mov [rsp + 16], rax\n");
  gen_runtime_prologue();
  extcall("snprintf");
  gen_runtime_epilogue();
  fprintf(xo, "  mov rax, [rsp + 8]\n");
  fprintf(xo, "  add rsp, 64\n");
  int ti = ins->dst - xf->nvars;
  if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
    fprintf(xo, "  mov %s, rax\n", reg_name(xa->temps[ti].reg));
  } else {
    store_slot(ins->dst);
  }
}

/**
 * @brief Emits tonum (parses a string into a double)
 * @param ins Tonum instruction
 */
static void sel_to_num(IrInstr *ins) {
  load_rax(op_of(ins->list[0]));
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz null_trap\n");
  fprintf(xo, "  mov %s, rax\n", areg(0));
  fprintf(xo, "  xor %s, %s\n", reg_name32(xt->arg_regs[1]),
          reg_name32(xt->arg_regs[1]));
  gen_runtime_prologue();
  extcall("strtod");
  gen_runtime_epilogue();
  fprintf(xo, "  movq rax, xmm0\n");
  int ti = ins->dst - xf->nvars;
  if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
    int reg = xa->temps[ti].reg;
    if (reg >= R_XMM0) {
      fprintf(xo, "  movq %s, rax\n", reg_name(reg));
    } else {
      fprintf(xo, "  mov %s, rax\n", reg_name(reg));
    }
  } else {
    store_slot(ins->dst);
  }
}

/**
 * @brief Emits readFile (whole file into a heap string)
 * @param ins Readfile instruction
 */
static void sel_read_file(IrInstr *ins) {
  load_rax(op_of(ins->list[0]));
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz null_trap\n");
  fprintf(xo, "  sub rsp, 64\n");
  fprintf(xo, "  mov [rsp + 0], rax\n");
  fprintf(xo, "  mov %s, [rsp + 0]\n", areg(0));
  fprintf(xo, "  lea %s, [rel fmt_open_r]\n", areg(1));
  gen_runtime_prologue();
  extcall("fopen");
  gen_runtime_epilogue();
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz open_trap\n");
  fprintf(xo, "  mov [rsp + 8], rax\n");
  fprintf(xo, "  mov %s, [rsp + 8]\n", areg(0));
  fprintf(xo, "  xor %s, %s\n", reg_name32(xt->arg_regs[1]),
          reg_name32(xt->arg_regs[1]));
  fprintf(xo, "  mov %s, 2\n", areg(2));
  gen_runtime_prologue();
  extcall("fseek");
  gen_runtime_epilogue();
  fprintf(xo, "  mov %s, [rsp + 8]\n", areg(0));
  gen_runtime_prologue();
  extcall("ftell");
  gen_runtime_epilogue();
  fprintf(xo, "  movsxd rax, eax\n");
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  js open_trap\n");
  fprintf(xo, "  mov [rsp + 16], rax\n");
  fprintf(xo, "  mov %s, [rsp + 16]\n", areg(0));
  fprintf(xo, "  add %s, 1\n", areg(0));
  fprintf(xo, "  jo overflow_trap\n");
  gen_runtime_prologue();
  extcall("malloc");
  gen_runtime_epilogue();
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz alloc_trap\n");
  fprintf(xo, "  mov [rsp + 24], rax\n");
  fprintf(xo, "  mov %s, [rsp + 8]\n", areg(0));
  fprintf(xo, "  xor %s, %s\n", reg_name32(xt->arg_regs[1]),
          reg_name32(xt->arg_regs[1]));
  fprintf(xo, "  xor %s, %s\n", reg_name32(xt->arg_regs[2]),
          reg_name32(xt->arg_regs[2]));
  gen_runtime_prologue();
  extcall("fseek");
  gen_runtime_epilogue();
  fprintf(xo, "  mov %s, [rsp + 24]\n", areg(0));
  fprintf(xo, "  mov %s, 1\n", areg(1));
  fprintf(xo, "  mov %s, [rsp + 16]\n", areg(2));
  fprintf(xo, "  mov %s, [rsp + 8]\n", areg(3));
  gen_runtime_prologue();
  extcall("fread");
  gen_runtime_epilogue();
  fprintf(xo, "  mov %s, [rsp + 8]\n", areg(0));
  gen_runtime_prologue();
  extcall("fclose");
  gen_runtime_epilogue();
  fprintf(xo, "  mov %s, [rsp + 24]\n", areg(0));
  fprintf(xo, "  mov %s, [rsp + 16]\n", areg(1));
  fprintf(xo, "  mov byte [%s + %s], 0\n", areg(0), areg(1));
  fprintf(xo, "  mov rax, [rsp + 24]\n");
  fprintf(xo, "  add rsp, 64\n");
  int ti = ins->dst - xf->nvars;
  if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
    fprintf(xo, "  mov %s, rax\n", reg_name(xa->temps[ti].reg));
  } else {
    store_slot(ins->dst);
  }
}

/**
 * @brief Emits writeFile (writes text, result is bytes written)
 * @param ins Writefile instruction
 */
static void sel_write_file(IrInstr *ins) {
  load_rax(op_of(ins->list[0]));
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz null_trap\n");
  fprintf(xo, "  sub rsp, 64\n");
  fprintf(xo, "  mov [rsp + 0], rax\n");
  load_rax(op_of(ins->list[1]));
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz null_trap\n");
  fprintf(xo, "  mov [rsp + 8], rax\n");
  fprintf(xo, "  mov %s, [rsp + 8]\n", areg(0));
  gen_runtime_prologue();
  extcall("strlen");
  gen_runtime_epilogue();
  fprintf(xo, "  mov [rsp + 16], rax\n");
  fprintf(xo, "  mov %s, [rsp + 0]\n", areg(0));
  fprintf(xo, "  lea %s, [rel fmt_open_w]\n", areg(1));
  gen_runtime_prologue();
  extcall("fopen");
  gen_runtime_epilogue();
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz open_trap\n");
  fprintf(xo, "  mov [rsp + 24], rax\n");
  fprintf(xo, "  mov %s, [rsp + 8]\n", areg(0));
  fprintf(xo, "  mov %s, 1\n", areg(1));
  fprintf(xo, "  mov %s, [rsp + 16]\n", areg(2));
  fprintf(xo, "  mov %s, [rsp + 24]\n", areg(3));
  gen_runtime_prologue();
  extcall("fwrite");
  gen_runtime_epilogue();
  fprintf(xo, "  mov [rsp + 32], rax\n");
  fprintf(xo, "  mov %s, [rsp + 24]\n", areg(0));
  gen_runtime_prologue();
  extcall("fclose");
  gen_runtime_epilogue();
  fprintf(xo, "  mov rax, [rsp + 32]\n");
  fprintf(xo, "  add rsp, 64\n");
  int ti = ins->dst - xf->nvars;
  if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
    fprintf(xo, "  mov %s, rax\n", reg_name(xa->temps[ti].reg));
  } else {
    store_slot(ins->dst);
  }
}

/**
 * @brief Emits char (code to 1-byte heap string, traps outside 0-255)
 * @param ins Chr instruction (list[0] is the code)
 * @details Float codes truncate toward zero first (NaN and out-of-range
 * magnitudes trap via the range check, matching cvttsd2si saturation).
 */
static void sel_chr(IrInstr *ins) {
  Op a = op_of(ins->list[0]);
  if (a.type == IR_FLOAT) {
    load_xmm(a, R_XMM0);
    fprintf(xo, "  cvttsd2si rax, xmm0\n");
  } else {
    load_rax(a);
  }
  fprintf(xo, "  cmp rax, 0\n");
  fprintf(xo, "  jl char_trap\n");
  fprintf(xo, "  cmp rax, 255\n");
  fprintf(xo, "  jg char_trap\n");
  fprintf(xo, "  sub rsp, 64\n");
  fprintf(xo, "  mov [rsp + 0], rax\n");
  fprintf(xo, "  mov %s, 2\n", areg(0));
  gen_runtime_prologue();
  extcall("malloc");
  gen_runtime_epilogue();
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz alloc_trap\n");
  fprintf(xo, "  mov rbx, [rsp + 0]\n");
  fprintf(xo, "  mov [rax], bl\n");
  fprintf(xo, "  mov byte [rax + 1], 0\n");
  fprintf(xo, "  add rsp, 64\n");
  int ti = ins->dst - xf->nvars;
  if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
    fprintf(xo, "  mov %s, rax\n", reg_name(xa->temps[ti].reg));
  } else {
    store_slot(ins->dst);
  }
}

/** Sequence counter for per-site loop labels (args array builder) */
static int x_seq = 0;

/**
 * @brief Emits args (command-line arguments to a heap string array)
 * @param ins Args instruction (no operands; reads jot_argc/jot_argv)
 * @details Duplicates every argv entry into fresh heap strings so the
 * array owns its memory; element 0 is the program name per C convention.
 */
static void sel_args(IrInstr *ins) {
  int seq = x_seq++;
  fprintf(xo, "  mov rax, [rel jot_argc]\n");
  fprintf(xo, "  mov rbx, [rel jot_argv]\n");
  fprintf(xo, "  sub rsp, 64\n");
  fprintf(xo, "  mov [rsp + 0], rax\n");
  fprintf(xo, "  mov [rsp + 8], rbx\n");
  fprintf(xo, "  mov rax, [rsp + 0]\n");
  fprintf(xo, "  add rax, 1\n");
  fprintf(xo, "  jo overflow_trap\n");
  fprintf(xo, "  shl rax, 3\n");
  fprintf(xo, "  mov %s, rax\n", areg(0));
  gen_runtime_prologue();
  extcall("malloc");
  gen_runtime_epilogue();
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz alloc_trap\n");
  fprintf(xo, "  mov [rsp + 16], rax\n");
  fprintf(xo, "  mov r10, [rsp + 0]\n");
  fprintf(xo, "  mov [rax], r10\n");
  fprintf(xo, "  mov qword [rsp + 24], 0\n");
  fprintf(xo, "args_loop%d:\n", seq);
  fprintf(xo, "  mov r10, [rsp + 24]\n");
  fprintf(xo, "  cmp r10, [rsp + 0]\n");
  fprintf(xo, "  jge args_done%d\n", seq);
  fprintf(xo, "  mov r11, [rsp + 8]\n");
  fprintf(xo, "  mov rax, [r11 + r10*8]\n");
  fprintf(xo, "  mov [rsp + 32], rax\n");
  fprintf(xo, "  mov %s, rax\n", areg(0));
  gen_runtime_prologue();
  extcall("strlen");
  gen_runtime_epilogue();
  fprintf(xo, "  mov [rsp + 40], rax\n");
  fprintf(xo, "  mov %s, rax\n", areg(0));
  fprintf(xo, "  add %s, 1\n", areg(0));
  gen_runtime_prologue();
  extcall("malloc");
  gen_runtime_epilogue();
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz alloc_trap\n");
  fprintf(xo, "  mov [rsp + 48], rax\n");
  fprintf(xo, "  mov %s, [rsp + 48]\n", areg(0));
  fprintf(xo, "  mov %s, [rsp + 32]\n", areg(1));
  fprintf(xo, "  mov %s, [rsp + 40]\n", areg(2));
  fprintf(xo, "  add %s, 1\n", areg(2));
  gen_runtime_prologue();
  extcall("memcpy");
  gen_runtime_epilogue();
  fprintf(xo, "  mov r10, [rsp + 16]\n");
  fprintf(xo, "  mov r11, [rsp + 24]\n");
  fprintf(xo, "  mov rax, [rsp + 48]\n");
  fprintf(xo, "  mov [r10 + r11*8 + 8], rax\n");
  fprintf(xo, "  inc qword [rsp + 24]\n");
  fprintf(xo, "  jmp args_loop%d\n", seq);
  fprintf(xo, "args_done%d:\n", seq);
  fprintf(xo, "  mov rax, [rsp + 16]\n");
  fprintf(xo, "  add rsp, 64\n");
  int ti = ins->dst - xf->nvars;
  if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
    fprintf(xo, "  mov %s, rax\n", reg_name(xa->temps[ti].reg));
  } else {
    store_slot(ins->dst);
  }
}

/**
 * @brief Emits system (run a shell command, result is the exit code)
 * @param ins System instruction (list[0] is the command)
 * @details Inherits stdio so command output shows. C system() returns the
 * exit code directly on Windows but wait status on System V (need_plt marks
 * that target), so the latter extracts WEXITSTATUS and reports -1 when the
 * child did not exit normally.
 */
static void sel_system(IrInstr *ins) {
  load_rax(op_of(ins->list[0]));
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz null_trap\n");
  fprintf(xo, "  mov %s, rax\n", areg(0));
  gen_runtime_prologue();
  extcall("system");
  gen_runtime_epilogue();
  if (xt->need_plt) {
    int seq = x_seq++;
    fprintf(xo, "  mov rbx, rax\n");
    fprintf(xo, "  test bl, 0x7f\n");
    fprintf(xo, "  jnz sys_fail%d\n", seq);
    fprintf(xo, "  mov eax, ebx\n");
    fprintf(xo, "  shr eax, 8\n");
    fprintf(xo, "  and eax, 0xff\n");
    fprintf(xo, "  jmp sys_done%d\n", seq);
    fprintf(xo, "sys_fail%d:\n", seq);
    fprintf(xo, "  mov rax, -1\n");
    fprintf(xo, "sys_done%d:\n", seq);
  }
  int ti = ins->dst - xf->nvars;
  if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
    fprintf(xo, "  mov %s, rax\n", reg_name(xa->temps[ti].reg));
  } else {
    store_slot(ins->dst);
  }
}

/**
 * @brief Concatenates two strings, result left in rax
 * @details IN: rax holds left pointer, rbx holds right pointer, rsp aligned.
 * Allocates len1+len2+1 bytes and copies both parts including the NUL.
 */
static void gen_string_concat(void) {
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz null_trap\n");
  fprintf(xo, "  test rbx, rbx\n");
  fprintf(xo, "  jz null_trap\n");
  fprintf(xo, "  sub rsp, 64\n");
  fprintf(xo, "  mov [rsp + 0], rax\n");
  fprintf(xo, "  mov [rsp + 8], rbx\n");
  fprintf(xo, "  mov %s, [rsp + 0]\n", areg(0));
  gen_runtime_prologue();
  extcall("strlen");
  gen_runtime_epilogue();
  fprintf(xo, "  mov [rsp + 16], rax\n");
  fprintf(xo, "  mov %s, [rsp + 8]\n", areg(0));
  gen_runtime_prologue();
  extcall("strlen");
  gen_runtime_epilogue();
  fprintf(xo, "  mov [rsp + 24], rax\n");
  fprintf(xo, "  mov rax, [rsp + 16]\n");
  fprintf(xo, "  add rax, [rsp + 24]\n");
  fprintf(xo, "  jo overflow_trap\n");
  fprintf(xo, "  add rax, 1\n");
  fprintf(xo, "  jo overflow_trap\n");
  fprintf(xo, "  mov %s, rax\n", areg(0));
  gen_runtime_prologue();
  extcall("malloc");
  gen_runtime_epilogue();
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz alloc_trap\n");
  fprintf(xo, "  mov [rsp + 32], rax\n");
  fprintf(xo, "  mov %s, [rsp + 32]\n", areg(0));
  fprintf(xo, "  mov %s, [rsp + 0]\n", areg(1));
  fprintf(xo, "  mov %s, [rsp + 16]\n", areg(2));
  gen_runtime_prologue();
  extcall("memcpy");
  gen_runtime_epilogue();
  fprintf(xo, "  mov rax, [rsp + 32]\n");
  fprintf(xo, "  add rax, [rsp + 16]\n");
  fprintf(xo, "  mov %s, rax\n", areg(0));
  fprintf(xo, "  mov %s, [rsp + 8]\n", areg(1));
  fprintf(xo, "  mov %s, [rsp + 24]\n", areg(2));
  fprintf(xo, "  add %s, 1\n", areg(2));
  gen_runtime_prologue();
  extcall("memcpy");
  gen_runtime_epilogue();
  fprintf(xo, "  mov rax, [rsp + 32]\n");
  fprintf(xo, "  add rsp, 64\n");
}

/**
 * @brief Copies the instance pointer in rax into a fresh heap instance
 * @param alloc Bytes to allocate (payload plus header for classes)
 * @param size Payload bytes to copy
 * @param class_id Class id for the header, or -1 for structs
 * @details Null sources trap. Leaves the fresh pointer in rax.
 */
static void gen_instance_copy(long long alloc, long long size, int class_id) {
  int is_cls = class_id >= 0;
  fprintf(xo, "  push rax\n");
  fprintf(xo, "  mov %s, %lld\n", areg(0), alloc);
  gen_runtime_prologue();
  extcall("malloc");
  gen_runtime_epilogue();
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz alloc_trap\n");
  fprintf(xo, "  mov rbx, rax\n");
  fprintf(xo, "  pop rdx\n");
  fprintf(xo, "  test rdx, rdx\n");
  fprintf(xo, "  jz null_trap\n");
  if (is_cls) {
    fprintf(xo, "  mov QWORD [rbx], %d\n", class_id);
    fprintf(xo, "  lea %s, [rbx + 8]\n", areg(0));
  } else {
    fprintf(xo, "  mov %s, rbx\n", areg(0));
  }
  fprintf(xo, "  mov %s, rdx\n", areg(1));
  fprintf(xo, "  mov %s, %lld\n", areg(2), size);
  gen_runtime_prologue();
  extcall("memcpy");
  gen_runtime_epilogue();
  fprintf(xo, "  mov rax, rbx\n");
  if (is_cls) {
    fprintf(xo, "  add rax, 8\n");
  }
}

/**
 * @brief Stores a call result from rax into a temp
 * @param dst Destination temp slot
 */
static void store_call_result(int dst) {
  int ti = dst - xf->nvars;
  if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
    int reg = xa->temps[ti].reg;
    if (reg >= R_XMM0) {
      fprintf(xo, "  movq %s, rax\n", reg_name(reg));
    } else {
      fprintf(xo, "  mov %s, rax\n", reg_name(reg));
    }
  } else {
    store_slot(dst);
  }
}

/**
 * @brief Emits an array allocation with evaluated elements on the stack
 * @param ins New-array instruction (aux holds the count)
 */
static void sel_newarr(IrInstr *ins) {
  for (int i = 0; i < ins->nlist; i++) {
    load_rax(op_of(ins->list[i]));
    fprintf(xo, "  push rax\n");
  }
  fprintf(xo, "  mov %s, %d\n", areg(0), (ins->aux + 1) * 8);
  gen_runtime_prologue();
  extcall("malloc");
  gen_runtime_epilogue();
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz alloc_trap\n");
  fprintf(xo, "  mov rbx, rax\n");
  fprintf(xo, "  mov QWORD [rbx], %d\n", ins->aux);
  if (ins->dst >= 0) {
    int ti = ins->dst - xf->nvars;
    if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
      fprintf(xo, "  mov %s, rbx\n", reg_name(xa->temps[ti].reg));
    } else {
      fprintf(xo, "  mov [rbp - %d], rbx\n", slot_off(ins->dst));
    }
  }
  for (int i = ins->aux - 1; i >= 0; i--) {
    fprintf(xo, "  pop rax\n");
    fprintf(xo, "  mov [rbx + %d], rax\n", i * 8 + 8);
  }
}

/**
 * @brief Emits a bounds-checked array element load
 * @param ins Load instruction (array, index)
 */
static void sel_arr_load(IrInstr *ins) {
  load_rax(op_of(ins->v[0]));
  fprintf(xo, "  push rax\n");
  load_rax(op_of(ins->v[1]));
  fprintf(xo, "  mov r10, [rsp]\n");
  fprintf(xo, "  test r10, r10\n");
  fprintf(xo, "  jz null_trap\n");
  if (ins->aux == 0) {
    fprintf(xo, "  mov r11, [r10]\n");
    fprintf(xo, "  cmp rax, 0\n");
    fprintf(xo, "  jl index_trap\n");
    fprintf(xo, "  cmp rax, r11\n");
    fprintf(xo, "  jae index_trap\n");
  }
  fprintf(xo, "  mov r10, [rsp]\n");
  fprintf(xo, "  mov rax, [r10 + rax*8 + 8]\n");
  fprintf(xo, "  add rsp, 8\n");
  int ti = ins->dst - xf->nvars;
  if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
    int reg = xa->temps[ti].reg;
    if (reg >= R_XMM0) {
      fprintf(xo, "  movq %s, rax\n", reg_name(reg));
    } else {
      fprintf(xo, "  mov %s, rax\n", reg_name(reg));
    }
  } else {
    store_slot(ins->dst);
  }
}

/**
 * @brief Emits a bounds-checked array element store (=, +=, -=)
 * @param ins Store instruction (array, index, value; aux holds the op)
 */
static void sel_arr_store(IrInstr *ins) {
  load_rax(op_of(ins->v[0]));
  fprintf(xo, "  push rax\n");
  load_rax(op_of(ins->v[1]));
  fprintf(xo, "  mov r10, [rsp]\n");
  fprintf(xo, "  test r10, r10\n");
  fprintf(xo, "  jz null_trap\n");
  fprintf(xo, "  mov r11, [r10]\n");
  fprintf(xo, "  cmp rax, 0\n");
  fprintf(xo, "  jl index_trap\n");
  fprintf(xo, "  cmp rax, r11\n");
  fprintf(xo, "  jae index_trap\n");
  fprintf(xo, "  lea r11, [r10 + rax*8 + 8]\n");
  fprintf(xo, "  mov [rsp], r11\n");
  if (ins->aux == 0) {
    load_rax(op_of(ins->v[2]));
    fprintf(xo, "  mov r10, [rsp]\n");
    fprintf(xo, "  mov [r10], rax\n");
    fprintf(xo, "  add rsp, 8\n");
    return;
  }
  if (ins->aux2 == (int)IR_STR) {
    fprintf(xo, "  mov r10, [rsp]\n");
    fprintf(xo, "  mov rax, [r10]\n");
    fprintf(xo, "  push rax\n");
    load_rax(op_of(ins->v[2]));
    fprintf(xo, "  mov rbx, rax\n");
    fprintf(xo, "  pop rax\n");
    gen_string_concat();
    fprintf(xo, "  mov r10, [rsp]\n");
    fprintf(xo, "  mov [r10], rax\n");
    fprintf(xo, "  add rsp, 8\n");
    return;
  }
  if (ins->aux2 == (int)IR_FLOAT) {
    load_rax(op_of(ins->v[2]));
    fprintf(xo, "  movq xmm1, rax\n");
    fprintf(xo, "  mov r10, [rsp]\n");
    fprintf(xo, "  movq xmm0, [r10]\n");
    if (ins->aux == 1) {
      fprintf(xo, "  addsd xmm0, xmm1\n");
    } else {
      fprintf(xo, "  subsd xmm0, xmm1\n");
    }
    fprintf(xo, "  movq rax, xmm0\n");
    fprintf(xo, "  mov [r10], rax\n");
    fprintf(xo, "  add rsp, 8\n");
    return;
  }
  load_rax(op_of(ins->v[2]));
  fprintf(xo, "  mov r10, [rsp]\n");
  fprintf(xo, "  mov r11, [r10]\n");
  if (ins->aux == 1) {
    fprintf(xo, "  add rax, r11\n");
  } else {
    fprintf(xo, "  sub r11, rax\n");
    fprintf(xo, "  mov rax, r11\n");
  }
  fprintf(xo, "  jo overflow_trap\n");
  fprintf(xo, "  mov [r10], rax\n");
  fprintf(xo, "  add rsp, 8\n");
}

/**
 * @brief Emits an array length read
 * @param ins Length instruction
 */
static void sel_arr_len(IrInstr *ins) {
  load_rax(op_of(ins->v[0]));
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz null_trap\n");
  fprintf(xo, "  mov rax, [rax]\n");
  int ti = ins->dst - xf->nvars;
  if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
    fprintf(xo, "  mov %s, rax\n", reg_name(xa->temps[ti].reg));
  } else {
    store_slot(ins->dst);
  }
}

/**
 * @brief Emits a string character-code load with bounds checks
 * @param ins Index instruction (string, index)
 */
static void sel_str_idx(IrInstr *ins) {
  load_rax(op_of(ins->v[0]));
  fprintf(xo, "  push rax\n");
  load_rax(op_of(ins->v[1]));
  fprintf(xo, "  mov rbx, rax\n");
  fprintf(xo, "  pop rax\n");
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz null_trap\n");
  fprintf(xo, "  cmp rbx, 0\n");
  fprintf(xo, "  jl index_trap\n");
  fprintf(xo, "  sub rsp, 64\n");
  fprintf(xo, "  mov [rsp + 0], rax\n");
  fprintf(xo, "  mov [rsp + 8], rbx\n");
  fprintf(xo, "  mov %s, [rsp + 0]\n", areg(0));
  gen_runtime_prologue();
  extcall("strlen");
  gen_runtime_epilogue();
  fprintf(xo, "  cmp [rsp + 8], rax\n");
  fprintf(xo, "  jae index_trap\n");
  fprintf(xo, "  mov rax, [rsp + 0]\n");
  fprintf(xo, "  add rax, [rsp + 8]\n");
  fprintf(xo, "  movzx eax, byte [rax]\n");
  fprintf(xo, "  add rsp, 64\n");
  int ti = ins->dst - xf->nvars;
  if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
    fprintf(xo, "  mov %s, rax\n", reg_name(xa->temps[ti].reg));
  } else {
    store_slot(ins->dst);
  }
}

/**
 * @brief Emits a struct/class allocation with evaluated fields
 * @param ins New-instance instruction (aux holds bytes, aux2 the class)
 */
static void sel_newinst(IrInstr *ins) {
  for (int i = 0; i < ins->nlist; i++) {
    load_rax(op_of(ins->list[i]));
    fprintf(xo, "  push rax\n");
  }
  fprintf(xo, "  mov %s, %d\n", areg(0), ins->aux);
  gen_runtime_prologue();
  extcall("malloc");
  gen_runtime_epilogue();
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz alloc_trap\n");
  fprintf(xo, "  mov r11, rax\n");
  if (ins->aux2 > 0) {
    fprintf(xo, "  mov QWORD [r11], %d\n", ins->aux2 - 1);
    fprintf(xo, "  add r11, 8\n");
  }
  for (int i = ins->nlist - 1; i >= 0; i--) {
    fprintf(xo, "  pop rdx\n");
    fprintf(xo, "  mov [r11 + %d], rdx\n", i * 8);
  }
  fprintf(xo, "  mov rax, r11\n");
  int ti = ins->dst - xf->nvars;
  if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
    fprintf(xo, "  mov %s, rax\n", reg_name(xa->temps[ti].reg));
  } else {
    store_slot(ins->dst);
  }
}

/**
 * @brief Emits a field load with a null check
 * @param ins Field load (object in v[0], field index in aux)
 */
static void sel_field_load(IrInstr *ins) {
  load_rax(op_of(ins->v[0]));
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz null_trap\n");
  fprintf(xo, "  mov rax, [rax + %d]\n", ins->aux * 8);
  int ti = ins->dst - xf->nvars;
  if (ti >= 0 && ti < xa->ntemps && xa->temps[ti].is_reg) {
    int reg = xa->temps[ti].reg;
    if (reg >= R_XMM0) {
      fprintf(xo, "  movq %s, rax\n", reg_name(reg));
    } else {
      fprintf(xo, "  mov %s, rax\n", reg_name(reg));
    }
  } else {
    store_slot(ins->dst);
  }
}

/**
 * @brief Emits a field store with a null check
 * @param ins Field store (object, value; field index in aux)
 */
static void sel_field_store(IrInstr *ins) {
  load_rax(op_of(ins->v[1]));
  fprintf(xo, "  push rax\n");
  load_rax(op_of(ins->v[0]));
  fprintf(xo, "  test rax, rax\n");
  fprintf(xo, "  jz null_trap\n");
  fprintf(xo, "  pop rbx\n");
  fprintf(xo, "  mov [rax + %d], rbx\n", ins->aux * 8);
}

/**
 * @brief Emits callee-saved register restores (mirrors the prologue saves)
 */
static void emit_restore(void) {
  int xj = 0;
  for (int r = R_XMM0; r <= R_XMM15; r++) {
    if ((xa->used_saved_xmm & REG_BIT(r)) != 0) {
      xj++;
      fprintf(xo, "  movups %s, [rbp - %d]\n", reg_name(r),
              x_frame + x_npush * 8 + xj * 16);
    }
  }
  for (int r = 15; r >= 0; r--) {
    if ((xa->used_saved_gp & REG_BIT(r)) != 0) {
      int kk = 0;
      for (int q = 0; q < r; q++) {
        if ((xa->used_saved_gp & REG_BIT(q)) != 0) {
          kk++;
        }
      }
      fprintf(xo, "  mov %s, [rbp - %d]\n", reg_name(r),
              x_frame + (kk + 1) * 8);
    }
  }
}
/**
 * @brief Counts operand uses of a frame slot in a function
 * @param slot Frame slot to count
 * @return Use count across all blocks
 */
static int count_uses(int slot) {
  int n = 0;
  for (int b = 0; b < xf->nblocks; b++) {
    for (int k = 0; k < xf->blocks[b].nins; k++) {
      IrInstr *ins = &xf->blocks[b].ins[k];
      if (ins->op == IR_NOP) {
        continue;
      }
      for (int i = 0; i < ins->nv; i++) {
        if ((ins->v[i].kind == IRV_TEMP || ins->v[i].kind == IRV_VAR) &&
            ins->v[i].idx == slot) {
          n++;
        }
      }
      for (int i = 0; i < ins->nlist; i++) {
        if ((ins->list[i].kind == IRV_TEMP ||
             ins->list[i].kind == IRV_VAR) &&
            ins->list[i].idx == slot) {
          n++;
        }
      }
    }
  }
  return n;
}

/**
 * @brief Emits an integer conditional jump with fallthrough elision
 * @param fi Function index for labels
 * @param cond Comparison condition
 * @param t True block, f false block, next layout-next block (-1 none)
 */
static void sense_int_jcc(int fi, int cond, int t, int f, int next) {
  const char *jt = "je";
  const char *jf = "jne";
  if (cond == IR_CNE) {
    jt = "jne";
    jf = "je";
  } else if (cond == IR_CLT) {
    jt = "jl";
    jf = "jge";
  } else if (cond == IR_CGT) {
    jt = "jg";
    jf = "jle";
  } else if (cond == IR_CLE) {
    jt = "jle";
    jf = "jg";
  } else if (cond == IR_CGE) {
    jt = "jge";
    jf = "jl";
  }
  if (f == next) {
    fprintf(xo, "  %s f%d_bb%d\n", jt, fi, t);
  } else if (t == next) {
    fprintf(xo, "  %s f%d_bb%d\n", jf, fi, f);
  } else {
    fprintf(xo, "  %s f%d_bb%d\n", jt, fi, t);
    fprintf(xo, "  jmp f%d_bb%d\n", fi, f);
  }
}

/**
 * @brief Emits a NaN-safe float conditional jump with elision
 * @param fi Function index for labels
 * @param cond Comparison condition
 * @param t True block, f false block, next layout-next block (-1 none)
 * @details Unordered (NaN) inputs take the false edge except for !=.
 * Each form below is checked against the ucomisd flag truth table
 * (unordered sets ZF=PF=CF).
 */
static void sense_float_jcc(int fi, int cond, int t, int f, int next) {
  if (cond == IR_CEQ) {
    if (f == next) {
      fprintf(xo, "  je f%d_bb%d\n", fi, t);
    } else if (t == next) {
      fprintf(xo, "  jp f%d_bb%d\n", fi, f);
      fprintf(xo, "  jne f%d_bb%d\n", fi, f);
    } else {
      fprintf(xo, "  jp f%d_bb%d\n", fi, f);
      fprintf(xo, "  je f%d_bb%d\n", fi, t);
      fprintf(xo, "  jmp f%d_bb%d\n", fi, f);
    }
    return;
  }
  if (cond == IR_CNE) {
    if (t == next) {
      fprintf(xo, "  jp f%d_bb%d\n", fi, t);
      fprintf(xo, "  je f%d_bb%d\n", fi, f);
    } else if (f == next) {
      fprintf(xo, "  jp f%d_bb%d\n", fi, t);
      fprintf(xo, "  jne f%d_bb%d\n", fi, t);
    } else {
      fprintf(xo, "  jp f%d_bb%d\n", fi, t);
      fprintf(xo, "  jne f%d_bb%d\n", fi, t);
      fprintf(xo, "  jmp f%d_bb%d\n", fi, f);
    }
    return;
  }
  if (cond == IR_CLT) {
    if (f == next) {
      fprintf(xo, "  jb f%d_bb%d\n", fi, t);
    } else if (t == next) {
      fprintf(xo, "  jp f%d_bb%d\n", fi, f);
      fprintf(xo, "  jae f%d_bb%d\n", fi, f);
    } else {
      fprintf(xo, "  jp f%d_bb%d\n", fi, f);
      fprintf(xo, "  jb f%d_bb%d\n", fi, t);
      fprintf(xo, "  jmp f%d_bb%d\n", fi, f);
    }
    return;
  }
  if (cond == IR_CGT) {
    if (f == next) {
      fprintf(xo, "  ja f%d_bb%d\n", fi, t);
    } else if (t == next) {
      fprintf(xo, "  jbe f%d_bb%d\n", fi, f);
    } else {
      fprintf(xo, "  jp f%d_bb%d\n", fi, f);
      fprintf(xo, "  ja f%d_bb%d\n", fi, t);
      fprintf(xo, "  jmp f%d_bb%d\n", fi, f);
    }
    return;
  }
  if (cond == IR_CLE) {
    if (f == next) {
      fprintf(xo, "  jbe f%d_bb%d\n", fi, t);
    } else if (t == next) {
      fprintf(xo, "  jp f%d_bb%d\n", fi, f);
      fprintf(xo, "  ja f%d_bb%d\n", fi, f);
    } else {
      fprintf(xo, "  jp f%d_bb%d\n", fi, f);
      fprintf(xo, "  jbe f%d_bb%d\n", fi, t);
      fprintf(xo, "  jmp f%d_bb%d\n", fi, f);
    }
    return;
  }
  /* >= : unordered fails jae on its own. */
  if (f == next) {
    fprintf(xo, "  jae f%d_bb%d\n", fi, t);
  } else if (t == next) {
    fprintf(xo, "  jb f%d_bb%d\n", fi, f);
  } else {
    fprintf(xo, "  jae f%d_bb%d\n", fi, t);
    fprintf(xo, "  jmp f%d_bb%d\n", fi, f);
  }
}

static int sel_instr(int fi, int bi, int k, IrInstr *ins, int is_entry) {
  IrBlock *blk = &xf->blocks[bi];
  int next = (bi + 1 < xf->nblocks) ? xf->blocks[bi + 1].id : -1;
  switch (ins->op) {
  case IR_NOP:
    break;
  case IR_CONST_I:
  case IR_CONST_F:
  case IR_CONST_S:
  case IR_CONST_NULL:
  case IR_COPY:
  case IR_I2F:
  case IR_F2I:
  case IR_TRUNC8:
  case IR_LOAD:
  case IR_LOADG:
    sel_move(ins);
    break;
  case IR_STORE:
  case IR_STOREG:
    sel_store(ins);
    break;
  case IR_ADD:
  case IR_SUB:
  case IR_MUL:
  case IR_DIV:
  case IR_MOD:
    if (ins->type == IR_FLOAT) {
      sel_arith_float(ins);
    } else {
      sel_arith_int(ins);
    }
    break;
  case IR_AND:
  case IR_OR:
  case IR_XOR:
  case IR_SHL:
  case IR_SHR:
  case IR_NOT:
    sel_bits_int(ins);
    break;
  case IR_CMP: {
    /* Fuse an immediately following branch on a single-use result. */
    if (k + 1 < blk->nins) {
      IrInstr *nxt = &blk->ins[k + 1];
      if (nxt->op == IR_BR && nxt->nv == 1 && nxt->v[0].kind == IRV_TEMP &&
          nxt->v[0].idx == ins->dst && count_uses(ins->dst) == 1) {
        return sel_fused(fi, ins, nxt, next);
      }
    }
    sel_cmp(ins);
    break;
  }
  case IR_JUMP:
    if (ins->t != next) {
      fprintf(xo, "  jmp f%d_bb%d\n", fi, ins->t);
    }
    break;
  case IR_BR: {
    Op c = op_of(ins->v[0]);
    if (c.is_reg) {
      fprintf(xo, "  cmp %s, 0\n", reg_name(c.reg));
    } else if (c.is_mem) {
      fprintf(xo, "  cmp qword [rbp - %d], 0\n", slot_off(c.slot));
    } else if (c.is_global) {
      fprintf(xo, "  cmp qword [rel global_%s], 0\n",
              xm->globals[c.gidx].name);
    } else if (c.is_imm && c.imm == 0) {
      fprintf(xo, "  jmp f%d_bb%d\n", fi, ins->f);
      break;
    } else if (c.is_imm) {
      fprintf(xo, "  jmp f%d_bb%d\n", fi, ins->t);
      break;
    } else {
      load_rax(c);
      fprintf(xo, "  cmp rax, 0\n");
    }
    if (ins->f == next) {
      fprintf(xo, "  jne f%d_bb%d\n", fi, ins->t);
    } else if (ins->t == next) {
      fprintf(xo, "  je f%d_bb%d\n", fi, ins->f);
    } else {
      fprintf(xo, "  je f%d_bb%d\n", fi, ins->f);
      fprintf(xo, "  jmp f%d_bb%d\n", fi, ins->t);
    }
    break;
  }
  case IR_CALL:
    sel_call(ins, 0);
    break;
  case IR_CALLM:
    sel_call(ins, 1);
    break;
  case IR_RET:
  case IR_EXIT:
    if (ins->op == IR_EXIT) {
      load_rax(op_of(ins->v[0]));
      fprintf(xo, "  mov %s, rax\n", areg(0));
      ccall("exit");
    } else if (!is_entry) {
      if (ins->nv > 0) {
        load_rax(op_of(ins->v[0]));
      }
      emit_restore();
      fprintf(xo, "  mov rsp, rbp\n");
      fprintf(xo, "  pop rbp\n");
      fprintf(xo, "  ret\n");
    }
    break;
  case IR_PRINT_S:
  case IR_PRINT_V: {
    /* Print emission lives in sel_print below via direct calls. */
    if (ins->op == IR_PRINT_S) {
      fprintf(xo, "  lea %s, [rel fmt_str]\n", areg(0));
      fprintf(xo, "  lea %s, [rel str%d]\n", areg(1), ins->aux);
      ccall("printf");
    } else {
      load_rax(op_of(ins->v[0]));
      if (ins->type == IR_STR) {
        fprintf(xo, "  lea %s, [rel fmt_str]\n", areg(0));
        fprintf(xo, "  mov %s, rax\n", areg(1));
        ccall("printf");
      } else if (ins->type == IR_FLOAT) {
        fprintf(xo, "  movq xmm0, rax\n");
        fprintf(xo, "  movq rdx, xmm0\n");
        if (ins->aux) {
          fprintf(xo, "  lea %s, [rel fmt_float_raw]\n", areg(0));
        } else {
          fprintf(xo, "  lea %s, [rel fmt_float]\n", areg(0));
        }
        fprintf(xo, "  movapd xmm1, xmm0\n");
        vararg_eax(1);
        ccall("printf");
      } else {
        if (ins->aux) {
          fprintf(xo, "  lea %s, [rel fmt_int_raw]\n", areg(0));
        } else {
          fprintf(xo, "  lea %s, [rel fmt_int]\n", areg(0));
        }
        fprintf(xo, "  mov %s, rax\n", areg(1));
        vararg_eax(0);
        ccall("printf");
      }
    }
    break;
  }
  case IR_INPUT:
    sel_input(ins);
    break;
  case IR_LEN:
    sel_len(ins);
    break;
  case IR_TOSTR:
    sel_to_str(ins);
    break;
  case IR_TONUM:
    sel_to_num(ins);
    break;
  case IR_READFILE:
    sel_read_file(ins);
    break;
  case IR_WRITEFILE:
    sel_write_file(ins);
    break;
  case IR_CHR:
    sel_chr(ins);
    break;
  case IR_ARGS:
    sel_args(ins);
    break;
  case IR_SYSTEM:
    sel_system(ins);
    break;
  case IR_NEWARR:
    sel_newarr(ins);
    break;
  case IR_ARR_LOAD:
    sel_arr_load(ins);
    break;
  case IR_ARR_STORE:
    sel_arr_store(ins);
    break;
  case IR_ARR_LEN:
    sel_arr_len(ins);
    break;
  case IR_STR_IDX:
    sel_str_idx(ins);
    break;
  case IR_CONCAT:
    load_rax(op_of(ins->v[0]));
    fprintf(xo, "  push rax\n");
    load_rax(op_of(ins->v[1]));
    fprintf(xo, "  mov rbx, rax\n");
    fprintf(xo, "  pop rax\n");
    gen_string_concat();
    store_call_result(ins->dst);
    break;
  case IR_NEWINST:
    sel_newinst(ins);
    break;
  case IR_COPYINST:
    load_rax(op_of(ins->v[0]));
    gen_instance_copy(ins->aux, ins->aux2, ins->callee);
    store_call_result(ins->dst);
    break;
  case IR_FIELD_LOAD:
    sel_field_load(ins);
    break;
  case IR_FIELD_STORE:
    sel_field_store(ins);
    break;
  }
  return 0;
}

/**
 * @brief Counts set bits in a register mask
 * @param mask Register bitmask
 * @return Population count
 */
static int pop_mask(uint32_t mask) {
  int n = 0;
  while (mask != 0) {
    n += (int)(mask & 1u);
    mask >>= 1;
  }
  return n;
}

/**
 * @brief Emits one function with params, blocks, and epilogue
 * @param fi Function index (for block labels)
 * @param is_entry Non-zero for the entry program (no epilogue)
 */
void x86_emit_func(IrModule *m, IrFunc *f, int fi, int is_entry,
                   const Target *t, const RegAlloc *a, FILE *out) {
  xm = m;
  xo = out;
  xt = t;
  xf = f;
  xa = a;
  int npush = pop_mask(a->used_saved_gp);
  int nxmm = pop_mask(a->used_saved_xmm);
  int frame = (f->nvars + f->ntemps) * 8 + 256;
  if (frame < 2048) {
    frame = 2048;
  }
  frame = (frame + 15) & ~15;
  int total = frame + npush * 8 + nxmm * 16;
  if (total % 16 != 0) {
    total += 8;
  }
  x_frame = frame;
  x_npush = npush;
  fprintf(out, "%s:\n", f->label);
  if (is_entry) {
    /* C main(argc, argv) arrives in the first two argument registers on
       both targets; stash them for the args() builtin before anything
       clobbers them. argc is 32-bit, so zero-extend it first. */
    fprintf(out, "  mov %s, %s\n", reg_name32(xt->arg_regs[0]),
            reg_name32(xt->arg_regs[0]));
    fprintf(out, "  mov [rel jot_argc], %s\n",
            reg_name(xt->arg_regs[0]));
    fprintf(out, "  mov [rel jot_argv], %s\n",
            reg_name(xt->arg_regs[1]));
    fprintf(out, "  mov rax, rsp\n");
    fprintf(out, "  sub rax, 262144\n");
    fprintf(out, "  mov [rel stack_floor], rax\n");
  }
  fprintf(out, "  push rbp\n");
  fprintf(out, "  mov rbp, rsp\n");
  fprintf(out, "  sub rsp, %d\n", total);
  int k = 0;
  for (int r = 0; r < 16; r++) {
    if ((a->used_saved_gp & REG_BIT(r)) != 0) {
      fprintf(out, "  mov [rbp - %d], %s\n", frame + (k + 1) * 8,
              reg_name(r));
      k++;
    }
  }
  int kx = 0;
  for (int r = R_XMM0; r <= R_XMM15; r++) {
    if ((a->used_saved_xmm & REG_BIT(r)) != 0) {
      kx++;
      fprintf(out, "  movups [rbp - %d], %s\n",
              frame + npush * 8 + kx * 16, reg_name(r));
    }
  }
  fprintf(out, "  cmp rsp, [rel stack_floor]\n");
  fprintf(out, "  jb stack_overflow_trap\n");
  int pidx = 0;
  for (int s = 0; s < f->nvars; s++) {
    if (!f->var_is_param[s]) {
      continue;
    }
    int abs_idx = f->is_method ? pidx + 1 : pidx;
    if (abs_idx < t->nargs_regs) {
      fprintf(out, "  mov [rbp - %d], %s\n", slot_off(s),
              reg_name(t->arg_regs[abs_idx]));
    } else {
      /* Stack args sit above the shadow, return address, and saved rbp. */
      int off = xt->shadow + 16 + 8 * (abs_idx - t->nargs_regs);
      fprintf(out, "  mov rax, [rbp + %d]\n", off);
      fprintf(out, "  mov [rbp - %d], rax\n", slot_off(s));
    }
    pidx++;
  }
  for (int b = 0; b < f->nblocks; b++) {
    fprintf(out, "f%d_bb%d:\n", fi, f->blocks[b].id);
    for (int i = 0; i < f->blocks[b].nins; i++) {
      if (sel_instr(fi, b, i, &f->blocks[b].ins[i], is_entry)) {
        i++;
      }
    }
  }
  if (!is_entry) {
    emit_restore();
    fprintf(out, "  mov rsp, rbp\n");
    fprintf(out, "  pop rbp\n");
    fprintf(out, "  ret\n");
  }
}

/**
 * @brief Emits the data section (formats, globals, strings, vtables)
 * @param m Module
 * @param out Assembly stream
 */
void x86_data_section(IrModule *m, FILE *out) {
  xm = m;
  xo = out;
  fprintf(out, "section .data\n");
  fprintf(out, "  fmt_int db \"%%lld\", 10, 0\n");
  fprintf(out, "  fmt_int_raw db \"%%lld\", 0\n");
  /* 17 significant digits: every double round-trips (15 could merge
     neighboring values, e.g. 0.1 + 0.2 printed without its final 4). */
  fprintf(out, "  fmt_float db \"%%.17g\", 10, 0\n");
  fprintf(out, "  fmt_float_raw db \"%%.17g\", 0\n");
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
  fprintf(out, "  fmt_char db \"invalid char code\", 10, 0\n");
  fprintf(out, "  stack_floor dq 0\n");
  fprintf(out, "  jot_argc dq 0\n");
  fprintf(out, "  jot_argv dq 0\n");
  for (int i = 0; i < m->nglobals; i++) {
    fprintf(out, "  global_%s dq 0\n", m->globals[i].name);
  }
  for (int i = 0; i < m->nstrings; i++) {
    fprintf(out, "  str%d db ", i);
    if (m->strings[i][0] == '\0') {
      fprintf(out, "0");
    } else {
      const char *value = m->strings[i];
      size_t len = strlen(value);
      size_t p = 0;
      int first = 1;
      while (p < len) {
        size_t run = p;
        while (run < len && (unsigned char)value[run] >= 0x20 &&
               (unsigned char)value[run] <= 0x7E && value[run] != '\'') {
          run++;
        }
        if (run > p) {
          if (!first) {
            fprintf(out, ", ");
          }
          fprintf(out, "'%.*s'", (int)(run - p), value + p);
          first = 0;
          p = run;
          continue;
        }
        if (!first) {
          fprintf(out, ", ");
        }
        fprintf(out, "%u", (unsigned)(unsigned char)value[p]);
        first = 0;
        p++;
      }
      if (first) {
        fprintf(out, "0");
        return;
      }
      fprintf(out, ", 0");
    }
    fprintf(out, "\n");
  }
  for (int c = 0; c < m->nclasses; c++) {
    fprintf(out, "  jotvt_%d dq", m->classes[c].class_id);
    int first = 1;
    for (int s = 0; s < m->classes[c].nslots; s++) {
      int owner = m->classes[c].vtable[s];
      int midx = m->classes[c].vtable_mth[s];
      if (owner < 0 || midx < 0) {
        fprintf(out, "%s 0", first ? "" : ",");
      } else {
        int oc = -1;
        for (int q = 0; q < m->nclasses; q++) {
          if (m->classes[q].class_id == owner) {
            oc = q;
            break;
          }
        }
        fprintf(out, "%s %s__%s", first ? "" : ",",
                m->classes[oc].name, m->classes[oc].method_names[midx]);
      }
      first = 0;
    }
    if (first) {
      fprintf(out, " 0");
    }
    fprintf(out, "\n");
  }
  if (m->nclasses > 0) {
    int max_id = 0;
    for (int c = 0; c < m->nclasses; c++) {
      if (m->classes[c].class_id > max_id) {
        max_id = m->classes[c].class_id;
      }
    }
    fprintf(out, "  jot_vtables dq");
    for (int id = 0; id <= max_id; id++) {
      int found = -1;
      for (int c = 0; c < m->nclasses; c++) {
        if (m->classes[c].class_id == id) {
          found = c;
          break;
        }
      }
      fprintf(out, "%s jotvt_%d", id == 0 ? "" : ",",
              found >= 0 ? m->classes[found].class_id : 0);
    }
    fprintf(out, "\n");
  }
}

/**
 * @brief Emits a fatal runtime trap (message to stdout, exit 3)
 * @param label Trap label to define
 * @param format Data label holding the message
 */
static void x86_trap(const char *label, const char *format) {
  fprintf(xo, "%s:\n", label);
  fprintf(xo, "  mov rax, rsp\n");
  fprintf(xo, "  and rax, 8\n");
  fprintf(xo, "  sub rsp, rax\n");
  fprintf(xo, "  lea %s, [rel %s]\n", areg(0), format);
  ccall("printf");
  fprintf(xo, "  mov %s, 3\n", areg(0));
  ccall("exit");
}

/**
 * @brief Emits the runtime trap handlers
 * @param t Target descriptor (argument register for printf/exit)
 * @param out Assembly stream
 */
void x86_emit_traps(const Target *t, FILE *out) {
  xo = out;
  xt = t;
  x86_trap("overflow_trap", "fmt_overflow");
  x86_trap("divzero_trap", "fmt_divzero");
  x86_trap("stack_overflow_trap", "fmt_stack");
  x86_trap("input_error_trap", "fmt_invalid");
  x86_trap("index_trap", "fmt_index");
  x86_trap("alloc_trap", "fmt_alloc");
  x86_trap("null_trap", "fmt_null");
  x86_trap("open_trap", "fmt_openfail");
  x86_trap("char_trap", "fmt_char");
}

void x86_emit_halt(const Target *t, FILE *out) {
  if (t->shadow != 0) {
    fprintf(out, "  xor ecx, ecx\n");
    fprintf(out, "  sub rsp, 32\n");
    fprintf(out, "  call exit\n");
  } else {
    fprintf(out, "  xor edi, edi\n");
    if (t->need_plt) {
      fprintf(out, "  call exit wrt ..plt\n");
    } else {
      fprintf(out, "  call exit\n");
    }
  }
}
