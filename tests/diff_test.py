#!/usr/bin/env python3
"""
Differential tests: send identical bytes to redis-server (reference) and
redis-mc, and require byte-identical replies.

    python3 tests/diff_test.py [--ref PORT] [--ours PORT] [--host HOST]

Defaults: reference on 6379, redis-mc on 6380 (see CLAUDE.md). Both must be
running. Only keys starting with "redismc:test:" are touched.

Each case is a list of byte chunks. Every chunk is written as its own send()
on a fresh connection; after each chunk we collect whatever arrives until the
line is quiet for IDLE seconds, and note whether the server closed the
connection. Cases come from docs/spec/resp.md and docs/spec/commands.md.
"""
import argparse
import socket
import sys
import threading

IDLE = 0.1
P = "redismc:test:"


def cmd(*args):
    """Encode one RESP multibulk request."""
    out = b"*%d\r\n" % len(args)
    for a in args:
        a = a.encode() if isinstance(a, str) else a
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out


def esc(b):
    out = []
    for c in b:
        if c == 13: out.append("\\r")
        elif c == 10: out.append("\\n")
        elif c == 92: out.append("\\\\")
        elif 32 <= c < 127: out.append(chr(c))
        else: out.append("\\x%02x" % c)
    return "".join(out)


def K(name):
    return P + name


# ---------------------------------------------------------------------------
# Cases: (name, [chunk, chunk, ...]). Run in order; later cases may rely on
# keys set by earlier ones, identically on both servers.
# ---------------------------------------------------------------------------
CASES = [
    # resp.md section 1: basic replies
    ("PING", [cmd("PING")]),
    ("PING with argument", [cmd("PING", "hello")]),
    ("SET", [cmd("SET", K("str"), "hello")]),
    ("GET existing", [cmd("GET", K("str"))]),
    ("SET empty value", [cmd("SET", K("empty"), "")]),
    ("GET empty value", [cmd("GET", K("empty"))]),
    ("GET missing", [cmd("GET", K("missing"))]),
    ("unknown command with args", [cmd("FOOBAR", "a", "b")]),
    ("unknown command no args", [cmd("FOOBAR")]),
    ("wrong arity GET", [cmd("GET")]),
    ("INCR new key", [cmd("INCR", K("cnt"))]),
    ("INCR again", [cmd("INCR", K("cnt"))]),
    ("INCR non-integer", [cmd("INCR", K("str"))]),

    # resp.md section 3: pipelining
    ("pipeline 3 commands", [cmd("PING") + cmd("SET", K("str"), "v2") + cmd("GET", K("str"))]),
    ("inline pipeline", [b"PING\r\nPING\r\n"]),

    # resp.md section 4: split packets
    ("split in header", [b"*2\r\n$3\r\nGET\r\n$", b"%d\r\n%s\r\n" % (len(K("str")), K("str").encode())]),
    ("split in bulk payload", [cmd("SET", K("str"), "world")[:-4], cmd("SET", K("str"), "world")[-4:]]),
    ("complete + partial", [cmd("PING") + b"*1\r\n$4\r\nPI", b"NG\r\n"]),
    ("split between CR and LF", [b"*1\r\n$4\r\nPING\r", b"\n"]),
    ("byte at a time", [bytes([b]) for b in cmd("GET", K("str"))]),

    # resp.md section 5: inline
    ("inline PING", [b"PING\r\n"]),
    ("inline LF only", [b"PING\n"]),
    ("inline with args", [("GET " + K("str") + "\r\n").encode()]),
    ("inline quoted + hex escape", [('SET ' + K("str") + ' "a b\\x41"\r\n').encode(), ("GET " + K("str") + "\r\n").encode()]),
    ("inline empty line", [b"\r\n", b"PING\r\n"]),
    ("inline unknown command", [b"FOO bar\r\n"]),
    ("inline extra spaces", [b"  PING   \r\n"]),

    # resp.md section 7: protocol errors (connection closes)
    ("bad multibulk length", [b"*abc\r\n"]),
    ("bad multibulk then PING", [b"*abc\r\n*1\r\n$4\r\nPING\r\n"]),
    ("bad bulk length", [b"*1\r\n$x\r\n"]),
    ("negative bulk length", [b"*1\r\n$-1\r\n"]),
    ("bulk length too big", [b"*1\r\n$600000000\r\n"]),
    ("expected $", [b"*1\r\nPING\r\n"]),
    ("unbalanced quotes", [b'SET a "b\r\n']),
    ("PING then protocol error", [b"PING\r\n*abc\r\n"]),
    ("unknown command keeps connection", [b"FOO\r\nPING\r\n"]),
    ("*0 skipped", [b"*0\r\nPING\r\n"]),
    ("*-1 skipped", [b"*-1\r\nPING\r\n"]),
    ("trailing bytes not checked", [b"*1\r\n$4\r\nPINGxx"]),
    ("huge multibulk count", [b"*2000000000\r\n"]),

    # commands.md sections 1-2: names and arity
    ("lowercase name", [cmd("ping")]),
    ("mixed-case name", [cmd("PiNg")]),
    ("PING too many args", [cmd("PING", "a", "b")]),
    ("SET one arg", [cmd("SET", K("a"))]),
    ("lowercase set one arg", [cmd("set", K("a"))]),
    ("GET two args", [cmd("GET", K("a"), "x")]),
    ("INCR no args", [cmd("INCR")]),
    ("DEL no args", [cmd("DEL")]),
    ("EXISTS no args", [cmd("EXISTS")]),

    # commands.md section 4: SET / GET
    ("SET overwrite", [cmd("SET", K("a"), "1"), cmd("SET", K("a"), "2"), cmd("GET", K("a"))]),
    ("SET bogus option", [cmd("SET", K("a"), "v", "bogus")]),
    ("binary value", [cmd("SET", K("bin"), b"\x00\xff\r\n"), cmd("GET", K("bin"))]),
    ("binary key", [cmd("SET", K("k\x00\r\n"), "v"), cmd("GET", K("k\x00\r\n")), cmd("GET", K("k"))]),
    ("large value 1MB", [cmd("SET", K("big"), b"x" * (1 << 20)), cmd("GET", K("big"))]),

    # commands.md section 5: DEL / EXISTS
    ("DEL/EXISTS", [cmd("SET", K("b"), "x"),
                    cmd("EXISTS", K("a"), K("a"), K("missing")),
                    cmd("DEL", K("a"), K("a"), K("missing")),
                    cmd("DEL", K("b")), cmd("DEL", K("b"))]),
]

# commands.md section 6: INCR value rules
for v in ["0", "-0", "01", "+1", " 1", "1 ", "1.0", "", "-9223372036854775808",
          "9223372036854775806", "9223372036854775807", "99999999999999999999",
          "123456789012345678901", "-1", "-10"]:
    CASES.append(("INCR on %r" % v, [cmd("SET", K("n"), v), cmd("INCR", K("n")), cmd("GET", K("n"))]))

# commands.md section 7: unknown command message
CASES += [
    ("unknown: 30 args", [cmd("FOO", *["a%d" % i for i in range(30)])]),
    ("unknown: 129-byte arg", [cmd("FOO", "x" * 129)]),
    ("unknown: 100+100", [cmd("FOO", "a" * 100, "b" * 100)]),
    ("unknown: 122+5", [cmd("FOO", "a" * 122, "bbbbb")]),
    ("unknown: 200-byte name", [cmd("N" * 200)]),
    ("unknown: quotes and spaces", [cmd("FOO", "a b", "c'd")]),
    ("unknown: CRLF in arg", [cmd("FOO", "line\r\nbreak")]),
    ("unknown: CRLF in name", [cmd("F\r\nOO", "x")]),
    ("unknown: empty arg", [cmd("FOO", "")]),
    ("unknown: empty name", [cmd("")]),
    ("unknown: NUL in arg", [cmd("FOO", b"a\x00b")]),
    ("unknown: NUL in name", [cmd(b"F\x00O\tO")]),
    ("unknown: raw control bytes", [cmd("FOO", b"a\x01b\x7fc\xffd\te\x1bf")]),
]

# Deliberate differences: reported, but do not fail the run.
KNOWN_DIFFERENCES = {
    "huge multibulk count":
        "redis-mc rejects more than RESP_MAX_ARGS (1M) arguments; Redis waits for them",
}

CLEANUP_KEYS = ["str", "empty", "missing", "cnt", "a", "b", "bin", "k\x00\r\n", "k", "big", "n"]


# ---------------------------------------------------------------------------

def connect(host, port):
    s = socket.create_connection((host, port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def collect(s):
    """Read until IDLE seconds of silence. Returns (bytes, closed)."""
    s.settimeout(IDLE)
    buf = b""
    while True:
        try:
            d = s.recv(1 << 20)
        except socket.timeout:
            return buf, False
        except ConnectionResetError:
            return buf, True
        if not d:
            return buf, True
        buf += d


def run_case(host, port, chunks):
    """Returns a list of (reply_bytes, closed) per chunk."""
    s = connect(host, port)
    out = []
    try:
        for ch in chunks:
            try:
                s.sendall(ch)
            except (BrokenPipeError, ConnectionResetError):
                out.append((b"", True))
                break
            reply, closed = collect(s)
            out.append((reply, closed))
            if closed:
                break
    finally:
        s.close()
    return out


def run_both(host, ref, ours, chunks):
    res = {}

    def go(port):
        res[port] = run_case(host, port, chunks)

    threads = [threading.Thread(target=go, args=(p,)) for p in (ref, ours)]
    for t in threads: t.start()
    for t in threads: t.join()
    return res[ref], res[ours]


def show(steps):
    return "\n".join("        %s%s" % (esc(r) if len(r) < 400 else esc(r[:200]) + "...(%d bytes)" % len(r),
                                     "  [closed]" if c else "") for r, c in steps)


def cleanup(host, port):
    s = connect(host, port)
    s.sendall(cmd("DEL", *[K(k) for k in CLEANUP_KEYS]))
    collect(s)
    s.close()


def concurrency(host, port, threads=8, per_thread=2000, batch=100):
    """Many threads INCR one key at once. Returns the final value."""
    key = K("concurrent")
    cleanup_one(host, port, key)

    def worker():
        s = connect(host, port)
        req = cmd("INCR", key) * batch
        for _ in range(per_thread // batch):
            s.sendall(req)
            need = batch
            buf = b""
            s.settimeout(10)
            while buf.count(b"\r\n") < need:
                buf += s.recv(65536)
        s.close()

    ts = [threading.Thread(target=worker) for _ in range(threads)]
    for t in ts: t.start()
    for t in ts: t.join()
    s = connect(host, port)
    s.sendall(cmd("GET", key))
    reply, _ = collect(s)
    s.close()
    cleanup_one(host, port, key)
    return reply


def cleanup_one(host, port, key):
    s = connect(host, port)
    s.sendall(cmd("DEL", key))
    collect(s)
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--ref", type=int, default=6379)
    ap.add_argument("--ours", type=int, default=6380)
    a = ap.parse_args()

    for port, who in ((a.ref, "reference redis-server"), (a.ours, "redis-mc")):
        try:
            connect(a.host, port).close()
        except OSError as e:
            print("cannot reach %s on %s:%d (%s)" % (who, a.host, port, e))
            return 2

    for port in (a.ref, a.ours):
        cleanup(a.host, port)

    passed = failed = known = 0
    for name, chunks in CASES:
        r, o = run_both(a.host, a.ref, a.ours, chunks)
        if r == o:
            passed += 1
            if name in KNOWN_DIFFERENCES:
                print("NOTE  %s: listed as a known difference but now matches" % name)
            continue
        if name in KNOWN_DIFFERENCES:
            known += 1
            print("KNOWN %s  (%s)" % (name, KNOWN_DIFFERENCES[name]))
            continue
        failed += 1
        print("FAIL  %s\n    sent:\n%s\n    redis-server:\n%s\n    redis-mc:\n%s" % (
            name, "\n".join("        " + esc(c[:200]) for c in chunks), show(r), show(o)))

    for port in (a.ref, a.ours):
        cleanup(a.host, port)

    want = b"$5\r\n16000\r\n"
    cr = concurrency(a.host, a.ref)
    co = concurrency(a.host, a.ours)
    if cr == co == want:
        passed += 1
    else:
        failed += 1
        print("FAIL  concurrent INCR (8 threads x 2000): redis-server %s, redis-mc %s, want %s"
              % (esc(cr), esc(co), esc(want)))

    print("\n%d passed, %d failed, %d known differences" % (passed, failed, known))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
