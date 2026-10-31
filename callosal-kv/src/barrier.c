// barrier.c — UDP client side of the coordinator barrier.
//
// Written with retransmission rather than a single shot: UDP is lossy,
// and a worker racing past a dropped READY would hang on recvfrom until
// the driver kills it. Retransmit every ~500 ms keeps the protocol
// trivial on the coordinator side (idempotent, it just re-records the
// address) while being robust to single-packet loss.

#include "barrier.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BARRIER_RECV_BUF 256
#define BARRIER_RETRANSMIT_MS 500

int barrier_parse_spec(const char *spec, char *host_out, size_t host_cap,
                       uint16_t *port_out) {
    if (spec == NULL || host_out == NULL || port_out == NULL || host_cap == 0U) {
        return -1;
    }
    const char *colon = strrchr(spec, ':');
    if (colon == NULL || colon == spec || *(colon + 1) == '\0') {
        return -1;
    }
    size_t host_len = (size_t)(colon - spec);
    if (host_len + 1U > host_cap) {
        return -1;
    }
    memcpy(host_out, spec, host_len);
    host_out[host_len] = '\0';

    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(colon + 1, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0' || v == 0U || v > 65535U) {
        return -1;
    }
    *port_out = (uint16_t)v;
    return 0;
}

static int resolve_host(const char *host, uint16_t port,
                        struct sockaddr_in *out) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    char portstr[16];
    (void)snprintf(portstr, sizeof portstr, "%u", (unsigned)port);

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0 || res == NULL) {
        fprintf(stderr, "barrier: getaddrinfo(%s): %s\n",
                host, gai != 0 ? gai_strerror(gai) : "no result");
        if (res != NULL) {
            freeaddrinfo(res);
        }
        return -1;
    }
    memcpy(out, res->ai_addr, sizeof *out);
    freeaddrinfo(res);
    return 0;
}

static int parse_go(const char *buf, size_t len, uint64_t *start_ns_out) {
    // Expected: "GO <u64>\n". We tolerate a missing trailing newline.
    if (len < 4U || strncmp(buf, "GO ", 3U) != 0) {
        return -1;
    }
    const char *num = buf + 3;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(num, &end, 10);
    if (errno != 0 || end == num) {
        return -1;
    }
    *start_ns_out = (uint64_t)v;
    return 0;
}

int barrier_wait(const char *host, uint16_t port, uint32_t node_id,
                 int timeout_ms, uint64_t *start_ns_out) {
    if (host == NULL || start_ns_out == NULL || timeout_ms <= 0) {
        return -1;
    }

    struct sockaddr_in coord;
    if (resolve_host(host, port, &coord) != 0) {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "barrier: socket: %s\n", strerror(errno));
        return -1;
    }

    char ready[64];
    int ready_len = snprintf(ready, sizeof ready, "READY %u\n", node_id);
    if (ready_len <= 0 || (size_t)ready_len >= sizeof ready) {
        (void)close(fd);
        return -1;
    }

    int elapsed = 0;
    int rc = -1;
    while (elapsed < timeout_ms) {
        ssize_t sent = sendto(fd, ready, (size_t)ready_len, 0,
                              (struct sockaddr *)&coord, sizeof coord);
        if (sent < 0) {
            fprintf(stderr, "barrier: sendto: %s\n", strerror(errno));
            break;
        }

        int wait_ms = BARRIER_RETRANSMIT_MS;
        if (wait_ms > timeout_ms - elapsed) {
            wait_ms = timeout_ms - elapsed;
        }

        struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&p, 1, wait_ms);
        if (pr < 0) {
            if (errno == EINTR) {
                elapsed += wait_ms;
                continue;
            }
            fprintf(stderr, "barrier: poll: %s\n", strerror(errno));
            break;
        }
        if (pr == 0) {
            elapsed += wait_ms;
            continue;
        }

        char buf[BARRIER_RECV_BUF];
        ssize_t n = recv(fd, buf, sizeof buf - 1U, 0);
        if (n < 0) {
            fprintf(stderr, "barrier: recv: %s\n", strerror(errno));
            break;
        }
        buf[n] = '\0';
        if (parse_go(buf, (size_t)n, start_ns_out) == 0) {
            rc = 0;
            break;
        }
        // Unexpected packet: keep waiting (don't reset retransmit clock).
        elapsed += wait_ms;
    }

    if (rc != 0) {
        fprintf(stderr, "barrier: no GO from %s:%u within %d ms\n",
                host, port, timeout_ms);
    }
    (void)close(fd);
    return rc;
}
