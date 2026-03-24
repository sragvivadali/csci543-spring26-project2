# Build PostgreSQL from the vendored `postgres/` tree (your fork for learned optimizer work).
#
# Dev (live ./postgres mount, recompile on start):  docker compose build && docker compose watch
# Frozen server (source copied at image build):     docker compose build --target runtime

FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    pkg-config \
    flex \
    bison \
    perl \
    libreadline-dev \
    zlib1g-dev \
    libssl-dev \
    libicu-dev \
    libxml2-dev \
    libxslt1-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY postgres /build/postgres
WORKDIR /build/postgres

# --enable-debug helps gdb / planner breakpoints during development
RUN ./configure \
    --prefix=/usr/local/pgsql \
    --enable-debug \
    --with-openssl \
    --with-libxml \
    --with-libxslt \
    CFLAGS="-O0 -g" \
    && make -j"$(nproc)" \
    && make install

# --- Development image: bind-mount ./postgres at /build/postgres; recompiles on each start ---
# Run with live source + auto-restart on edits: docker compose watch
FROM debian:bookworm-slim AS dev

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    pkg-config \
    flex \
    bison \
    perl \
    libreadline-dev \
    zlib1g-dev \
    libssl-dev \
    libicu-dev \
    libxml2-dev \
    libxslt1-dev \
    libreadline8 \
    zlib1g \
    libssl3 \
    libicu72 \
    libxml2 \
    libxslt1.1 \
    util-linux \
    && rm -rf /var/lib/apt/lists/*

ENV PATH=/usr/local/pgsql/bin:$PATH
ENV PGDATA=/var/lib/postgresql/data

RUN mkdir -p /usr/local/pgsql /build/postgres \
    && groupadd -r postgres --gid=999 \
    && useradd -r -g postgres --uid=999 --home-dir=/var/lib/postgresql postgres \
    && mkdir -p /var/lib/postgresql/data \
    && chown -R postgres:postgres /var/lib/postgresql

COPY docker-entrypoint-dev.sh /usr/local/bin/docker-entrypoint-dev.sh
RUN chmod +x /usr/local/bin/docker-entrypoint-dev.sh

WORKDIR /var/lib/postgresql
EXPOSE 5432
ENTRYPOINT ["/usr/local/bin/docker-entrypoint-dev.sh"]

# --- Runtime image (source baked in at build time; no live mount) ---
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libreadline8 \
    zlib1g \
    libssl3 \
    libicu72 \
    libxml2 \
    libxslt1.1 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/pgsql /usr/local/pgsql

ENV PATH=/usr/local/pgsql/bin:$PATH
ENV PGDATA=/var/lib/postgresql/data

RUN groupadd -r postgres --gid=999 \
    && useradd -r -g postgres --uid=999 --home-dir=/var/lib/postgresql postgres \
    && mkdir -p /var/lib/postgresql/data \
    && chown -R postgres:postgres /var/lib/postgresql

COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

USER postgres
WORKDIR /var/lib/postgresql

EXPOSE 5432
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
