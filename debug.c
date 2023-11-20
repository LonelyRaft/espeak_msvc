#include <stdio.h>
#include <stdarg.h>
#include "speech.h"
#include "debug.h"

#ifdef DEBUG_ENABLED

#ifdef _MSC_VER
#include <winsock.h>

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    static const uint64_t EPOCH_DIFFERENCE =
            116444736000000000ULL;
    FILETIME ft;
    uint64_t ullTime = 0;
    ULARGE_INTEGER uliTime;

    GetSystemTimeAsFileTime(&ft);

    uliTime.LowPart = ft.dwLowDateTime;
    uliTime.HighPart = ft.dwHighDateTime;
    ullTime = uliTime.QuadPart;

    ullTime -= EPOCH_DIFFERENCE;
    ullTime /= 10;

    tv->tv_sec = (long)(ullTime / 1000000);
    tv->tv_usec = (long)(ullTime % 1000000);
    return 0;
}

#else
#include <sys/time.h>
#include <unistd.h>
#endif

static FILE *fd_log = NULL;
static const char *FILENAME = "/tmp/espeak.log";

void debug_init() {
    if ((fd_log = fopen(FILENAME, "a")) != NULL)
        setvbuf(fd_log, NULL, _IONBF, 0);
}

void debug_enter(const char *text) {
    struct timeval tv;

    gettimeofday(&tv, NULL);

    if (!fd_log) {
        debug_init();
    }

    if (fd_log) {
        fprintf(fd_log, "%03d.%03dms > ENTER %s\n",
                (int) (tv.tv_sec % 1000),
                (int) (tv.tv_usec / 1000), text);
    }
}


void debug_show(const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (!fd_log) {
        debug_init();
    }
    if (fd_log) {
        vfprintf(fd_log, format, args);
    }
    va_end(args);
}

void debug_time(const char *text) {
    struct timeval tv;

    gettimeofday(&tv, NULL);

    if (!fd_log) {
        debug_init();
    }
    if (fd_log) {
        fprintf(fd_log, "%03d.%03dms > %s\n",
                (int) (tv.tv_sec % 1000),
                (int) (tv.tv_usec / 1000), text);
    }
}

#endif
