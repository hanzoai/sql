# Hanzo SQL (pg-wire) — multi-tenant Postgres-wire endpoint over pglite.
# Image: ghcr.io/hanzoai/sql-wire  (semver tags only, e.g. v0.1.0 — never :latest, never sha)
# Built in CI on hanzo-build-linux-amd64 via hanzoai/.github docker-build.yml (linux/amd64).
FROM ghcr.io/hanzoai/nodejs:v24.18.0

WORKDIR /app

# Deterministic install of the two embedded-Postgres deps (pinned).
COPY package.json ./
RUN npm install --omit=dev --no-audit --no-fund

COPY server.mjs ./

# Tenant data dirs (embedded-Postgres data directories) live on a mounted PVC.
# Per-tenant Unix sockets live on an ephemeral in-pod dir.
ENV HANZO_SQL_DATA=/data \
    HANZO_SQL_SOCKDIR=/run/hanzo-sql \
    HANZO_SQL_HOST=0.0.0.0 \
    HANZO_SQL_PORT=5432
VOLUME ["/data"]
EXPOSE 5432

# Non-root; the PVC is chown'd to this gid via the CR's fsGroup.
USER 65532:65532

CMD ["node", "server.mjs"]
