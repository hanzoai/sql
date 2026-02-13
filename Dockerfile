ARG PG_MAJOR=16

# Stage 1: Build pgvector extension from Hanzo fork
FROM postgres:${PG_MAJOR}-alpine AS builder
ARG PG_MAJOR

RUN apk add --no-cache --virtual .build-deps \
        build-base \
        clang19 \
        llvm19 \
        postgresql${PG_MAJOR}-dev && \
    cd /tmp && \
    wget -qO- https://github.com/hanzoai/sql-vector/archive/refs/heads/master.tar.gz | tar xz && \
    cd sql-vector-master && \
    make clean && \
    make OPTFLAGS="" PG_CONFIG=/usr/local/bin/pg_config && \
    make install PG_CONFIG=/usr/local/bin/pg_config && \
    cd / && rm -rf /tmp/sql-vector-master && \
    apk del .build-deps

# Stage 2: Runtime image
FROM postgres:${PG_MAJOR}-alpine

LABEL maintainer="dev@hanzo.ai"
LABEL org.opencontainers.image.source="https://github.com/hanzoai/sql"
LABEL org.opencontainers.image.description="Hanzo SQL - PostgreSQL with pgvector for AI workloads"

# Copy pgvector extension from builder (shared object + SQL files)
COPY --from=builder /usr/local/lib/postgresql/vector.so /usr/local/lib/postgresql/
COPY --from=builder /usr/local/share/postgresql/extension/vector* /usr/local/share/postgresql/extension/

# Custom postgresql.conf tuned for AI workloads
COPY conf/postgresql.conf /etc/postgresql/postgresql.conf

# Init scripts: enable pgvector on startup
COPY docker-entrypoint-initdb.d/ /docker-entrypoint-initdb.d/

# Health check
HEALTHCHECK --interval=15s --timeout=3s --start-period=30s --retries=3 \
    CMD pg_isready -U "${POSTGRES_USER:-postgres}" || exit 1

EXPOSE 5432

CMD ["postgres", "-c", "config_file=/etc/postgresql/postgresql.conf"]
