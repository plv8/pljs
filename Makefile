.PHONY: lintcheck format cleansql docs clean test all

PLJS_VERSION = 0.8.1

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
INCLUDEDIR := ${shell $(PG_CONFIG) --includedir}
INCLUDEDIR_SERVER := ${shell $(PG_CONFIG) --includedir-server}


CP = cp
SRCS = src/pljs.c src/cache.c src/functions.c src/types.c src/params.c
OBJS = src/pljs.o src/cache.o src/functions.o src/types.o src/params.o
MODULE_big = pljs
EXTENSION = pljs
DATA = pljs.control pljs--$(PLJS_VERSION).sql
PG_CFLAGS += -fPIC -Wall -Wextra -Wno-unused-parameter -Wno-declaration-after-statement -Wno-cast-function-type -std=c11 -DPLJS_VERSION=\"$(PLJS_VERSION)\"
SHLIB_LINK = -Ldeps/quickjs -lquickjs

REGRESS = init-extension function json jsonb json_conv types bytea context \
	cursor array_spread plv8_regressions memory_limits inline composites \
	trigger procedure find_function

all: deps/quickjs/quickjs.h deps/quickjs/libquickjs.a pljs--$(PLJS_VERSION).sql

include $(PGXS)

src/pljs.o: deps/quickjs/libquickjs.a

deps/quickjs/quickjs.h:
	mkdir -p deps
	git submodule update --init --recursive
	patch -p1 <patches/01-shared-lib-build

deps/quickjs/libquickjs.a: deps/quickjs/quickjs.h
	cd deps/quickjs && make

format:
	clang-format -i $(SRCS) src/pljs.h

pljs--$(PLJS_VERSION).sql: pljs.sql
	$(CP) pljs.sql pljs--$(PLJS_VERSION).sql

lintcheck:
	clang-tidy $(SRCS) -- -I$(INCLUDEDIR) -I$(INCLUDEDIR_SERVER) -I$(PWD) --std=c11

all: deps/quickjs/quickjs.h deps/quickjs/libquickjs.a pljs--$(PLJS_VERSION).sql

clean: cleansql

cleansql:
	$(RM) -f pljs--$(PLJS_VERSION).sql

docs:
	doxygen src/Doxyfile
