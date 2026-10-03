#include "lexer.h"

static char *global_buffer;
size_t tokens_index = 0;

Token *Lexer(FILE *file) {
  fseek(file, 0, SEEK_END);
  long length = ftell(file);
  fseek(file, 0, SEEK_SET);

  unsigned char *buffer = malloc(length + 1);
  size_t bytes_read = fread(buffer, 1, length, file);
  buffer[bytes_read] = '\0';

  fclose(file);

  global_buffer = (char *)buffer;

  int current_index = 0;

  Token *tokens = malloc(1024 * sizeof(Token));

  while (buffer[current_index] != '\0') {

    char character = buffer[current_index];

    if (isspace(character)) {
      current_index++;
    } else if (character == ';') {
      Token *token = malloc(sizeof(Token));
      token->value = ";";
      token->type = SEPARATOR;
      tokens[tokens_index] = *token;
      tokens_index++;
      current_index++;
      free(token);
    } else if (character == '(') {
      Token *token = malloc(sizeof(Token));
      token->value = "(";
      token->type = SEPARATOR;
      tokens[tokens_index] = *token;
      tokens_index++;
      current_index++;
      free(token);
    } else if (character == ')') {
      Token *token = malloc(sizeof(Token));
      token->value = ")";
      token->type = SEPARATOR;
      tokens[tokens_index] = *token;
      tokens_index++;
      current_index++;
      free(token);
    } else if (isdigit(character)) {
      Token *token = lex_int(character, &current_index);
      tokens[tokens_index] = *token;
      tokens_index++;
      free(token);
    } else if (isalpha(character)) {
      Token *token = lex_keyword(character, &current_index);
      tokens[tokens_index] = *token;
      tokens_index++;
      free(token);
    } else {
      current_index++;
    }
  }

  free(buffer);

  tokens[tokens_index].value = NULL;
  tokens[tokens_index].type = END_OF_TOKENS;

  return tokens;
}

void print_token(Token token) {
  printf("Token Value: ");
  for (int i = 0; token.value[i] != '\0'; i++) {
    printf("%c", token.value[i]);
  }
  if (token.type == INT) {
    printf(", Token Type: INT \n");
  }
  if (token.type == KEYWORD) {
    printf(", Token Type: KEYWORD \n");
  }
  if (token.type == SEPARATOR) {
    printf(", Token Type: SEPARATOR \n");
  }
  if (token.type == UNKNOWN) {
    printf(", Token Type: UNKNOWN \n");
  }
}

Token *lex_int(char current_char, int *current_index) {
  Token *token = malloc(sizeof(Token));
  token->type = INT;

  char value[32];
  int value_index = 0;

  while (isdigit(current_char) && current_char != '\0' && value_index < 31) {
    value[value_index++] = current_char;
    (*current_index)++;
    current_char = global_buffer[*current_index];
  }

  value[value_index] = '\0';
  size_t len = strlen(value);
  char *value_copy = malloc(len + 1);
  memcpy(value_copy, value, len);
  value_copy[len] = '\0';
  token->value = value_copy;

  return token;
}

Token *lex_keyword(char current_char, int *current_index) {
  Token *token = malloc(sizeof(Token));
  char keyword[32];
  int keyword_index = 0;

  while (isalpha(current_char) && current_char != '\0' && keyword_index < 31) {
    keyword[keyword_index++] = current_char;
    (*current_index)++;
    current_char = global_buffer[*current_index];
  }

  keyword[keyword_index] = '\0';

  if (strcmp(keyword, "return") == 0) {
    token->type = KEYWORD;
    token->value = "RETURN";
  } else {
    token->type = UNKNOWN;
    token->value = "UNKNOWN";
  }

  return token;
}

void free_tokens(Token *tokens) {
  if (tokens == NULL)
    return;

  for (size_t i = 0;; i++) {
    if (tokens[i].type == END_OF_TOKENS) {
      break;
    }
    if (tokens[i].value != NULL && tokens[i].type == INT) {
      free(tokens[i].value);
    }
  }
  free(tokens);
}
