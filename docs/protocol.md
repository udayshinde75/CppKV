# CppKV Protocol, Version 1

This document specifies what CppKV implements. The implementation lives in
`src/cppkv/protocol/` (framing, parsing, encoding) and `src/cppkv/server/command_dispatcher.cpp`
(command semantics). Every rule below is covered by a test; test names are given where useful.

CppKV's protocol is its own design. It is not RESP and is not compatible with Redis clients.

## 1. Transport

- TCP. Default address `127.0.0.1:7379` (`--bind`, `--port`).
- One connection carries any number of requests. Each request gets exactly one response, in order,
  except empty lines, which get none.
- **Pipelining is allowed:** a client may send many requests before reading any response.
- TCP is a byte stream, so requests may arrive split across reads or merged with other requests.
  The server handles both (tests: `LineFramer.EveryTwoWaySplitGivesTheSameLines`,
  `ConnectionHandler.FullSessionSplitAtEveryPairOfPositions`).
- Many connections are served concurrently (ADR 0002), up to `--max-connections` (default 128). Each
  command is atomic: a `GET` never observes half of a concurrent `SET`. Between different connections, the
  order of commands is whatever order the server executes them in; within one connection, it is the send order.

## 2. Requests

### 2.1 Framing

- A request is one line terminated by LF (`\n`, 0x0A).
- One CR (`\r`, 0x0D) directly before the LF is removed, so `\r\n` works too (`telnet`).
- A line consisting only of a terminator (`\n` or `\r\n`) is ignored and gets no response.
- **Maximum line length: 65 536 bytes**, not counting the terminator.

### 2.2 Syntax

```
request = command [ " " argument ]...     tokens separated by exactly one space (0x20)

PING
DBSIZE
QUIT
GET <key>
DEL <key>
EXISTS <key>
SET <key> <value>                          <value> = every byte after the space that follows <key>
```

- Command names are ASCII case-insensitive: `get`, `GET` and `GeT` are the same command.
- Tokens are separated by exactly **one** space. A leading space, a trailing space, or two spaces in a row
  produce an empty token, which is an error (`ERR PROTOCOL`). Typing `PING ` (with a trailing space) in
  `nc` is therefore an error.
- **The `SET` value is the exception:** it is taken verbatim, so it may contain spaces, start or end with
  spaces, or be empty.

| Line | Stored value |
|------|--------------|
| `SET city Frankfurt am Main` | `Frankfurt am Main` |
| `SET k  two` | ` two` (leading space kept) |
| `SET k ` | empty value |
| `SET k` | rejected: `ERR WRONG_ARGS` |

### 2.3 Keys and values

| | Rule | Error |
|-|------|-------|
| Key | 1–512 bytes, each in printable ASCII 0x21–0x7E (no space, no control bytes, no bytes ≥ 0x80) | `ERR INVALID_KEY` |
| Value | 0 or more bytes, any byte except CR and LF (NUL and bytes ≥ 0x80 are allowed) | CR → `ERR PROTOCOL` |
| Case | Keys are case-sensitive: `Key` and `key` are different keys | — |

A CR anywhere inside a line (other than the one before LF) is an error, including inside a `SET` value.
Values containing CR or LF cannot be stored in protocol v1; a binary-safe framing is a possible future version.

## 3. Responses

Every response is one line terminated by CRLF (`\r\n`).

| Response | Meaning |
|----------|---------|
| `PONG` | Reply to `PING` |
| `OK` | `SET` stored the value (inserted or replaced) |
| `VALUE <len> <bytes>` | `GET` found the key. `<len>` is the exact byte length of `<bytes>` in decimal. An empty value is `VALUE 0 ` (note the space). |
| `NIL` | `GET`: the key does not exist. Not an error. |
| `INT <n>` | Non-negative decimal integer |
| `BYE` | Reply to `QUIT`; the server then closes the connection |
| `ERR <CODE> <message>` | The request failed. `<CODE>` is for programs, `<message>` for humans. |

Clients should read `VALUE` responses by length: read `VALUE`, the length, one space, then exactly
`<len>` bytes, then `\r\n`. Because values may contain NUL bytes, a client must not treat the payload as a C string.
(A value cannot contain CR or LF in v1, but length-prefixing keeps the format ready for binary values.)

## 4. Commands

| Command | Response | Notes |
|---------|----------|-------|
| `PING` | `PONG` | Health check |
| `SET <key> <value>` | `OK` | Inserts or replaces |
| `GET <key>` | `VALUE <len> <bytes>` or `NIL` | |
| `DEL <key>` | `INT 1` if the key existed, `INT 0` otherwise | |
| `EXISTS <key>` | `INT 1` or `INT 0` | |
| `DBSIZE` | `INT <number of keys>` | |
| `QUIT` | `BYE`, then the connection closes | Requests sent after `QUIT` are not executed |

## 5. Errors

| Code | Cause | Connection |
|------|-------|------------|
| `UNKNOWN_COMMAND` | Command name not recognised (echoed, sanitised, max 32 bytes) | stays open |
| `WRONG_ARGS` | Wrong number of arguments | stays open |
| `INVALID_KEY` | Key breaks the rules in §2.3 | stays open |
| `PROTOCOL` | Empty token, or CR inside the line | stays open |
| `TOO_LARGE` | Line longer than 65 536 bytes | **closed** — the rest of the line may still be arriving, so the next request's start cannot be found reliably |
| `BUSY` | Connection limit reached: `ERR BUSY too many connections`, sent instead of serving the client | closed |
| `INTERNAL` | Unexpected server failure (e.g. out of memory) | closed |

A line is checked in this order, and the first failing check decides the error:
CR in line → empty command name → unknown command → empty tokens → argument count → key validity.
For example, `FLY\r` is `PROTOCOL`, `FLY  a` is `UNKNOWN_COMMAND`, `GET a b ` is `PROTOCOL`.

`TOO_LARGE` is reported as soon as the line can no longer fit: once 65 537 bytes have arrived without LF
(65 538 if the last one is CR, since `\r\n` may follow). The server does not wait for the end of an oversized line.

## 6. Examples

```
C: PING
S: PONG
C: SET greeting hello  world
S: OK
C: GET greeting
S: VALUE 12 hello  world
C: GET missing
S: NIL
C: EXISTS greeting
S: INT 1
C: DEL greeting
S: INT 1
C: DEL greeting
S: INT 0
C: DBSIZE
S: INT 0
C: get Greeting
S: NIL
C: GET  greeting
S: ERR PROTOCOL empty token (leading, trailing or repeated space)
C: FLY away
S: ERR UNKNOWN_COMMAND unknown command 'FLY'
C: QUIT
S: BYE
```

Pipelined (one write from the client, several responses):

```
C: SET a 1\nSET b 2\nDBSIZE\n
S: OK\r\nOK\r\nINT 2\r\n
```

## 7. Server-side limits

| Limit | Value | Behaviour |
|-------|-------|-----------|
| Request line | 65 536 bytes | `ERR TOO_LARGE`, connection closed |
| Key | 512 bytes | `ERR INVALID_KEY` |
| Connections | `--max-connections`, default 128 | `ERR BUSY`, connection closed |
| Pending responses per connection | ~64 KiB per batch | The server produces at most about one batch of responses before writing it out, and does not read new requests while a write is blocked. A client that pipelines requests but never reads the responses therefore stalls only its own connection, instead of making the server buffer unbounded output |

## 8. Durability

None yet. All data is in memory and is lost when the server stops. `OK` means "stored in memory".
Persistence arrives in Phase 6, with its guarantees documented there.
