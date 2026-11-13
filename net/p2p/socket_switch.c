// SPDX-License-Identifier: GPL-3.0-or-later
/* Non-blocking sockets for RMG-K's n02 client on the Switch.
 *
 * n02's k_socket.cpp makes its UDP socket non-blocking with ioctl(sock, FIONBIO, &one). libnx's ioctl()
 * passes FIONBIO straight to the bsd service's Ioctl (it only rewrites a few interface requests), and
 * whether Horizon's bsd honours FIONBIO is not documented; libnx's fcntl(F_SETFL, O_NONBLOCK) maps to
 * the service's Fcntl, which is what other homebrew relies on (RetroArch's socket_nonblock for the
 * Switch). k_socket.cpp is compiled with -Dioctl=znx_ioctl, so FIONBIO lands here and is done with
 * fcntl; any other request still goes to ioctl. The first call reports in the log whether the socket
 * really is non-blocking afterwards. */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

void znx_log_line(const char* line);

int znx_ioctl(int fd, int request, ...)
{
    va_list args;
    void* argument;
    va_start(args, request);
    argument = va_arg(args, void*);
    va_end(args);
    if ((unsigned)request == (unsigned)FIONBIO) {
        static int reported;
        const int on = argument != NULL && *(const int*)argument != 0;
        int flags = fcntl(fd, F_GETFL, 0);
        int result;
        if (flags < 0)
            flags = 0;
        result = fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
        if (!reported) {
            char line[160];
            const int now = fcntl(fd, F_GETFL, 0);
            snprintf(line, sizeof(line), "net: n02 socket %d non-blocking by fcntl: %s (flags 0x%x%s%s)", fd,
                     result == 0 && now >= 0 && (now & O_NONBLOCK) ? "yes" : "NO", (unsigned)now,
                     result != 0 ? ", error " : "", result != 0 ? strerror(errno) : "");
            znx_log_line(line);
            reported = 1;
        }
        return result;
    }
    return ioctl(fd, request, argument);
}
