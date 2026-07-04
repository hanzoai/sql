# Hanzo SQL (pg-wire) — multi-tenant Postgres-wire endpoint over pglite.
# Image: ghcr.io/hanzoai/sql-wire  (semver tags only, e.g. 0.1.1 — never :latest, never sha)
# Built in CI on hanzo-build-linux-amd64 (sql-wire-v* tag → .github/workflows/sql-wire.yml).
FROM ghcr.io/hanzoai/nodejs:v24.18.0

WORKDIR /app

# Deterministic install of the two embedded-Postgres deps (pinned).
COPY package.json ./
RUN npm install --omit=dev --no-audit --no-fund

COPY server.mjs ./

# Tenant data dirs (embedded-Postgres data directories) live on a mounted PVC.
# HANZO_SQL_LISTEN_PORT (not *_PORT) so a k8s service-link env `<svc>_PORT=tcp://…`
# can never shadow it (that shadowing crashed the pod once); the code also
# ignores a non-numeric legacy HANZO_SQL_PORT defensively.
ENV HANZO_SQL_DATA=/data \
    HANZO_SQL_HOST=0.0.0.0 \
    HANZO_SQL_LISTEN_PORT=5432
VOLUME ["/data"]
EXPOSE 5432

# Non-root; the PVC is chown'd to this gid via the CR's fsGroup.
USER 65532:65532

CMD ["node", "server.mjs"]
