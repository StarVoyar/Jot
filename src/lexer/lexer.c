#include "lexer.h"

static char *global_buffer;

void Lexer(FILE *file) {
  fseek(file, 0, SEEK_END);
  long length = ftell(file);
  fseek(file, 0, SEEK_SET);

  char *buffer = malloc(length + 1);
  fread(buffer, 1, length, file);
  buffer[length] = '\0';

  fclose(file);

  global_buffer = buffer;

  int current_index = 0;

  while (buffer[current_index] != '\0') {

    char character = buffer[current_index];

    if (isspace(character)) {
      current_index++;
    } else if (character == ';') {
      printf("Semicolon\n");
      current_index++;
    } else if (character == '(') {
      printf("Open Paren\n");
      current_index++;
    } else if (character == ')') {
      printf("Close Paren\n");
      current_index++;
    } else if (isdigit(character)) {
      TokenLiteral *current_digit = lex_int_literal(character, &current_index);
      printf("Digit: %s\n", current_digit->value);
      free(current_digit->value);
      free(current_digit);
    } else if (isalpha(character)) {
      TokenKeyword *current_keyword = lex_keyword(character, &current_index);
      if (current_keyword->type == RETURN) {
        printf("Return Keyword\n");
      } else if (current_keyword->type == UNKNOWN) {
        printf("Unknown Keyword\n");
      }
      free(current_keyword);
    } else {
      current_index++;
    }
  }

  printf("END_OF_FILE\n");

  free(buffer);
}

TokenLiteral *lex_int_literal(char current_char, int *current_index) {
  TokenLiteral *token = malloc(sizeof(TokenLiteral));
  token->type = INT;

  char *value = malloc(32);
  int value_index = 0;

  while (isdigit(current_char)) {
    value[value_index++] = current_char;
    (*current_index)++;
    current_char = global_buffer[*current_index];
  }

  value[value_index] = '\0';
  token->value = value;

  return token;
}

TokenKeyword *lex_keyword(char current_char, int *current_index) {
  TokenKeyword *token = malloc(sizeof(TokenKeyword));
  char *keyword = malloc(32);
  int keyword_index = 0;

  while (isalpha(current_char)) {
    keyword[keyword_index++] = current_char;
    (*current_index)++;
    current_char = global_buffer[*current_index];
  }

  keyword[keyword_index] = '\0';

  if (strcmp(keyword, "return") == 0) {
    token->type = RETURN;
  } else {
    token->type = UNKNOWN;
  }

  free(keyword);
  return token;
}
