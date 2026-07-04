/* SPDX-License-Identifier: MIT */
#include "embervisor.h"
#include <stdarg.h>
#include <errno.h>

bool ember_verbose;

void die(const char *fmt, ...)
{
    va_list ap;
    int saved = errno;

    fflush(stdout);
    fprintf(stderr, "embervisor: fatal: ");
    va_start(ap, fmt);
    errno = saved;              /* let callers use %m */
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void info(const char *fmt, ...)
{
    va_list ap;

    if (!ember_verbose)
        return;
    fprintf(stderr, "embervisor: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
