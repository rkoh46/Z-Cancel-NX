// SPDX-License-Identifier: GPL-3.0-or-later
/* libjuice on libnx: its poll thread (conn_poll.c, JUICE_CONCURRENCY_MODE_POLL) sleeps in poll() on every
 * agent's socket plus the read end of a pipe, and an agent wakes it by writing one byte to the pipe. libnx
 * has no pipe(), and its poll() only knows sockets anyway, so conn_poll.c is compiled with
 * -Dpipe=znx_juice_pipe (builder/build-switch.sh) and gets two UDP sockets on the loopback address instead,
 * the second connected to the first: write() on fds[1] makes fds[0] readable, read() drains it, close()
 * closes them. libjuice's own Windows branch does the same with one socket that sends to itself. If the
 * loopback address cannot be bound, the console's own address is tried. */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "znx_switch.h"

static int bound_pair(uint32_t address, int fds[2])
{
    struct sockaddr_in in;
    socklen_t length = sizeof(in);
    int reader = socket(AF_INET, SOCK_DGRAM, 0), writer = socket(AF_INET, SOCK_DGRAM, 0);
    if (reader < 0 || writer < 0)
        goto fail;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(address);
    in.sin_port = 0;
    if (bind(reader, (struct sockaddr*)&in, sizeof(in)) != 0 || getsockname(reader, (struct sockaddr*)&in, &length) != 0)
        goto fail;
    if (connect(writer, (struct sockaddr*)&in, sizeof(in)) != 0)
        goto fail;
    fds[0] = reader;
    fds[1] = writer;
    return 0;
fail:
    if (reader >= 0) close(reader);
    if (writer >= 0) close(writer);
    return -1;
}

int znx_juice_pipe(int fds[2])
{
    if (bound_pair(INADDR_LOOPBACK, fds) == 0) {
        znx_log("lobby: libjuice's wake-up pipe is a loopback UDP socket pair (%d, %d)", fds[0], fds[1]);
        return 0;
    }
    {
        /* the console's own address, as the lobby binds its agents to it */
        const long host = gethostid();
        if (host != 0 && bound_pair(ntohl((uint32_t)host), fds) == 0) {
            znx_log("lobby: libjuice's wake-up pipe is a UDP socket pair on the console's address (%d, %d)", fds[0], fds[1]);
            return 0;
        }
    }
    znx_log("lobby: no socket pair for libjuice's wake-up pipe (errno %d)", errno);
    errno = EMFILE;
    return -1;
}
