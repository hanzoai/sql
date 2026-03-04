# Makefile for zap_fdw — ZAP binary protocol extension for hanzo/sql
#
# Adds native ZAP support to hanzo/sql.
# Listens on port 9651 for ZAP binary connections, routes to SPI/KV.

MODULE_big = zap_fdw
OBJS = zap_listener.o zap_spi.o zap_kv.o

EXTENSION = zap_fdw
DATA = zap_fdw--1.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/zap_fdw
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
