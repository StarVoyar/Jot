#ifndef LEXER_H
#define LEXER_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  INT,
  KEYWORD,
  SEPARATOR,
  END_OF_TOKENS,
  UNKNOWN,
} TokenType;

typedef struct {
  TokenType type;
  char *value;
} Token;

Token *Lexer(FILE *file);

void print_token(Token token);
Token *lex_int(char current_char, int *current_index);
Token *lex_keyword(char current_char, int *current_index);
void free_tokens(Token *tokens);

#endif
