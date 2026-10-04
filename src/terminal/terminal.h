#ifndef TERMINAL_H
#define TERMINAL_H

/**
 * @brief Enables ANSI colors if stdout is a terminal
 * @return Non-zero if color codes should be printed
 */
int terminal_setup_colors(void);

#endif
