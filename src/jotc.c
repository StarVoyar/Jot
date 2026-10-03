#define _CRT_SECURE_NO_WARNINGS

#include "lexer/lexer.h"

#include <stdio.h>

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Usage: %s <file.jot>\n", argv[0]);
    return 1;
  }

  FILE *file;
  file = fopen(argv[1], "r");
  if (!file) {
    printf("Error: Could not open file '%s'\n", argv[1]);
    return 1;
  }

  Token *tokens = Lexer(file);

  for (size_t i = 0; tokens[i].type != END_OF_TOKENS; i++) {
    print_token(tokens[i]);
  }

  return 0;
}
