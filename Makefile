# ============================================================================
# mllvm -- makefile
# ----------------------------------------------------------------------------
#   make            build bin/mllvm
#   make test       build and run the test suite
#   make clean      remove the build artefacts
#   make install    install to $(PREFIX)/bin
# ============================================================================
PREFIX  ?= /usr/local
CC      ?= cc
CFLAGS  ?= -O2 -g
LDFLAGS ?=
LDLIBS  ?=

MLLVM_CFLAGS := -std=c11 -Wall -Wextra -Wshadow -Wstrict-prototypes \
                -Wmissing-prototypes

BIN     := bin/mllvm
SRCDIR  := src
OBJDIR  := build

SOURCES := $(wildcard $(SRCDIR)/*.c)
OBJECTS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))
DEPS    := $(OBJECTS:.o=.d)

.PHONY: all clean test install uninstall

all: $(BIN)

$(BIN): $(OBJECTS) | bin
	$(CC) $(CFLAGS) $(MLLVM_CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(MLLVM_CFLAGS) -MMD -MP -c -o $@ $<

bin $(OBJDIR):
	mkdir -p $@

test: $(BIN)
	./tests/run.sh

install: $(BIN)
	install -Dm755 $(BIN) "$(PREFIX)/bin/mllvm"

uninstall:
	rm -f "$(PREFIX)/bin/mllvm"

clean:
	rm -rf $(OBJDIR) $(BIN)

-include $(DEPS)
