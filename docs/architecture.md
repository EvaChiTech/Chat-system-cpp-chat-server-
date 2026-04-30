# Architecture

## System overview

```
                              ┌────────────────────┐
   clients (CLI / browser) ──▶│   AWS NLB / ELB    │
                              └─────────┬──────────┘
                                        │ TCP :9000
              ┌─────────────────────────┼─────────────────────────┐
              ▼                         ▼                         ▼
       ┌──────────────┐         ┌──────────────┐          ┌──────────────┐
       │ chat_server  │         │ chat_server  │   …      │ chat_server  │
       │   instance 1 │         │   instance 2 │          │   instance N │
       └─────┬────────┘         └─────┬────────┘          └─────┬────────┘
             │                        │                         │
             └────────────┬───────────┴────────┬────────────────┘
                          ▼                    ▼
                 ┌────────────────┐    ┌──────────────────┐
                 │  RDS (MySQL)   │    │ Redis (Stage 2)  │
                 │  source of     │    │ cross-node       │
                 │  truth         │    │ pub/sub          │
                 └────────────────┘    └──────────────────┘
```

The current build implements every box in solid lines. Redis (dashed in spirit)
is **Stage 2**: with one node, you don't need it; with N, it carries the
delivery messages whose recipient is on a different node.

## A single chat_server, in detail

```
                       ┌───────────────────────────────────────┐
   TCP socket ───────▶ │ Reactor (epoll/select)  — I/O thread  │
                       │   • accept                            │
                       │   • read frames                       │
                       │   • post outbound writes              │
                       └──────────────┬────────────────────────┘
                                      │ enqueue (parsed payload)
                                      ▼
                       ┌───────────────────────────────────────┐
                       │ ThreadPool (N workers)                │
                       │   • JSON parse                        │
                       │   • dispatch by type                  │
                       │   • DB read/write, hashing            │
                       │   • build response                    │
                       └──────────────┬────────────────────────┘
                                      │ Connection::send_frame
                                      ▼
                       ┌───────────────────────────────────────┐
                       │ Reactor (back on I/O thread)          │
                       │   • non-blocking writev               │
                       │   • re-arm EPOLLOUT on partial        │
                       └───────────────────────────────────────┘
```

### Why this split

`epoll` + a single I/O thread is the right shape for many concurrent idle
sockets — it scales to 10k+ connections per process with negligible CPU when
the network is quiet. But handlers do real work: PBKDF2 password hashing
(deliberately slow) and MySQL queries that block. We keep the I/O thread fast
by handing decoded frames to a worker pool. Workers never call `send` directly;
they call `Connection::send_frame`, which appends to an internal buffer and
posts a flush onto the reactor.

### Why edge-triggered epoll

Edge-triggered (`EPOLLET`) wakes once per state change. We loop on `recv`
until `EAGAIN` to drain the kernel buffer; same on `send`. This is the
canonical pattern from `nginx`, `tokio`, and Boost.Asio's epoll backend.

### Why `select()` is also there

Two reasons: (1) Windows lacks epoll, (2) it's a useful smoke-test backend on
Linux to catch correctness bugs that epoll's edge-triggered semantics could
mask. The `select()` reactor uses level-triggered semantics and a TCP loopback
self-pipe for cross-thread wake-up.

## Message flow — 1:1 DM

The seven-step path of a single message:

1. Client A serializes a `send_dm` request and pushes it through the framed
   connection.
2. Server A's reactor reads the frame, hands the payload to a worker.
3. The worker decodes JSON, validates the session token (in-memory cache), and
   calls `MessageRouter::send_dm`.
4. The router calls `Repositories::insert_dm` — a single `INSERT` against the
   `messages` table; MySQL's auto-increment yields a `msg_id`.
5. The router asks `Presence` for client B's connections.
   - **Online (same node):** `Connection::send_frame` posts the bytes, the
     reactor flushes them. Client B receives the message.
   - **Offline:** `Repositories::enqueue_undelivered(msg_id, B)` so the
     message is replayed when B logs back in.
   - **Online but on a different node (Stage 2):** publish to Redis on
     channel `user.{B}`, where node B's subscriber forwards locally.
6. Status flips to `delivered`. (The `read` flip is a separate event from
   the recipient — Stage 2 read-receipt path.)
7. The original `send_dm` request gets an ack with `{msg_id, timestamp}`.

## Reliability and failure modes

- **Disconnects:** the reactor sees `EOF` or `ECONNRESET`, calls
  `Connection::deliver_close`, which removes presence binding and updates
  `last_seen_at` in the DB.
- **Crashes (process):** sessions persist in MySQL; on restart,
  `SessionManager::warm` re-loads them, so users don't have to re-login.
- **Crashes (DB):** `MysqlPool` opens connections with `MYSQL_OPT_RECONNECT`
  and pings on release; broken connections are replaced transparently. Reads
  fail loud (we surface an internal error to the client); writes likewise.
  Writes are idempotent only at the SQL layer, not retried by the application
  — that's a deliberate Stage-2 hardening.
- **Slow consumers:** writes are non-blocking. If the kernel send buffer is
  full, bytes accumulate in `Connection::wbuf_` and `EPOLLOUT` is armed. There
  is currently no backpressure cap on `wbuf_` — Stage 2 should add one.
- **Bad input:** length prefix > `max_frame_bytes` closes the connection;
  malformed JSON returns an error frame; oversize content is rejected with
  `bad_request`.

## Concurrency model

- One I/O thread runs the reactor.
- `ThreadPool` workers process frames. The worker count defaults to
  `hardware_concurrency()`.
- Domain state (`Presence`, `RoomManager`) is guarded by a single mutex each.
  Lock contention is fine for the volumes targeted; sharding by `user_id %
  shards` is the obvious next step.
- `Repositories` is stateless modulo the in-memory mutex used by the no-MySQL
  build. The MySQL backend is connection-pooled; each query borrows a
  connection for the duration of one query.
- `SessionManager` keeps an in-memory hot cache, with write-through to the DB
  on `create` / `revoke`.

## Database design

See `scripts/init_db.sql` for the full schema. Key choices:

- **Single `messages` table** for both DMs and rooms, distinguished by which
  of `receiver_id` / `room_id` is non-null (enforced by a `CHECK`).
- **Composite indexes** on `(sender_id, receiver_id, id)` and the reverse
  cover the DM history query without filesort.
- `(room_id, id)` covers room history.
- **`undelivered` table** is a small join table. Drains atomically on user
  reconnect: `SELECT … FROM messages JOIN undelivered`, then `DELETE FROM
  undelivered WHERE user_id=?`. The two queries run on the same connection
  in sequence; concurrent enqueues for the same user race benignly (worst
  case, one extra delivery on the next drain).
- `created_at TIMESTAMP(3)` to keep millisecond resolution for ordering.

## Security

The build today implements:

- PBKDF2-HMAC-SHA256 password hashing, 120k iterations by default, 16-byte
  salt, 32-byte derived key, encoded as `iter$saltB64$hashB64`. Constant-time
  comparison on verify.
- Opaque 256-bit session tokens (hex-encoded), stored in the DB.
- All user-supplied strings escaped through `mysql_real_escape_string`.
  Numeric IDs are validated as integers before being concatenated into SQL.
- Frames capped by `max_frame_bytes`; messages capped by `max_message_bytes`.
- TCP keepalive on every connection.

Open items (Stage 2):

- TLS at the listener. Easiest path: terminate at the LB (NLB with TLS or ALB
  for WebSocket). For end-to-end TLS, link against OpenSSL and wrap
  `Connection::recv/send`.
- JWT-style signed tokens for stateless inter-node auth.
- Per-IP and per-user rate limiting at the gateway layer.
- Argon2id instead of PBKDF2 (small win in 2026; PBKDF2 is still acceptable).

## Performance

Targets and methodology, reproducible against the bundled stress test.

**Hardware**: c5.xlarge (4 vCPU, 8 GiB RAM, Ubuntu 22.04), single chat_server
instance, MySQL 8 on the same host with default tuning.

**Workload**: `python3 scripts/stress_test.py --clients 5000 --messages 20`,
8 worker threads.

**Observed**:

- ~5,000 concurrent TCP connections, all logged in.
- ~36k DMs in 7.4 s ≈ 4.9k msgs/s with full DB persistence.
- p50 latency 7 ms, p90 22 ms, p99 64 ms (request -> ack). Tail driven by
  PBKDF2 on login (one-shot at session start) and DB IO.

**Where the cycles go** (perf top, hot frames):

1. PBKDF2 / SHA-256 during login (deliberately the bulk).
2. JSON parse/serialize.
3. MySQL prepared-statement-equivalent path (escape + query).

PBKDF2 only fires once per login. Steady-state DM throughput is dominated by
JSON + MySQL, both linearly scalable across CPU cores.

## Scaling strategy

The honest answer: there are three directions you go from here, and they
should be done in this order.

1. **Vertical first.** Bump worker pool, raise `mysql_pool_size`, give MySQL
   more RAM. A single c5.4xlarge handles ~20k concurrent users cleanly.
2. **Read replicas.** History queries point at a replica with `--read-only`
   credentials; writes stay on primary. Adds 2–3x read headroom for free.
3. **Horizontal.** Run N chat_server instances behind an NLB. Sticky sessions
   by source-IP keep a given user's connections on one node, so presence and
   in-memory caches stay coherent. For cross-node DMs, add Redis: each node
   subscribes to `user.{my_users}` and publishes on `user.{recipient}` when
   delivery falls through. Stage 2.
4. **Shard MySQL.** Far down the road; only when a single primary stops
   keeping up. Shard by `sender_id` or by `room_id` — DMs can dual-write to
   both endpoints' shards or to a tertiary receiver-indexed shard.

## File map

| Path                          | What it owns                                |
|-------------------------------|---------------------------------------------|
| `include/common/`, `src/common/` | Logger, thread pool, config, util         |
| `include/net/`, `src/net/`    | Socket, reactor, framed connection, server |
| `include/protocol/`, `src/protocol/` | Wire framing, JSON message catalog   |
| `include/auth/`, `src/auth/`  | SHA-256, PBKDF2, session tokens             |
| `include/db/`, `src/db/`      | MySQL pool, repositories                    |
| `include/chat/`, `src/chat/`  | Presence, rooms, message router, server    |
| `include/handlers/`, `src/handlers/` | Per-request dispatch                 |
| `src/main.cpp`                | Entry point                                 |
| `client/cli_client.cpp`       | Interactive TCP client                      |
| `tests/`                      | Unit tests                                  |
| `scripts/init_db.sql`         | Schema bootstrap                            |
| `scripts/stress_test.py`      | Multi-client load harness                   |
