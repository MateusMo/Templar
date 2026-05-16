/*
 * http.c — Minimal HTTP/1.1 server.
 *
 * Design:
 */
#define _GNU_SOURCE
/*
 * (rest of design comment)
 *   - SO_REUSEPORT: multiple worker threads each own a listening socket on the
 *     same port. The kernel distributes incoming connections across them.
 *     No explicit load balancer needed per worker; the LB container uses nginx.
 *   - Each worker: epoll + non-blocking accept + read-in-one-shot for small
 *     payloads (< 64 KB, which all fraud-score payloads satisfy).
 *   - Zero-copy response: build response directly on the stack and send().
 *   - Keep-alive supported (Connection: keep-alive) for batched test runners.
 */
#include "http.h"
#include "fraud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>

#define MAX_EVENTS   256
#define RECV_BUF     65536  /* 64 KB per connection buffer */
#define SEND_BUF     512

/* ── Connection state (one per fd) ─────────────────────────────────── */
typedef struct Conn {
    int     fd;
    char    buf[RECV_BUF];
    int     buf_used;
    bool    keep_alive;
} Conn;

/* ── Global config ──────────────────────────────────────────────────── */
static int   g_port;
static int   g_n_threads;
static bool  g_ready = false;

/* ── Helpers ────────────────────────────────────────────────────────── */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int make_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    /* disable Nagle for lower latency */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 4096) < 0) { perror("listen"); close(fd); return -1; }
    set_nonblocking(fd);
    return fd;
}

/* ── Request parsing ────────────────────────────────────────────────── */
/* Returns pointer to body start and body_len, or NULL if incomplete. */
static const char *find_body(const char *buf, int used, int *body_len,
                              bool *keep_alive, bool *is_fraud_score,
                              bool *is_get_ready) {
    /* Find \r\n\r\n */
    const char *hdr_end = NULL;
    for (int i = 0; i <= used - 4; i++) {
        if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n') {
            hdr_end = buf + i + 4;
            break;
        }
    }
    if (!hdr_end) return NULL; /* headers incomplete */

    /* Method + path */
    *is_fraud_score = (strncmp(buf, "POST /fraud-score", 17) == 0);
    *is_get_ready   = (strncmp(buf, "GET /ready", 10) == 0);

    /* Content-Length */
    const char *cl = strcasestr(buf, "Content-Length:");
    int content_len = 0;
    if (cl) content_len = atoi(cl + 15);

    /* Keep-Alive */
    const char *conn = strcasestr(buf, "Connection:");
    *keep_alive = true; /* default HTTP/1.1 */
    if (conn && strncasecmp(conn + 12, "close", 5) == 0)
        *keep_alive = false;

    int body_start_off = (int)(hdr_end - buf);
    int body_received  = used - body_start_off;
    if (body_received < content_len) return NULL; /* body incomplete */

    *body_len = content_len;
    return hdr_end;
}

/* ── Response builder ───────────────────────────────────────────────── */
static int build_fraud_response(FraudResult r, char *out, int out_cap) {
    char body[64];
    int body_len = snprintf(body, sizeof(body),
        "{\"approved\":%s,\"fraud_score\":%.4f}",
        r.approved ? "true" : "false",
        (double)r.fraud_score);

    return snprintf(out, out_cap,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "%s",
        body_len, body);
}

static const char READY_RESP[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "Connection: keep-alive\r\n"
    "\r\nOK";

static const char NOT_FOUND_RESP[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

static const char BAD_REQUEST_RESP[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

/* ── Connection handler ─────────────────────────────────────────────── */
static void handle_conn(Conn *c) {
    /* Try to read more data */
    while (c->buf_used < RECV_BUF) {
        int n = (int)recv(c->fd, c->buf + c->buf_used,
                          RECV_BUF - c->buf_used, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            goto close_conn;
        }
        if (n == 0) goto close_conn;
        c->buf_used += n;
        break; /* single recv per epoll event is enough for small payloads */
    }

    /* Try to parse a complete request (loop for keep-alive pipelining) */
    while (c->buf_used > 0) {
        int body_len;
        bool keep_alive, is_fraud, is_ready;
        const char *body = find_body(c->buf, c->buf_used, &body_len,
                                     &keep_alive, &is_fraud, &is_ready);
        if (!body) break; /* incomplete request */

        char resp[RECV_BUF];
        int  resp_len;

        if (is_ready && g_ready) {
            send(c->fd, READY_RESP, sizeof(READY_RESP) - 1, MSG_NOSIGNAL);
        } else if (is_fraud) {
            FraudResult r = fraud_score(body, body_len);
            resp_len = build_fraud_response(r, resp, sizeof(resp));
            send(c->fd, resp, resp_len, MSG_NOSIGNAL);
        } else {
            send(c->fd, NOT_FOUND_RESP, sizeof(NOT_FOUND_RESP) - 1, MSG_NOSIGNAL);
        }

        /* Consume this request from the buffer */
        int consumed = (int)(body - c->buf) + body_len;
        if (consumed < c->buf_used)
            memmove(c->buf, c->buf + consumed, c->buf_used - consumed);
        c->buf_used -= consumed;
        c->keep_alive = keep_alive;

        if (!keep_alive) goto close_conn;
    }
    return;

close_conn:
    close(c->fd);
    free(c);
}

/* ── Worker thread ──────────────────────────────────────────────────── */
typedef struct { int port; } WorkerArg;

static void *worker_thread(void *arg) {
    (void)arg;
    int port = g_port;

    int listen_fd = make_listen_socket(port);
    if (listen_fd < 0) pthread_exit(NULL);

    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); pthread_exit(NULL); }

    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.fd = listen_fd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == listen_fd) {
                /* Accept new connections */
                for (;;) {
                    int cfd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        break;
                    }
                    int opt = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

                    Conn *c = calloc(1, sizeof(Conn));
                    if (!c) { close(cfd); continue; }
                    c->fd = cfd;

                    struct epoll_event cev = {
                        .events   = EPOLLIN | EPOLLET | EPOLLRDHUP,
                        .data.ptr = c
                    };
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                }
            } else {
                Conn *c = (Conn *)events[i].data.ptr;
                if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                    close(c->fd); free(c);
                } else {
                    handle_conn(c);
                    if (!c->keep_alive) {
                        epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                    }
                }
            }
        }
    }
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────── */
void http_set_ready(bool ready) {
    g_ready = ready;
}

int http_serve(int port, int n_threads) {
    signal(SIGPIPE, SIG_IGN);
    g_port      = port;
    g_n_threads = n_threads;

    pthread_t tids[64];
    if (n_threads > 64) n_threads = 64;

    for (int i = 0; i < n_threads; i++) {
        if (pthread_create(&tids[i], NULL, worker_thread, NULL) != 0) {
            perror("pthread_create"); return -1;
        }
    }
    /* Block main thread forever */
    for (int i = 0; i < n_threads; i++)
        pthread_join(tids[i], NULL);
    return 0;
}