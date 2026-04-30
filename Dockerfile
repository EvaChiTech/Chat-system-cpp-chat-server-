# ---- builder ------------------------------------------------------------
FROM ubuntu:22.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates libmysqlclient-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)" --target chat_server cli_client

# ---- runtime ------------------------------------------------------------
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      libmysqlclient21 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -s /usr/sbin/nologin chat

WORKDIR /app
COPY --from=builder /src/build/chat_server /app/chat_server
COPY --from=builder /src/build/cli_client  /app/cli_client
COPY config/server.conf.example /app/server.conf

USER chat
EXPOSE 9000
ENTRYPOINT ["/app/chat_server", "--config", "/app/server.conf"]
