// SPDX-License-Identifier: GPL-3.0-or-later
/* libjuice on libnx: its poll thread (conn_poll.c, JUICE_CONCURRENCY_MODE_POLL) sleeps in poll() on every
 * agent's socket plus the read end of a pipe, and an agent wakes it by writing one byte to the pipe. libnx
 * has no pipe(), and its poll() only knows sockets anyway, so conn_poll.c is compiled with
 * -Dpipe=znx_juice_pipe (builder/build-switch.sh) and gets two UDP sockets on the loopback address instead,
 * the second connected to the first: write() on fds[1] makes fds[0] readable, read() drains it, close()
 * closes them. libjuice's own Windows branch does the same with one socket that sends to itself. If the
 * loopback address cannot be bound, the console's own address is tried. */

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
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

/* libnx's getnameinfo() looks the host up in DNS even when NI_NUMERICHOST asks for the number. libjuice
 * writes its candidates with it (ice.c ice_create_local_candidate, addr.c addr_record_to_string), so on a
 * connection whose public address has a reverse DNS name the console offered its server-reflexive
 * candidate as that name ("a=candidate:2 1 UDP ... customer.example-isp.com 6857 typ srflx"). RMG-K's
 * libjuice does not resolve hostnames in candidates and dropped it, so no player outside the house could
 * reach the console. libjuice's sources are compiled with -Dgetnameinfo=znx_juice_getnameinfo
 * (builder/build-switch.sh): the numeric flags are honoured here with inet_ntop, anything else goes to libnx. */
int znx_juice_getnameinfo(const struct sockaddr* sa, socklen_t salen, char* host, socklen_t hostlen, char* serv,
                          socklen_t servlen, int flags)
{
    const void* address = NULL;
    unsigned port = 0;
    if (sa == NULL)
        return EAI_FAIL;
    if (sa->sa_family == AF_INET && salen >= (socklen_t)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in* in = (const struct sockaddr_in*)sa;
        address = &in->sin_addr;
        port = ntohs(in->sin_port);
    } else if (sa->sa_family == AF_INET6 && salen >= (socklen_t)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6* in6 = (const struct sockaddr_in6*)sa;
        address = &in6->sin6_addr;
        port = ntohs(in6->sin6_port);
    }
    if (address == NULL || ((host == NULL || hostlen == 0 || (flags & NI_NUMERICHOST)) &&
                            (serv == NULL || servlen == 0 || (flags & NI_NUMERICSERV))) == 0)
        return getnameinfo(sa, salen, host, hostlen, serv, servlen, flags);
    if (host != NULL && hostlen > 0 && inet_ntop(sa->sa_family, address, host, hostlen) == NULL)
        return EAI_OVERFLOW;
    if (serv != NULL && servlen > 0) {
        const int n = snprintf(serv, servlen, "%u", port);
        if (n < 0 || n >= (int)servlen)
            return EAI_OVERFLOW;
    }
    return 0;
}
