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

  Lexer(file);
  return 0;
}
