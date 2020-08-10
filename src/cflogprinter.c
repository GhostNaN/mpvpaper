#include <cflogprinter.h>

void cflp_good(char *msg, ...) {
    printf("%s[+] ", GREEN);

    va_list arg;

    va_start (arg, msg);
    vfprintf (stdout, msg, arg);
    va_end (arg);

    printf("%s\n", RESET);

}

void cflp_bad(char *msg, ...) {
    printf("%s[-] ", RED);

    va_list arg;

    va_start (arg, msg);
    vfprintf (stdout, msg, arg);
    va_end (arg);

    printf("%s\n", RESET);

}

void cflp_error(char *msg, ...) {
    printf("%s[-] ", RED);

    va_list arg;

    va_start (arg, msg);
    vfprintf (stderr, msg, arg); //print to stderr instead of stdout!
    va_end (arg);

    printf("%s\n", RESET);

}

void cflp_warning(char *msg, ...) {
    printf("%s[!] ", RED);

    va_list arg;

    va_start (arg, msg);
    vfprintf (stdout, msg, arg);
    va_end (arg);

    printf("%s\n", RESET);

}

void cflp_info(char *msg, ...) {
    printf("[*] ");

    va_list arg;

    va_start (arg, msg);
    vfprintf (stdout, msg, arg);
    va_end (arg);

    printf("%s\n", RESET);

}

void cflp_custom(char *color, char *msg, ...) {
    printf("%s", color);

    va_list arg;

    va_start (arg, msg);
    vfprintf (stdout, msg, arg);
    va_end (arg);

    printf("%s\n", RESET);

}
