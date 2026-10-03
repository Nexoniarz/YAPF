# YAPF — simple build for Linux, macOS and other Unix systems.
#
#   make                 library (static + shared) and the `yapf` tool, in build/
#   make test            run the C test suite (and the JS one if node is present)
#   sudo make install    install header, libraries and tool under $(PREFIX)
#   make NO_SIMD=1       plain C only
#
# Windows: use build.bat (LLVM) or any MinGW / MSVC compiler on yapf.c.

CC      ?= cc
CFLAGS  ?= -O2
PREFIX  ?= /usr/local
BUILD   ?= build

YAPF_CFLAGS = -std=c99 -Wall -Wextra $(CFLAGS) $(if $(NO_SIMD),-DYAPF_NO_SIMD)
LDLIBS      = -lm -pthread

ifeq ($(shell uname -s),Darwin)
  SHARED = libyapf.dylib
  SHARED_FLAGS = -dynamiclib -install_name $(PREFIX)/lib/libyapf.dylib
else
  SHARED = libyapf.so
  SHARED_FLAGS = -shared
endif

all: $(BUILD)/libyapf.a $(BUILD)/$(SHARED) $(BUILD)/yapf

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/yapf.o: yapf.c yapf.h | $(BUILD)
	$(CC) $(YAPF_CFLAGS) -fPIC -c yapf.c -o $@

$(BUILD)/libyapf.a: $(BUILD)/yapf.o
	$(AR) rcs $@ $^

$(BUILD)/$(SHARED): $(BUILD)/yapf.o
	$(CC) $(SHARED_FLAGS) $^ -o $@ -pthread

$(BUILD)/yapf: tools/yapf_cli.c yapf.c yapf.h | $(BUILD)
	$(CC) $(YAPF_CFLAGS) tools/yapf_cli.c yapf.c -o $@ $(LDLIBS)

$(BUILD)/test_yapf: tests/test_yapf.c yapf.c yapf.h | $(BUILD)
	$(CC) $(YAPF_CFLAGS) tests/test_yapf.c yapf.c -o $@ $(LDLIBS)

test: $(BUILD)/test_yapf
	$(BUILD)/test_yapf other/YAPF.YAPF
	@if command -v node >/dev/null 2>&1; then node bindings/js/test.js; \
	 else echo "(node not found: skipped the JavaScript tests)"; fi

install: all
	install -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/bin
	install -m 644 yapf.h $(DESTDIR)$(PREFIX)/include/yapf.h
	install -m 644 $(BUILD)/libyapf.a $(DESTDIR)$(PREFIX)/lib/libyapf.a
	install -m 755 $(BUILD)/$(SHARED) $(DESTDIR)$(PREFIX)/lib/$(SHARED)
	install -m 755 $(BUILD)/yapf $(DESTDIR)$(PREFIX)/bin/yapf

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/include/yapf.h $(DESTDIR)$(PREFIX)/lib/libyapf.a \
	      $(DESTDIR)$(PREFIX)/lib/$(SHARED) $(DESTDIR)$(PREFIX)/bin/yapf

clean:
	rm -rf $(BUILD)

.PHONY: all test install uninstall clean
