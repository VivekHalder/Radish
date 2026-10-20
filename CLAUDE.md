# redis-mc

A Redis-compatible server written from scratch in C11.

- **Target:** Linux (WSL2, Ubuntu).
- **Goal:** wire-compatible with RESP2 and RESP3, and multi-core, so commands do not run on a single thread the way they do in Redis.

## Workflow

- Write ONE file at a time. Then stop and wait for the user's review before starting the next file.

## Git

- Remote: `origin` = https://github.com/VivekHalder/Radish
- Never run `git push`. The user reviews and pushes every change themselves.
- Commit only when the user asks, with their exact message. Do not add a `Co-Authored-By` trailer.
- Never rewrite history (`--amend`, rebase, reset, force) without asking first.

## Source rules

- Never copy code from the Redis or Valkey source.
- Implement only from the specs in `docs/spec/*.md`.

## Build

- Use a Makefile with `-std=c11 -Wall -Wextra -Werror`.
- Provide an `asan` target that builds with AddressSanitizer and UndefinedBehaviorSanitizer (`-fsanitize=address,undefined`).

## I/O

- Use epoll for I/O.

## Testing

- Every feature ships with a differential test against a real `redis-server`.
- The reference `redis-server` runs on port **6379**. redis-mc runs on port **6380**.
