#define _CRT_SECURE_NO_WARNINGS
#undef strdup

#include "codegen.h"
#include "regalloc.h"
#include "target.h"
#include "x86.h"

void GenerateAssemblyFp(IrModule *mod, const char *source, FILE *out,
                        const char *target) {
  const Target *t = target_host();
  if (target != NULL) {
    t = target_by_name(target);
    if (t == NULL) {
      char message[128];
      snprintf(message, sizeof(message),
               "Unknown target '%s' (expected win64 or elf64)", target);
      term_report(TERM_ERROR, source, 1, 1, 1, message);
      return;
    }
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
  x86_data_section(mod, out);
  fprintf(out, "section .text\n");

  for (int fi = 0; fi < mod->nfuncs; fi++) {
    int is_entry = (fi == 0);
    RegAlloc alloc;
    regalloc_func(&mod->funcs[fi], t, &alloc);
    x86_emit_func(mod, &mod->funcs[fi], fi, is_entry, t, &alloc, out);
    regalloc_free(&alloc);
    if (is_entry) {
      /* Entry always ends in EXIT, but keep a halt for safety. */
      x86_emit_halt(t, out);
    }
  }

  x86_emit_traps(t, out);
}

void GenerateAssembly(IrModule *mod, const char *source, const char *output,
                      const char *target) {
  FILE *out = fopen(output, "wb");
  if (!out) {
    char message[256];
    snprintf(message, sizeof(message), "Could not open output file '%s'",
             output);
    term_report(TERM_ERROR, source, 1, 1, 1, message);
    return;
  }

  GenerateAssemblyFp(mod, source, out, target);
  fclose(out);
}
