#define _CRT_SECURE_NO_WARNINGS
#undef strdup

#include "codegen.h"

/** Output assembly file */
static FILE *out;

/** Module being emitted */
static IrModule *cg_mod;

/** Source file for diagnostics */
static const char *cg_source;

/**
 * @brief Frame slot address operand offset
 * @param slot Frame slot (variables first, then temps)
 * @return Byte offset below rbp
 */
static int slot_off(int slot) { return (slot + 1) * 8; }

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
 * @brief Emits a function prologue with room for locals
 * @param frame Reserved stack bytes
 */
static void gen_prologue(long frame) {
  fprintf(out, "  push rbp\n");
  fprintf(out, "  mov rbp, rsp\n");
  fprintf(out, "  sub rsp, %ld\n", frame);
  fprintf(out, "  cmp rsp, [rel stack_floor]\n");
  fprintf(out, "  jb stack_overflow_trap\n");
}

/**
 * @brief Checks if a byte can appear verbatim inside a NASM quoted string
 * @param c Byte to test
 * @return Non-zero for printable ASCII except the quote character
 */
static int nasm_verbatim_byte(unsigned char c) {
  return c >= 0x20 && c <= 0x7E && c != '\'';
}

/**
 * @brief Writes a string value as NASM db content
 * @param value Raw string value (escapes already decoded by the lexer)
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
 * @brief Copies the instance pointer in rax into a fresh heap instance
 * @param alloc Bytes to allocate (payload plus header for classes)
 * @param size Payload bytes to copy
 * @param class_id Class id for the header, or -1 for structs
 * @details Null sources trap. Leaves the fresh pointer in rax.
 */
static void gen_instance_copy(long long alloc, long long size, int class_id) {
  int is_cls = class_id >= 0;
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
    fprintf(out, "  mov QWORD [rbx], %d\n", class_id);
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

/*__CG2__*/

/**
 * @brief Materializes a value into rax
 * @param f Owning function (for temp slot types)
 * @param v Value to load
 */
static void cg_val(IrFunc *f, IrVal v) {
  (void)f;
  switch (v.kind) {
  case IRV_TEMP:
  case IRV_VAR:
    fprintf(out, "  mov rax, [rbp - %d]\n", slot_off(v.idx));
    break;
  case IRV_GLOBAL:
    fprintf(out, "  mov rax, [rel global_%s]\n", cg_mod->globals[v.idx].name);
    break;
  case IRV_IMM_I:
    fprintf(out, "  mov rax, %lld\n", v.imm);
    break;
  case IRV_IMM_F:
    fprintf(out, "  mov rax, 0x%llx\n", (unsigned long long)v.imm);
    break;
  case IRV_STR:
    fprintf(out, "  lea rax, [rel str%d]\n", v.idx);
    break;
  case IRV_NULL:
    fprintf(out, "  xor eax, eax\n");
    break;
  }
}

/**
 * @brief Materializes a value into rbx
 * @param f Owning function (for temp slot types)
 * @param v Value to load
 */
static void cg_val_rbx(IrFunc *f, IrVal v) {
  (void)f;
  switch (v.kind) {
  case IRV_TEMP:
  case IRV_VAR:
    fprintf(out, "  mov rbx, [rbp - %d]\n", slot_off(v.idx));
    break;
  case IRV_GLOBAL:
    fprintf(out, "  mov rbx, [rel global_%s]\n", cg_mod->globals[v.idx].name);
    break;
  case IRV_IMM_I:
    fprintf(out, "  mov rbx, %lld\n", v.imm);
    break;
  case IRV_IMM_F:
    fprintf(out, "  mov rbx, 0x%llx\n", (unsigned long long)v.imm);
    break;
  case IRV_STR:
    fprintf(out, "  lea rbx, [rel str%d]\n", v.idx);
    break;
  case IRV_NULL:
    fprintf(out, "  xor ebx, ebx\n");
    break;
  }
}

/**
 * @brief Stores rax into a frame slot
 * @param dst Destination temp slot
 */
static void cg_store(int dst) {
  fprintf(out, "  mov [rbp - %d], rax\n", slot_off(dst));
}

/**
 * @brief Emits integer arithmetic with overflow and divide traps
 * @param f Owning function
 * @param ins Arithmetic instruction (operands already integer-typed)
 */
static void cg_arith_int(IrFunc *f, IrInstr *ins) {
  cg_val(f, ins->v[0]);
  cg_val_rbx(f, ins->v[1]);
  switch (ins->op) {
  case IR_ADD:
    fprintf(out, "  add rax, rbx\n");
    fprintf(out, "  jo overflow_trap\n");
    break;
  case IR_SUB:
    fprintf(out, "  sub rax, rbx\n");
    fprintf(out, "  jo overflow_trap\n");
    break;
  case IR_MUL:
    fprintf(out, "  imul rax, rbx\n");
    fprintf(out, "  jo overflow_trap\n");
    break;
  case IR_DIV:
    fprintf(out, "  test rbx, rbx\n");
    fprintf(out, "  jz divzero_trap\n");
    fprintf(out, "  cqo\n");
    fprintf(out, "  idiv rbx\n");
    break;
  default:
    fprintf(out, "  test rbx, rbx\n");
    fprintf(out, "  jz divzero_trap\n");
    fprintf(out, "  cqo\n");
    fprintf(out, "  idiv rbx\n");
    fprintf(out, "  mov rax, rdx\n");
    break;
  }
  cg_store(ins->dst);
}

/**
 * @brief Emits float arithmetic (operands are double bits in rax/rbx)
 * @param f Owning function
 * @param ins Arithmetic instruction
 */
static void cg_arith_float(IrFunc *f, IrInstr *ins) {
  cg_val(f, ins->v[0]);
  if (ir_val_type(f, ins->v[0]) == IR_FLOAT) {
    fprintf(out, "  movq xmm0, rax\n");
  } else {
    fprintf(out, "  cvtsi2sd xmm0, rax\n");
  }
  cg_val_rbx(f, ins->v[1]);
  if (ir_val_type(f, ins->v[1]) == IR_FLOAT) {
    fprintf(out, "  movq xmm1, rbx\n");
  } else {
    fprintf(out, "  cvtsi2sd xmm1, rbx\n");
  }
  if (ins->op == IR_ADD) {
    fprintf(out, "  addsd xmm0, xmm1\n");
  } else if (ins->op == IR_SUB) {
    fprintf(out, "  subsd xmm0, xmm1\n");
  } else if (ins->op == IR_MUL) {
    fprintf(out, "  mulsd xmm0, xmm1\n");
  } else {
    fprintf(out, "  divsd xmm0, xmm1\n");
  }
  fprintf(out, "  movq rax, xmm0\n");
  cg_store(ins->dst);
}

/**
 * @brief Emits an integer comparison with 0/1 result in rax
 * @param cond Comparison condition
 */
static void cg_setcc_int(int cond) {
  if (cond == IR_CEQ) {
    fprintf(out, "  sete al\n");
  } else if (cond == IR_CNE) {
    fprintf(out, "  setne al\n");
  } else if (cond == IR_CLT) {
    fprintf(out, "  setl al\n");
  } else if (cond == IR_CGT) {
    fprintf(out, "  setg al\n");
  } else if (cond == IR_CLE) {
    fprintf(out, "  setle al\n");
  } else {
    fprintf(out, "  setge al\n");
  }
  fprintf(out, "  movzx rax, al\n");
}

/**
 * @brief Emits a comparison with 0/1 result stored to dst
 * @param f Owning function
 * @param ins Comparison instruction
 */
static void cg_cmp(IrFunc *f, IrInstr *ins) {
  if (ins->type == IR_STR) {
    cg_val(f, ins->v[0]);
    fprintf(out, "  push rax\n");
    cg_val(f, ins->v[1]);
    fprintf(out, "  mov rbx, rax\n");
    fprintf(out, "  pop rax\n");
    fprintf(out, "  mov rcx, rax\n");
    fprintf(out, "  mov rdx, rbx\n");
    gen_runtime_prologue();
    fprintf(out, "  call strcmp\n");
    gen_runtime_epilogue();
    fprintf(out, "  test eax, eax\n");
    cg_setcc_int(ins->cond);
    cg_store(ins->dst);
    return;
  }
  cg_val(f, ins->v[0]);
  cg_val_rbx(f, ins->v[1]);
  if (ins->type == IR_FLOAT) {
    if (ir_val_type(f, ins->v[0]) == IR_FLOAT) {
      fprintf(out, "  movq xmm0, rax\n");
    } else {
      fprintf(out, "  cvtsi2sd xmm0, rax\n");
    }
    if (ir_val_type(f, ins->v[1]) == IR_FLOAT) {
      fprintf(out, "  movq xmm1, rbx\n");
    } else {
      fprintf(out, "  cvtsi2sd xmm1, rbx\n");
    }
    fprintf(out, "  ucomisd xmm0, xmm1\n");
    if (ins->cond == IR_CEQ) {
      fprintf(out, "  sete al\n");
      fprintf(out, "  setnp bl\n");
      fprintf(out, "  and al, bl\n");
    } else if (ins->cond == IR_CNE) {
      fprintf(out, "  setne al\n");
      fprintf(out, "  setp bl\n");
      fprintf(out, "  or al, bl\n");
    } else if (ins->cond == IR_CLT) {
      fprintf(out, "  setb al\n");
      fprintf(out, "  setnp bl\n");
      fprintf(out, "  and al, bl\n");
    } else if (ins->cond == IR_CGT) {
      fprintf(out, "  seta al\n");
    } else if (ins->cond == IR_CLE) {
      fprintf(out, "  setbe al\n");
      fprintf(out, "  setnp bl\n");
      fprintf(out, "  and al, bl\n");
    } else {
      fprintf(out, "  setae al\n");
    }
    fprintf(out, "  movzx rax, al\n");
    cg_store(ins->dst);
    return;
  }
  fprintf(out, "  cmp rax, rbx\n");
  cg_setcc_int(ins->cond);
  cg_store(ins->dst);
}

/**
 * @brief Emits a value-connection instruction (const/copy/conversion/load)
 * @param f Owning function
 * @param ins Instruction
 */
static void cg_move(IrFunc *f, IrInstr *ins) {
  switch (ins->op) {
  case IR_CONST_I:
    fprintf(out, "  mov rax, %lld\n", ins->v[0].imm);
    break;
  case IR_CONST_F:
    fprintf(out, "  mov rax, 0x%llx\n",
            (unsigned long long)ins->v[0].imm);
    break;
  case IR_CONST_S:
    fprintf(out, "  lea rax, [rel str%d]\n", ins->v[0].idx);
    break;
  case IR_CONST_NULL:
    fprintf(out, "  xor eax, eax\n");
    break;
  case IR_COPY:
    cg_val(f, ins->v[0]);
    break;
  case IR_I2F:
    cg_val(f, ins->v[0]);
    fprintf(out, "  cvtsi2sd xmm0, rax\n");
    fprintf(out, "  movq rax, xmm0\n");
    break;
  case IR_F2I:
    cg_val(f, ins->v[0]);
    fprintf(out, "  movq xmm0, rax\n");
    fprintf(out, "  cvttsd2si rax, xmm0\n");
    break;
  case IR_TRUNC8:
    cg_val(f, ins->v[0]);
    fprintf(out, "  movzx rax, al\n");
    break;
  case IR_LOAD:
    fprintf(out, "  mov rax, [rbp - %d]\n", slot_off(ins->v[0].idx));
    break;
  case IR_LOADG:
    fprintf(out, "  mov rax, [rel global_%s]\n",
            cg_mod->globals[ins->v[0].idx].name);
    break;
  default:
    break;
  }
  cg_store(ins->dst);
}

/**
 * @brief Emits a store into a variable or global
 * @param f Owning function
 * @param ins Store instruction (v[0] is the destination)
 */
static void cg_store_op(IrFunc *f, IrInstr *ins) {
  cg_val(f, ins->v[1]);
  if (ins->op == IR_STORE) {
    fprintf(out, "  mov [rbp - %d], rax\n", slot_off(ins->v[0].idx));
  } else {
    fprintf(out, "  mov [rel global_%s], rax\n",
            cg_mod->globals[ins->v[0].idx].name);
  }
}

/**
 * @brief Emits a jump or conditional branch
 * @param f Owning function
 * @param fi Function index (for label names)
 * @param ins Jump instruction
 */
static void cg_jump(IrFunc *f, int fi, IrInstr *ins) {
  (void)f;
  if (ins->op == IR_JUMP) {
    fprintf(out, "  jmp f%d_bb%d\n", fi, ins->t);
    return;
  }
  cg_val(f, ins->v[0]);
  fprintf(out, "  cmp rax, 0\n");
  fprintf(out, "  je f%d_bb%d\n", fi, ins->f);
  fprintf(out, "  jmp f%d_bb%d\n", fi, ins->t);
}

/*__CG3__*/

/**
 * @brief Maps a function name to its assembly label
 * @param name Function name from source
 * @return Assembly label (fn main lives at jot_main, entry stays main)
 */
static const char *func_label(const char *name) {
  if (cg_mod->has_user_main && strcmp(name, "main") == 0) {
    return "jot_main";
  }
  return name;
}

/**
 * @brief Emits a plain call with the Windows x64 frame convention
 * @param f Owning function
 * @param ins Call instruction (v[0] is the receiver for methods)
 * @param is_method Non-zero for method dispatch
 */
static void cg_call(IrFunc *f, IrInstr *ins, int is_method) {
  const char *regs[4] = {"rcx", "rdx", "r8", "r9"};
  int first = is_method ? 1 : 0;
  int total = first + ins->nlist;
  int frame = 64 + 8 * (total > 4 ? total - 4 : 0);
  if (frame % 16 != 0) {
    frame += 8;
  }
  fprintf(out, "  sub rsp, %d\n", frame);
  int i = 0;
  if (is_method) {
    cg_val(f, ins->v[0]);
    fprintf(out, "  test rax, rax\n");
    fprintf(out, "  jz null_trap\n");
    fprintf(out, "  mov [rsp + %d], rax\n", frame - 32);
    i = 1;
  }
  for (int k = 0; k < ins->nlist; k++, i++) {
    cg_val(f, ins->list[k]);
    if (i < 4) {
      fprintf(out, "  mov [rsp + %d], rax\n", frame - 32 + 8 * i);
    } else {
      fprintf(out, "  mov [rsp + %d], rax\n", 32 + 8 * (i - 4));
    }
  }
  for (i = 0; i < total && i < 4; i++) {
    fprintf(out, "  mov %s, [rsp + %d]\n", regs[i], frame - 32 + 8 * i);
  }
  if (is_method) {
    /* Virtual dispatch: class id header -> class vtable -> method. */
    fprintf(out, "  mov rax, [rsp + %d]\n", frame - 32);
    fprintf(out, "  mov rax, [rax - 8]\n");
    fprintf(out, "  lea r11, [rel jot_vtables]\n");
    fprintf(out, "  mov r11, [r11 + rax*8]\n");
    fprintf(out, "  mov rax, [r11 + %d]\n", ins->callee * 8);
    fprintf(out, "  call rax\n");
  } else {
    fprintf(out, "  call %s\n", func_label(ins->name));
  }
  fprintf(out, "  add rsp, %d\n", frame);
  if (ins->dst >= 0) {
    cg_store(ins->dst);
  }
}

/**
 * @brief Emits an input call with an optional prompt
 * @param f Owning function
 * @param ins Input instruction (v[0] is the prompt when present)
 */
static void cg_input(IrFunc *f, IrInstr *ins) { if (ins->nlist > 0) { cg_val(f, ins->list[0]);
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
  cg_store(ins->dst);
}

/**
 * @brief Emits a string length read
 * @param f Owning function
 * @param ins Length instruction
 */
static void cg_len(IrFunc *f, IrInstr *ins) { cg_val(f, ins->list[0]);
  fprintf(out, "  mov rcx, rax\n");
  gen_runtime_prologue();
  fprintf(out, "  call strlen\n");
  gen_runtime_epilogue();
  cg_store(ins->dst);
}

/**
 * @brief Emits tostr (formats into a malloc'd buffer)
 * @param f Owning function
 * @param ins Tostr instruction
 */
static void cg_to_str(IrFunc *f, IrInstr *ins) { int is_float = ir_val_type(f, ins->list[0]) == IR_FLOAT; cg_val(f, ins->list[0]);
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
  fprintf(out, "  lea r8, [rel %s]\n", is_float ? "fmt_float_raw" : "fmt_int_raw");
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
  cg_store(ins->dst);
}

/**
 * @brief Emits tonum (parses a string into a double)
 * @param f Owning function
 * @param ins Tonum instruction
 */
static void cg_to_num(IrFunc *f, IrInstr *ins) { cg_val(f, ins->list[0]);
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz null_trap\n");
  fprintf(out, "  mov rcx, rax\n");
  fprintf(out, "  xor edx, edx\n");
  gen_runtime_prologue();
  fprintf(out, "  call strtod\n");
  gen_runtime_epilogue();
  fprintf(out, "  movq rax, xmm0\n");
  cg_store(ins->dst);
}

/**
 * @brief Emits readFile (whole file into a heap string)
 * @param f Owning function
 * @param ins Readfile instruction
 */
static void cg_read_file(IrFunc *f, IrInstr *ins) { cg_val(f, ins->list[0]);
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz null_trap\n");
  fprintf(out, "  sub rsp, 64\n");
  fprintf(out, "  mov [rsp + 0], rax\n");
  fprintf(out, "  mov rcx, [rsp + 0]\n");
  fprintf(out, "  lea rdx, [rel fmt_open_r]\n");
  gen_runtime_prologue();
  fprintf(out, "  call fopen\n");
  gen_runtime_epilogue();
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz open_trap\n");
  fprintf(out, "  mov [rsp + 8], rax\n");
  fprintf(out, "  mov rcx, [rsp + 8]\n");
  fprintf(out, "  xor edx, edx\n");
  fprintf(out, "  mov r8d, 2\n");
  gen_runtime_prologue();
  fprintf(out, "  call fseek\n");
  gen_runtime_epilogue();
  fprintf(out, "  mov rcx, [rsp + 8]\n");
  gen_runtime_prologue();
  fprintf(out, "  call ftell\n");
  gen_runtime_epilogue();
  fprintf(out, "  movsxd rax, eax\n");
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  js open_trap\n");
  fprintf(out, "  mov [rsp + 16], rax\n");
  fprintf(out, "  mov rcx, [rsp + 16]\n");
  fprintf(out, "  add rcx, 1\n");
  fprintf(out, "  jo overflow_trap\n");
  gen_runtime_prologue();
  fprintf(out, "  call malloc\n");
  gen_runtime_epilogue();
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz alloc_trap\n");
  fprintf(out, "  mov [rsp + 24], rax\n");
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
  fprintf(out, "  mov rcx, [rsp + 8]\n");
  gen_runtime_prologue();
  fprintf(out, "  call fclose\n");
  gen_runtime_epilogue();
  fprintf(out, "  mov rcx, [rsp + 24]\n");
  fprintf(out, "  mov rdx, [rsp + 16]\n");
  fprintf(out, "  mov byte [rcx + rdx], 0\n");
  fprintf(out, "  mov rax, [rsp + 24]\n");
  fprintf(out, "  add rsp, 64\n");
  cg_store(ins->dst);
}

/**
 * @brief Emits writeFile (writes text, result is bytes written)
 * @param f Owning function
 * @param ins Writefile instruction
 */
static void cg_write_file(IrFunc *f, IrInstr *ins) {
  cg_val(f, ins->list[0]);
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz null_trap\n");
  fprintf(out, "  sub rsp, 64\n");
  fprintf(out, "  mov [rsp + 0], rax\n");
  cg_val(f, ins->list[1]);
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz null_trap\n");
  fprintf(out, "  mov [rsp + 8], rax\n");
  fprintf(out, "  mov rcx, [rsp + 8]\n");
  gen_runtime_prologue();
  fprintf(out, "  call strlen\n");
  gen_runtime_epilogue();
  fprintf(out, "  mov [rsp + 16], rax\n");
  fprintf(out, "  mov rcx, [rsp + 0]\n");
  fprintf(out, "  lea rdx, [rel fmt_open_w]\n");
  gen_runtime_prologue();
  fprintf(out, "  call fopen\n");
  gen_runtime_epilogue();
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz open_trap\n");
  fprintf(out, "  mov [rsp + 24], rax\n");
  fprintf(out, "  mov rcx, [rsp + 8]\n");
  fprintf(out, "  mov rdx, 1\n");
  fprintf(out, "  mov r8, [rsp + 16]\n");
  fprintf(out, "  mov r9, [rsp + 24]\n");
  gen_runtime_prologue();
  fprintf(out, "  call fwrite\n");
  gen_runtime_epilogue();
  fprintf(out, "  mov [rsp + 32], rax\n");
  fprintf(out, "  mov rcx, [rsp + 24]\n");
  gen_runtime_prologue();
  fprintf(out, "  call fclose\n");
  gen_runtime_epilogue();
  fprintf(out, "  mov rax, [rsp + 32]\n");
  fprintf(out, "  add rsp, 64\n");
  cg_store(ins->dst);
}

/**
 * @brief Emits a print of a literal chunk or a value
 * @param f Owning function
 * @param ins Print instruction (aux selects raw formats for interpolation)
 */
static void cg_print(IrFunc *f, IrInstr *ins) {
  if (ins->op == IR_PRINT_S) {
    fprintf(out, "  lea rcx, [rel fmt_str]\n");
    fprintf(out, "  lea rdx, [rel str%d]\n", ins->aux);
    fprintf(out, "  sub rsp, 32\n");
    fprintf(out, "  call printf\n");
    fprintf(out, "  add rsp, 32\n");
    return;
  }
  cg_val(f, ins->v[0]);
  if (ins->type == IR_STR) {
    fprintf(out, "  lea rcx, [rel fmt_str]\n");
    fprintf(out, "  mov rdx, rax\n");
  } else if (ins->type == IR_FLOAT) {
    fprintf(out, "  movq xmm0, rax\n");
    fprintf(out, "  movq rdx, xmm0\n");
    if (ins->aux) {
      fprintf(out, "  lea rcx, [rel fmt_float_raw]\n");
    } else {
      fprintf(out, "  lea rcx, [rel fmt_float]\n");
    }
    fprintf(out, "  movapd xmm1, xmm0\n");
  } else {
    if (ins->aux) {
      fprintf(out, "  lea rcx, [rel fmt_int_raw]\n");
    } else {
      fprintf(out, "  lea rcx, [rel fmt_int]\n");
    }
    fprintf(out, "  mov rdx, rax\n");
  }
  fprintf(out, "  sub rsp, 32\n");
  fprintf(out, "  call printf\n");
  fprintf(out, "  add rsp, 32\n");
}

/**
 * @brief Emits a function return or process exit
 * @param f Owning function
 * @param ins Return or exit instruction
 * @param is_entry Non-zero for the entry program (exit, never ret)
 */
static void cg_ret(IrFunc *f, IrInstr *ins, int is_entry) {
  if (ins->op == IR_EXIT) {
    cg_val(f, ins->v[0]);
    fprintf(out, "  mov rcx, rax\n");
    fprintf(out, "  sub rsp, 32\n");
    fprintf(out, "  call exit\n");
    return;
  }
  if (is_entry) {
    return;
  }
  if (ins->nv > 0) {
    cg_val(f, ins->v[0]);
  }
  fprintf(out, "  mov rsp, rbp\n");
  fprintf(out, "  pop rbp\n");
  fprintf(out, "  ret\n");
}

/**
 * @brief Emits an array allocation with evaluated elements on the stack
 * @param f Owning function
 * @param ins New-array instruction (aux holds the count)
 */
static void cg_newarr(IrFunc *f, IrInstr *ins) {
  for (int i = 0; i < ins->nlist; i++) {
    cg_val(f, ins->list[i]);
    fprintf(out, "  push rax\n");
  }
  fprintf(out, "  mov rcx, %d\n", (ins->aux + 1) * 8);
  gen_runtime_prologue();
  fprintf(out, "  call malloc\n");
  gen_runtime_epilogue();
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz alloc_trap\n");
  fprintf(out, "  mov rbx, rax\n");
  fprintf(out, "  mov QWORD [rbx], %d\n", ins->aux);
  cg_store(ins->dst);
  for (int i = ins->aux - 1; i >= 0; i--) {
    fprintf(out, "  pop rax\n");
    fprintf(out, "  mov [rbx + %d], rax\n", i * 8 + 8);
  }
}

/**
 * @brief Emits a bounds-checked array element load
 * @param f Owning function
 * @param ins Load instruction (array, index)
 */
static void cg_arr_load(IrFunc *f, IrInstr *ins) {
  cg_val(f, ins->v[0]);
  fprintf(out, "  push rax\n");
  cg_val(f, ins->v[1]);
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
  cg_store(ins->dst);
}

/**
 * @brief Emits a bounds-checked array element store (=, +=, -=)
 * @param f Owning function
 * @param ins Store instruction (array, index, value; aux holds the op)
 */
static void cg_arr_store(IrFunc *f, IrInstr *ins) {
  cg_val(f, ins->v[0]);
  fprintf(out, "  push rax\n");
  cg_val(f, ins->v[1]);
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
  if (ins->aux == 0) {
    cg_val(f, ins->v[2]);
    fprintf(out, "  mov rdx, [rsp]\n");
    fprintf(out, "  mov [rdx], rax\n");
    fprintf(out, "  add rsp, 8\n");
    return;
  }
  if (ins->aux2 == (int)IR_STR) {
    fprintf(out, "  mov rdx, [rsp]\n");
    fprintf(out, "  mov rax, [rdx]\n");
    fprintf(out, "  push rax\n");
    cg_val(f, ins->v[2]);
    fprintf(out, "  mov rbx, rax\n");
    fprintf(out, "  pop rax\n");
    gen_string_concat();
    fprintf(out, "  mov rdx, [rsp]\n");
    fprintf(out, "  mov [rdx], rax\n");
    fprintf(out, "  add rsp, 8\n");
    return;
  }
  if (ins->aux2 == (int)IR_FLOAT) {
    cg_val(f, ins->v[2]);
    fprintf(out, "  movq xmm1, rax\n");
    fprintf(out, "  mov rdx, [rsp]\n");
    fprintf(out, "  movq xmm0, [rdx]\n");
    if (ins->aux == 1) {
      fprintf(out, "  addsd xmm0, xmm1\n");
    } else {
      fprintf(out, "  subsd xmm0, xmm1\n");
    }
    fprintf(out, "  movq rax, xmm0\n");
    fprintf(out, "  mov [rdx], rax\n");
    fprintf(out, "  add rsp, 8\n");
    return;
  }
  cg_val(f, ins->v[2]);
  fprintf(out, "  mov rdx, [rsp]\n");
  fprintf(out, "  mov rcx, [rdx]\n");
  if (ins->aux == 1) {
    fprintf(out, "  add rax, rcx\n");
  } else {
    fprintf(out, "  sub rcx, rax\n");
    fprintf(out, "  mov rax, rcx\n");
  }
  fprintf(out, "  jo overflow_trap\n");
  fprintf(out, "  mov [rdx], rax\n");
  fprintf(out, "  add rsp, 8\n");
}

/**
 * @brief Emits an array length read
 * @param f Owning function
 * @param ins Length instruction
 */
static void cg_arr_len(IrFunc *f, IrInstr *ins) {
  cg_val(f, ins->v[0]);
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz null_trap\n");
  fprintf(out, "  mov rax, [rax]\n");
  cg_store(ins->dst);
}

/**
 * @brief Emits a string character-code load with bounds checks
 * @param f Owning function
 * @param ins Index instruction (string, index)
 */
static void cg_str_idx(IrFunc *f, IrInstr *ins) {
  cg_val(f, ins->v[0]);
  fprintf(out, "  push rax\n");
  cg_val(f, ins->v[1]);
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
  cg_store(ins->dst);
}

/**
 * @brief Emits a struct/class allocation with evaluated fields
 * @param f Owning function
 * @param ins New-instance instruction (aux holds bytes, aux2 the class)
 */
static void cg_newinst(IrFunc *f, IrInstr *ins) {
  for (int i = 0; i < ins->nlist; i++) {
    cg_val(f, ins->list[i]);
    fprintf(out, "  push rax\n");
  }
  fprintf(out, "  mov rcx, %d\n", ins->aux);
  gen_runtime_prologue();
  fprintf(out, "  call malloc\n");
  gen_runtime_epilogue();
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz alloc_trap\n");
  fprintf(out, "  mov r11, rax\n");
  if (ins->aux2 > 0) {
    fprintf(out, "  mov QWORD [r11], %d\n", ins->aux2 - 1);
    fprintf(out, "  add r11, 8\n");
  }
  for (int i = ins->nlist - 1; i >= 0; i--) {
    fprintf(out, "  pop rdx\n");
    fprintf(out, "  mov [r11 + %d], rdx\n", i * 8);
  }
  fprintf(out, "  mov rax, r11\n");
  cg_store(ins->dst);
}

/**
 * @brief Emits a field load with a null check
 * @param f Owning function
 * @param ins Field load (object in v[0], field index in aux)
 */
static void cg_field_load(IrFunc *f, IrInstr *ins) {
  cg_val(f, ins->v[0]);
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz null_trap\n");
  fprintf(out, "  mov rax, [rax + %d]\n", ins->aux * 8);
  cg_store(ins->dst);
}

/**
 * @brief Emits a field store with a null check
 * @param f Owning function
 * @param ins Field store (object, value; field index in aux)
 */
static void cg_field_store(IrFunc *f, IrInstr *ins) {
  cg_val(f, ins->v[1]);
  fprintf(out, "  push rax\n");
  cg_val(f, ins->v[0]);
  fprintf(out, "  test rax, rax\n");
  fprintf(out, "  jz null_trap\n");
  fprintf(out, "  pop rdx\n");
  fprintf(out, "  mov [rax + %d], rdx\n", ins->aux * 8);
}

/**
 * @brief Emits one IR instruction
 * @param f Owning function
 * @param fi Function index (for block labels)
 * @param ins Instruction
 * @param is_entry Non-zero for the entry program
 */
static void cg_instr(IrFunc *f, int fi, IrInstr *ins, int is_entry) {
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
    cg_move(f, ins);
    break;
  case IR_STORE:
  case IR_STOREG:
    cg_store_op(f, ins);
    break;
  case IR_ADD:
  case IR_SUB:
  case IR_MUL:
  case IR_DIV:
  case IR_MOD:
    if (ins->type == IR_FLOAT) {
      cg_arith_float(f, ins);
    } else {
      cg_arith_int(f, ins);
    }
    break;
  case IR_CMP:
    cg_cmp(f, ins);
    break;
  case IR_JUMP:
  case IR_BR:
    cg_jump(f, fi, ins);
    break;
  case IR_CALL:
    cg_call(f, ins, 0);
    break;
  case IR_CALLM:
    cg_call(f, ins, 1);
    break;
  case IR_RET:
  case IR_EXIT:
    cg_ret(f, ins, is_entry);
    break;
  case IR_PRINT_S:
  case IR_PRINT_V:
    cg_print(f, ins);
    break;
  case IR_INPUT:
    cg_input(f, ins);
    break;
  case IR_LEN:
    cg_len(f, ins);
    break;
  case IR_TOSTR:
    cg_to_str(f, ins);
    break;
  case IR_TONUM:
    cg_to_num(f, ins);
    break;
  case IR_READFILE:
    cg_read_file(f, ins);
    break;
  case IR_WRITEFILE:
    cg_write_file(f, ins);
    break;
  case IR_NEWARR:
    cg_newarr(f, ins);
    break;
  case IR_ARR_LOAD:
    cg_arr_load(f, ins);
    break;
  case IR_ARR_STORE:
    cg_arr_store(f, ins);
    break;
  case IR_ARR_LEN:
    cg_arr_len(f, ins);
    break;
  case IR_STR_IDX:
    cg_str_idx(f, ins);
    break;
  case IR_CONCAT:
    cg_val(f, ins->v[0]);
    fprintf(out, "  push rax\n");
    cg_val(f, ins->v[1]);
    fprintf(out, "  mov rbx, rax\n");
    fprintf(out, "  pop rax\n");
    gen_string_concat();
    cg_store(ins->dst);
    break;
  case IR_NEWINST:
    cg_newinst(f, ins);
    break;
  case IR_COPYINST:
    cg_val(f, ins->v[0]);
    gen_instance_copy(ins->aux, ins->aux2, ins->callee);
    cg_store(ins->dst);
    break;
  case IR_FIELD_LOAD:
    cg_field_load(f, ins);
    break;
  case IR_FIELD_STORE:
    cg_field_store(f, ins);
    break;
  }
}

/**
 * @brief Emits one function with params, blocks, and epilogue
 * @param fi Function index (for block labels)
 * @param f IR function
 * @param is_entry Non-zero for the entry program (no epilogue)
 */
static void cg_func(int fi, IrFunc *f, int is_entry) {
  long frame = (long)(f->nvars + f->ntemps) * 8 + 256;
  if (frame < 2048) {
    frame = 2048;
  }
  frame = (frame + 15) & ~15L;
  const char *param_regs[4] = {"rcx", "rdx", "r8", "r9"};
  fprintf(out, "%s:\n", f->label);
  if (is_entry) {
    fprintf(out, "  mov rax, rsp\n");
    fprintf(out, "  sub rax, 262144\n");
    fprintf(out, "  mov [rel stack_floor], rax\n");
  }
  gen_prologue(frame);
  int pidx = 0;
  for (int s = 0; s < f->nvars; s++) {
    if (!f->var_is_param[s]) {
      continue;
    }
    int abs_idx = f->is_method ? pidx + 1 : pidx;
    if (abs_idx < 4) {
      fprintf(out, "  mov [rbp - %d], %s\n", slot_off(s),
              param_regs[abs_idx]);
    } else {
      fprintf(out, "  mov rax, [rbp + %d]\n", 48 + 8 * (abs_idx - 4));
      fprintf(out, "  mov [rbp - %d], rax\n", slot_off(s));
    }
    pidx++;
  }
  for (int b = 0; b < f->nblocks; b++) {
    fprintf(out, "f%d_bb%d:\n", fi, f->blocks[b].id);
    for (int k = 0; k < f->blocks[b].nins; k++) {
      cg_instr(f, fi, &f->blocks[b].ins[k], is_entry);
    }
  }
  if (!is_entry) {
    fprintf(out, "  mov rsp, rbp\n");
    fprintf(out, "  pop rbp\n");
    fprintf(out, "  ret\n");
  }
}

/**
 * @brief Emits the data section (formats, globals, strings, vtables)
 * @param m Module
 */
static void gen_data_section(IrModule *m) {
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
  for (int i = 0; i < m->nglobals; i++) {
    fprintf(out, "  global_%s dq 0\n", m->globals[i].name);
  }
  for (int i = 0; i < m->nstrings; i++) {
    fprintf(out, "  str%d db ", i);
    if (m->strings[i][0] == '\0') {
      fprintf(out, "0");
    } else {
      write_nasm_string(m->strings[i]);
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
        for (int k = 0; k < m->nclasses; k++) {
          if (m->classes[k].class_id == owner) {
            oc = k;
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
    /* Vtables are indexed by class id, so order them by id. */
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

void GenerateAssembly(IrModule *mod, const char *source, const char *output) {
  cg_mod = mod;
  cg_source = source;
  (void)cg_source;

  out = fopen(output, "w");
  if (!out) {
    char message[256];
    snprintf(message, sizeof(message), "Could not open output file '%s'",
             output);
    term_report(TERM_ERROR, source, 1, 1, 1, message);
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
  gen_data_section(mod);
  fprintf(out, "section .text\n");

  for (int fi = 0; fi < mod->nfuncs; fi++) {
    int is_entry = (fi == 0);
    IrFunc *f = &mod->funcs[fi];
    cg_func(fi, f, is_entry);
    if (is_entry) {
      /* Entry always ends in EXIT, but keep a halt for safety. */
      fprintf(out, "  xor ecx, ecx\n");
      fprintf(out, "  sub rsp, 32\n");
      fprintf(out, "  call exit\n");
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
