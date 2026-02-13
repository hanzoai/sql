ARG PG_MAJOR=16

# Stage 1: Build pgvector extension from source
FROM postgres:${PG_MAJOR}-alpine AS builder
ARG PG_MAJOR

RUN apk add --no-cache --virtual .build-deps \
        build-base \
        clang \
        llvm-dev \
        postgresql${PG_MAJOR}-dev && \
    cd /tmp && \
    wget -qO- https://github.com/hanzoai/sql-vector/archive/refs/heads/main.tar.gz | tar xz && \
    cd sql-vector-main && \
    make clean && \
    make OPTFLAGS="" && \
    make install && \
    cd / && rm -rf /tmp/sql-vector-main && \
    apk del .build-deps

# Stage 2: Runtime image
FROM postgres:${PG_MAJOR}-alpine

LABEL maintainer="dev@hanzo.ai"
LABEL org.opencontainers.image.source="https://github.com/hanzoai/sql"
LABEL org.opencontainers.image.description="Hanzo SQL - PostgreSQL with pgvector for AI workloads"

ARG PG_MAJOR

# Copy pgvector extension from builder
COPY --from=builder /usr/local/lib/postgresql/vector.so /usr/local/lib/postgresql/
COPY --from=builder /usr/local/lib/postgresql/bitcode/vector/ /usr/local/lib/postgresql/bitcode/vector/
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
