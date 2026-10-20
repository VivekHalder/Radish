# Command behaviour (observed): milestone 1

Exact replies for `PING`, `SET`, `GET`, `DEL`, `EXISTS`, `INCR` and for
unknown commands. Captured from `redis-server` 7.0.15 on `127.0.0.1:6379`
over raw sockets on 2026-10-20, with the same method and byte notation as
`resp.md`. RESP2 throughout.

---

## 1. Command names

Names are **case-insensitive**:

```
C: *1\r\n$4\r\nping\r\n          S: +PONG\r\n
C: *1\r\n$4\r\nPiNg\r\n          S: +PONG\r\n
```

## 2. Arity errors

The wrong number of arguments gives this, with the name **lower-cased**,
whatever case was sent:

```
-ERR wrong number of arguments for '<lowercase name>' command\r\n
```

| Command | Accepted argument counts (after the name) | Observed error input |
|---|---|---|
| `PING` | 0 or 1 | `PING a b` |
| `GET` | exactly 1 | `GET k x` |
| `SET` | 2 or more | `SET k`, `set k` → `'set'` |
| `INCR` | exactly 1 | `INCR` |
| `DEL` | 1 or more | `DEL` |
| `EXISTS` | 1 or more | `EXISTS` |

## 3. PING

See `resp.md` §1: `+PONG\r\n`, or the argument echoed as a bulk string.

## 4. SET / GET

```
C: SET k 1      S: +OK\r\n
C: SET k 2      S: +OK\r\n          (overwrites)
C: GET k        S: $1\r\n2\r\n
C: GET missing  S: $-1\r\n
```

Values are **binary-safe**: any byte, including `\x00` and `\r\n`, round-trips:

```
C: *3\r\n$3\r\nSET\r\n$14\r\nredismc:spec:a\r\n$4\r\n\x00\xff\r\n\r\n
S: +OK\r\n
C: *2\r\n$3\r\nGET\r\n$14\r\nredismc:spec:a\r\n
S: $4\r\n\x00\xff\r\n\r\n
```

**SET options** (`NX`, `XX`, `EX`, …) are out of scope for milestone 1.
Observed for reference:

```
C: SET k v bogus   S: -ERR syntax error\r\n
C: SET k v NX      S: $-1\r\n               (k existed)
```

redis-mc milestone 1 answers `-ERR syntax error\r\n` to **any** extra
argument. That matches the `bogus` case, and is a known difference for
real options.

## 5. DEL / EXISTS

Both reply with an integer.

```
setup: SET a 1, SET b x
C: EXISTS a a missing   S: :2\r\n     duplicates are counted each time
C: DEL a a missing      S: :1\r\n     a key is only deleted once
C: DEL b                S: :1\r\n
C: DEL b                S: :0\r\n
```

`DEL` returns the number of keys that existed and were removed. Processing
the keys left to right gives exactly this: the second `a` is already gone.

## 6. INCR

```
C: INCR new-key   S: :1\r\n         a missing key counts as 0
C: GET new-key    S: $1\r\n1\r\n    the result is stored as decimal text
```

### Which stored values count as integers

The value must be a **canonical** decimal 64-bit signed integer: optional
`-`, then digits, with no leading zeros, no `+`, no spaces, and no `-0`.

| Stored value | `INCR` reply |
|---|---|
| `0` | `:1\r\n` |
| `-9223372036854775808` (minimum) | `:-9223372036854775807\r\n` |
| `9223372036854775806` | `:9223372036854775807\r\n` |
| `9223372036854775807` (maximum) | `-ERR increment or decrement would overflow\r\n` |
| `-0` | `-ERR value is not an integer or out of range\r\n` |
| `01` | `-ERR value is not an integer or out of range\r\n` |
| `+1` | `-ERR value is not an integer or out of range\r\n` |
| ` 1` (leading space) | `-ERR value is not an integer or out of range\r\n` |
| `1 ` (trailing space) | `-ERR value is not an integer or out of range\r\n` |
| `1.0` | `-ERR value is not an integer or out of range\r\n` |
| empty string | `-ERR value is not an integer or out of range\r\n` |
| `99999999999999999999` (above the maximum) | `-ERR value is not an integer or out of range\r\n` |
| `123456789012345678901` (21 digits) | `-ERR value is not an integer or out of range\r\n` |

On any error the stored value is **unchanged** (a `GET` afterwards returns the original bytes).

## 7. Unknown command error

```
-ERR unknown command '<name>', with args beginning with: <args>\r\n
```

`<args>` is built from the arguments after the name, left to right:

```
cur = 0                                  # bytes of <args> written so far
for each argument, while cur < 128:
    n = min(len(arg), 128 - cur)
    append  "'" + first n bytes of arg + "' "
    cur += n + 3
```

So each part is quote, text, quote, **space**; the section always ends in a
space (or is empty) before `\r\n`.

The rule matches every observation:

| Arguments | Observed `<args>` (length) |
|---|---|
| none | empty (0) |
| `x` | `'x' ` (4) |
| 30 args `a0`…`a29` | `'a0' 'a1' … 'a22' ` (128, stops after a22) |
| one arg of 125 / 126 / 127 / 128 bytes | 128 / 129 / 130 / 131 |
| one arg of 129 or 200 bytes | first 128 bytes shown (131) |
| 100 bytes, then 100 bytes | 2nd arg cut to 25 bytes (131) |
| 120 + `bbbbb` | `bbbbb` in full (131) |
| 121 + `bbbbb` | `bbbb` (131) |
| 122 + `bbbbb` | `bbb` (131) |
| 123 + `bbbbb` | `bb` (131) |
| `a b`, `c'd` | `'a b' 'c'd' `: no escaping of quotes or spaces |
| empty string | `'' ` |

### The name

- It's echoed **with its original case**, cut to **128 bytes**.
- An empty name gives `'',`, as in `-ERR unknown command '', with args beginning with: \r\n`.

### Bytes inside the name and arguments

| Byte | Shown as |
|---|---|
| `\r` or `\n` | a space (each byte → one space): `line\r\nbreak` → `'line  break'` |
| `\x00` | **ends** that name/argument: `a\x00b…` → `'a'`, and name `F\x00O\tO` → `'F'` |
| anything else (`\x01`, `\t`, `\x1b`, `\x7f`, `\xff`, …) | sent **raw**, unchanged |

Not yet observed: whether a `\x00` cut also shortens the 128-byte budget
count. redis-mc counts the bytes it actually writes.
