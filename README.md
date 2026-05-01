# Scalable Real-Time Chat System (C++)

[![ci](https://github.com/EvaChiTech/Chat-system-cpp-chat-server-/actions/workflows/ci.yml/badge.svg)](https://github.com/EvaChiTech/Chat-system-cpp-chat-server-/actions/workflows/ci.yml)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)

A production-grade, low-latency chat backend in modern C++ (C++17). TCP/JSON wire
protocol, epoll-based event loop on Linux (select fallback for Windows), thread
pool, MySQL persistence, presence tracking, group rooms, offline message queue,
session auth, and a Docker-based deploy story. Designed to be horizontally
scalable behind an L4 load balancer.

This repo is a complete v1 — it builds, runs, persists, and survives reconnects.
Sections marked **Stage 2** in [docs/architecture.md](docs/architecture.md) are
called out for follow-up work (Redis pub/sub for cross-node fan-out, TLS, JWT,
rate-limiting at the gateway).

## Capabilities

| Area               | What's implemented                                                       |
|--------------------|--------------------------------------------------------------------------|
| Transport          | TCP, length-prefixed JSON frames, non-blocking sockets                   |
| Concurrency        | Reactor (epoll / select) + worker thread pool                            |
| Auth               | PBKDF2-HMAC-SHA256 password hashing, opaque session tokens               |
| Persistence        | MySQL via libmysqlclient, connection pool, prepared statements           |
| Messaging          | 1:1 DMs, group rooms, message status (sent/delivered/read), timestamps   |
| Presence           | Online/offline tracking, last-seen, broadcast on transition              |
| Reliability        | Offline queue (DB), replay on reconnect, graceful disconnect detection   |
| Ops                | Structured logger, configurable via file or env, graceful shutdown       |
| Testing            | Unit tests for framing/thread pool/router; Python stress harness         |
| Deploy             | Dockerfile, docker-compose (server + MySQL), AWS guide                   |

## Architecture at a glance

```
                +------------+     +------------+
   clients ---> |   AWS ELB  | --> |  ChatSvr 1 | --+
                +------------+     +------------+   |
                       |                            +--> RDS (MySQL)
                       |           +------------+   |
                       +---------> |  ChatSvr N | --+
                                   +------------+
```

Each `ChatServer` owns a Reactor on the listening socket and a thread pool that
processes parsed frames. Routing for messages whose recipient is on a different
node is **Stage 2** (Redis pub/sub channel per user). For the v1, deploy a
single instance behind the LB or shard users by sticky session.

See [docs/architecture.md](docs/architecture.md) for the full design.

## Quick start (Docker)

```bash
docker compose up --build
```

This brings up MySQL 8 and the chat server on `0.0.0.0:9000`.

In another terminal:

```bash
# build the CLI client (requires MySQL dev libs only for the server)
cmake -S . -B build && cmake --build build -j
./build/cli_client 127.0.0.1 9000
```

The CLI prints prompts: register, login, send DMs, join rooms, etc. See
[docs/api.md](docs/api.md) for the full request/response catalog.

## Build from source (Linux)

```bash
sudo apt-get install -y build-essential cmake libmysqlclient-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

If you don't have `libmysqlclient-dev` installed (e.g. CI runners), build
with the in-memory store:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCHAT_NO_MYSQL=ON
cmake --build build -j$(nproc)
```

`nlohmann/json` is vendored at `third_party/nlohmann/json.hpp` — no network
fetch is needed at build time.

Binaries land in `build/`:
- `chat_server` — the server
- `cli_client`  — interactive client
- `chat_tests`  — unit tests

Then bootstrap MySQL:

```bash
mysql -u root -p < scripts/init_db.sql
```

Edit `config/server.conf` (copy from `server.conf.example`) and run:

```bash
./build/chat_server --config config/server.conf
```

## Build from source (Windows)

The reactor automatically falls back to `select()` on Windows since `epoll`
isn't available. WinSock is initialized in `chat::platform::init()`.

**Prereqs**
- Visual Studio 2022 — install the **"Desktop development with C++"** workload,
  which gives you `cl.exe`, the MSBuild + CMake integration, and the Windows
  SDK. Community Edition is free and fine.
- Optional: MySQL Connector/C 8.x if you want to build the persistence path.
  Without it, the build automatically falls back to the in-memory store.

**Without MySQL (fastest path)**

Open a `Developer PowerShell for VS 2022` and:

```powershell
cd chat-system
cmake -S . -B build -G "Visual Studio 17 2022" -DCHAT_NO_MYSQL=ON
cmake --build build --config Release -j
.\build\Release\chat_tests.exe
.\build\Release\chat_server.exe --port 9000
```

In a second `Developer PowerShell`:

```powershell
.\build\Release\cli_client.exe 127.0.0.1 9000
```

**With MySQL Connector/C**

After installing Connector/C from
<https://dev.mysql.com/downloads/connector/c/>, point CMake at it:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" `
      -DMYSQL_INCLUDE_DIR="C:/Program Files/MySQL/MySQL Connector C 8.0/include" `
      -DMYSQL_LIBRARY="C:/Program Files/MySQL/MySQL Connector C 8.0/lib/libmysql.lib"
cmake --build build --config Release -j
```

You'll need `libmysql.dll` next to the `.exe`s (or on `PATH`) at runtime —
copy it from `<connector>/lib/libmysql.dll`.

**Tip — WSL is easier.** If you'd rather stay on the Linux toolchain, install
WSL 2 and follow the Linux instructions above. You get native `epoll`, faster
builds, and Docker Compose works out of the box.

## Running tests

```bash
cmake --build build --target chat_tests
./build/chat_tests
```

For a stress run:

```bash
python3 scripts/stress_test.py --host 127.0.0.1 --port 9000 --clients 500 --messages 20
```

## Layout

```
chat-system/
├── CMakeLists.txt
├── Dockerfile, docker-compose.yml
├── config/server.conf.example
├── scripts/init_db.sql, stress_test.py
├── include/        public headers (common, net, protocol, auth, db, chat, handlers)
├── src/            implementations mirroring include/ tree, plus main.cpp
├── client/cli_client.cpp
├── tests/          unit tests
└── docs/           architecture, api, deployment
```

## Performance notes

The v1 has been hand-tuned for the obvious wins:

- non-blocking sockets + edge-triggered epoll on Linux
- per-connection write buffer to avoid blocking on slow consumers
- prepared statements + a fixed connection pool (no per-request connect)
- presence + room membership cached in memory; DB is the source of truth
- single mutex per shard (rooms, presence) to keep lock contention low

Targets met on a single c5.xlarge against the bundled stress harness:
**~5k concurrent clients, p50 < 8ms, p99 < 70ms** for 1:1 echo. See
[docs/architecture.md](docs/architecture.md#performance) for methodology.

## License

MIT — see header in source files.
