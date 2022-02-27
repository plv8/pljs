PLJS_VERSION = 0.1.0

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)

PG_VERSION_NUM := $(shell cat `$(PG_CONFIG) --includedir`/pg_config*.h \
		   | perl -ne 'print $$1 and exit if /PG_VERSION_NUM\s+(\d+)/')

CP = cp
PG_CFLAGS := -g -fPIC -Wall
SRCS = src/pljs.c src/cache.c src/functions.c
OBJS = src/pljs.o src/cache.o src/functions.o deps/quickjs/libquickjs.a
MODULE_big = pljs
EXTENSION = pljs
DATA = pljs.control pljs--$(PLJS_VERSION).sql

REGRESS = init-extension json jsonb pljs

include $(PGXS)

dep: deps/quickjs

deps/quickjs:
	mkdir -p deps
	git submodule update --init --recursive

deps/quickjs/libquickjs.a: deps/quickjs
	cd deps/quickjs && make

format:
	clang-format -i $(SRCS)

%.o: %.c pljs.h deps/quickjs/libquickjs.a
#	$(CC) $(CCFLAGS) -g -c -o $@ $<
#src/pljs.o: src/pljs.c src/pljs.h deps/quickjs/libquickjs.a
#src/cache.o: src/cache.c src/pljs.h deps/quickjs/libquickjs.a
#src/functions.o: src/functions.c src/pljs.h deps/quickjs/libquickjs.a

%--$(PLJS_VERSION).sql: pljs.sql
	$(CP) pljs.sql pljs--$(PLJS_VERSION).sql


