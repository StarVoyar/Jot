#include "terminal.h"

/* Declared in lexer.h (not included: windows.h clashes with TokenType) */
const char *lexer_source_line(int line, int *out_len);
const char *lexer_source_line_in(const char *file, int line, int *out_len);

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

/**
 * @brief Enables ANSI colors if stdout is a terminal
 * @return Non-zero if color codes should be printed
 */
int terminal_setup_colors(void) {
#ifdef _WIN32
  HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (handle == NULL || handle == INVALID_HANDLE_VALUE) {
    return 0;
  }
  if (!_isatty(_fileno(stdout))) {
    return 0;
  }
  if (!GetConsoleMode(handle, &mode)) {
    return 0;
  }
  SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  return 1;
#else
  return isatty(fileno(stdout));
#endif
}

/**
 * @brief Prints a diagnostic with file:line:col, snippet, caret and squiggle
 * @param level Error or warning
 * @param filename Source file name, may be NULL
 * @param line 1-based line number
 * @param col 1-based column number
 * @param width Squiggle width in characters, at least 1
 * @param message Diagnostic message without the kind prefix
 */
/** Number of errors reported so far in this process */
static int term_error_count = 0;

/** Number of warnings reported so far in this process */
static int term_warning_count = 0;

void term_tally(TermLevel level) {
  if (level == TERM_ERROR) {
    term_error_count++;
  } else {
    term_warning_count++;
  }
}

int term_errors(void) { return term_error_count; }

int term_warnings(void) { return term_warning_count; }

void term_report(TermLevel level, const char *filename, int line, int col,
                 int width, const char *message) {
  term_tally(level);
  size_t len = strlen(message);
  if (len > 0 && message[len - 1] == '\n') {
    len--;
  }

  int color = terminal_setup_colors();
  const char *kind = level == TERM_ERROR ? "Error" : "Warning";
  const char *mark = "";
  if (color) {
    mark = level == TERM_ERROR ? "\x1b[1;31m" : "\x1b[1;33m";
  }
  const char *bold = color ? "\x1b[1m" : "";
  const char *reset = color ? "\x1b[0m" : "";

  if (line < 1) {
    line = 1;
  }
  if (col < 1) {
    col = 1;
  }
  if (width < 1) {
    width = 1;
  }
  if (filename == NULL) {
    filename = "<unknown>";
  }

  fprintf(stdout, "%s%s:%s %.*s\n", mark, kind, reset, (int)len, message);
  fprintf(stdout, "  --> %s:%d:%d\n", filename, line, col);

  int text_len = 0;
  const char *text = lexer_source_line_in(filename, line, &text_len);
  if (text == NULL) {
    text = lexer_source_line(line, &text_len);
  }
  if (text != NULL) {
    fprintf(stdout, "     |\n");
    fprintf(stdout, "%4d | ", line);
    int shown = 0;
    for (int i = 0; i < text_len; i++) {
      if (text[i] == '\t') {
        int next = ((shown / 4) + 1) * 4;
        while (shown < next) {
          fputc(' ', stdout);
          shown++;
        }
      } else {
        fputc(text[i], stdout);
        shown++;
      }
    }
    fputc('\n', stdout);

    int caret = 0;
    for (int i = 0; i < col - 1 && i < text_len; i++) {
      if (text[i] == '\t') {
        caret = ((caret / 4) + 1) * 4;
      } else {
        caret++;
      }
    }
    fprintf(stdout, "     | ");
    for (int i = 0; i < caret; i++) {
      fputc(' ', stdout);
    }
    if (width > 1) {
      fprintf(stdout, "%s", mark);
      for (int i = 0; i < width; i++) {
        fputc('~', stdout);
      }
      fprintf(stdout, "%s", reset);
    } else {
      fprintf(stdout, "%s^%s", bold, reset);
    }
    fputc('\n', stdout);
  }
}
