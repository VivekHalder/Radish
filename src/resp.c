/*
 * resp.c - RESP protocol layer. Interface in resp.h; byte-level behaviour
 * from docs/spec/resp.md.
 */
#include "resp.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * Allocation
 * ======================================================================== */

static void die_oom(void)
{
    fputs("redis-mc: out of memory\n", stderr);
    abort();
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (q == NULL)
        die_oom();
    return q;
}

/* ===========================================================================
 * struct buf
 * ======================================================================== */

void buf_init(struct buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(struct buf *b)
{
    free(b->data);
    buf_init(b);
}

void buf_reserve(struct buf *b, size_t extra)
{
    if (extra > SIZE_MAX - b->len)
        die_oom();
    size_t need = b->len + extra;
    if (need <= b->cap)
        return;

    size_t cap = b->cap ? b->cap : 64;
    while (cap < need)
        cap = (cap > SIZE_MAX / 2) ? need : cap * 2;

    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_append(struct buf *b, const void *p, size_t n)
{
    if (n == 0)
        return;               /* memcpy with a NULL pointer is UB, even for 0 bytes */
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void buf_consume(struct buf *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

/* ===========================================================================
 * Parsing helpers
 * ======================================================================== */

/* Error texts from docs/spec/resp.md §7. */
static const char ERR_MBULK[]  = "ERR Protocol error: invalid multibulk length";
static const char ERR_BULK[]   = "ERR Protocol error: invalid bulk length";
static const char ERR_QUOTES[] = "ERR Protocol error: unbalanced quotes in request";
/* Not captured in the spec yet: redis-mc's own wording for the inline limit. */
static const char ERR_INLINE_BIG[] = "ERR Protocol error: too big inline request";

/* "expected '$', got 'X'" needs the offending byte, so it is formatted into a
 * per-thread buffer: each worker thread parses independently. */
static _Thread_local char err_expected[64];

void resp_cmd_init(struct resp_cmd *cmd)
{
    cmd->argv = NULL;
    cmd->argc = 0;
    cmd->argv_cap = 0;
    buf_init(&cmd->scratch);
}

void resp_cmd_free(struct resp_cmd *cmd)
{
    free(cmd->argv);
    buf_free(&cmd->scratch);
    resp_cmd_init(cmd);
}

static void push_arg(struct resp_cmd *cmd, const char *p, size_t n)
{
    if (cmd->argc == cmd->argv_cap) {
        size_t cap = cmd->argv_cap ? cmd->argv_cap * 2 : 8;
        cmd->argv = xrealloc(cmd->argv, cap * sizeof *cmd->argv);
        cmd->argv_cap = cap;
    }
    cmd->argv[cmd->argc].ptr = p;
    cmd->argv[cmd->argc].len = n;
    cmd->argc++;
}

/* Strict decimal: optional '-', at least one digit, nothing else, no overflow. */
static bool parse_ll(const char *p, size_t n, long long *out)
{
    bool neg = false;
    if (n > 0 && *p == '-') {
        neg = true;
        p++;
        n--;
    }
    if (n == 0)
        return false;

    long long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9')
            return false;
        int d = p[i] - '0';
        if (v > (LLONG_MAX - d) / 10)
            return false;
        v = v * 10 + d;
    }
    *out = neg ? -v : v;
    return true;
}

/*
 * Find the end of a header line ("*3\r\n" or "$5\r\n") whose text starts at
 * in[start]. On RESP_OK: *text_len = bytes before "\r\n", *next = index just
 * after "\r\n". RESP_ERROR means a CR not followed by LF, or no CR within the
 * line limit; the caller picks the error message.
 */
static enum resp_status read_header(const char *in, size_t len, size_t start,
                                    size_t *text_len, size_t *next)
{
    size_t avail = len - start;
    size_t scan = avail < RESP_MAX_INLINE_LEN ? avail : RESP_MAX_INLINE_LEN;
    const char *cr = memchr(in + start, '\r', scan);

    if (cr == NULL)
        return avail >= RESP_MAX_INLINE_LEN ? RESP_ERROR : RESP_INCOMPLETE;

    size_t cr_at = (size_t)(cr - in);
    if (cr_at + 1 >= len)
        return RESP_INCOMPLETE;          /* have the CR, LF not here yet */
    if (in[cr_at + 1] != '\n')
        return RESP_ERROR;

    *text_len = cr_at - start;
    *next = cr_at + 2;
    return RESP_OK;
}

/* ===========================================================================
 * Multibulk: *<count>\r\n then <count> x $<len>\r\n<bytes>\r\n
 * ======================================================================== */

static enum resp_status parse_multibulk(struct resp_cmd *cmd, const char *in,
                                        size_t len, size_t *consumed,
                                        const char **err)
{
    size_t text_len, pos;
    enum resp_status st = read_header(in, len, 1, &text_len, &pos);
    if (st == RESP_INCOMPLETE)
        return st;

    long long count;
    if (st == RESP_ERROR || !parse_ll(in + 1, text_len, &count) ||
        count > RESP_MAX_ARGS) {
        *err = ERR_MBULK;
        return RESP_ERROR;
    }
    if (count <= 0) {                    /* *0 and *-1 are skipped (spec §7) */
        *consumed = pos;
        return RESP_OK;
    }

    for (long long i = 0; i < count; i++) {
        if (pos >= len)
            return RESP_INCOMPLETE;

        if (in[pos] != '$') {
            /* Never echo a control byte: it could break the reply's framing. */
            unsigned char c = (unsigned char)in[pos];
            snprintf(err_expected, sizeof err_expected,
                     "ERR Protocol error: expected '$', got '%c'",
                     (c >= 0x20 && c < 0x7f) ? c : '?');
            *err = err_expected;
            return RESP_ERROR;
        }

        size_t data;
        st = read_header(in, len, pos + 1, &text_len, &data);
        if (st == RESP_INCOMPLETE)
            return st;

        long long blen;
        if (st == RESP_ERROR || !parse_ll(in + pos + 1, text_len, &blen) ||
            blen < 0 || blen > RESP_MAX_BULK_LEN) {
            *err = ERR_BULK;
            return RESP_ERROR;
        }

        /* Payload plus 2 trailing bytes. Like redis-server (spec §7), the 2
         * bytes are skipped without checking they are "\r\n". To be strict
         * instead, reject here when they are not. */
        size_t need = (size_t)blen + 2;
        if (len - data < need)
            return RESP_INCOMPLETE;

        push_arg(cmd, in + data, (size_t)blen);
        pos = data + need;
    }

    *consumed = pos;
    return RESP_OK;
}

/* ===========================================================================
 * Inline: one line of space-separated words, "double quotes" allowed
 * ======================================================================== */

static bool is_space(char c)
{
    return c == ' ' || c == '\t';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static enum resp_status parse_inline(struct resp_cmd *cmd, const char *in,
                                     size_t len, size_t *consumed,
                                     const char **err)
{
    size_t scan = len < RESP_MAX_INLINE_LEN ? len : RESP_MAX_INLINE_LEN;
    const char *nl = memchr(in, '\n', scan);
    if (nl == NULL) {
        if (len >= RESP_MAX_INLINE_LEN) {
            *err = ERR_INLINE_BIG;
            return RESP_ERROR;
        }
        return RESP_INCOMPLETE;
    }

    size_t lf_at = (size_t)(nl - in);
    /* "\r\n" and a bare "\n" both end the line (spec §5). */
    size_t end = (lf_at > 0 && in[lf_at - 1] == '\r') ? lf_at - 1 : lf_at;

    /* Decoding never makes text longer, so reserving `end` bytes now means
     * scratch.data cannot move while we hand out pointers into it. */
    cmd->scratch.len = 0;
    buf_reserve(&cmd->scratch, end);
    char *out = cmd->scratch.data;

    size_t i = 0;   /* read position in the line */
    size_t w = 0;   /* write position in scratch */
    for (;;) {
        while (i < end && is_space(in[i]))
            i++;
        if (i == end)
            break;

        size_t arg_start = w;

        if (in[i] == '"') {
            bool closed = false;
            i++;
            while (i < end) {
                char c = in[i];
                if (c == '"') {
                    closed = true;
                    i++;
                    break;
                }
                if (c == '\\' && i + 1 < end) {
                    char e = in[i + 1];
                    int hi, lo;
                    if (e == 'x' && i + 3 < end &&
                        (hi = hex_val(in[i + 2])) >= 0 &&
                        (lo = hex_val(in[i + 3])) >= 0) {
                        out[w++] = (char)(hi * 16 + lo);
                        i += 4;
                        continue;
                    }
                    switch (e) {
                    case 'n': out[w++] = '\n'; break;
                    case 'r': out[w++] = '\r'; break;
                    case 't': out[w++] = '\t'; break;
                    case 'b': out[w++] = '\b'; break;
                    case 'a': out[w++] = '\a'; break;
                    default:  out[w++] = e;    break;   /* \\ \" and anything else */
                    }
                    i += 2;
                    continue;
                }
                out[w++] = c;
                i++;
            }
            /* A closing quote must end the word: `"a"b` is also unbalanced. */
            if (!closed || (i < end && !is_space(in[i]))) {
                *err = ERR_QUOTES;
                return RESP_ERROR;
            }
        } else {
            while (i < end && !is_space(in[i]))
                out[w++] = in[i++];
        }

        push_arg(cmd, out + arg_start, w - arg_start);
    }

    cmd->scratch.len = w;
    *consumed = lf_at + 1;   /* argc == 0 for an empty line: skipped (spec §5) */
    return RESP_OK;
}

/* ===========================================================================
 * Public parser entry point
 * ======================================================================== */

enum resp_status resp_parse(struct resp_cmd *cmd, const char *in, size_t len,
                            size_t *consumed, const char **err)
{
    cmd->argc = 0;
    if (len == 0)
        return RESP_INCOMPLETE;
    if (in[0] == '*')
        return parse_multibulk(cmd, in, len, consumed, err);
    return parse_inline(cmd, in, len, consumed, err);
}

/* ===========================================================================
 * Reply writers (RESP2)
 * Simple strings and errors must not contain '\r' or '\n'; callers ensure it.
 * ======================================================================== */

/* Append <type><text>\r\n */
static void put_line(struct buf *out, char type, const char *text, size_t n)
{
    buf_reserve(out, n + 3);
    out->data[out->len++] = type;
    if (n > 0) {
        memcpy(out->data + out->len, text, n);
        out->len += n;
    }
    out->data[out->len++] = '\r';
    out->data[out->len++] = '\n';
}

static void put_size(struct buf *out, char type, size_t v)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%zu", v);
    put_line(out, type, tmp, (size_t)n);
}

void resp_simple(struct buf *out, const char *s)
{
    put_line(out, '+', s, strlen(s));
}

void resp_error(struct buf *out, const char *msg)
{
    put_line(out, '-', msg, strlen(msg));
}

void resp_bulk(struct buf *out, const char *p, size_t n)
{
    put_size(out, '$', n);
    buf_reserve(out, n + 2);
    if (n > 0) {
        memcpy(out->data + out->len, p, n);
        out->len += n;
    }
    out->data[out->len++] = '\r';
    out->data[out->len++] = '\n';
}

void resp_null(struct buf *out)
{
    buf_append(out, "$-1\r\n", 5);
}

void resp_int(struct buf *out, long long v)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%lld", v);
    put_line(out, ':', tmp, (size_t)n);
}

void resp_array_len(struct buf *out, size_t n)
{
    put_size(out, '*', n);
}
