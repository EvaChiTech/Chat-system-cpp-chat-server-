#!/usr/bin/env python3
"""
End-to-end stress test for chat_server.

Spawns N concurrent TCP clients, registers/logs in each, and has each client
send M messages to a randomly-chosen peer. Reports throughput and per-message
latency percentiles.

usage:
  python3 stress_test.py --host 127.0.0.1 --port 9000 \
                         --clients 500 --messages 20

Requires Python 3.8+. No third-party dependencies.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import random
import struct
import string
import time
from typing import List


def pack(payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + payload


async def read_frame(reader: asyncio.StreamReader) -> bytes:
    hdr = await reader.readexactly(4)
    (length,) = struct.unpack(">I", hdr)
    return await reader.readexactly(length)


async def request(reader, writer, payload: dict, seq: int) -> dict:
    payload = {**payload, "id": seq}
    writer.write(pack(json.dumps(payload).encode()))
    await writer.drain()
    while True:
        frame = await read_frame(reader)
        msg = json.loads(frame)
        # Skip server-pushed events; wait for matching ack/error.
        if msg.get("id") == seq:
            return msg


def rand_word(n: int = 8) -> str:
    return "".join(random.choices(string.ascii_lowercase + string.digits, k=n))


async def one_client(idx: int, host: str, port: int, peers: list, msgs: int,
                     latencies: List[float]):
    reader, writer = await asyncio.open_connection(host, port)
    seq = 0

    user = f"u{idx}_{rand_word(4)}"
    pw = "pw_" + rand_word(8)

    seq += 1
    await request(reader, writer, {"type": "register", "username": user, "password": pw}, seq)
    seq += 1
    login_resp = await request(reader, writer, {"type": "login", "username": user, "password": pw}, seq)
    if login_resp.get("type") != "ack":
        print(f"[client {idx}] login failed: {login_resp}")
        writer.close()
        return
    my_uid = login_resp["user_id"]
    peers.append(my_uid)

    # Wait until everyone has registered, then send.
    while len(peers) < min(20, msgs):
        await asyncio.sleep(0.01)

    for _ in range(msgs):
        target = random.choice(peers)
        if target == my_uid:
            continue
        seq += 1
        body = {"type": "send_dm", "receiver_id": target, "content": "hi " + rand_word(20)}
        t0 = time.perf_counter()
        resp = await request(reader, writer, body, seq)
        dt = (time.perf_counter() - t0) * 1000.0
        if resp.get("type") == "ack":
            latencies.append(dt)

    writer.close()
    await writer.wait_closed()


def percentile(xs: List[float], p: float) -> float:
    if not xs: return 0.0
    s = sorted(xs)
    k = max(0, min(len(s) - 1, int(round(p * (len(s) - 1)))))
    return s[k]


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--clients", type=int, default=200)
    ap.add_argument("--messages", type=int, default=10)
    args = ap.parse_args()

    peers: list = []
    latencies: List[float] = []

    t0 = time.perf_counter()
    tasks = [
        asyncio.create_task(
            one_client(i, args.host, args.port, peers, args.messages, latencies))
        for i in range(args.clients)
    ]
    await asyncio.gather(*tasks, return_exceptions=True)
    total_s = time.perf_counter() - t0

    n = len(latencies)
    if n == 0:
        print("no successful sends")
        return
    rps = n / max(total_s, 1e-6)
    print(f"clients={args.clients}  total_msgs={n}  wall={total_s:.2f}s  rps={rps:.1f}")
    print(f"latency_ms p50={percentile(latencies,0.5):.2f} "
          f"p90={percentile(latencies,0.9):.2f} "
          f"p99={percentile(latencies,0.99):.2f} "
          f"max={max(latencies):.2f}")


if __name__ == "__main__":
    asyncio.run(main())
