ARG PG_MAJOR=18

# Stage 1: Build all extensions on Debian (documentdb requires glibc toolchain)
FROM postgres:${PG_MAJOR}-bookworm AS builder
ARG PG_MAJOR

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies for pgvector, pg_cron, and pg_documentdb
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        curl \
        ca-certificates \
        pkg-config \
        postgresql-server-dev-${PG_MAJOR} \
        libpq-dev \
        libicu-dev \
        libkrb5-dev \
        # PostGIS (runtime dep for documentdb CREATE EXTENSION)
        postgresql-${PG_MAJOR}-postgis-3 \
    && rm -rf /var/lib/apt/lists/*

# --- pgvector ---
RUN cd /tmp && \
    curl -sL https://github.com/hanzoai/sql-vector/archive/refs/heads/master.tar.gz | tar xz && \
    cd sql-vector-master && \
    make clean && \
    make OPTFLAGS="" PG_CONFIG=/usr/bin/pg_config && \
    make install PG_CONFIG=/usr/bin/pg_config && \
    cd / && rm -rf /tmp/sql-vector-master

# --- pg_cron ---
ARG PG_CRON_VERSION=v1.6.7
RUN cd /tmp && \
    curl -sL https://github.com/citusdata/pg_cron/archive/refs/tags/${PG_CRON_VERSION}.tar.gz | tar xz && \
    cd pg_cron-* && \
    make PG_CONFIG=/usr/bin/pg_config && \
    make install PG_CONFIG=/usr/bin/pg_config && \
    cd / && rm -rf /tmp/pg_cron-*

# --- pg_documentdb dependencies: libbson, Intel Decimal Math Lib, pcre2 ---

ARG LIBBSON_VERSION=1.28.0
RUN cd /tmp && \
    curl -sL https://github.com/mongodb/mongo-c-driver/releases/download/${LIBBSON_VERSION}/mongo-c-driver-${LIBBSON_VERSION}.tar.gz | tar xz && \
    cd mongo-c-driver-${LIBBSON_VERSION}/build && \
    cmake -DENABLE_MONGOC=ON -DMONGOC_ENABLE_ICU=OFF -DENABLE_ICU=OFF \
          -DCMAKE_C_FLAGS="-fPIC -g" -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr .. && \
    make -j$(nproc) install && \
    cd / && rm -rf /tmp/mongo-c-driver-*

ARG INTEL_MATH_LIB_VERSION=applied/2.0u3-1
RUN cd /tmp && \
    mkdir -p intelmathlib/lib/intelmathlib && \
    cd intelmathlib/lib/intelmathlib && \
    git init && \
    git remote add origin https://git.launchpad.net/ubuntu/+source/intelrdfpmath && \
    git fetch --depth 1 origin "${INTEL_MATH_LIB_VERSION}" && \
    git checkout FETCH_HEAD && \
    cd LIBRARY && \
    make -j$(nproc) _CFLAGS_OPT=-fPIC CC=gcc CALL_BY_REF=0 GLOBAL_RND=0 GLOBAL_FLAGS=0 UNCHANGED_BINARY_FLAGS=0 && \
    cd /tmp/intelmathlib && \
    # Create pkg-config file
    printf 'prefix=/usr/lib/intelmathlib\nlibdir=${prefix}/LIBRARY\nincludedir=${prefix}/LIBRARY/src\n\nName: intelmathlib\nDescription: Intel Decimal Floating point math library\nVersion: 2.0 Update 2\nCflags: -I${includedir}\nLibs: -L${libdir} -lbid\n' > intelmathlib.pc && \
    cp -R lib/intelmathlib /usr/lib/ && \
    cp intelmathlib.pc /usr/lib/pkgconfig/ && \
    cd / && rm -rf /tmp/intelmathlib

ARG PCRE2_VERSION=10.40
RUN cd /tmp && \
    curl -sL https://github.com/PCRE2Project/pcre2/releases/download/pcre2-${PCRE2_VERSION}/pcre2-${PCRE2_VERSION}.tar.gz | tar xz && \
    cd pcre2-${PCRE2_VERSION} && \
    ./configure --prefix=/usr --disable-shared --enable-static --enable-jit && \
    make -j$(nproc) AM_CFLAGS=-fPIC install && \
    cd / && rm -rf /tmp/pcre2-*

# --- pg_documentdb (core + api) ---
ARG DOCUMENTDB_VERSION=main
RUN cd /tmp && \
    curl -sL https://github.com/microsoft/documentdb/archive/refs/heads/${DOCUMENTDB_VERSION}.tar.gz | tar xz && \
    cd documentdb-* && \
    make -C pg_documentdb_core PG_CONFIG=/usr/bin/pg_config install && \
    make -C pg_documentdb PG_CONFIG=/usr/bin/pg_config install && \
    cd / && rm -rf /tmp/documentdb-*

# Stage 2: Runtime image (Debian for glibc compat with documentdb)
FROM postgres:${PG_MAJOR}-bookworm

LABEL maintainer="dev@hanzo.ai"
LABEL org.opencontainers.image.source="https://github.com/hanzoai/sql"
LABEL org.opencontainers.image.description="Hanzo SQL - PostgreSQL with pgvector, pg_cron, and pg_documentdb"

# Runtime deps: libpq (pg_cron), PostGIS (documentdb geospatial), ICU
ARG PG_MAJOR=18
RUN apt-get update && apt-get install -y --no-install-recommends \
        libpq5 \
        postgresql-${PG_MAJOR}-postgis-3 \
    && rm -rf /var/lib/apt/lists/*

# Copy Intel Decimal Math Library (runtime dep for documentdb)
COPY --from=builder /usr/lib/intelmathlib /usr/lib/intelmathlib

# Copy pgvector extension
COPY --from=builder /usr/lib/postgresql/${PG_MAJOR}/lib/vector.so /usr/lib/postgresql/${PG_MAJOR}/lib/
COPY --from=builder /usr/share/postgresql/${PG_MAJOR}/extension/vector* /usr/share/postgresql/${PG_MAJOR}/extension/

# Copy pg_cron extension
COPY --from=builder /usr/lib/postgresql/${PG_MAJOR}/lib/pg_cron.so /usr/lib/postgresql/${PG_MAJOR}/lib/
COPY --from=builder /usr/share/postgresql/${PG_MAJOR}/extension/pg_cron* /usr/share/postgresql/${PG_MAJOR}/extension/

# Copy pg_documentdb_core extension
COPY --from=builder /usr/lib/postgresql/${PG_MAJOR}/lib/pg_documentdb_core.so /usr/lib/postgresql/${PG_MAJOR}/lib/
COPY --from=builder /usr/share/postgresql/${PG_MAJOR}/extension/documentdb_core* /usr/share/postgresql/${PG_MAJOR}/extension/

# Copy pg_documentdb extension
COPY --from=builder /usr/lib/postgresql/${PG_MAJOR}/lib/pg_documentdb.so /usr/lib/postgresql/${PG_MAJOR}/lib/
COPY --from=builder /usr/share/postgresql/${PG_MAJOR}/extension/documentdb* /usr/share/postgresql/${PG_MAJOR}/extension/

# Custom postgresql.conf tuned for AI workloads
COPY conf/postgresql.conf /etc/postgresql/postgresql.conf

# Init scripts: enable extensions on startup
COPY docker-entrypoint-initdb.d/ /docker-entrypoint-initdb.d/

# Health check
HEALTHCHECK --interval=15s --timeout=3s --start-period=30s --retries=3 \
    CMD pg_isready -U "${POSTGRES_USER:-postgres}" || exit 1

EXPOSE 5432

CMD ["postgres", "-c", "config_file=/etc/postgresql/postgresql.conf"]
