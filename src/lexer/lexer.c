#include "lexer.h"

/** Global buffer holding the source file contents (kept for error snippets) */
static char *global_buffer;

/** Index tracking the current token position in the token array */
size_t tokens_index = 0;

/** Current source line number (1-based) */
static int token_line = 1;

/** Buffer index where the current line starts */
static int line_start = 0;

/** Column of the token being lexed (1-based) */
static int token_col = 1;

/** Maximum loaded files kept around for error snippets */
#define MAX_LOADED_FILES 64

/** Buffers of every lexed file, keyed by name, for error snippets */
static char *loaded_names[MAX_LOADED_FILES];

/** Buffers of every lexed file, parallel to loaded_names */
static char *loaded_buffers[MAX_LOADED_FILES];

/** Number of registered loaded files */
static int loaded_count = 0;

/** Name to record for the next Lexer call */
static const char *pending_file_name;

void lexer_set_file_name(const char *file) { pending_file_name = file; }

/**
 * @brief Registers a lexed buffer under its file name for later snippets
 * @param name File name, may be NULL
 * @param buffer Source buffer to keep
 */
static void register_loaded_file(const char *name, char *buffer) {
  if (name == NULL || loaded_count >= MAX_LOADED_FILES) {
    return;
  }
  for (int i = 0; i < loaded_count; i++) {
    if (strcmp(loaded_names[i], name) == 0) {
      return;
    }
  }
  loaded_names[loaded_count] = (char *)name;
  loaded_buffers[loaded_count] = buffer;
  loaded_count++;
}

/**
 * @brief Lexes a source file into a stream of tokens
 * @param file Source file to lex
 * @return Array of tokens terminated by END_OF_TOKENS
 */
Token *Lexer(FILE *file) {
  fseek(file, 0, SEEK_END);
  long length = ftell(file);
  if (length < 0) {
    length = 0;
  }
  fseek(file, 0, SEEK_SET);

  size_t size = (size_t)length;
  unsigned char *buffer = malloc(size + 1);
  size_t bytes_read = fread(buffer, 1, size, file);
  buffer[bytes_read] = '\0';

  fclose(file);

  global_buffer = (char *)buffer;
  register_loaded_file(pending_file_name, (char *)buffer);

  int current_index = 0;
  tokens_index = 0;
  token_line = 1;
  line_start = 0;

  Token *tokens = malloc(65536 * sizeof(Token));

  while (buffer[current_index] != '\0') {

    char character = buffer[current_index];
    token_col = current_index - line_start + 1;

    if (isspace(character)) {
      if (character == '\n') {
        token_line++;
        line_start = current_index + 1;
      }
      current_index++;
    } else if (isdigit(character)) {
      Token *token = lex_int(character, &current_index);
      tokens[tokens_index] = *token;
      tokens_index++;
      free(token);
    } else if (character == '.' && isdigit(global_buffer[current_index + 1])) {
      Token *token = lex_float(character, &current_index);
      tokens[tokens_index] = *token;
      tokens_index++;
      free(token);
    } else if (isalpha(character) || character == '_') {
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
      case ',':
      case '.': {
        Token *token = lex_separator(character);
        tokens[tokens_index] = *token;
        tokens_index++;
        current_index++;
        free(token);
        break;
      }
      case '/': {
        if (global_buffer[current_index + 1] == '/') {
          while (buffer[current_index] != '\n' &&
                 buffer[current_index] != '\0') {
            current_index++;
          }
          break;
        }
        Token *token = lex_operator(character, &current_index);
        tokens[tokens_index] = *token;
        tokens_index++;
        free(token);
        break;
      }
      case '+':
      case '-':
      case '*':
      case '%':
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
      case '\'': {
        Token *token = lex_char(&current_index);
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

  tokens[tokens_index].value = NULL;
  tokens[tokens_index].type = END_OF_TOKENS;
  tokens[tokens_index].line = token_line;
  tokens[tokens_index].col = current_index - line_start + 1;

  return tokens;
}

/**
 * @brief Returns the text of a source line for error snippets
 * @param line 1-based line number
 * @param out_len Receives line length without newline, may be NULL
 * @return Pointer into the source buffer, NULL if out of range
 */
const char *lexer_source_line(int line, int *out_len) {
  return lexer_source_line_in(NULL, line, out_len);
}

const char *lexer_source_line_in(const char *file, int line, int *out_len) {
  if (line < 1) {
    return NULL;
  }
  const char *base = global_buffer;
  if (file != NULL) {
    base = NULL;
    for (int i = 0; i < loaded_count; i++) {
      if (strcmp(loaded_names[i], file) == 0) {
        base = loaded_buffers[i];
        break;
      }
    }
    if (base == NULL) {
      return NULL;
    }
  }
  if (base == NULL) {
    return NULL;
  }
  const char *start = base;
  for (int i = 1; i < line; i++) {
    start = strchr(start, '\n');
    if (start == NULL) {
      return NULL;
    }
    start++;
  }
  const char *end = strchr(start, '\n');
  size_t len;
  if (end == NULL) {
    len = strlen(start);
  } else {
    len = (size_t)(end - start);
  }
  if (len > 0 && start[len - 1] == '\r') {
    len--;
  }
  if (out_len != NULL) {
    *out_len = (int)len;
  }
  return start;
}

/**
 * @brief Saves the source buffer before parsing an imported file
 * @param snapshot Receives the current state
 */
void lexer_save(LexerSnapshot *snapshot) { snapshot->buffer = global_buffer; }

/**
 * @brief Restores the source buffer after parsing an imported file
 * @param snapshot State to restore
 */
void lexer_restore(const LexerSnapshot *snapshot) {
  global_buffer = snapshot->buffer;
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
  token->line = token_line;
  token->col = token_col;

  char value[32];
  int value_index = 0;

  while (isdigit(current_char) && current_char != '\0' && value_index < 31) {
    value[value_index++] = current_char;
    (*current_index)++;
    current_char = global_buffer[*current_index];
  }

  if (current_char == '.' && isdigit(global_buffer[*current_index + 1])) {
    /* Integer part already in value[0..value_index-1]; continue as float. */
    char fvalue[64];
    int f_index = 0;
    for (int k = 0; k < value_index && f_index < 63; k++) {
      fvalue[f_index++] = value[k];
    }
    /* Consume '.' */
    if (f_index < 63) {
      fvalue[f_index++] = '.';
    }
    (*current_index)++;
    char next_char = global_buffer[*current_index];
    while (isdigit(next_char) && next_char != '\0' && f_index < 63) {
      fvalue[f_index++] = next_char;
      (*current_index)++;
      next_char = global_buffer[*current_index];
    }
    fvalue[f_index] = '\0';
    free(token);
    Token *ftoken = malloc(sizeof(Token));
    ftoken->type = FLOAT;
    ftoken->line = token_line;
    ftoken->col = token_col;
    size_t len = strlen(fvalue);
    char *value_copy = malloc(len + 1);
    memcpy(value_copy, fvalue, len);
    value_copy[len] = '\0';
    ftoken->value = value_copy;
    return ftoken;
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
 * @brief Lexes a floating point literal
 * @param current_char Starting character (should be '.')
 * @param current_index Current position in buffer (updated)
 * @return Token for floating point literal
 */
Token *lex_float(char current_char, int *current_index) {
  Token *token = malloc(sizeof(Token));
  token->type = FLOAT;
  token->line = token_line;
  token->col = token_col;

  char value[64];
  int value_index = 0;

  if (current_char == '.') {
    value[value_index++] = '0';
    value[value_index++] = '.';
    (*current_index)++;
  }

  char next_char = global_buffer[*current_index];
  while (isdigit(next_char) && next_char != '\0' && value_index < 63) {
    value[value_index++] = next_char;
    (*current_index)++;
    next_char = global_buffer[*current_index];
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
 * @brief Lexes a keyword or identifier (digits allowed after first char)
 * @param current_char Starting character
 * @param current_index Current position in buffer (updated)
 * @return Token for keyword or identifier
 */
Token *lex_keyword(char current_char, int *current_index) {
  Token *token = malloc(sizeof(Token));
  token->line = token_line;
  token->col = token_col;
  char keyword[32];
  int keyword_index = 0;

  while ((isalnum(current_char) || current_char == '_') &&
         current_char != '\0' && keyword_index < 31) {
    keyword[keyword_index++] = current_char;
    (*current_index)++;
    current_char = global_buffer[*current_index];
  }

  keyword[keyword_index] = '\0';

  if (strcmp(keyword, "return") == 0 || strcmp(keyword, "if") == 0 ||
      strcmp(keyword, "else") == 0 || strcmp(keyword, "while") == 0 ||
      strcmp(keyword, "for") == 0 || strcmp(keyword, "fn") == 0 ||
      strcmp(keyword, "print") == 0 || strcmp(keyword, "num") == 0 ||
      strcmp(keyword, "bool") == 0 || strcmp(keyword, "str") == 0 ||
      strcmp(keyword, "arr") == 0 || strcmp(keyword, "struct") == 0 ||
      strcmp(keyword, "class") == 0 || strcmp(keyword, "new") == 0 ||
      strcmp(keyword, "inherit") == 0 || strcmp(keyword, "break") == 0 ||
      strcmp(keyword, "continue") == 0 || strcmp(keyword, "null") == 0 ||
      strcmp(keyword, "char") == 0 || strcmp(keyword, "global") == 0) {
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
  token->line = token_line;
  token->col = token_col;
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
  token->line = token_line;
  token->col = token_col;
  char op[3] = {character, '\0', '\0'};
  (*current_index)++;
  char next_char = global_buffer[*current_index];

  if ((character == '=' && next_char == '=') ||
      (character == '!' && next_char == '=') ||
      (character == '<' && next_char == '=') ||
      (character == '>' && next_char == '=') ||
      (character == '+' && next_char == '=') ||
      (character == '-' && next_char == '=') ||
      (character == '-' && next_char == '>')) {
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
 * @brief Lexes a string literal, decoding escape sequences
 * @param current_index Current position in buffer (updated)
 * @return Token for string literal
 * @details Supports \" \\ \n \t \r \0; any other backslash sequence keeps
 * the character as written so \x stays two characters
 */
Token *lex_string(int *current_index) {
  Token *token = malloc(sizeof(Token));
  token->line = token_line;
  token->col = token_col;
  (*current_index)++;

  char string[256];
  int string_index = 0;
  char current_char = global_buffer[*current_index];

  while (current_char != '"' && current_char != '\0' && string_index < 255) {
    if (current_char == '\\' && global_buffer[*current_index + 1] != '\0') {
      char escape = global_buffer[*current_index + 1];
      char decoded = escape;
      int is_escape = 1;
      switch (escape) {
      case 'n':
        decoded = '\n';
        break;
      case 't':
        decoded = '\t';
        break;
      case 'r':
        decoded = '\r';
        break;
      case '"':
        decoded = '"';
        break;
      case '\\':
        decoded = '\\';
        break;
      default:
        is_escape = 0;
        break;
      }
      if (is_escape) {
        (*current_index) += 2;
        string[string_index++] = decoded;
        current_char = global_buffer[*current_index];
        continue;
      }
      /* Unknown escape: keep both characters, but never overflow the
         buffer (the literal pass below copies them one at a time). */
    }
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
 * @brief Lexes a character literal, decoding escape sequences
 * @param current_index Current position in buffer (updated)
 * @return INT token with the character's numeric value, or UNKNOWN for
 * invalid literals so the parser reports them with position information
 * @details Supports \n \t \r \0 \\ \' \"; a literal must hold exactly one
 * character between single quotes
 */
Token *lex_char(int *current_index) {
  Token *token = malloc(sizeof(Token));
  token->line = token_line;
  token->col = token_col;
  (*current_index)++; /* skip opening ' */

  int code = -1;
  int invalid = 0;
  int raw_done = 0;
  char prefix[3];
  int prefix_len = 0;
  char current_char = global_buffer[*current_index];

  if (current_char == '\'') {
    /* Empty literal. */
    invalid = 1;
    raw_done = 1;
    (*current_index)++;
  } else if (current_char == '\0') {
    invalid = 1;
  } else if (current_char == '\\' &&
             global_buffer[*current_index + 1] != '\0') {
    char escape = global_buffer[*current_index + 1];
    int decoded = (unsigned char)escape;
    int is_escape = 1;
    switch (escape) {
    case 'n':
      decoded = '\n';
      break;
    case 't':
      decoded = '\t';
      break;
    case 'r':
      decoded = '\r';
      break;
    case '0':
      decoded = '\0';
      break;
    case '\'':
      decoded = '\'';
      break;
    case '"':
      decoded = '"';
      break;
    case '\\':
      decoded = '\\';
      break;
    default:
      is_escape = 0;
      break;
    }
    if (is_escape) {
      code = decoded;
      (*current_index) += 2;
      if (global_buffer[*current_index] == '\'') {
        (*current_index)++;
      } else {
        prefix[0] = '\\';
        prefix[1] = escape;
        prefix_len = 2;
        invalid = 1;
      }
    } else {
      /* Unknown escape: not a single character, report below. */
      invalid = 1;
    }
  } else {
    code = (unsigned char)current_char;
    (*current_index)++;
    if (global_buffer[*current_index] == '\'') {
      (*current_index)++;
    } else {
      prefix[0] = current_char;
      prefix_len = 1;
      invalid = 1;
    }
  }

  if (!invalid) {
    char digits[24];
    int n = snprintf(digits, sizeof(digits), "%d", code);
    token->value = malloc((size_t)n + 1);
    memcpy(token->value, digits, (size_t)n + 1);
    token->type = INT;
    return token;
  }

  /* Invalid literal: capture the raw text through the closing quote so the
     parser's "Unexpected '...' " message shows what was written. */
  char raw[64];
  int raw_index = 0;
  if (prefix_len > 0) {
    memcpy(raw, prefix, (size_t)prefix_len);
    raw_index = prefix_len;
  }
  while (!raw_done && raw_index < 63) {
    current_char = global_buffer[*current_index];
    if (current_char == '\0' || current_char == '\n') {
      break;
    }
    if (current_char == '\'') {
      (*current_index)++;
      break;
    }
    raw[raw_index++] = current_char;
    (*current_index)++;
  }
  raw[raw_index] = '\0';
  token->value = malloc((size_t)raw_index + 1);
  memcpy(token->value, raw, (size_t)raw_index + 1);
  token->type = UNKNOWN;
  return token;
}

/**
 * @brief Lexes an unknown character
 * @param character Unknown character
 * @return Token for unknown character
 */
Token *lex_unknown(char character) {
  Token *token = malloc(sizeof(Token));
  token->line = token_line;
  token->col = token_col;
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
  case FLOAT:
    printf(", Token Type: FLOAT \n");
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
        (tokens[i].type == INT || tokens[i].type == FLOAT ||
         tokens[i].type == IDENTIFIER || tokens[i].type == KEYWORD ||
         tokens[i].type == OPERATOR || tokens[i].type == STRING ||
         tokens[i].type == SEPARATOR || tokens[i].type == UNKNOWN)) {
      free(tokens[i].value);
    }
  }
  free(tokens);
}
