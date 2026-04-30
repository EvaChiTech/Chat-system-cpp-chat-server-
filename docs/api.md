# API Reference

Wire format: every frame is **`[4-byte big-endian length][JSON payload]`**.
The payload is a JSON object with at least a `"type"` field. Requests may set
a numeric `"id"`, which is echoed in the response.

There are three kinds of payloads:
- **Requests** (client → server) — have an `id`.
- **Responses** (server → client) — `type: "ack"` or `type: "error"`, with the
  request's `id` echoed.
- **Events** (server → client, unsolicited) — `type: "event_*"`, no `id`.

## Authentication model

Most requests need an authenticated session. There are two ways to provide it:

1. **Bound connection.** After `login`, the connection itself is bound to the
   user — subsequent requests on the same socket carry the identity
   automatically. This is the normal path.
2. **Token in the request.** Pass `"token": "<hex>"` on each request. The
   server validates and binds the connection on first use.

A bound connection is also bound to `Presence`, so the user shows online for
others.

---

## Requests

### `register`

Create a new account. Does **not** log in.

```json
{ "id": 1, "type": "register", "username": "alice", "password": "hunter2" }
```

Ack: `{ "type": "ack", "id": 1, "user_id": 7, "username": "alice" }`

Errors: `bad_request` (missing/short fields), `conflict` (username taken).

### `login`

```json
{ "id": 2, "type": "login", "username": "alice", "password": "hunter2" }
```

Ack: `{ "type": "ack", "id": 2, "user_id": 7, "username": "alice", "token": "<hex>", "ttl_s": 604800 }`

The `token` may be reused on a fresh connection.

Errors: `unauthorized`.

### `logout`

```json
{ "id": 3, "type": "logout" }
```

Revokes the current session token and unbinds presence.

### `send_dm`

```json
{ "id": 4, "type": "send_dm", "receiver_id": 12, "content": "hi!" }
```

Ack: `{ "type": "ack", "id": 4, "msg_id": 884, "timestamp": 1730000000123 }`

The recipient (and any other live sessions of the sender) receives an
`event_message`. If the recipient is offline, the message is queued and
replayed on their next login.

Errors: `unauthorized`, `bad_request`, `not_found` (no such receiver).

### `send_room`

```json
{ "id": 5, "type": "send_room", "room_id": 3, "content": "hello channel" }
```

Ack: `{ "type": "ack", "id": 5, "msg_id": 885, "timestamp": 1730000000456 }`

Fans out an `event_message` to every online room member (including other
sessions of the sender). Offline members get the message queued.

Errors: `unauthorized`, `bad_request` (not a member, or content too large).

### `create_room`

```json
{ "id": 6, "type": "create_room", "name": "general" }
```

Creates the room (idempotent: returns the existing room if the name is taken
and you'd be allowed to join), then auto-joins the caller.

Ack: `{ "type": "ack", "id": 6, "room_id": 3, "name": "general" }`

### `join_room`

```json
{ "id": 7, "type": "join_room", "room_id": 3 }
```

…or by name: `{"id": 7, "type": "join_room", "name": "general"}`.

Ack: `{ "type": "ack", "id": 7, "room_id": 3 }`

### `leave_room`

```json
{ "id": 8, "type": "leave_room", "room_id": 3 }
```

### `list_rooms`

```json
{ "id": 9, "type": "list_rooms" }
```

Ack:
```json
{
  "type": "ack", "id": 9,
  "rooms": [
    { "id": 3, "name": "general", "created_at": 1729900000000 }
  ]
}
```

### `history_dm`

```json
{ "id": 10, "type": "history_dm", "user_id": 12, "before_id": 0, "limit": 50 }
```

`before_id: 0` means "from the latest." Set it to the lowest `id` from the
previous page for cursor-based pagination. `limit` is capped at 200.

Ack:
```json
{
  "type": "ack", "id": 10,
  "messages": [
    { "id": 884, "sender_id": 7, "receiver_id": 12,
      "content": "hi!", "status": 1, "timestamp": 1730000000123 }
  ]
}
```

### `history_room`

```json
{ "id": 11, "type": "history_room", "room_id": 3, "before_id": 0, "limit": 50 }
```

Same shape as `history_dm`, with `room_id` instead of `receiver_id`.

### `presence`

```json
{ "id": 12, "type": "presence", "user_id": 12 }
```

Ack: `{ "type": "ack", "id": 12, "user_id": 12, "username": "bob", "online": true, "last_seen": 1729999999999 }`

### `ping`

```json
{ "id": 13, "type": "ping" }
```

Reply: `{ "type": "pong", "id": 13, "t": 1730000001234 }` — useful for
liveness checks and one-way RTT measurement.

---

## Server-pushed events

### `event_message`

Sent on a new DM or room message that involves this user.

```json
{
  "type": "event_message",
  "msg_id": 884,
  "sender_id": 7,
  "receiver_id": 12,        // present for DMs
  "room_id": 3,             // present for room messages
  "content": "hi!",
  "timestamp": 1730000000123
}
```

Exactly one of `receiver_id` / `room_id` is present.

### `event_presence` (Stage 2)

Sent when a followed user transitions online or offline.

```json
{
  "type": "event_presence",
  "user_id": 12,
  "username": "bob",
  "online": true,
  "last_seen": 1730000000000
}
```

### `event_delivered` (Stage 2)

Sent to the original sender when the recipient acknowledges receipt.

---

## Errors

```json
{ "type": "error", "id": <echoed>, "code": "<symbol>", "message": "<human>" }
```

| code              | when                                                |
|-------------------|-----------------------------------------------------|
| `bad_request`     | malformed JSON, missing fields, oversize content    |
| `unauthorized`    | missing/expired token, or wrong credentials         |
| `not_found`       | unknown user / room                                 |
| `conflict`        | unique-constraint violation (e.g. duplicate name)   |
| `rate_limited`    | (Stage 2) per-user / per-IP rate limit exceeded     |
| `internal_error`  | server-side failure                                 |

## Worked example: a complete session

The frame bytes are not shown; mentally prepend a 4-byte length prefix to each
JSON below.

```
client → { "id": 1, "type": "register", "username": "alice", "password": "p1" }
server → { "type": "ack", "id": 1, "user_id": 1, "username": "alice" }

client → { "id": 2, "type": "login", "username": "alice", "password": "p1" }
server → { "type": "ack", "id": 2, "user_id": 1, "username": "alice",
           "token": "abc...", "ttl_s": 604800 }

client → { "id": 3, "type": "send_dm", "receiver_id": 2, "content": "hey bob" }
server → { "type": "ack", "id": 3, "msg_id": 42, "timestamp": 1730000000000 }
        // and bob's connection receives:
        // { "type": "event_message", "msg_id": 42, "sender_id": 1,
        //   "receiver_id": 2, "content": "hey bob", "timestamp": 1730000000000 }
```
