# RESP wire behaviour (observed)

Exact bytes exchanged with a reference `redis-server`, recorded so redis-mc
can match them byte for byte. Everything here was observed on the wire, not
taken from Redis source.

## How this was captured

- **Server:** `redis-server` 7.0.15 on `127.0.0.1:6379` (WSL2, Ubuntu 24.04), default config.
- **Method:** raw TCP sockets from a Python client. No packet-capture tool
  (`tcpdump`/`tshark`) was available, so each request was written with
  `TCP_NODELAY` set and everything the server sent back was recorded until
  the line had been quiet for 300 ms.
- **Packets:** each `C:` line below is one `send()` call. Where a case shows
  several `C:` lines, there was a 300 ms pause between them, and the `S:` line
  under each shows what arrived during that pause (`(nothing)` = zero bytes).
- **Keys:** all test keys used the prefix `redismc:spec:` and were deleted afterwards.
- **Date:** 2026-10-20.

### Byte notation

Each example is the exact byte sequence written as a C-style escaped string:

| Notation | Byte |
|---|---|
| `\r` | 0x0D (CR) |
| `\n` | 0x0A (LF) |
| `\\` | one literal backslash (0x5C) |
| `\xNN` | any other non-printable byte |
| anything else | that ASCII character, literally |

`C:` = client to server, `S:` = server to client. Trailing spaces are significant;
where one appears before `\r\n` it is called out.

---

## 1. Basic replies (RESP2)

A new connection speaks RESP2 until it sends `HELLO 3`.

### PING

```
C: *1\r\n$4\r\nPING\r\n
S: +PONG\r\n
```

With an argument, the reply becomes a **bulk string**, not a simple string:

```
C: *2\r\n$4\r\nPING\r\n$5\r\nhello\r\n
S: $5\r\nhello\r\n
```

### SET / GET

```
C: *3\r\n$3\r\nSET\r\n$16\r\nredismc:spec:str\r\n$5\r\nhello\r\n
S: +OK\r\n
```

```
C: *2\r\n$3\r\nGET\r\n$16\r\nredismc:spec:str\r\n
S: $5\r\nhello\r\n
```

GET of an empty string is a zero-length bulk. It is **not** the same as nil:

```
C: *2\r\n$3\r\nGET\r\n$18\r\nredismc:spec:empty\r\n
S: $0\r\n\r\n
```

### GET on a missing key

RESP2 null bulk string:

```
C: *2\r\n$3\r\nGET\r\n$20\r\nredismc:spec:missing\r\n
S: $-1\r\n
```

### Unknown command

```
C: *3\r\n$6\r\nFOOBAR\r\n$1\r\na\r\n$1\r\nb\r\n
S: -ERR unknown command 'FOOBAR', with args beginning with: 'a' 'b' \r\n
```

With no arguments:

```
C: *1\r\n$6\r\nFOOBAR\r\n
S: -ERR unknown command 'FOOBAR', with args beginning with: \r\n
```

Things to match exactly:
- The command name is echoed **as sent**, keeping its original case, in single quotes.
- Each argument is rendered as `'arg'` followed by **one space**.
- The message therefore always ends in a **space before `\r\n`**, even with zero arguments.
- The connection stays open; later commands are processed normally (see §5).

### Wrong number of arguments

The command name is **lower-cased** here, unlike the unknown-command error:

```
C: *1\r\n$3\r\nGET\r\n
S: -ERR wrong number of arguments for 'get' command\r\n
```

### INCR

```
C: *2\r\n$4\r\nINCR\r\n$16\r\nredismc:spec:cnt\r\n
S: :1\r\n
```

The second call on the same key:

```
C: *2\r\n$4\r\nINCR\r\n$16\r\nredismc:spec:cnt\r\n
S: :2\r\n
```

On a value that is not an integer (`redismc:spec:str` holds `hello`):

```
C: *2\r\n$4\r\nINCR\r\n$16\r\nredismc:spec:str\r\n
S: -ERR value is not an integer or out of range\r\n
```

### LRANGE on a list

Setup: `RPUSH redismc:spec:list a bb ccc`

```
C: *4\r\n$6\r\nLRANGE\r\n$17\r\nredismc:spec:list\r\n$1\r\n0\r\n$2\r\n-1\r\n
S: *3\r\n$1\r\na\r\n$2\r\nbb\r\n$3\r\nccc\r\n
```

A missing key gives an **empty array**, not a null:

```
C: *4\r\n$6\r\nLRANGE\r\n$20\r\nredismc:spec:missing\r\n$1\r\n0\r\n$2\r\n-1\r\n
S: *0\r\n
```

---

## 2. Nested array: `COMMAND INFO GET` (RESP2)

```
C: *3\r\n$7\r\nCOMMAND\r\n$4\r\nINFO\r\n$3\r\nGET\r\n
```

Reply, one byte stream, broken into lines here at each `\r\n` and indented by
nesting depth for readability (the bytes contain no spaces or line breaks
beyond the `\r\n` shown):

```
S: *1\r\n                         outer array: 1 command
     *10\r\n                      command entry: 10 fields
       $3\r\nget\r\n              [1] name (lower-case)
       :2\r\n                     [2] arity
       *2\r\n                     [3] flags
         +readonly\r\n
         +fast\r\n
       :1\r\n                     [4] first key
       :1\r\n                     [5] last key
       :1\r\n                     [6] key step
       *3\r\n                     [7] ACL categories
         +@read\r\n
         +@string\r\n
         +@fast\r\n
       *0\r\n                     [8] tips
       *1\r\n                     [9] key specs: 1 spec
         *6\r\n                   spec as flat key/value array (3 pairs)
           $5\r\nflags\r\n
           *2\r\n
             +RO\r\n
             +access\r\n
           $12\r\nbegin_search\r\n
           *4\r\n
             $4\r\ntype\r\n
             $5\r\nindex\r\n
             $4\r\nspec\r\n
             *2\r\n
               $5\r\nindex\r\n
               :1\r\n
           $9\r\nfind_keys\r\n
           *4\r\n
             $4\r\ntype\r\n
             $5\r\nrange\r\n
             $4\r\nspec\r\n
             *6\r\n
               $7\r\nlastkey\r\n
               :0\r\n
               $7\r\nkeystep\r\n
               :1\r\n
               $5\r\nlimit\r\n
               :0\r\n
       *0\r\n                     [10] subcommands
```

The same bytes as one unbroken string:

```
S: *1\r\n*10\r\n$3\r\nget\r\n:2\r\n*2\r\n+readonly\r\n+fast\r\n:1\r\n:1\r\n:1\r\n*3\r\n+@read\r\n+@string\r\n+@fast\r\n*0\r\n*1\r\n*6\r\n$5\r\nflags\r\n*2\r\n+RO\r\n+access\r\n$12\r\nbegin_search\r\n*4\r\n$4\r\ntype\r\n$5\r\nindex\r\n$4\r\nspec\r\n*2\r\n$5\r\nindex\r\n:1\r\n$9\r\nfind_keys\r\n*4\r\n$4\r\ntype\r\n$5\r\nrange\r\n$4\r\nspec\r\n*6\r\n$7\r\nlastkey\r\n:0\r\n$7\r\nkeystep\r\n:1\r\n$5\r\nlimit\r\n:0\r\n*0\r\n
```

Notes:
- Flags and ACL categories are **simple strings** (`+`); key-spec field names are **bulk strings** (`$`).
- The content of this reply depends on the Redis version (7.0.15 here). Differential tests should compare it only against the same reference version.
- In RESP3 the same reply uses sets and maps (§6).

---

## 3. Pipelining: several commands in one packet

Three commands in a single `send()`:

```
C: *1\r\n$4\r\nPING\r\n*3\r\n$3\r\nSET\r\n$16\r\nredismc:spec:str\r\n$2\r\nv2\r\n*2\r\n$3\r\nGET\r\n$16\r\nredismc:spec:str\r\n
S: +PONG\r\n+OK\r\n$2\r\nv2\r\n
```

- Replies come back in request order and are simply concatenated.
- All three replies arrived together in one read.

Inline commands pipeline the same way:

```
C: PING\r\nPING\r\n
S: +PONG\r\n+PONG\r\n
```

---

## 4. Commands split across packets

### Split in the middle of a header

```
C: *2\r\n$3\r\nGET\r\n$
S: (nothing)
C: 16\r\nredismc:spec:str\r\n
S: $2\r\nv2\r\n
```

### Split inside a bulk payload

```
C: *3\r\n$3\r\nSET\r\n$16\r\nredismc:spec:str\r\n$5\r\nwor
S: (nothing)
C: ld\r\n
S: +OK\r\n
```

### Complete command followed by a partial one

```
C: *1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPI
S: +PONG\r\n
C: NG\r\n
S: +PONG\r\n
```

What this shows:
- The server buffers partial input and sends nothing until a command is complete.
- A split can fall anywhere, even between `$` and its length digits.
- Complete commands at the front of a buffer are executed and answered **straight away**; a trailing partial command does not hold them back.

---

## 5. Inline commands

A request that does not start with `*` is parsed as an inline command: one
line, arguments separated by spaces.

```
C: PING\r\n
S: +PONG\r\n
```

A bare `\n` line ending is also accepted:

```
C: PING\n
S: +PONG\r\n
```

With arguments (`redismc:spec:str` holds `world` at this point):

```
C: GET redismc:spec:str\r\n
S: $5\r\nworld\r\n
```

Double-quoted arguments may contain spaces, and escape sequences inside them
are decoded. Here the client sends a literal backslash followed by `x41`,
which the server stores as `A`:

```
C: SET redismc:spec:str "a b\\x41"\r\n
S: +OK\r\n
C: GET redismc:spec:str\r\n
S: $4\r\na bA\r\n
```

An empty line is silently ignored, with no reply:

```
C: \r\n
S: (nothing)
C: PING\r\n
S: +PONG\r\n
```

An unknown inline command produces the same error as the RESP form:

```
C: FOO bar\r\n
S: -ERR unknown command 'FOO', with args beginning with: 'bar' \r\n
```

---

## 6. RESP3 (after `HELLO 3`)

### HELLO 3

The reply is a RESP3 **map** (`%`), sent already in RESP3:

```
C: *2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n
S: %7\r\n$6\r\nserver\r\n$5\r\nredis\r\n$7\r\nversion\r\n$6\r\n7.0.15\r\n$5\r\nproto\r\n:3\r\n$2\r\nid\r\n:35\r\n$4\r\nmode\r\n$10\r\nstandalone\r\n$4\r\nrole\r\n$6\r\nmaster\r\n$7\r\nmodules\r\n*0\r\n
```

`id` is the connection ID and will differ between runs. `server` and `version`
are values redis-mc must decide on: clients may check `version`.

### Replies that change in RESP3

GET on a missing key returns the RESP3 **null** instead of `$-1`:

```
C: *2\r\n$3\r\nGET\r\n$20\r\nredismc:spec:missing\r\n
S: _\r\n
```

### Replies that do not change in RESP3

```
C: *2\r\n$4\r\nINCR\r\n$16\r\nredismc:spec:cnt\r\n
S: :3\r\n
```

```
C: *4\r\n$6\r\nLRANGE\r\n$17\r\nredismc:spec:list\r\n$1\r\n0\r\n$2\r\n-1\r\n
S: *3\r\n$1\r\na\r\n$2\r\nbb\r\n$3\r\nccc\r\n
```

```
C: *1\r\n$6\r\nFOOBAR\r\n
S: -ERR unknown command 'FOOBAR', with args beginning with: \r\n
```

### COMMAND INFO GET in RESP3

The structure matches §2, with flags, categories, tips and subcommands sent as **sets** (`~`), and each key spec and its sub-objects sent as **maps** (`%`), with the count of pairs, not elements:

```
S: *1\r\n*10\r\n$3\r\nget\r\n:2\r\n~2\r\n+readonly\r\n+fast\r\n:1\r\n:1\r\n:1\r\n~3\r\n+@read\r\n+@string\r\n+@fast\r\n~0\r\n~1\r\n%3\r\n$5\r\nflags\r\n~2\r\n+RO\r\n+access\r\n$12\r\nbegin_search\r\n%2\r\n$4\r\ntype\r\n$5\r\nindex\r\n$4\r\nspec\r\n%1\r\n$5\r\nindex\r\n:1\r\n$9\r\nfind_keys\r\n%2\r\n$4\r\ntype\r\n$5\r\nrange\r\n$4\r\nspec\r\n%3\r\n$7\r\nlastkey\r\n:0\r\n$7\r\nkeystep\r\n:1\r\n$5\r\nlimit\r\n:0\r\n~0\r\n
```

Changes compared with RESP2:

| Field | RESP2 | RESP3 |
|---|---|---|
| flags, ACL categories, tips | `*N` array | `~N` set |
| key-spec list | `*1` array | `~1` set |
| one key spec | `*6` flat array | `%3` map |
| `begin_search` / `find_keys` objects | `*4` | `%2` |
| their `spec` objects | `*2` / `*6` | `%1` / `%3` |
| subcommands | `*0` | `~0` |
| outer list and the 10-field command entry | `*` | `*` (no change) |

---

## 7. Protocol errors

| Input | Reply | Connection |
|---|---|---|
| `*abc\r\n` | `-ERR Protocol error: invalid multibulk length\r\n` | **closed** |
| `*1\r\n$x\r\n` | `-ERR Protocol error: invalid bulk length\r\n` | **closed** |
| `*1\r\n$-1\r\n` | `-ERR Protocol error: invalid bulk length\r\n` | **closed** |
| `*1\r\n$600000000\r\n` (above 512 MB) | `-ERR Protocol error: invalid bulk length\r\n` | **closed** |
| `*1\r\nPING\r\n` | `-ERR Protocol error: expected '$', got 'P'\r\n` | **closed** |
| `SET a "b\r\n` (inline) | `-ERR Protocol error: unbalanced quotes in request\r\n` | **closed** |

- After a protocol error the server sends the error and then closes the
  connection. Commands already in the same buffer after the bad bytes are
  **not** executed. For example, `*abc\r\n*1\r\n$4\r\nPING\r\n` gets only the
  error, with no `+PONG`.
- Command errors such as unknown command, wrong arity or wrong type are
  ordinary replies, and the connection stays open:

```
C: FOO\r\nPING\r\n
S: -ERR unknown command 'FOO', with args beginning with: \r\n+PONG\r\n
```

### Edge cases that are not errors

| Input | Behaviour |
|---|---|
| `*0\r\nPING\r\n` | `*0` is skipped silently; reply is just `+PONG\r\n` |
| `*-1\r\nPING\r\n` | `*-1` is skipped silently; reply is just `+PONG\r\n` |
| `*2000000000\r\n` | No reply, connection stays open: the server waits for 2 billion arguments |
| `*1\r\n$4\r\nPINGxx` | Reply `+PONG\r\n`. The 2 bytes after a bulk payload are **consumed without checking** that they are `\r\n` |

The last two rows are reference behaviour, not necessarily desirable.
redis-mc needs a deliberate decision on each (match exactly, or document the
difference), and the differential tests should record that decision.
