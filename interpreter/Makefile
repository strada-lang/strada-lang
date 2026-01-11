# Makefile for the Strada tree-walking interpreter
# Reuses the compiler's Lexer and Parser, with Strada::Interpreter library for eval

# Include project config if it exists (run ../configure to generate)
-include ../config.mk

STRADAC = ../stradac
BOOTSTRAP_STRADAC = ../bootstrap/stradac
RUNTIME_DIR = ../runtime
COMPILER_DIR = ../compiler
LIB_DIR = ../lib
CC ?= gcc
CFLAGS = -O2 -I$(RUNTIME_DIR)
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

# Source files (order matters - concatenated)
# Stubs.strada provides sanitize_name() needed by Parser before Strada::Interpreter is loaded
# Strada::Interpreter is pulled in via "use" from lib/Strada/Interpreter.strada
COMPILER_SRCS = $(COMPILER_DIR)/AST.strada $(COMPILER_DIR)/Lexer.strada Stubs.strada $(COMPILER_DIR)/Parser.strada
INTERP_SRCS = Main.strada

.PHONY: all clean test test-compiler-compat

all: strada-interp

# Concatenate compiler front-end + interpreter entry point
Combined.strada: $(COMPILER_SRCS) $(INTERP_SRCS) $(LIB_DIR)/Strada/Interpreter.strada
	cat $(COMPILER_SRCS) $(INTERP_SRCS) > Combined.strada

# Compile to C using the self-hosting compiler
Combined.c: Combined.strada $(STRADAC)
	$(STRADAC) Combined.strada Combined.c

# Build the interpreter executable
strada-interp: Combined.c $(RUNTIME_DIR)/strada_runtime.c
	$(CC) $(CFLAGS) -o strada-interp Combined.c $(RUNTIME_DIR)/strada_runtime.c $(LDFLAGS)

# Fallback: use bootstrap compiler if self-hosting isn't built yet
bootstrap: Combined.strada $(BOOTSTRAP_STRADAC)
	$(BOOTSTRAP_STRADAC) Combined.strada Combined.c
	$(CC) $(CFLAGS) -o strada-interp Combined.c $(RUNTIME_DIR)/strada_runtime.c $(LDFLAGS)

# Run interpreter's own test suite
test: strada-interp
	bash test/run_tests.sh

# Run compiler test suite through interpreter to find language gaps
test-compiler-compat: strada-interp
	bash test/run_compiler_tests.sh

clean:
	rm -f Combined.strada Combined.c strada-interp
