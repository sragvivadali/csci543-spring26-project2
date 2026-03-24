#!/bin/sh
set -e

if [ -z "$PGDATA" ]; then
  PGDATA=/var/lib/postgresql/data
  export PGDATA
fi

if [ ! -s "$PGDATA/PG_VERSION" ]; then
  initdb -D "$PGDATA" -U postgres --encoding=UTF8 --locale=C
  {
    echo ""
    echo "# Added for Docker dev (trust all — change for production)"
    echo "host all all all trust"
  } >> "$PGDATA/pg_hba.conf"
fi

exec postgres -D "$PGDATA" -c listen_addresses='*' -c max_connections=100
