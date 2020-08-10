#ifndef CFLOGPRINTER_H
#define CFLOGPRINTER_H

#define RED         "\033[1;31m"
#define GREEN       "\033[0;32m"
#define YELLOW      "\033[0;33m"
#define BLUE        "\033[0;34m"
#define MAGENTA     "\033[0;35m"
#define CYAN        "\033[0;36m"
#define RESET       "\033[0m"

#include <stdio.h>
#include <stdarg.h>

void cflp_success(char *msg, ...);
void cflp_error(char *msg, ...);
void cflp_warning(char *msg, ...);
void cflp_info(char *msg, ...);
void cflp_custom(char *color, char *msg, ...);

#endif
