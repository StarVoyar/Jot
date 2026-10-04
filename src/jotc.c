#define _CRT_SECURE_NO_WARNINGS

#include "codegen/codegen.h"
#include "lexer/lexer.h"
#include "parser/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

/**
 * @brief Creates a directory if it does not exist
 * @param path Directory path to create
 */
static void ensure_dir(const char *path) {
#ifdef _WIN32
  _mkdir(path);
#else
  mkdir(path, 0755);
#endif
}

/**
 * @brief Builds the default asm path for an input file
 * @param input Input file path (e.g. test/test.jot)
 * @return Allocated output path (build/bin/generated/<base>.asm)
 */
static char *default_out_path(const char *input) {
  const char *base = strrchr(input, '/');
  const char *base_win = strrchr(input, '\\');
  if (base_win != NULL && (base == NULL || base_win > base)) {
    base = base_win;
  }
  if (base != NULL) {
    base++;
  } else {
    base = input;
  }

  size_t len = strlen(base);
  if (len > 4 && strcmp(base + len - 4, ".jot") == 0) {
    len -= 4;
  }

  const char *dir = "build/bin/generated/";
  size_t dir_len = strlen(dir);
  char *out = malloc(dir_len + len + 5);
  memcpy(out, dir, dir_len);
  memcpy(out + dir_len, base, len);
  memcpy(out + dir_len + len, ".asm", 5);
  return out;
}

int main(int argc, char *argv[]) {
  if (argc < 2 || strcmp(argv[1], "--help") == 0) {
    printf("Usage: %s <file.jot> [output.asm] [--debug]\n", argv[0]);
    return 1;
  }

  int debug = 0;
  const char *out_arg = NULL;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--debug") == 0) {
      debug = 1;
    } else if (out_arg == NULL) {
      out_arg = argv[i];
    } else {
      printf("Usage: %s <file.jot> [output.asm] [--debug]\n", argv[0]);
      return 1;
    }
  }

  FILE *file;
  file = fopen(argv[1], "r");
  if (!file) {
    printf("Error: Could not open file '%s'\n", argv[1]);
    return 1;
  }

  Token *tokens = Lexer(file);

  if (debug) {
    for (size_t i = 0; tokens[i].type != END_OF_TOKENS; i++) {
      print_token(tokens[i]);
    }
  }

  Node *ast = Parser(tokens, argv[1]);
  if (debug) {
    print_tree(ast);
    printf("\n");
  }

  char *default_out = NULL;
  const char *out_file = NULL;
  if (out_arg != NULL) {
    out_file = out_arg;
  } else {
    ensure_dir("build");
    ensure_dir("build/bin");
    ensure_dir("build/bin/generated");
    default_out = default_out_path(argv[1]);
    out_file = default_out;
  }

  GenerateAssembly(ast, out_file);
  if (debug) {
    printf("Assembly written to '%s'\n", out_file);
  }

  free(default_out);

  return 0;
}
