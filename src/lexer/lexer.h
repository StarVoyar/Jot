#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  RETURN,
  UNKNOWN,
} TokenTypeKeyword;

typedef enum {
  INT,
} TokenTypeLiteral;

typedef enum {
  SEMICOLON,
  OPEN_PAREN,
  CLOSE_PAREN,
  END_OF_FILE,
} TokenTypeSeparator;

typedef struct {
  TokenTypeKeyword type;
} TokenKeyword;

typedef struct {
  TokenTypeLiteral type;
  char *value;
} TokenLiteral;

typedef struct {
  TokenTypeSeparator type;
} TokenSeparator;

void Lexer(FILE *file);

TokenLiteral *lex_int_literal(char current_char, int *current_index);
TokenKeyword *lex_keyword(char current_char, int *current_index);
