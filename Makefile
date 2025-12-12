# BeRoTinyPascal Compiler and VM
# Makefile for cross-platform builds

### Platform detection
UNAME_S := $(shell uname -s 2>/dev/null)

ifeq ($(OS),Windows_NT)
    PLATFORM := windows
    BINDIR   := bin/win
else ifeq ($(UNAME_S),Linux)
    PLATFORM := linux
    BINDIR   := bin/linux
else ifeq ($(UNAME_S),Darwin)
    PLATFORM := macos
    BINDIR   := bin/mac
else
    PLATFORM := unknown
    BINDIR   := bin
endif

ifeq ($(PLATFORM),windows)
    CC  ?= gcc
    FPC ?= fpc
    EXE := .exe
else
    CC  ?= cc
    FPC ?= fpc
    EXE :=
endif

### Sources
BTVM_SRC   := src/vm/btvm.c
BTPC_SRC   := src/btpc64.pas
TEST_SRC   := test.pas

### Targets
BTVM   := $(BINDIR)/btvm$(EXE)
BTPC   := $(BINDIR)/btpc64$(EXE)
TEST_BTBC := $(BINDIR)/test.btbc

### C Compiler flags
CSTD     := -std=c11
CWARN    := -Wall -Wextra -Wpedantic
COPTREL  := -O2
COPTDBG  := -O0 -g

CCOMMON   := $(CSTD) $(CWARN)
CRELFLAGS := $(CCOMMON) $(COPTREL)
CDBGFLAGS := $(CCOMMON) $(COPTDBG)

### Pascal Compiler flags
FPCFLAGS := -O2

### Targets
.PHONY: all release debug clean info btvm btpc test run

all: release

release: $(BINDIR) btpc btvm test

debug: CRELFLAGS := $(CDBGFLAGS)
debug: $(BINDIR) btpc btvm test

$(BINDIR):
	@mkdir -p $(BINDIR)

# Build Pascal compiler
btpc: $(BTPC)

$(BTPC): $(BTPC_SRC) | $(BINDIR)
	@echo "Building btpc64 [$(PLATFORM)]"
	$(FPC) $(FPCFLAGS) -o$(BTPC) $(BTPC_SRC)

# Build VM
btvm: $(BTVM)

$(BTVM): $(BTVM_SRC) | $(BINDIR)
	@echo "Building btvm [$(PLATFORM), release]"
	$(CC) $(CRELFLAGS) -o $(BTVM) $(BTVM_SRC)

# Compile test program
test: $(TEST_BTBC)

$(TEST_BTBC): $(TEST_SRC) $(BTPC) | $(BINDIR)
	@echo "Compiling test.pas -> test.btbc"
	$(BTPC) < $(TEST_SRC) > $(TEST_BTBC)

# Run test
run: $(TEST_BTBC) $(BTVM)
	@echo "Running test.btbc..."
	$(BTVM) $(TEST_BTBC)

# Run test with trace
trace: $(TEST_BTBC) $(BTVM)
	@echo "Running test.btbc with trace..."
	$(BTVM) --trace $(TEST_BTBC)

clean:
	rm -f $(BTVM) $(BTPC) $(TEST_BTBC)
	rm -f src/*.o src/*.ppu
	rm -f *.btbc

distclean: clean
	rm -rf bin/win bin/linux bin/mac

info:
	@echo "Platform    : $(PLATFORM)"
	@echo "C Compiler  : $(CC)"
	@echo "FPC         : $(FPC)"
	@echo "Bin dir     : $(BINDIR)"
	@echo "btpc64      : $(BTPC)"
	@echo "btvm        : $(BTVM)"
	@echo "test.btbc   : $(TEST_BTBC)"
