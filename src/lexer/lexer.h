#ifndef LEXER_H
#define LEXER_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Enumeration of token types
 */
typedef enum {
  INT,           /**< Integer literal */
  IDENTIFIER,    /**< Variable or function name */
  KEYWORD,       /**< Language keyword */
  OPERATOR,      /**< Arithmetic or comparison operator */
  STRING,        /**< String literal */
  SEPARATOR,     /**< Punctuation and delimiters */
  END_OF_TOKENS, /**< End of token stream */
  UNKNOWN,       /**< Unrecognized token */
} TokenType;

/**
 * @brief Token structure representing a lexical unit
 */
typedef struct {
  TokenType type; /**< Type of the token */
  char *value;    /**< String value of the token */
  int line;       /**< 1-based line number */
  int col;        /**< 1-based column number */
} Token;

/**
 * @brief Lexes a source file into a stream of tokens
 * @param file Source file to lex
 * @return Array of tokens terminated by END_OF_TOKENS
 */
Token *Lexer(FILE *file);

/**
 * @brief Lexes an integer literal
 * @param current_char Starting character
 * @param current_index Current position in buffer (updated)
 * @return Token for integer literal
 */
Token *lex_int(char current_char, int *current_index);

/**
 * @brief Lexes a keyword or identifier
 * @param current_char Starting character
 * @param current_index Current position in buffer (updated)
 * @return Token for keyword or identifier
 */
Token *lex_keyword(char current_char, int *current_index);

/**
 * @brief Lexes a separator character
 * @param character Separator character
 * @return Token for separator
 */
Token *lex_separator(char character);

/**
 * @brief Lexes an operator (including multi-character operators)
 * @param character Starting character
 * @param current_index Current position in buffer (updated)
 * @return Token for operator
 */
Token *lex_operator(char character, int *current_index);

/**
 * @brief Lexes a string literal
 * @param current_index Current position in buffer (updated)
 * @return Token for string literal
 */
Token *lex_string(int *current_index);

/**
 * @brief Lexes an unknown character
 * @param character Unknown character
 * @return Token for unknown character
 */
Token *lex_unknown(char character);

/**
 * @brief Prints a token's type and value
 * @param token Token to print
 */
void print_token(Token token);

/**
 * @brief Frees memory allocated for token array
 * @param tokens Token array to free
 */
void free_tokens(Token *tokens);

/**
 * @brief Returns the text of a source line for error snippets
 * @param line 1-based line number
 * @param out_len Receives line length without newline, may be NULL
 * @return Pointer into the source buffer, NULL if out of range
 */
const char *lexer_source_line(int line, int *out_len);

/**
 * @brief Snapshot of lexer state for nested file parsing
 */
typedef struct {
  char *buffer; /**< Source buffer (each Lexer call owns its token array) */
} LexerSnapshot;

/**
 * @brief Saves the source buffer before parsing an imported file
 * @param snapshot Receives the current state
 */
void lexer_save(LexerSnapshot *snapshot);

/**
 * @brief Restores the source buffer after parsing an imported file
 * @param snapshot State to restore
 */
void lexer_restore(const LexerSnapshot *snapshot);

#endif
