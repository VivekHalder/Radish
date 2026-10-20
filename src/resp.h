/*
 * resp.h - RESP protocol layer: byte buffers, request parsing, reply writing.
 *
 * Behaviour follows docs/spec/resp.md (bytes observed from redis-server 7.0.15).
 */
#ifndef REDIS_MC_RESP_H
#define REDIS_MC_RESP_H

#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Growable byte buffer. Used for connection input and for reply output.
 * Allocation failure aborts the process (no partial-failure paths to test).
 * ------------------------------------------------------------------------- */
struct buf {
    char  *data;
    size_t len;   /* bytes in use */
    size_t cap;   /* bytes allocated */
};

void buf_init(struct buf *b);
void buf_free(struct buf *b);
void buf_reserve(struct buf *b, size_t extra);              /* ensure room for `extra` more bytes */
void buf_append(struct buf *b, const void *p, size_t n);
void buf_consume(struct buf *b, size_t n);                  /* drop the first n bytes */

/* ---------------------------------------------------------------------------
 * Request parsing
 * ------------------------------------------------------------------------- */

/* Limits. Bulk limit matches spec §7 (600000000 > limit -> error).
 * The other two are redis-mc decisions: they cap how much a client can make
 * us buffer before a request is complete. */
#define RESP_MAX_BULK_LEN    (512L * 1024 * 1024)
#define RESP_MAX_INLINE_LEN  (64 * 1024)          /* also caps any header line */
#define RESP_MAX_ARGS        (1024L * 1024)

struct resp_arg {
    const char *ptr;
    size_t      len;
};

/* One parsed request. Reuse the same struct for every command on a
 * connection; its arrays grow as needed and are kept between calls. */
struct resp_cmd {
    struct resp_arg *argv;
    size_t           argc;
    size_t           argv_cap;
    struct buf       scratch;  /* decoded inline arguments live here */
};

enum resp_status {
    RESP_OK,          /* one request parsed; *consumed bytes used (argc may be 0) */
    RESP_INCOMPLETE,  /* need more bytes; nothing consumed */
    RESP_ERROR        /* protocol error; reply with *err, then close the connection */
};

void resp_cmd_init(struct resp_cmd *cmd);
void resp_cmd_free(struct resp_cmd *cmd);

/*
 * Parse ONE request from the start of in[0..len).
 *
 * RESP_OK:         cmd->argv[0..argc) holds the arguments, *consumed is the
 *                  request's size in bytes. argc == 0 means "valid but nothing
 *                  to run" (empty inline line, *0, *-1) - just consume it.
 *                  Argument pointers point into `in` or into cmd->scratch and
 *                  stay valid until `in` is modified or resp_parse is called again.
 * RESP_INCOMPLETE: the request is not fully here yet. Call again with more bytes.
 * RESP_ERROR:      *err is a static message without '-' or CRLF, e.g.
 *                  "ERR Protocol error: invalid bulk length".
 *
 * Handles both multibulk ("*2\r\n$3\r\nGET\r\n...") and inline ("GET k\r\n").
 */
enum resp_status resp_parse(struct resp_cmd *cmd, const char *in, size_t len,
                            size_t *consumed, const char **err);

/* ---------------------------------------------------------------------------
 * Reply writing (RESP2). Each call appends one complete value to `out`.
 * ------------------------------------------------------------------------- */
void resp_simple(struct buf *out, const char *s);            /* +s\r\n          */
void resp_error(struct buf *out, const char *msg);           /* -msg\r\n        */
void resp_bulk(struct buf *out, const char *p, size_t n);    /* $n\r\n<p>\r\n   */
void resp_null(struct buf *out);                             /* $-1\r\n         */
void resp_int(struct buf *out, long long v);                 /* :v\r\n          */
void resp_array_len(struct buf *out, size_t n);              /* *n\r\n          */

#endif /* REDIS_MC_RESP_H */
