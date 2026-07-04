# Hanzo SQL — pg-wire endpoint over embedded Postgres (pglite).
# Image: ghcr.io/hanzoai/sql-managed  (semver tags only, e.g. v0.1.0 — never :latest, never sha)
# Built in CI on hanzo-build-linux-amd64 via hanzoai/.github docker-build.yml (linux/amd64).
FROM ghcr.io/hanzoai/nodejs:v24.18.0

WORKDIR /app

# Deterministic install of the two embedded-Postgres deps (pinned).
COPY package.json ./
RUN npm install --omit=dev --no-audit --no-fund

COPY server.mjs provision.mjs ./

# One tenant's embedded Postgres data directory lives on a mounted PVC.
ENV HANZO_SQL_DATA=/data/db \
    HANZO_SQL_HOST=0.0.0.0 \
    HANZO_SQL_PORT=5432
VOLUME ["/data"]
EXPOSE 5432

# Non-root; the PVC is chown'd to this gid via the CR's fsGroup.
USER 65532:65532

CMD ["node", "server.mjs"]
