# Makefile for the Strada bytecode VM interpreter
# Reuses the compiler's Lexer and Parser, with vm.c / vm_compiler.c bytecode VM

# Include project config if it exists (run ../configure to generate)
-include ../config.mk

STRADAC = ../stradac
BOOTSTRAP_STRADAC = ../bootstrap/stradac
RUNTIME_DIR = ../runtime
COMPILER_DIR = ../compiler
LIB_DIR = ../lib
CC ?= gcc
# ccache-wrap like the top-level Makefile (the parent's `CC := ccache gcc` is
# not exported to this sub-make). Separate -c compiles below make it effective.
USE_CCACHE ?= auto
ifneq ($(USE_CCACHE),0)
ifeq ($(findstring ccache,$(CC)),)
ifneq ($(shell command -v ccache 2>/dev/null),)
CC := ccache $(CC)
endif
endif
endif
UNAME_S := $(shell uname -s)
CFLAGS = -O2 -I$(RUNTIME_DIR)
# macOS clang emits -Wunused-value noise on generated statement expressions.
ifeq ($(UNAME_S),Darwin)
CFLAGS += -Wno-unused-value
endif
LDFLAGS = -rdynamic -ldl -lm -lpthread

ifeq ($(HAVE_PCRE2),1)
CFLAGS += -DHAVE_PCRE2 $(PCRE2_CFLAGS)
ifeq ($(STATIC_PCRE2),1)
LDFLAGS += $(PCRE2_STATIC_LIB)
else
LDFLAGS += $(PCRE2_LIBS)
endif
endif

ifeq ($(HAVE_READLINE),1)
CFLAGS += -DHAVE_READLINE $(READLINE_CFLAGS)
LDFLAGS += $(READLINE_LIBS)
endif

ifeq ($(HAVE_CYCLE_GC),1)
CFLAGS += -DSTRADA_CYCLE_GC
endif
ifeq ($(HAVE_ARENA),1)
CFLAGS += -DSTRADA_ARENA
endif
ifeq ($(HAVE_EPOLL),1)
CFLAGS += -DSTRADA_HAVE_EPOLL
endif
ifeq ($(HAVE_EPOLL),0)
CFLAGS += -DSTRADA_NO_EPOLL
endif

# Source files (order matters - concatenated)
# Stubs.strada provides sanitize_name() needed by Parser before Strada::Interpreter is loaded
# Strada::Interpreter is pulled in via "use" from lib/Strada/Interpreter.strada
COMPILER_SRCS = $(COMPILER_DIR)/AST.strada $(COMPILER_DIR)/Lexer.strada Stubs.strada $(COMPILER_DIR)/Parser.strada
INTERP_SRCS = Main.strada

.PHONY: all clean test test-compiler-compat vm

all: strada-interp

# Concatenate compiler front-end + interpreter entry point
Combined.strada: $(COMPILER_SRCS) $(INTERP_SRCS) $(LIB_DIR)/Strada/Interpreter.strada
	cat $(COMPILER_SRCS) $(INTERP_SRCS) > Combined.strada

# Compile to C using the self-hosting compiler
Combined.c: Combined.strada $(STRADAC)
	$(STRADAC) Combined.strada Combined.c

# Opt level for the generated front end (Combined.c = lexer/parser/driver).
# gcc -O2 on it takes ~70s (the dominant cost of this build); -O1 ~37s, -O0 ~2s.
# Same trade the top-level makes for ./stradac (STRADAC_CFLAGS): the front end
# is allocation-bound, so -O1 costs little. The VM (vm.c, vm_compiler.c) and the
# runtime stay at -O2 — bytecode execution speed is unchanged.
# DEV=1 or QUICK=1 => -O0. Override: make INTERP_FRONT_OPT=-O2
ifneq ($(filter 1,$(DEV) $(QUICK)),)
INTERP_FRONT_OPT ?= -O0
else
INTERP_FRONT_OPT ?= -O1
endif
INTERP_CFLAGS = $(filter-out -O0 -O1 -O2 -O3 -Ofast,$(CFLAGS)) $(INTERP_FRONT_OPT)

# Build the interpreter executable (bytecode VM). Each source is compiled to
# its own object (interp_*.o) so `make -j` compiles them in parallel and ccache
# can cache each one — an edit to compiler/*.strada only recompiles Combined.c.
# The front-end object name carries its opt level so switching QUICK/DEV/
# INTERP_FRONT_OPT never silently reuses an object built at another level.
INTERP_FRONT_OBJ = interp_Combined$(INTERP_FRONT_OPT).o
INTERP_OBJS = $(INTERP_FRONT_OBJ) interp_vm.o interp_vm_compiler.o interp_runtime.o

$(INTERP_FRONT_OBJ): Combined.c $(RUNTIME_DIR)/strada_runtime.h
	$(CC) $(INTERP_CFLAGS) -I. -c Combined.c -o $@

interp_vm.o: vm.c vm.h vm_generic_builtins.inc $(RUNTIME_DIR)/strada_runtime.h
	$(CC) $(CFLAGS) -I. -c vm.c -o $@

interp_vm_compiler.o: vm_compiler.c vm_compiler.h vm.h $(RUNTIME_DIR)/strada_runtime.h
	$(CC) $(CFLAGS) -I. -c vm_compiler.c -o $@

interp_runtime.o: $(RUNTIME_DIR)/strada_runtime.c $(RUNTIME_DIR)/strada_runtime.h
	$(CC) $(CFLAGS) -I. -c $(RUNTIME_DIR)/strada_runtime.c -o $@

strada-interp: $(INTERP_OBJS)
	$(CC) -o strada-interp $(INTERP_OBJS) $(LDFLAGS)

# Bytecode VM runner — compiles .strada source to bytecode and executes
# Uses -O0 for Combined.c (35k lines) to avoid excessive memory during compilation
# VM components are compiled separately with -O2 for performance
vm: vm_run

Combined.o: Combined.c
	$(CC) -O0 -I$(RUNTIME_DIR) -c Combined.c -o Combined.o -Dmain=interpreter_main

vm.o: vm.c vm.h
	$(CC) $(CFLAGS) -c vm.c -o vm.o

vm_compiler.o: vm_compiler.c vm_compiler.h vm.h
	$(CC) $(CFLAGS) -c vm_compiler.c -o vm_compiler.o

vm_run.o: vm_run.c vm.h vm_compiler.h
	$(CC) $(CFLAGS) -DRUNTIME_DIR='"$(realpath $(RUNTIME_DIR))"' -c vm_run.c -o vm_run.o

strada_runtime.o: $(RUNTIME_DIR)/strada_runtime.c
	$(CC) $(CFLAGS) -c $(RUNTIME_DIR)/strada_runtime.c -o strada_runtime.o

vm_run: vm_run.o vm_compiler.o vm.o Combined.o strada_runtime.o
	$(CC) -o vm_run vm_run.o vm_compiler.o vm.o Combined.o strada_runtime.o $(LDFLAGS)

vm_test: vm_test.c vm.c vm.h
	$(CC) $(CFLAGS) -o vm_test vm_test.c vm.c $(LDFLAGS)

# Fallback: use bootstrap compiler if self-hosting isn't built yet
bootstrap: Combined.strada $(BOOTSTRAP_STRADAC)
	$(BOOTSTRAP_STRADAC) Combined.strada Combined.c
	$(CC) $(CFLAGS) -I. -o strada-interp Combined.c vm.c vm_compiler.c $(RUNTIME_DIR)/strada_runtime.c $(LDFLAGS)

# Run interpreter's own test suite
test: strada-interp
	bash test/run_tests.sh

# Run compiler test suite through interpreter to find language gaps
test-compiler-compat: strada-interp
	bash test/run_compiler_tests.sh

clean:
	rm -f Combined.strada Combined.c strada-interp
	rm -f vm_run vm_test *.o
