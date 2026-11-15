# QNX Bedside Guardian
#
# Linux / any POSIX host:   make && make check
# QNX 8.0 (x86_64 target):  source ~/qnx800/qnxsdp-env.sh && make QNX=1
#                           (see qnx/build-qnx.sh and docs/QNX.md)

QNX_ARCH  ?= x86_64
TRANSPORT ?= native

ifeq ($(QNX),1)
  CC       := qcc -Vgcc_nto$(QNX_ARCH)
  AR       := nto$(QNX_ARCH)-ar
  BUILD    ?= build-qnx-$(QNX_ARCH)
  CPPFLAGS += -D_QNX_SOURCE
  ifeq ($(TRANSPORT),posix)
    CPPFLAGS += -DBG_TRANSPORT_POSIX
    LDLIBS   += -lsocket
  endif
else
  CC       ?= cc
  BUILD    ?= build
  CPPFLAGS += -D_POSIX_C_SOURCE=200809L
  LDLIBS   += -lpthread
endif

CFLAGS   ?= -O2 -g
ifeq ($(QNX),1)
  CFLAGS += -std=gnu11
else
  CFLAGS += -std=c11
endif
CFLAGS   += -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
ifeq ($(WERROR),1)
  CFLAGS += -Werror
endif
CPPFLAGS += -Iinclude

LIB_SRC := src/sha256.c src/eventlog.c src/alarm.c src/scenario.c src/service.c \
           src/transport_posix.c src/transport_qnx.c
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/obj/%.o)
APPS    := bgd bgsim bgack bgverify bgscn
APP_BIN := $(APPS:%=$(BUILD)/%)

.PHONY: all check test e2e demo clean qnx-typecheck
all: $(APP_BIN) $(BUILD)/test_bg

$(BUILD)/obj/%.o: src/%.c include/bg/*.h | $(BUILD)/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/libbg.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/%: apps/%.c apps/common.h $(BUILD)/libbg.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD)/libbg.a $(LDLIBS) -o $@

$(BUILD)/test_bg: tests/test_main.c tests/*.h $(BUILD)/libbg.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -DBG_SOURCE_DIR='"$(CURDIR)"' $< $(BUILD)/libbg.a $(LDLIBS) -o $@

$(BUILD)/obj:
	mkdir -p $@

test: $(BUILD)/test_bg
	$(BUILD)/test_bg

e2e: $(APP_BIN)
	BUILD=$(BUILD) sh tests/e2e.sh

check: test e2e qnx-typecheck

# Compiles the QNX transport against declaration stubs (tests/qnx-stubs).
# Catches typos only; the real test is a qcc build from the QNX SDP.
qnx-typecheck:
	$(CC) -std=c11 -Wall -Wextra -Werror -fsyntax-only -D__QNXNTO__ -D_POSIX_C_SOURCE=200809L \
	    -Itests/qnx-stubs -Iinclude src/transport_qnx.c
	@echo "qnx-typecheck ok (stubs only, not a QNX build)"

demo: $(APP_BIN)
	BUILD=$(BUILD) sh scripts/demo.sh

clean:
	rm -rf build build-*
