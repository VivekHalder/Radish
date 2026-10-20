/*
 * server.c - worker threads, epoll networking and command execution.
 *
 * Threading model
 *   One worker thread per CPU core. Each worker has its own epoll instance and
 *   its own listening socket on the same port (SO_REUSEPORT), so the kernel
 *   spreads new connections across workers. A connection lives on one worker
 *   for its whole life, which keeps its commands in order. Different
 *   connections execute in parallel and only meet inside the sharded store.
 *
 * Usage: redis-mc [--port N] [--threads N] [--bind ADDR]
 */
#include "resp.h"
#include "store.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT      6380
#define DEFAULT_BIND      "127.0.0.1"     /* no auth yet: local only by default */
#define MAX_EVENTS        128
#define READ_CHUNK        (16 * 1024)
#define OUTPUT_HIGH_WATER (64UL * 1024 * 1024)
#define SHRINK_ABOVE      (1024 * 1024)   /* free idle buffers bigger than this */
#define ERR_SHOW_MAX      128             /* docs/spec/commands.md section 7 */

struct conn {
    int             fd;
    struct buf      in;          /* bytes received, not yet parsed */
    struct buf      out;         /* replies not yet sent */
    size_t          out_sent;    /* bytes of `out` already sent */
    struct resp_cmd cmd;         /* current request (argv points into `in`) */
    struct buf      tmp;         /* scratch: GET values, error messages */
    uint32_t        events;      /* epoll interest currently registered */
    bool            closing;     /* close as soon as `out` is flushed */
};

struct worker {
    pthread_t     thread;
    int           listen_fd;
    int           epfd;
    struct store *store;
};

struct config {
    const char *bind;
    int         port;
    int         threads;
};

static void die(const char *what)
{
    perror(what);
    exit(1);
}

/* ===========================================================================
 * Commands
 * ======================================================================== */

#define ARG(i) (c->cmd.argv[i])

typedef void (*cmd_fn)(struct worker *w, struct conn *c);

struct command {
    const char *name;       /* lower case: used in arity errors */
    size_t      min_argc;   /* counting the command name itself */
    size_t      max_argc;   /* 0 = no upper limit */
    cmd_fn      fn;
};

static void cmd_ping(struct worker *w, struct conn *c)
{
    (void)w;
    if (c->cmd.argc == 1)
        resp_simple(&c->out, "PONG");
    else
        resp_bulk(&c->out, ARG(1).ptr, ARG(1).len);
}

static void cmd_get(struct worker *w, struct conn *c)
{
    c->tmp.len = 0;
    if (store_get(w->store, ARG(1).ptr, ARG(1).len, &c->tmp))
        resp_bulk(&c->out, c->tmp.data, c->tmp.len);
    else
        resp_null(&c->out);
}

static void cmd_set(struct worker *w, struct conn *c)
{
    if (c->cmd.argc > 3) {   /* options (NX, EX, ...) not in milestone 1 */
        resp_error(&c->out, "ERR syntax error");
        return;
    }
    store_set(w->store, ARG(1).ptr, ARG(1).len, ARG(2).ptr, ARG(2).len);
    resp_simple(&c->out, "OK");
}

static void cmd_del(struct worker *w, struct conn *c)
{
    long long n = 0;
    for (size_t i = 1; i < c->cmd.argc; i++)
        n += store_del(w->store, ARG(i).ptr, ARG(i).len);
    resp_int(&c->out, n);
}

static void cmd_exists(struct worker *w, struct conn *c)
{
    long long n = 0;
    for (size_t i = 1; i < c->cmd.argc; i++)
        n += store_exists(w->store, ARG(i).ptr, ARG(i).len);
    resp_int(&c->out, n);
}

static void cmd_incr(struct worker *w, struct conn *c)
{
    long long v;
    switch (store_incr(w->store, ARG(1).ptr, ARG(1).len, &v)) {
    case STORE_INCR_OK:
        resp_int(&c->out, v);
        break;
    case STORE_INCR_NOT_INT:
        resp_error(&c->out, "ERR value is not an integer or out of range");
        break;
    case STORE_INCR_OVERFLOW:
        resp_error(&c->out, "ERR increment or decrement would overflow");
        break;
    }
}

static const struct command commands[] = {
    { "ping",   1, 2, cmd_ping   },
    { "get",    2, 2, cmd_get    },
    { "set",    3, 0, cmd_set    },
    { "del",    2, 0, cmd_del    },
    { "exists", 2, 0, cmd_exists },
    { "incr",   2, 2, cmd_incr   },
};

static const struct command *lookup(const struct resp_arg *name)
{
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        const struct command *cmd = &commands[i];
        if (strlen(cmd->name) == name->len &&
            strncasecmp(cmd->name, name->ptr, name->len) == 0)
            return cmd;
    }
    return NULL;
}

/* Append up to `max` bytes of p[0..len) the way error messages show them:
 * stop at a NUL byte, turn CR and LF into spaces. Returns bytes written. */
static size_t append_shown(struct buf *b, const char *p, size_t len, size_t max)
{
    size_t n = 0;
    while (n < len && n < max && p[n] != '\0') {
        char ch = (p[n] == '\r' || p[n] == '\n') ? ' ' : p[n];
        buf_append(b, &ch, 1);
        n++;
    }
    return n;
}

/* docs/spec/commands.md section 7 */
static void reply_unknown(struct conn *c)
{
    static const char head[] = "ERR unknown command '";
    static const char mid[]  = "', with args beginning with: ";
    struct buf *m = &c->tmp;

    m->len = 0;
    buf_append(m, head, sizeof head - 1);
    append_shown(m, ARG(0).ptr, ARG(0).len, ERR_SHOW_MAX);
    buf_append(m, mid, sizeof mid - 1);

    size_t cur = 0;
    for (size_t i = 1; i < c->cmd.argc && cur < ERR_SHOW_MAX; i++) {
        buf_append(m, "'", 1);
        cur += append_shown(m, ARG(i).ptr, ARG(i).len, ERR_SHOW_MAX - cur) + 3;
        buf_append(m, "' ", 2);
    }
    buf_append(m, "", 1);      /* NUL-terminate: resp_error takes a C string */
    resp_error(&c->out, m->data);
}

static void execute(struct worker *w, struct conn *c)
{
    const struct command *cmd = lookup(&ARG(0));
    if (cmd == NULL) {
        reply_unknown(c);
        return;
    }
    size_t argc = c->cmd.argc;
    if (argc < cmd->min_argc || (cmd->max_argc != 0 && argc > cmd->max_argc)) {
        char msg[96];
        snprintf(msg, sizeof msg,
                 "ERR wrong number of arguments for '%s' command", cmd->name);
        resp_error(&c->out, msg);
        return;
    }
    cmd->fn(w, c);
}

/* ===========================================================================
 * Connections
 * ======================================================================== */

static size_t pending_output(const struct conn *c)
{
    return c->out.len - c->out_sent;
}

static void conn_close(struct worker *w, struct conn *c)
{
    epoll_ctl(w->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    buf_free(&c->in);
    buf_free(&c->out);
    buf_free(&c->tmp);
    resp_cmd_free(&c->cmd);
    free(c);
}

static void set_events(struct worker *w, struct conn *c, uint32_t ev)
{
    if (ev == c->events)
        return;
    struct epoll_event e = { .events = ev, .data.ptr = c };
    if (epoll_ctl(w->epfd, EPOLL_CTL_MOD, c->fd, &e) < 0)
        die("epoll_ctl(MOD)");
    c->events = ev;
}

/* One recv per wake-up keeps busy clients from starving the others; epoll is
 * level-triggered, so leftover bytes wake us again. Returns false if closed. */
static bool read_input(struct worker *w, struct conn *c)
{
    buf_reserve(&c->in, READ_CHUNK);
    ssize_t n = recv(c->fd, c->in.data + c->in.len, c->in.cap - c->in.len, 0);
    if (n > 0) {
        c->in.len += (size_t)n;
        return true;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return true;
    conn_close(w, c);          /* 0 = client closed; anything else = error */
    return false;
}

/* Execute every complete request in c->in. Returns true if it paused because
 * too much output is queued (complete requests may remain). */
static bool process_input(struct worker *w, struct conn *c)
{
    size_t off = 0;
    bool paused = false;

    while (!c->closing && off < c->in.len) {
        if (pending_output(c) >= OUTPUT_HIGH_WATER) {
            paused = true;
            break;
        }
        size_t used = 0;
        const char *err = NULL;
        enum resp_status st = resp_parse(&c->cmd, c->in.data + off,
                                         c->in.len - off, &used, &err);
        if (st == RESP_INCOMPLETE)
            break;
        if (st == RESP_ERROR) {           /* reply, then close (resp.md section 7) */
            resp_error(&c->out, err);
            c->closing = true;
            break;
        }
        off += used;
        if (c->cmd.argc > 0)
            execute(w, c);                /* argv points into c->in: run before consuming */
    }

    if (c->closing)
        c->in.len = 0;                    /* drop anything after a protocol error */
    else
        buf_consume(&c->in, off);         /* one memmove per batch, not per command */

    if (c->in.len == 0 && c->in.cap > SHRINK_ABOVE)
        buf_free(&c->in);
    return paused;
}

/* Send queued output. Returns false if the connection was closed. */
static bool flush(struct worker *w, struct conn *c)
{
    while (c->out_sent < c->out.len) {
        /* MSG_NOSIGNAL: a closed peer gives EPIPE instead of killing us with SIGPIPE */
        ssize_t n = send(c->fd, c->out.data + c->out_sent, pending_output(c),
                         MSG_NOSIGNAL);
        if (n > 0) {
            c->out_sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true;                  /* socket full: wait for EPOLLOUT */
        conn_close(w, c);
        return false;
    }

    c->out.len = 0;
    c->out_sent = 0;
    if (c->out.cap > SHRINK_ABOVE)
        buf_free(&c->out);
    if (c->closing) {
        conn_close(w, c);
        return false;
    }
    return true;
}

static void on_conn_event(struct worker *w, struct conn *c, uint32_t ev)
{
    if (ev & EPOLLERR) {
        conn_close(w, c);
        return;
    }
    if ((ev & (EPOLLIN | EPOLLHUP)) && !read_input(w, c))
        return;

    bool paused;
    do {
        paused = process_input(w, c);
        if (!flush(w, c))
            return;
    } while (paused && pending_output(c) == 0);   /* drained: run the rest */

    uint32_t want = 0;
    if (!c->closing && pending_output(c) < OUTPUT_HIGH_WATER)
        want |= EPOLLIN;
    if (pending_output(c) > 0)
        want |= EPOLLOUT;
    set_events(w, c, want);
}

static void accept_all(struct worker *w)
{
    for (;;) {
        int fd = accept4(w->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                perror("accept4");        /* e.g. EMFILE: out of file descriptors */
            return;
        }

        int one = 1;   /* send small replies immediately (no Nagle delay) */
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        struct conn *c = malloc(sizeof *c);
        if (c == NULL) {
            close(fd);
            continue;
        }
        c->fd = fd;
        buf_init(&c->in);
        buf_init(&c->out);
        buf_init(&c->tmp);
        resp_cmd_init(&c->cmd);
        c->out_sent = 0;
        c->events = EPOLLIN;
        c->closing = false;

        struct epoll_event e = { .events = EPOLLIN, .data.ptr = c };
        if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, fd, &e) < 0) {
            perror("epoll_ctl(ADD)");
            close(fd);
            resp_cmd_free(&c->cmd);
            free(c);
        }
    }
}

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(w->epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            die("epoll_wait");
        }
        for (int i = 0; i < n; i++) {
            struct conn *c = events[i].data.ptr;
            if (c == NULL)                 /* the listener is registered with NULL */
                accept_all(w);
            else
                on_conn_event(w, c, events[i].events);
        }
    }
    return NULL;
}

/* ===========================================================================
 * Startup
 * ======================================================================== */

static struct sockaddr_in make_addr(const struct config *cfg)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)cfg->port);
    if (inet_pton(AF_INET, cfg->bind, &a.sin_addr) != 1) {
        fprintf(stderr, "redis-mc: invalid --bind address '%s'\n", cfg->bind);
        exit(1);
    }
    return a;
}

/* With SO_REUSEPORT, a second copy of redis-mc would bind the same port
 * without error and silently take half the connections. A plain bind
 * (no SO_REUSEPORT) fails if anything is already listening, so try that first. */
static void check_port_free(const struct config *cfg)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        die("socket");
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = make_addr(cfg);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "redis-mc: cannot use %s:%d: %s\n",
                cfg->bind, cfg->port, strerror(errno));
        exit(1);
    }
    close(fd);
}

static int create_listener(const struct config *cfg)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        die("socket");
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0)
        die("setsockopt(SO_REUSEADDR)");
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0)
        die("setsockopt(SO_REUSEPORT)");
    struct sockaddr_in a = make_addr(cfg);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0)
        die("bind");
    if (listen(fd, 511) < 0)
        die("listen");
    return fd;
}

static bool parse_int_arg(const char *s, int lo, int hi, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi)
        return false;
    *out = (int)v;
    return true;
}

static void usage(void)
{
    fprintf(stderr, "usage: redis-mc [--port N] [--threads N] [--bind ADDR]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    struct config cfg = { DEFAULT_BIND, DEFAULT_PORT, 0 };

    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc)
            usage();
        if (strcmp(argv[i], "--port") == 0) {
            if (!parse_int_arg(argv[++i], 1, 65535, &cfg.port))
                usage();
        } else if (strcmp(argv[i], "--threads") == 0) {
            if (!parse_int_arg(argv[++i], 1, 1024, &cfg.threads))
                usage();
        } else if (strcmp(argv[i], "--bind") == 0) {
            cfg.bind = argv[++i];
        } else {
            usage();
        }
    }
    if (cfg.threads == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        cfg.threads = (n > 0 && n <= 1024) ? (int)n : 1;
    }

    signal(SIGPIPE, SIG_IGN);   /* belt and braces next to MSG_NOSIGNAL */
    check_port_free(&cfg);

    struct store *store = store_create();
    struct worker *workers = calloc((size_t)cfg.threads, sizeof *workers);
    if (workers == NULL)
        die("calloc");

    for (int i = 0; i < cfg.threads; i++) {
        struct worker *w = &workers[i];
        w->store = store;
        w->listen_fd = create_listener(&cfg);
        w->epfd = epoll_create1(EPOLL_CLOEXEC);
        if (w->epfd < 0)
            die("epoll_create1");
        struct epoll_event e = { .events = EPOLLIN, .data.ptr = NULL };
        if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->listen_fd, &e) < 0)
            die("epoll_ctl(listener)");
    }
    for (int i = 0; i < cfg.threads; i++) {
        int rc = pthread_create(&workers[i].thread, NULL, worker_main, &workers[i]);
        if (rc != 0) {
            fprintf(stderr, "redis-mc: pthread_create: %s\n", strerror(rc));
            return 1;
        }
    }

    fprintf(stderr, "redis-mc listening on %s:%d with %d worker threads\n",
            cfg.bind, cfg.port, cfg.threads);

    for (int i = 0; i < cfg.threads; i++)
        pthread_join(workers[i].thread, NULL);
    return 0;
}
