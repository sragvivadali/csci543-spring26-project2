# CSCI 543 Project 2 — Learned query optimization (PostgreSQL)

This repository vendors the PostgreSQL source under `postgres/` and runs a **development build** in Docker with your local tree bind-mounted, so planner changes are compiled inside the container on each start (or automatically when using Compose Watch).

## Prerequisites

- [Docker](https://docs.docker.com/get-docker/) with **Docker Compose V2** (included with Docker Desktop).
- Enough disk and RAM for a full PostgreSQL compile (first start is slow; later starts are incremental).

## Repository layout

| Path | Purpose |
|------|--------|
| `postgres/` | PostgreSQL source tree (clone or submodule of [postgres/postgres](https://github.com/postgres/postgres)) |
| `Dockerfile` | Multi-stage image: `dev` (live mount + compile) and `runtime` (baked-in build) |
| `docker-compose.yml` | Runs the `dev` target with `./postgres` mounted at `/build/postgres` |
| `docker-entrypoint-dev.sh` | Configures if needed, `make && make install`, then starts `postgres` |
| `docker-entrypoint.sh` | Used only by the slim `runtime` image |

If `postgres/` is missing, clone it from the project root:

```bash
git clone https://github.com/postgres/postgres.git
```

## Quick start (recommended for daily work)

1. Start Docker (e.g. Docker Desktop).

2. Build the development image (toolchain only; source comes from the mount):

   ```bash
   docker compose build
   ```

3. Run with **automatic restarts when you edit** files under `postgres/` (requires Compose file watch support):

   ```bash
   docker compose watch
   ```

   The first time the container starts, it runs `./configure` (if there is no `Makefile`) and then a full build. That can take several minutes. After that, restarts usually run a quick incremental `make install`.

4. In another terminal, connect:

   ```bash
   docker compose exec postgres psql -U postgres
   ```

### Run without watch

If you prefer not to use `watch`:

```bash
docker compose up -d
```

After you change C code, restart so the entrypoint recompiles:

```bash
docker compose restart postgres
```

## Stopping and resetting the database

Stop containers:

```bash
docker compose down
```

Remove the **data volume** (wipes the cluster; next start runs `initdb` again):

```bash
docker compose down -v
```

## Frozen image (no bind mount)

To build a self-contained image that copies `postgres/` at **image build** time (useful for demos or CI), build the `runtime` target:

```bash
docker compose build --target runtime
```

You would run that image with a compose override or plain `docker run` that does **not** replace `/build/postgres` with a mount; the default `docker-compose.yml` in this repo is aimed at the `dev` workflow.

## Networking and security

- The server listens on **port 5432** on the host (`localhost:5432`).
- The development entrypoint appends **`trust`** authentication for all hosts in `pg_hba.conf` so you can connect without a password. **Do not use this configuration for production or on untrusted networks.**

## Troubleshooting

- **“Cannot connect to the Docker daemon”** — Start Docker Desktop (or your Linux Docker service).
- **`docker compose watch` / `develop.watch` errors** — Update Docker Compose; file watch requires a recent Compose V2. You can still use `docker compose up` and `docker compose restart postgres` after edits.
- **Strange build errors after building on the host (e.g. macOS) and then in Linux** — From the host, in `postgres/`, try `make distclean` and let the container run `./configure` again, or use a clean tree.
- **Root-owned files under `postgres/`** — The dev entrypoint runs `make` as root inside the container; bind-mounted build artifacts may appear as root on the host. Adjust ownership locally if your tools complain (`chown`), or we can document UID/GID mapping later.

## Project proposal (summary)

The course project explores **in-process learned query optimization** in PostgreSQL: offline-trained neural network weights, inference in C inside the planner, and evaluation on workloads such as TPC-H. This README only covers **building and running** the modified server in Docker; implementation details live in the codebase and course materials.
