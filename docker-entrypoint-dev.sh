#!/bin/sh
# Development entrypoint: compile mounted ./postgres then start the server.
# Run as root so `make install` can write to /usr/local/pgsql; server runs as postgres.
set -e

PG_SRC=/build/postgres
export PGDATA="${PGDATA:-/var/lib/postgresql/data}"

cd "$PG_SRC"

if [ ! -f Makefile ]; then
  echo "docker-entrypoint-dev: running ./configure (first start or clean tree)..."
  ./configure \
    --prefix=/usr/local/pgsql \
    --enable-debug \
    --with-openssl \
    --with-libxml \
    --with-libxslt \
    CFLAGS="-O0 -g"
fi

echo "docker-entrypoint-dev: incremental build + install..."
make -j"$(nproc)"
make install

if [ ! -s "$PGDATA/PG_VERSION" ]; then
  echo "docker-entrypoint-dev: initdb..."
  chown postgres:postgres "$PGDATA" 2>/dev/null || true
  runuser -u postgres -- initdb -D "$PGDATA" -U postgres --encoding=UTF8 --locale=C
  {
    echo ""
    echo "# Added for Docker dev (trust all — change for production)"
    echo "host all all all trust"
  } >> "$PGDATA/pg_hba.conf"
fi

chown -R postgres:postgres "$PGDATA" 2>/dev/null || true

echo "docker-entrypoint-dev: starting postgres..."
exec runuser -u postgres -- postgres -D "$PGDATA" -c listen_addresses='*' -c max_connections=100
