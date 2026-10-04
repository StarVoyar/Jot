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
  INT,         /**< Integer literal */
  IDENTIFIER,  /**< Variable or function name */
  KEYWORD,     /**< Language keyword */
  OPERATOR,    /**< Arithmetic or comparison operator */
  STRING,      /**< String literal */
  SEPARATOR,   /**< Punctuation and delimiters */
  END_OF_TOKENS, /**< End of token stream */
  UNKNOWN,     /**< Unrecognized token */
} TokenType;

/**
 * @brief Token structure representing a lexical unit
 */
typedef struct {
  TokenType type; /**< Type of the token */
  char *value;    /**< String value of the token */
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

#endif
