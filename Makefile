.PHONY: lintcheck format cleansql docs clean test all

PLJS_VERSION = 1.0.5

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
INCLUDEDIR := ${shell $(PG_CONFIG) --includedir}
INCLUDEDIR_SERVER := ${shell $(PG_CONFIG) --includedir-server}


CP = cp
SRCS = src/pljs.c src/cache.c src/functions.c src/types.c src/params.c
OBJS = src/pljs.o src/cache.o src/functions.o src/types.o src/params.o
MODULE_big = pljs
EXTENSION = pljs
DATA = pljs.control pljs--$(PLJS_VERSION).sql
PG_CFLAGS += -fPIC -Wall -Wextra -Wno-unused-parameter -Wno-declaration-after-statement \
    -Wno-cast-function-type -std=c11 -DPLJS_VERSION=\"$(PLJS_VERSION)\" -DEXPOSE_GC
SHLIB_LINK = -Ldeps/quickjs -lquickjs

ifeq ($(DEBUG), 1)
PG_CFLAGS += -g
SHLIB_LINK += -g
endif

ifeq ($(DEBUG_MEMORY), 1)
PG_CFLAGS += -fno-omit-frame-pointer -fsanitize=address
SHLIB_LINK += -fsanitize=address
endif

ifneq ($(DISABLE_DIRECT_JSONB_CONVERSION), 1)
PG_CFLAGS += -DJSONB_DIRECT_CONVERSION
endif

ifeq ($(EXPOSE_GC), 1)
PG_CFLAGS += -DEXPOSE_GC
endif

REGRESS = init-extension function json jsonb json_conv types bytea context \
	cursor array_spread plv8_regressions memory_limits inline composites \
	trigger procedure find_function start_proc window regressions \
	pg_flush_error_state pg_spi_freetuptable \
	pg_prepared_plan_lifetime \
	currentresource \
	pg_typedarray_views \
	pg_find_function_no_perm pg_cursor_error_recovery pg_prepared_plan_gc \
	pg_cancellation pg_stack_depth pg_memory_limit_set \
	pg_param_plan_leak \
	pg_object_keys_leak \
	pg_errordata_stack \
	pg_return_next_null_row \
	pg_column_name_mismatch \
	pg_record_column_leak \
	pg_param_plan_error_leak \
	pg_cursor_plan_lifetime \
	pg_record_no_column_list \
	pg_return_next_error_frames \
	pg_error_envelope_fields \
	pg_validator \
	pg_cross_backend_invalidation \
	pg_find_function_refcount \
	pg_trigger_spi \
	pg_nested_stack_anchor \
	pg_composite_null_datum \
	pg_error_sqlstate \
	pg_plan_argcount_sqlstate \
	pg_targeted_invalidation \
	pg_number_string_parse \
	pg_name_bind \
	pg_jsonb_array_return \
	pg_array_shape \
	pg_invalid_date

all: deps/quickjs/quickjs.h deps/quickjs/libquickjs.a pljs--$(PLJS_VERSION).sql

include $(PGXS)

src/pljs.o: deps/quickjs/libquickjs.a

deps/quickjs/quickjs.h:
	mkdir -p deps
	git submodule update --init --recursive
	patch -p1 <patches/01-shared-lib-build
	patch -p1 <patches/02-unicode-conflict

deps/quickjs/libquickjs.a: deps/quickjs/quickjs.h
	cd deps/quickjs && make

format:
	clang-format -i $(SRCS) src/pljs.h

pljs--$(PLJS_VERSION).sql: pljs.sql
	$(CP) pljs.sql pljs--$(PLJS_VERSION).sql

lintcheck:
	clang-tidy $(SRCS) -- $(LINTFLAGS) -I$(INCLUDEDIR) -I$(INCLUDEDIR_SERVER) -I$(PWD) --std=c11

all: deps/quickjs/quickjs.h deps/quickjs/libquickjs.a pljs--$(PLJS_VERSION).sql

clean: cleansql

cleansql:
	$(RM) -f pljs--$(PLJS_VERSION).sql

docs:
	doxygen src/Doxyfile
