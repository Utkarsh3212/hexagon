# Hexagon

Hexagon is a high-performance, in-memory data server written in C++17. It
combines nonblocking network I/O, predictable-latency data structures, typed
binary serialization, automatic expiration, and asynchronous cleanup in a
compact systems-oriented architecture.

## Features

- **Nonblocking event loop** built on `poll()` with partial read/write handling,
  connection multiplexing, response buffering, and request pipelining.
- **Progressively resized hash table** with intrusive chaining, O(1) average
  lookup, stable node addresses, and bounded migration work per operation.
- **Typed binary protocol** supporting nil, errors, byte strings, signed 64-bit
  integers, doubles, and recursively nested arrays.
- **Ordered collections** indexed simultaneously by member name and by the
  `(score, name)` tuple.
- **Augmented AVL tree** providing O(log N) insertion, deletion, lookup, and
  rank-based offsets regardless of offset size.
- **TTL expiration** backed by an indexed minimum heap for O(1) access to the
  next deadline and O(log N) updates or removals.
- **Idle connection management** using monotonic time and an intrusive timer
  list; inactive clients are closed after five seconds.
- **Worker thread pool** for moving expensive collection destruction out of the
  event loop. Collections with more than 1,000 members are unlinked immediately
  and reclaimed asynchronously.
- **Bounded maintenance work** for both hash-table migration and mass expiration
  so housekeeping cannot monopolize the event loop.
- **Portable POSIX implementation** that builds on Linux and macOS.

## Architecture

| Layer | Implementation | Key property |
| --- | --- | --- |
| Networking | Nonblocking sockets and `poll()` | Concurrent connections without one thread per client |
| Request processing | Length-prefixed pipelined messages | Handles fragmented and coalesced network reads |
| Primary index | Intrusive chaining hash table | O(1) average access with progressive resizing |
| Ordered index | Height-balanced AVL tree | O(log N) operations and rank offsets |
| Ordered collection | Hash index plus AVL index | Fast member lookup and deterministic score ordering |
| Expiration | Indexed minimum heap | Efficient arbitrary deadlines and direct removal |
| Idle timers | Intrusive doubly-linked list | O(1) connection timer refresh |
| Deferred cleanup | Four-worker pthread pool | Keeps large destructors off the event-loop thread |
| Serialization | Typed TLV values | Unambiguous scalar and nested response types |

## Build

Requirements:

- C++17 compiler
- POSIX sockets and pthreads
- Make
- Python 3 for integration tests

```sh
make
```

The build creates two executables:

- `server` — the data server
- `client` — a command-line protocol client

## Run

The server listens on port `2203` by default. Pass a port as the first argument
to use a different one.

```sh
./server
./server 22303
```

Send commands with the client:

```sh
./client set greeting hello
./client get greeting
./client --port 22303 dbsize
```

## Command reference

| Command | Result |
| --- | --- |
| `get key` | String value or nil |
| `set key value` | Nil on success; type error if the key contains an ordered collection |
| `del key` | `1` if removed, otherwise `0` |
| `dbsize` | Number of top-level keys |
| `keys` | Unordered array of all top-level keys |
| `pexpire key milliseconds` | `1` if the key exists, otherwise `0` |
| `pttl key` | Milliseconds remaining, `-1` without a deadline, or `-2` if absent |
| `persist key` | `1` if a deadline was removed, otherwise `0` |
| `zadd key score member` | `1` for a new member, `0` for a score update |
| `zrem key member` | `1` if removed, otherwise `0` |
| `zscore key member` | Double score or nil |
| `zquery key score member offset limit` | Flat member/score array beginning at the first tuple greater than or equal to `(score, member)` |

### Key/value operations

```sh
./client set user:42 active
./client get user:42
./client dbsize
./client keys
./client del user:42
```

### Ordered collections

```sh
./client zadd leaderboard 10 alice
./client zadd leaderboard 20 bob
./client zscore leaderboard alice
./client zquery leaderboard 0 "" 0 10
./client zrem leaderboard alice
```

Members are ordered by score first and name second, which produces stable
ordering when multiple members have the same score. `zquery` supports efficient
positive or negative rank offsets without walking through every skipped member.

### Expiration

```sh
./client set session:123 token
./client pexpire session:123 5000
./client pttl session:123
./client persist session:123
```

Deadlines use a monotonic clock, so wall-clock adjustments do not affect
existing timers. A negative `pexpire` duration removes the deadline; `persist`
provides the explicit equivalent.

## Wire protocol

Every request begins with a 4-byte body length. The body contains an argument
count followed by length-prefixed byte strings:

```text
u32 body_length
u32 argument_count
repeat argument_count times:
    u32 argument_length
    byte[argument_length] argument
```

Multiple complete requests can be written without waiting for their responses.
The server parses all available frames and returns responses in request order.

Responses also begin with a 4-byte body length. The body contains one typed TLV
value:

| Tag | Value | Encoding |
| --- | --- | --- |
| `0` | Nil | Tag only |
| `1` | Error | Tag, `u32 code`, `u32 length`, bytes |
| `2` | String | Tag, `u32 length`, bytes |
| `3` | Integer | Tag, `i64` |
| `4` | Double | Tag, IEEE-754 64-bit value |
| `5` | Array | Tag, `u32 element_count`, recursively encoded values |

Fixed-width values currently use host byte order, so communicating processes
must use the same endianness.

## Project layout

| Path | Purpose |
| --- | --- |
| `server.cpp` | Event loop, protocol handling, commands, timers, and database state |
| `client.cpp` | Command encoding and recursive response decoding |
| `hashtable.*` | Progressive intrusive hash table |
| `avl.*` | Height-balanced, rank-augmented binary tree |
| `zset.*` | Dual-index ordered collection |
| `heap.*` | Indexed minimum heap for expiration deadlines |
| `list.h` | Intrusive circular doubly-linked list |
| `thread_pool.*` | Pthread task queue and worker lifecycle |
| `tests/` | Data-structure and end-to-end protocol tests |

## Testing

```sh
make unit-test
make integration-test
make test
```

The unit suite validates hash-table growth, AVL invariants and offsets, ordered
collection indexing, heap back-references, and worker execution. The integration
suite covers command behavior, typed serialization, request pipelining,
expiration, idle timeouts, progressive rehashing, and asynchronous destruction
of a 1,001-member collection.
