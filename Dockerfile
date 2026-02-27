# PINNED to PG16 — production data on PVC is PG16 format.
# DO NOT bump to pg17+ without running pg_upgrade first.
ARG PG_VERSION=pg16

# Hanzo SQL: PostgreSQL + pgvector with Hanzo defaults
FROM pgvector/pgvector:${PG_VERSION}

LABEL maintainer="dev@hanzo.ai"
LABEL org.opencontainers.image.source="https://github.com/hanzoai/sql"
LABEL org.opencontainers.image.description="Hanzo SQL - PostgreSQL with pgvector and Hanzo defaults"

# Copy initialization scripts
COPY hanzo/init.sql /docker-entrypoint-initdb.d/00-hanzo-init.sql

ENV POSTGRES_DB=hanzo
ENV POSTGRES_USER=hanzo
ENV PGDATA=/var/lib/postgresql/data/pgdata

EXPOSE 5432

HEALTHCHECK --interval=10s --timeout=5s --start-period=30s --retries=5 \
    CMD pg_isready -U hanzo -d hanzo || exit 1

CMD ["postgres", \
     "-c", "shared_buffers=256MB", \
     "-c", "effective_cache_size=768MB", \
     "-c", "maintenance_work_mem=128MB", \
     "-c", "checkpoint_completion_target=0.9", \
     "-c", "wal_buffers=16MB", \
     "-c", "default_statistics_target=100", \
     "-c", "random_page_cost=1.1", \
     "-c", "effective_io_concurrency=200", \
     "-c", "work_mem=16MB", \
     "-c", "min_wal_size=256MB", \
     "-c", "max_wal_size=1GB", \
     "-c", "max_worker_processes=4", \
     "-c", "max_parallel_workers_per_gather=2", \
     "-c", "max_parallel_workers=4"]
