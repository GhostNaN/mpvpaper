#ifndef MPVPAPER_PROCESS_CHECK_H
#define MPVPAPER_PROCESS_CHECK_H

#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static inline int process_is_running(const char *name) {
    if (!name || !name[0] || name[0] == '-') {
        errno = EINVAL;
        return -1;
    }

    pid_t child;
    char *const arguments[] = {"pidof", "-q", (char *)name, NULL};
    int error = posix_spawnp(&child, "pidof", NULL, NULL, arguments, environ);
    if (error) {
        errno = error;
        return -1;
    }

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 1)
        return 0;

    errno = EIO;
    return -1;
}

#endif
