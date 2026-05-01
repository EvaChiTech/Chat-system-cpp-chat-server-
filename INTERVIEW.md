# Interview Talk-Track

Personal prep notes for talking about this project in a screening, technical
interview, or senior-engineer deep-dive. Internalize, don't memorize.

---

## 1. The 30-second elevator pitch

> "It's a low-latency real-time chat backend in modern C++. Think the server
> half of WhatsApp or Discord at a smaller scale: TCP with length-prefixed
> JSON, epoll-based event loop on Linux, a worker thread pool, MySQL for
> persistence, presence tracking with multi-device support, group rooms, and
> an offline-message queue. About 5,400 lines, fully tested, builds on Linux
> and Windows in CI, deploys via Docker Compose, with an AWS layout
> documented for production. The architecture is the same shape you'd use
> for the real thing — single-threaded reactor for I/O, worker pool for
> handlers — and I called out the Stage-2 work explicitly so the next
> commits write themselves."

Don't say "I built a chat app." That's a Twilio tutorial. Say "real-time
backend" and reference the specific design decisions.

---

## 2. The 5-minute whiteboard walk-through

When asked "walk me through your design," draw this exact picture:

Client ──TCP──> [ NLB ] ──> [ chat_server ] ──> [ MySQL RDS ]
│
├─ Reactor (epoll)   ── I/O thread
├─ ThreadPool (N)    ── workers
└─ Presence + Rooms  ── in-memory state

Then narrate the five-step flow of a single DM (memorize this — it's the
question you're guaranteed to get):

1. **Client A frames a `send_dm` request** — 4-byte big-endian length, then
   a JSON payload — and writes it to its TCP socket.
2. **The reactor's I/O thread reads** the bytes via non-blocking `recv` until
   `EAGAIN`, parses one or more complete frames out of the per-connection
   read buffer, and **hands each frame to the thread pool**. The I/O thread
   never blocks on application work.
3. **A worker thread parses the JSON**, validates the session token (in-memory
   cache, write-through to DB), looks up the receiver in the `users` table,
   and calls `MessageRouter::send_dm`.
4. **The router persists the message** with a single `INSERT INTO messages`,
   gets a `msg_id` back from MySQL's auto-increment, then asks `Presence`
   for the receiver's connections.
5. **If the receiver is online**, `Connection::send_frame` enqueues the
   bytes and posts a flush onto the reactor — the reactor drains the write
   buffer back on the I/O thread. **If offline**, the message is enqueued
   in the `undelivered` table and replayed on the receiver's next login.

Time budget: 5 minutes for that whole thing. Stop after step 5 unless they
ask follow-ups.

---

## 3. Canonical questions and answers

### "Why epoll instead of IOCP, Asio, or libuv?"

> "Three reasons. First, epoll is the right primitive for the
> high-concurrency Linux case — it scales to tens of thousands of idle
> sockets with negligible CPU. Second, building on the primitive directly
> rather than wrapping it forced me to actually understand the
> edge-triggered semantics, the EPOLLRDHUP cases, and the wake-up pattern.
> Third, the project has a `select()` fallback for Windows, so you can read
> both backends side-by-side and see what the abstraction layer would
> normally hide. If I were building production today, I'd reach for Asio —
> but for a portfolio project, the educational value of writing it yourself
> is the point."

### "What breaks first under load?"

> "The MySQL connection pool. With eight workers and a four-connection pool,
> if any DB query is slow you start blocking. The fix is to bump
> `mysql_pool_size` and `max_connections` in lockstep. Past that, the next
> thing is single-mutex contention on `Presence` and `RoomManager`. I'd
> shard those by `user_id % shards` — straightforward refactor, doubles the
> ceiling. Then per-connection write buffer growth on slow consumers, which
> needs a backpressure cap (currently unbounded — that's a known
> Stage-2 hardening)."

### "What's the worst bug or limitation in your code?"

> "There's a race in the offline-queue path. If sender A sends a DM at the
> same instant the receiver B disconnects, A's worker thread might call
> `Presence::connections_of(B)` *before* B's TCP FIN has been processed.
> The result is the message gets sent to B's already-dead connection,
> marked `delivered`, and silently lost — it doesn't end up in the
> `undelivered` table. The robust fix is to make delivery durable: every
> message goes into a per-user inbox, and 'online' is just an optimization
> for pushing bytes. I documented this race in `architecture.md` so it
> wasn't hidden. For a real production system this is a Stage-2 must-fix."

> If they push: "The pragmatic patch is an ack-and-retry from the recipient.
> Server marks delivered only after recipient ack; if no ack within timeout,
> re-enqueue. That's also Stage 2."

### "How does horizontal scaling actually work?"

> "Today, sticky sessions on the load balancer keep all of a given user's
> connections on one node, so presence and the in-memory caches stay
> coherent. Cross-node DMs aren't implemented — that's where Redis pub/sub
> comes in. Each chat_server instance subscribes to `user.{my_users}` and
> publishes on `user.{recipient}` when delivery falls through. About 200
> lines of code with a hiredis client. Sticky-session-only is fine up to
> ~20k concurrent users on a single instance class; past that, the Redis
> path is mandatory. The Redis cluster itself scales independently."

### "Why C++ for this and not Go or Rust?"

> "Honest answer: the project description called for it, and that's a
> choice you'd make in real life if you have an existing C++ codebase or if
> you're targeting platforms where Go's runtime overhead matters — game
> servers, voice chat, low-level networking. For a greenfield chat backend
> in 2026, I'd reach for Go: similar epoll-based runtime, a fraction of the
> code, and you don't have to write your own JSON parser. Rust gets you the
> same with stronger correctness guarantees but a steeper learning curve.
> The C++ here was a deliberate exercise in showing I can work close to
> the metal."

### "Why MySQL and not Postgres or Redis?"

> "MySQL is the default in the original spec and most companies still ship
> on it. Postgres would be a strict upgrade — better default tuning,
> richer types, JSONB if I wanted indexed JSON, real `COMMENT ON COLUMN`
> support — and the schema would port in an afternoon. Redis isn't a
> persistence layer here, it's the cross-node fan-out. You wouldn't use it
> as the source of truth for messages because it's not durable enough."

### "How do you handle malformed input?"

> "Length-prefixed framing makes oversize and zero-length frames trivial to
> reject — closes the connection. JSON parse failures return a structured
> `error` reply but keep the connection alive. Every user-supplied string
> goes through `mysql_real_escape_string` before SQL concatenation. There
> is no `eval` or template-string code path, so injection has to come
> through the SQL layer or not at all. Numeric IDs are typed as `int64`
> on parse, so they can't carry SQL. Frames are capped by
> `max_frame_bytes`; messages by `max_message_bytes`."

### "Walk me through how authentication works."

> "Register hashes the password with PBKDF2-HMAC-SHA256, 120k iterations,
> 16-byte salt, 32-byte derived key — formatted as `iter$saltB64$hashB64`
> for storage. Login verifies via constant-time compare. Successful login
> mints a 256-bit random session token, stored hex-encoded in the
> `sessions` table and in an in-memory cache. Subsequent requests on the
> same connection auto-bind the user from the cache; new connections pass
> the token explicitly. The token is opaque — no JWT — which means
> revocation is just a DELETE. Stage 2 would be to switch to JWT for
> stateless inter-node auth once Redis is in the picture."

### "What testing did you do?"

> "Unit tests for the pure-logic pieces — framing, thread pool, in-memory
> repositories, presence transitions, room cache, plus a SHA-256
> known-answer test from RFC 6234 because if your hash is broken nothing
> else is going to work. CI runs them on every push for Linux and Windows.
> Plus an end-to-end smoke test in the workflow that boots the binary,
> registers, logs in, pings, and tears down. Plus a Python stress harness
> for load testing — 5,000 concurrent clients, 20 messages each. I haven't
> built integration tests against a real MySQL because spinning one up in
> CI per push isn't worth the time, but `docker compose up` is the manual
> integration test."

### "What would you do differently with hindsight?"

> "Three things. **One**, durable delivery from day one. The race I
> mentioned wouldn't exist if the offline queue had been the canonical
> path. **Two**, prepared statements instead of escaped string
> concatenation in the MySQL repos. The current approach is correct but
> brittle — every new query is a chance to forget the escape. **Three**,
> structured logging from day one — JSON lines with stable field names —
> rather than the format-string approach. Future-me would thank past-me
> when grepping CloudWatch."

### "How long did this take?"

(Adjust to your actual story.)

> "End-to-end, about [X] days of evening work. The architecture took an
> afternoon to lay out — most of the time was the unsexy parts: the MySQL
> repository methods, the Windows compatibility shim, getting CI working
> on three platforms, debugging a header-typedef conflict on Ubuntu 22.04
> that didn't show up locally."

---

## 4. Numbers to cite

If asked for performance, give specifics — never wave at "fast."

- ~5,400 lines of C++17, 21 .cpp + 18 .h files
- 14 unit tests, runs in ~50ms locally
- CI: 3 jobs (Linux × 2, Windows), ~1m 10s wall time
- Stress harness target on a c5.xlarge: **5k concurrent clients, p50 < 8 ms,
  p99 < 70 ms** for 1:1 DM round-trip, with full DB persistence
- Memory: ~50 KB per connection in steady state

If you haven't actually run those numbers on real hardware, **say so**:
"Those are the targets in the architecture doc. I haven't yet measured
them on production-class hardware."

---

## 5. Questions you'll probably get and shouldn't blank on

- **"What's a thread pool?"** Fixed-size worker count, FIFO task queue
  guarded by a mutex, condition variable for wake-up. Tasks are
  `std::function<void()>`. Mine has graceful shutdown — drain queue, then
  join. Standard implementation.

- **"What's edge-triggered epoll?"** The kernel signals you once when a
  file descriptor transitions to a readable/writable state. You then loop
  reading or writing until EAGAIN. Level-triggered (the older mode) keeps
  signaling as long as the FD stays in that state, which is simpler to use
  but generates more wake-ups under load.

- **"What's a connection pool and why use one?"** A bounded set of
  pre-opened DB connections that workers borrow per query and return on
  completion. Avoids the TCP+TLS+auth handshake on every request — which
  is otherwise the dominant cost of a single short query. Mine uses a
  blocking queue with a condvar.

- **"What's PBKDF2?"** Password-based key derivation. You repeat HMAC of
  password+salt thousands of times to make brute-forcing the password
  expensive. The output is your derived key, which you store. On verify
  you re-derive with the same params and constant-time compare. Argon2id
  is the modern replacement; PBKDF2 is still acceptable for v1.

- **"How would you add file uploads?"** Out-of-band: an HTTP endpoint
  accepts the upload, returns a URL, the chat message references that URL.
  Don't push file bytes through the chat protocol — different scaling
  shape entirely. (S3 + signed URLs is the obvious AWS pattern.)

- **"What about end-to-end encryption?"** Different threat model. Today
  the server can read every message in the clear. E2E means clients hold
  long-lived keypairs, the server is just a relay for ciphertext, and
  presence becomes the only metadata you have. Signal Protocol or MLS for
  group chat — not something you'd retrofit casually.

---

## 6. Lines that land well

- **"I called the Stage-2 work out explicitly so I wasn't hiding it."** —
  Shows self-awareness without being apologetic.
- **"The honest answer is..."** — When acknowledging a trade-off. Sounds
  human, not defensive.
- **"I'd reach for Asio if I were building production, but the point of
  this project was..."** — Shows you know the tooling, you chose otherwise
  for a reason.
- **"That's exactly the case the offline queue was built to handle, but
  there's a race I haven't fully closed yet."** — Demonstrates depth on a
  specific known limitation.

---

## 7. Anti-patterns to avoid

- **Don't say "production-ready"** unless you've run it in production.
  Say "production-shaped" or "the architecture you'd use in production."
- **Don't read from your README aloud.** They have it. Tell them what's
  not in it.
- **Don't dismiss your bugs.** "It's just a v1" is weaker than "here's the
  specific limitation, here's exactly how I'd fix it."
- **Don't claim numbers you haven't measured.** Interviewers can smell it.
- **Don't overclaim Stage 2.** Say "I haven't built X yet, but here's the
  shape it would take in ~Y lines" — concrete, honest.

---

## 8. The five-minute closer

If they ask "anything else you want me to know," say something like:

> "The thing I'm most proud of isn't the code — it's that the
> documentation matches reality. The architecture doc explains the design,
> the API reference explains every wire-level detail, and the deployment
> guide is specific enough to actually deploy. I've been on the receiving
> end of portfolio repos where the README claims things the code doesn't
> support. I deliberately didn't do that. What's marked Stage 2 is genuinely
> Stage 2."

That's a hireable closing line. Use it.
