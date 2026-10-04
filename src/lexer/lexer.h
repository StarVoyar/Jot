#ifndef LEXER_H
#define LEXER_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  INT,
  IDENTIFIER,
  KEYWORD,
  OPERATOR,
  STRING,
  SEPARATOR,
  END_OF_TOKENS,
  UNKNOWN,
} TokenType;

typedef struct {
  TokenType type;
  char *value;
} Token;

Token *Lexer(FILE *file);

Token *lex_int(char current_char, int *current_index);
Token *lex_keyword(char current_char, int *current_index);
Token *lex_separator(char character);
Token *lex_operator(char character, int *current_index);
Token *lex_string(int *current_index);
Token *lex_unknown(char character);

void print_token(Token token);
void free_tokens(Token *tokens);

#endif
