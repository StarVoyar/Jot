#include "terminal.h"

#include <stdio.h>

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
