# contrib/hanzo_iam/Makefile
#
# OAuth validator for hanzo-sql: accepts a hanzo-sql token, signed RS256 by a
# key listed in hanzo_iam.dir.
#
# Built with PGXS against the installed server, PostgreSQL 18, which is the one
# hanzo-sql runs (Dockerfile). This tree is a later PostgreSQL, so the top-level
# build does not descend into this directory.
#
#   make && make install                  the module, hanzo_iam.so
#   make installcheck                     t/*.pl against the installed server
#   make fuzz                             fuzz/fuzz, a libFuzzer target; see fuzz/fuzz.c

MODULE_big = hanzo_iam
OBJS = hanzo_iam.o token.o
PGFILEDESC = "hanzo_iam - OAuth validator for hanzo-sql tokens"

SHLIB_LINK = -lcrypto

TAP_TESTS = 1
EXTRA_CLEAN = fuzz/fuzz

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# token.c is compiled for a frontend here, so jsonapi comes from libpgcommon.
FUZZ_CC ?= clang

fuzz/fuzz: fuzz/fuzz.c token.c token.h
	$(FUZZ_CC) -g -O1 -fno-omit-frame-pointer -Wall -Wno-ignored-attributes \
		-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
		-DFRONTEND $(CPPFLAGS) -I. $(filter %.c,$^) -o $@ \
		-L$(libdir) -L$(pkglibdir) -lpgcommon -lpgport -lcrypto -lm -lpthread

fuzz: fuzz/fuzz

.PHONY: fuzz
