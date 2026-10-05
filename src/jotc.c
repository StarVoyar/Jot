#define _CRT_SECURE_NO_WARNINGS

#include "codegen/codegen.h"
#include "lexer/lexer.h"
#include "parser/parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

/**
 * @brief Prints a compiler error in red (same color as code errors)
 * @param format printf-style message without the Error: prefix
 */
static void compiler_error(const char *format, ...) {
  int color = terminal_setup_colors();
  const char *mark = color ? "\x1b[1;31m" : "";
  const char *reset = color ? "\x1b[0m" : "";
  printf("%sError:%s ", mark, reset);
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
}

/**
 * @brief Prints a compiler warning in yellow (same color as code warnings)
 * @param format printf-style message without the Warning: prefix
 */
static void compiler_warning(const char *format, ...) {
  int color = terminal_setup_colors();
  const char *mark = color ? "\x1b[1;33m" : "";
  const char *reset = color ? "\x1b[0m" : "";
  printf("%sWarning:%s ", mark, reset);
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
}

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
 * @brief Checks if a directory path exists
 * @param path Directory path to check
 * @return Non-zero if directory exists
 */
static int dir_exists(const char *path) {
#ifdef _WIN32
  struct _stat info;
  if (_stat(path, &info) != 0) {
    return 0;
  }
  return (info.st_mode & _S_IFDIR) != 0;
#else
  struct stat info;
  if (stat(path, &info) != 0) {
    return 0;
  }
  return (info.st_mode & S_IFDIR) != 0;
#endif
}

/**
 * @brief Extracts the directory portion from a file path
 * @param path Full file path
 * @return Allocated directory path (caller must free)
 */
static char *extract_dir(const char *path) {
  const char *last_sep = strrchr(path, '/');
  const char *last_sep_win = strrchr(path, '\\');
  if (last_sep_win != NULL && (last_sep == NULL || last_sep_win > last_sep)) {
    last_sep = last_sep_win;
  }
  if (last_sep == NULL) {
    char *result = malloc(2);
    result[0] = '.';
    result[1] = '\0';
    return result;
  }
  size_t len = last_sep - path;
  char *result = malloc(len + 1);
  memcpy(result, path, len);
  result[len] = '\0';
  return result;
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
    printf("Usage: %s <file.jot> [-o output] [--debug]\n", argv[0]);
    printf("  -o output    Output file (e.g., main.exe, main.o, or main)\n");
    printf("  --debug      Print tokens, AST, and assembly path\n");
    return 1;
  }

  int debug = 0;
  const char *out_arg = NULL;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--debug") == 0) {
      debug = 1;
    } else if (strcmp(argv[i], "-o") == 0) {
      if (i + 1 < argc) {
        out_arg = argv[i + 1];
        i++;
      } else {
        compiler_error("-o requires an output path\n");
        return 1;
      }
    } else if (out_arg == NULL) {
      out_arg = argv[i];
    } else {
      printf("Usage: %s <file.jot> [-o output] [--debug]\n", argv[0]);
      return 1;
    }
  }

  FILE *file;
  file = fopen(argv[1], "r");
  if (!file) {
    compiler_error("Could not open file '%s'\n", argv[1]);
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
  char *out_dir = NULL;
  if (out_arg != NULL) {
    out_file = out_arg;
    out_dir = extract_dir(out_arg);
    if (!dir_exists(out_dir)) {
      compiler_error("Output directory '%s' does not exist\n", out_dir);
      free(out_dir);
      free(default_out);
      return 1;
    }
    free(out_dir);
  } else {
    ensure_dir("build");
    ensure_dir("build/bin");
    ensure_dir("build/bin/generated");
    default_out = default_out_path(argv[1]);
    out_file = default_out;
  }

  FILE *check = fopen(out_file, "r");
  if (check != NULL) {
    fclose(check);
    compiler_warning(
        "Output file '%s' already exists and will be overwritten\n", out_file);
  }

  GenerateAssembly(ast, argv[1], out_file);
  if (debug) {
    printf("Assembly written to '%s'\n", out_file);
  }

  free(default_out);

  return 0;
}
