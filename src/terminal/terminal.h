#ifndef TERMINAL_H
#define TERMINAL_H

#if defined(_MSC_VER)
#define NORETURN __declspec(noreturn)
#else
#define NORETURN __attribute__((noreturn))
#endif

#include <stdio.h>
#include <string.h>

/**
 * @brief Diagnostic severity selecting label and color
 */
typedef enum {
  TERM_ERROR,  /**< Red Error: */
  TERM_WARNING /**< Yellow Warning: */
} TermLevel;

/**
 * @brief Enables ANSI colors if stdout is a terminal
 * @return Non-zero if color codes should be printed
 */
int terminal_setup_colors(void);

/**
 * @brief Prints a diagnostic with file:line:col, snippet, caret and squiggle
 * @param level Error or warning
 * @param filename Source file name, may be NULL
 * @param line 1-based line number
 * @param col 1-based column number
 * @param width Squiggle width in characters, at least 1
 * @param message Diagnostic message without the kind prefix
 */
void term_report(TermLevel level, const char *filename, int line, int col,
                 int width, const char *message);

#endif
