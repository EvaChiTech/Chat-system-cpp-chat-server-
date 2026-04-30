# Deployment Guide

Two deploy paths are supported:

1. **Local Docker Compose** (one server + MySQL). Demo / dev path.
2. **AWS** (EC2 servers + RDS for MySQL + an NLB). Production path.

---

## 1. Local with Docker Compose

```bash
docker compose up --build
```

`docker-compose.yml` brings up `mysql:8.0` and the chat server, with the
init SQL applied automatically on first boot. The server listens on
`0.0.0.0:9000`. Tear down with `docker compose down -v` (the `-v` removes the
MySQL volume).

The compose file uses environment variables to override the config file at
runtime — every key in `config/server.conf` can be set this way (uppercased).

---

## 2. AWS production layout

This is the layout you'd describe in an interview.

```
                              Route 53
                                  │
                                  ▼
                        ┌────────────────────┐
                        │  Network LB (NLB)  │   :9000 TCP
                        │  cross-AZ          │
                        └─────────┬──────────┘
                                  │
            ┌─────────────────────┼─────────────────────┐
            ▼                     ▼                     ▼
      EC2 ASG (AZ-a)        EC2 ASG (AZ-b)        EC2 ASG (AZ-c)
      chat_server x2        chat_server x2        chat_server x2
            │                     │                     │
            └─────────────────────┼─────────────────────┘
                                  ▼
                        ┌────────────────────┐
                        │   RDS MySQL 8      │  primary + replica
                        │   Multi-AZ         │
                        └────────────────────┘
```

### Step-by-step

**VPC**

- One VPC, three private subnets (one per AZ) for EC2 + RDS, three public
  subnets for the NLB.
- A single NAT gateway for outbound traffic (e.g. pulling docker images).
  Cheaper than three; the loss of cross-AZ NAT is acceptable for an admin
  channel.

**Security groups**

- `sg-nlb`: ingress 9000/tcp from `0.0.0.0/0`. Egress to `sg-chat`.
- `sg-chat`: ingress 9000/tcp from `sg-nlb` only. Egress to `sg-rds:3306` and
  `0.0.0.0/0:443` (for image pulls / package mirrors).
- `sg-rds`: ingress 3306/tcp from `sg-chat`.

**RDS**

- MySQL 8.0, Multi-AZ enabled.
- `db.r6g.large` is plenty for v1 (~10k concurrent users). Bump to
  `db.r6g.xlarge` once `Threads_running` regularly exceeds 8.
- Parameters worth setting:
  - `max_connections >= 4 * (chat instances) * (mysql_pool_size)` plus
    headroom for admin tools.
  - `innodb_buffer_pool_size` ~70% of instance RAM (RDS default is fine).
  - `binlog_format = ROW` (default).
- Apply `scripts/init_db.sql` once (e.g. via a bastion or RDS Query Editor).

**EC2**

- AMI: Ubuntu 22.04 LTS.
- Instance: c6i.xlarge handles ~5k concurrent users with headroom.
- User data:

  ```bash
  #!/bin/bash
  apt-get update
  apt-get install -y docker.io
  systemctl enable --now docker
  docker login <your-ecr-registry>
  docker run -d --restart=always -p 9000:9000 \
    -e MYSQL_HOST=<rds-endpoint> \
    -e MYSQL_USER=chat \
    -e MYSQL_PASSWORD=<from-secrets-manager> \
    -e MYSQL_DATABASE=chat_system \
    -e LOG_LEVEL=info \
    <your-ecr-registry>/chat:latest
  ```

- Wrap that into an Auto Scaling Group with min=2, max=8. Scale on
  CPU > 60% for 5 minutes.

**NLB (not ALB)**

- We need TCP, not HTTP. Use a Network Load Balancer.
- Listener: `TCP :9000` → target group on instance port 9000.
- Health check: TCP on 9000 (or, better, a future `/healthz` if we add a
  control plane). Healthy threshold 2, unhealthy threshold 2, interval 10s.
- **Sticky sessions**: enable source-IP stickiness. This keeps a user on the
  same node so presence + in-memory caches stay coherent. Without
  stickiness, the Stage-2 Redis fan-out is required for cross-node delivery.

**TLS**

- Easiest: terminate at the NLB with an ACM cert. The chat server itself
  remains plaintext TCP inside the VPC.
- For end-to-end: link the server against OpenSSL and wrap
  `Connection::recv`/`send`. That's a Stage-2 task.

**Secrets**

- DB password in Secrets Manager. The user-data script reads it at boot via
  the EC2 IAM role.

**Logs and metrics**

- The server logs JSON-ish lines to stderr. Run docker with the awslogs
  driver to ship them to CloudWatch Logs. Logging structured fields (in v1
  we just structure the prefix, not the message body) makes a future Athena
  query story trivial.
- Metrics worth scraping: connection_count, frames_per_sec, ack_p99_ms,
  mysql_pool_idle. Not implemented yet — Stage 2.

---

## Operational runbook

### Deploys

The chat server has no client-affinity beyond a single TCP connection's
lifetime. To deploy a new image:

1. Roll one instance at a time out of the target group.
2. Wait for it to drain (NLB drain timer, 30s default).
3. Replace and re-register.
4. Repeat.

A connection severed mid-deploy reconnects to a different node and replays
any queued messages on login — no data loss.

### Scaling

- More users? Add EC2 instances. Sticky-source-IP keeps them sane.
- More messages per user? Bump RDS instance class first; consider read
  replicas for history queries.
- Latency spikes? Check `mysql_pool_idle == 0` (pool exhausted) and
  `Threads_running` on RDS. If both are pegged, raise `mysql_pool_size`
  *and* `max_connections` together.

### Backups

- RDS automated backups: enabled, 14-day retention.
- Restoring is a clean operation: spin a new RDS, point the chat instances
  at the new endpoint via SSM parameter store, deploy.

### Failure drills (good interview material)

1. **Kill MySQL primary.** RDS Multi-AZ flips DNS to the standby in ~60s.
   Chat servers' pools see broken connections, replace them on next acquire.
2. **Kill a chat instance.** ASG restarts. NLB target health drains the
   dead one. Connected clients reconnect to a different node, replay
   offline messages.
3. **Network partition between an AZ and RDS.** Servers in that AZ get
   `2003 Can't connect to MySQL`. Health checks fail, NLB stops sending
   traffic, ASG replaces them once the partition resolves.

---

## Single-VM deploy (no AWS)

If you don't have AWS, the same Docker image runs on any Linux VM:

```bash
# install docker + docker-compose plugin
sudo apt-get install -y docker.io docker-compose-plugin

git clone <repo> chat-system && cd chat-system
docker compose up -d --build
```

Open port 9000 in the VM's firewall and you're live. For TLS, put it behind
an `nginx` reverse-proxy doing TCP `stream` termination, or use
`stunnel`.
