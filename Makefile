.PHONY: lintcheck format dep deps/quickjs cleandepend docs

PLJS_VERSION = 0.8.0

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
INCLUDEDIR := ${shell $(PG_CONFIG) --includedir}
INCLUDEDIR_SERVER := ${shell $(PG_CONFIG) --includedir-server}

CP = cp
PG_CFLAGS += -fPIC -Wall -Wextra -Wno-unused-parameter -Wno-declaration-after-statement -Wno-cast-function-type -std=c11 -DPLJS_VERSION=\"$(PLJS_VERSION)\" -g -O0
SRCS = src/pljs.c src/cache.c src/functions.c src/types.c src/params.c
OBJS = src/pljs.o src/cache.o src/functions.o src/types.o src/params.o deps/quickjs/libquickjs.a
MODULE_big = pljs
EXTENSION = pljs
DATA = pljs.control pljs--$(PLJS_VERSION).sql

REGRESS = init-extension function json jsonb json_conv types bytea context \
	cursor array_spread plv8_regressions memory_limits inline composites

include $(PGXS)

dep: deps/quickjs

deps/quickjs:
	mkdir -p deps
	git submodule update --init --recursive

deps/quickjs/libquickjs.a: deps/quickjs
	cd deps/quickjs && make

format:
	clang-format -i $(SRCS) src/pljs.h

%.o: %.c pljs.h deps/quickjs/libquickjs.a

%--$(PLJS_VERSION).sql: pljs.sql
	$(CP) pljs.sql pljs--$(PLJS_VERSION).sql


lintcheck:
	clang-tidy $(SRCS) -- -I$(INCLUDEDIR) -I$(INCLUDEDIR_SERVER) -I$(PWD) --std=c11

.depend:
	$(RM) -f .depend
	$(foreach SRC,$(SRCS),$(CC) $(PG_CFLAGS) -I$(INCLUDEDIR) -I$(INCLUDEDIR_SERVER) \
	   -I$(PWD) -MM -MT $(SRC:.c=.o) $(SRC) >> .depend;)

include .depend

clean: cleandepend

cleandepend:
	$(RM) -f .depend

docs:
	doxygen src/Doxyfile