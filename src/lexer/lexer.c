#include "lexer.h"

/** Global buffer holding the source file contents */
static char *global_buffer;

/** Index tracking the current token position in the token array */
size_t tokens_index = 0;

/**
 * @brief Lexes a source file into a stream of tokens
 * @param file Source file to lex
 * @return Array of tokens terminated by END_OF_TOKENS
 */
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
      switch (character) {
      case ';':
      case '(':
      case ')':
      case '{':
      case '}':
      case '[':
      case ']':
      case ',': {
        Token *token = lex_separator(character);
        tokens[tokens_index] = *token;
        tokens_index++;
        current_index++;
        free(token);
        break;
      }
      case '+':
      case '-':
      case '*':
      case '/':
      case '=':
      case '!':
      case '<':
      case '>': {
        Token *token = lex_operator(character, &current_index);
        tokens[tokens_index] = *token;
        tokens_index++;
        free(token);
        break;
      }
      case '"': {
        Token *token = lex_string(&current_index);
        tokens[tokens_index] = *token;
        tokens_index++;
        free(token);
        break;
      }
      default: {
        Token *token = lex_unknown(character);
        tokens[tokens_index] = *token;
        tokens_index++;
        current_index++;
        free(token);
        break;
      }
      }
    }
  }

  free(buffer);

  tokens[tokens_index].value = NULL;
  tokens[tokens_index].type = END_OF_TOKENS;

  return tokens;
}

/**
 * @brief Lexes an integer literal
 * @param current_char Starting character
 * @param current_index Current position in buffer (updated)
 * @return Token for integer literal
 */
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

/**
 * @brief Lexes a keyword or identifier
 * @param current_char Starting character
 * @param current_index Current position in buffer (updated)
 * @return Token for keyword or identifier
 */
Token *lex_keyword(char current_char, int *current_index) {
  Token *token = malloc(sizeof(Token));
  char keyword[32];
  int keyword_index = 0;

  while ((isalpha(current_char) || current_char == '_') && current_char != '\0' && keyword_index < 31) {
    keyword[keyword_index++] = current_char;
    (*current_index)++;
    current_char = global_buffer[*current_index];
  }

  keyword[keyword_index] = '\0';

  if (strcmp(keyword, "return") == 0 || strcmp(keyword, "if") == 0 ||
      strcmp(keyword, "else") == 0 || strcmp(keyword, "while") == 0 ||
      strcmp(keyword, "for") == 0 || strcmp(keyword, "fn") == 0 ||
      strcmp(keyword, "print") == 0 || strcmp(keyword, "int") == 0 ||
      strcmp(keyword, "bool") == 0 || strcmp(keyword, "string") == 0 ||
      strcmp(keyword, "char") == 0 || strcmp(keyword, "array") == 0) {
    token->type = KEYWORD;
    size_t len = strlen(keyword);
    token->value = malloc(len + 1);
    memcpy(token->value, keyword, len);
    token->value[len] = '\0';
  } else {
    token->type = IDENTIFIER;
    size_t len = strlen(keyword);
    token->value = malloc(len + 1);
    memcpy(token->value, keyword, len);
    token->value[len] = '\0';
  }

  return token;
}

/**
 * @brief Lexes a separator character
 * @param character Separator character
 * @return Token for separator
 */
Token *lex_separator(char character) {
  Token *token = malloc(sizeof(Token));
  char separator[2] = {character, '\0'};
  token->value = malloc(2);
  memcpy(token->value, separator, 2);
  token->type = SEPARATOR;
  return token;
}

/**
 * @brief Lexes an operator (including multi-character operators)
 * @param character Starting character
 * @param current_index Current position in buffer (updated)
 * @return Token for operator
 */
Token *lex_operator(char character, int *current_index) {
  Token *token = malloc(sizeof(Token));
  char op[3] = {character, '\0', '\0'};
  (*current_index)++;
  char next_char = global_buffer[*current_index];

  if ((character == '=' && next_char == '=') || (character == '!' && next_char == '=') ||
      (character == '<' && next_char == '=') || (character == '>' && next_char == '=')) {
    op[1] = next_char;
    (*current_index)++;
  }

  size_t len = strlen(op);
  token->value = malloc(len + 1);
  memcpy(token->value, op, len);
  token->value[len] = '\0';
  token->type = OPERATOR;
  return token;
}

/**
 * @brief Lexes a string literal
 * @param current_index Current position in buffer (updated)
 * @return Token for string literal
 */
Token *lex_string(int *current_index) {
  Token *token = malloc(sizeof(Token));
  (*current_index)++;

  char string[256];
  int string_index = 0;
  char current_char = global_buffer[*current_index];

  while (current_char != '"' && current_char != '\0' && string_index < 255) {
    string[string_index++] = current_char;
    (*current_index)++;
    current_char = global_buffer[*current_index];
  }

  (*current_index)++;
  string[string_index] = '\0';

  size_t len = strlen(string);
  token->value = malloc(len + 1);
  memcpy(token->value, string, len);
  token->value[len] = '\0';
  token->type = STRING;
  return token;
}

/**
 * @brief Lexes an unknown character
 * @param character Unknown character
 * @return Token for unknown character
 */
Token *lex_unknown(char character) {
  Token *token = malloc(sizeof(Token));
  char unknown[2] = {character, '\0'};
  token->value = malloc(2);
  memcpy(token->value, unknown, 2);
  token->type = UNKNOWN;
  return token;
}

/**
 * @brief Prints a token's type and value
 * @param token Token to print
 */
void print_token(Token token) {
  printf("Token Value: ");
  printf("'");
  for (int i = 0; token.value[i] != '\0'; i++) {
    printf("%c", token.value[i]);
  }
  printf("'");

  switch (token.type) {
  case INT:
    printf(", Token Type: INT \n");
    break;
  case IDENTIFIER:
    printf(", Token Type: IDENTIFIER \n");
    break;
  case KEYWORD:
    printf(", Token Type: KEYWORD \n");
    break;
  case OPERATOR:
    printf(", Token Type: OPERATOR \n");
    break;
  case STRING:
    printf(", Token Type: STRING \n");
    break;
  case SEPARATOR:
    printf(", Token Type: SEPARATOR \n");
    break;
  case UNKNOWN:
    printf(", Token Type: UNKNOWN \n");
    break;
  default:
    break;
  }
}

/**
 * @brief Frees memory allocated for token array
 * @param tokens Token array to free
 */
void free_tokens(Token *tokens) {
  if (tokens == NULL)
    return;

  for (size_t i = 0;; i++) {
    if (tokens[i].type == END_OF_TOKENS) {
      break;
    }
    if (tokens[i].value != NULL &&
        (tokens[i].type == INT || tokens[i].type == IDENTIFIER ||
         tokens[i].type == KEYWORD || tokens[i].type == OPERATOR ||
         tokens[i].type == STRING || tokens[i].type == SEPARATOR ||
         tokens[i].type == UNKNOWN)) {
      free(tokens[i].value);
    }
  }
  free(tokens);
}
