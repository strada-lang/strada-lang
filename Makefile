# Makefile for Strada Language
#
# Build order:
#   1. Run ./configure to detect dependencies (optional but recommended)
#   2. Bootstrap compiler (C) - builds bootstrap/stradac
#   3. Stage 1: Bootstrap compiles self-hosting compiler -> stradac_stage1
#   4. Stage 2: stradac_stage1 recompiles itself -> ./stradac (with proper cleanup)
#   5. Examples use self-hosting compiler (./stradac)
#

# Include generated config if it exists (run ./configure to generate)
-include config.mk

# Default values if config.mk doesn't exist
CC ?= gcc
PREFIX ?= /usr/local
HAVE_MYSQL ?= 0
HAVE_SQLITE ?= 1
HAVE_POSTGRES ?= 0
HAVE_CRYPT ?= 0
HAVE_READLINE ?= 0
HAVE_SSL ?= 0
DBI_DEFINES ?=
DBI_LIBS ?= -lsqlite3
CRYPT_LIBS ?= -lcrypt
READLINE_LIBS ?= -lreadline
SSL_LIBS ?= -lssl -lcrypto
HAVE_PCRE2 ?= 0
PCRE2_CFLAGS ?=
PCRE2_LIBS ?= -lpcre2-8
STATIC_PCRE2 ?= 0
PCRE2_STATIC_LIB ?=
BUNDLED_PCRE2 ?= 0
# Base warning flags (portable)
CFLAGS_BASE = -Wall -Wextra -Wno-unused-variable -Wno-return-type -Wno-unused-result -Wno-comment -O2 -std=c99 -ffunction-sections -fdata-sections
# GCC-specific flags (not available on clang/macOS)
CFLAGS_GCC = -Wno-unused-but-set-variable
# Detect compiler and set appropriate flags
ifeq ($(shell $(CC) --version 2>&1 | grep -q clang && echo clang),clang)
CFLAGS = $(CFLAGS_BASE)
else
CFLAGS = $(CFLAGS_BASE) $(CFLAGS_GCC)
endif
LDFLAGS = -ldl -lm -lpthread -Wl,--gc-sections
ifeq ($(HAVE_PCRE2),1)
ifeq ($(STATIC_PCRE2),1)
LDFLAGS += $(PCRE2_STATIC_LIB)
else
LDFLAGS += $(PCRE2_LIBS)
endif
endif
RUNTIME_DIR = runtime
BOOTSTRAP_DIR = bootstrap
COMPILER_DIR = compiler
EXAMPLES_DIR = examples

# Bootstrap compiler (C version)
BOOTSTRAP_STRADAC = $(BOOTSTRAP_DIR)/stradac

# Self-hosting compiler (Strada version) - this is the primary compiler
STRADAC = ./stradac

RUNTIME_SRC = $(RUNTIME_DIR)/strada_runtime.c
RUNTIME_HDR = $(RUNTIME_DIR)/strada_runtime.h
RUNTIME_OBJ = $(RUNTIME_DIR)/strada_runtime.o
RUNTIME_TCC_OBJ = $(RUNTIME_DIR)/strada_runtime_tcc.o

.PHONY: all clean test test-all test-examples test-selfhost test-suite runtime bootstrap compiler examples run run-bootstrap help info selfhost install uninstall tools libs lib-dbi lib-crypt lib-ssl lib-readline configure-check stage1 interpreter test-interp

# Default: build everything including self-hosting compiler and tools
all: stradac $(RUNTIME_OBJ) tools
	@echo ""
	@echo "✓ Build complete!"
	@echo "  Compiler: ./stradac"
	@echo "  Tools:    tools/stradadoc, tools/strada-soinfo, tools/strada-md2man, tools/strada-md2html, tools/strada-jit"
	@echo ""
	@echo "Usage: ./strada -r hello.strada"
	@echo "   or: make run PROG=test_simple"
	@echo "   or: tools/strada-jit"
	@echo ""
	@echo "Run 'make install' to install to /usr/local (or PREFIX=/path make install)"

# Bundled PCRE2 static library
VENDOR_PCRE2_DIR = vendor/pcre2
VENDOR_PCRE2_LIB = $(VENDOR_PCRE2_DIR)/libpcre2-8.a

$(VENDOR_PCRE2_LIB):
	@echo "=== Building bundled PCRE2 ==="
	$(MAKE) -C $(VENDOR_PCRE2_DIR) CC="$(CC)"

# Pre-compiled runtime object (for faster program compilation)
# Built with -ffunction-sections so -Wl,--gc-sections can strip unused functions
# When using bundled PCRE2, depend on the vendored .a being built first
$(RUNTIME_OBJ): $(RUNTIME_SRC) $(RUNTIME_HDR) $(if $(filter 1,$(BUNDLED_PCRE2)),$(VENDOR_PCRE2_LIB))
	@echo "=== Building pre-compiled runtime ==="
	$(CC) $(CFLAGS) $(if $(filter 1,$(HAVE_PCRE2)),-DHAVE_PCRE2 $(PCRE2_CFLAGS)) -c $(RUNTIME_SRC) -I$(RUNTIME_DIR) -o $(RUNTIME_OBJ)

# TCC-compatible runtime object (for REPL with TCC backend)
# -Wa,-mrelax-relocations=no avoids R_X86_64_REX_GOTPCRELX relocations that TCC can't handle
# Some assemblers don't support this flag, so try with it first, fall back without
$(RUNTIME_TCC_OBJ): $(RUNTIME_SRC) $(RUNTIME_HDR) $(RUNTIME_DIR)/strada_runtime_tcc.h
	@echo "=== Building TCC runtime ==="
	$(CC) $(CFLAGS) -fPIC -DSTRADA_NO_TLS -Wa,-mrelax-relocations=no -c $(RUNTIME_SRC) -I$(RUNTIME_DIR) -o $(RUNTIME_TCC_OBJ) 2>/dev/null || \
	$(CC) $(CFLAGS) -fPIC -DSTRADA_NO_TLS -c $(RUNTIME_SRC) -I$(RUNTIME_DIR) -o $(RUNTIME_TCC_OBJ)

# Build the runtime test
runtime: $(RUNTIME_DIR)/test_runtime

$(RUNTIME_DIR)/test_runtime: $(RUNTIME_DIR)/test_runtime.c $(RUNTIME_SRC) $(RUNTIME_HDR)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_DIR)/test_runtime.c $(RUNTIME_SRC) $(LDFLAGS)

# Test the runtime
test: runtime
	@echo "=== Testing Strada Runtime ==="
	./$(RUNTIME_DIR)/test_runtime
	@echo ""
	@echo "✓ Runtime tests passed"

# Build bootstrap compiler (C version)
bootstrap:
	$(MAKE) -C $(BOOTSTRAP_DIR)

# Create combined source for self-hosting compiler
$(COMPILER_DIR)/Combined.strada: $(COMPILER_DIR)/AST.strada $(COMPILER_DIR)/Lexer.strada $(COMPILER_DIR)/Parser.strada $(COMPILER_DIR)/Semantic.strada $(COMPILER_DIR)/CodeGen.strada $(COMPILER_DIR)/Main.strada
	@echo "=== Creating combined compiler source ==="
	cat $(COMPILER_DIR)/AST.strada $(COMPILER_DIR)/Lexer.strada $(COMPILER_DIR)/Parser.strada $(COMPILER_DIR)/Semantic.strada $(COMPILER_DIR)/CodeGen.strada $(COMPILER_DIR)/Main.strada > $(COMPILER_DIR)/Combined.strada

# Stage 1: Compile combined source to C using bootstrap compiler
$(COMPILER_DIR)/Combined_stage1.c: $(COMPILER_DIR)/Combined.strada $(BOOTSTRAP_STRADAC)
	@echo "=== Stage 1: Compiling self-hosting compiler (bootstrap -> C) ==="
	$(BOOTSTRAP_STRADAC) $(COMPILER_DIR)/Combined.strada $(COMPILER_DIR)/Combined_stage1.c

# Build bootstrap compiler if it doesn't exist
$(BOOTSTRAP_STRADAC):
	$(MAKE) -C $(BOOTSTRAP_DIR)

# Build the stage 1 compiler executable
# Note: -rdynamic exports symbols so that shared libraries loaded at compile time
# (via import_lib) can access runtime functions
stradac_stage1: $(COMPILER_DIR)/Combined_stage1.c $(RUNTIME_OBJ)
	@echo "=== Building stage 1 compiler ==="
	$(CC) $(CFLAGS) -rdynamic -o stradac_stage1 $(COMPILER_DIR)/Combined_stage1.c $(RUNTIME_OBJ) -I$(RUNTIME_DIR) $(LDFLAGS)

stage1: stradac_stage1

# Stage 2: Self-hosting compiler recompiles itself (with proper cleanup)
$(COMPILER_DIR)/Combined.c: $(COMPILER_DIR)/Combined.strada stradac_stage1
	@echo "=== Stage 2: Self-hosting compiler recompiles itself ==="
	./stradac_stage1 $(COMPILER_DIR)/Combined.strada $(COMPILER_DIR)/Combined.c

# Build the final self-hosting compiler executable
stradac: $(COMPILER_DIR)/Combined.c $(RUNTIME_OBJ)
	@echo "=== Building self-hosting compiler executable ==="
	$(CC) $(CFLAGS) -Wno-unused-function -rdynamic -o stradac $(COMPILER_DIR)/Combined.c $(RUNTIME_OBJ) -I$(RUNTIME_DIR) $(LDFLAGS)
	@echo "✓ Self-hosting compiler built: ./stradac (stage 2)"

# Aliases
compiler: stradac
selfhost: stradac

# Compile and run an example using SELF-HOSTING compiler
# Usage: make run PROG=test_simple
run: stradac $(RUNTIME_OBJ)
	@if [ -z "$(PROG)" ]; then echo "Usage: make run PROG=example_name"; exit 1; fi
	@echo "=== Compiling $(PROG) with self-hosting compiler ==="
	$(STRADAC) $(EXAMPLES_DIR)/$(PROG).strada $(EXAMPLES_DIR)/$(PROG).c
	$(CC) $(CFLAGS) -o $(EXAMPLES_DIR)/$(PROG) $(EXAMPLES_DIR)/$(PROG).c $(RUNTIME_OBJ) -I$(RUNTIME_DIR) $(LDFLAGS)
	@echo ""
	@echo "=== Running $(PROG) ==="
	./$(EXAMPLES_DIR)/$(PROG)

# Compile and run using BOOTSTRAP compiler (for comparison)
# Usage: make run-bootstrap PROG=test_simple
run-bootstrap: bootstrap $(RUNTIME_OBJ)
	@if [ -z "$(PROG)" ]; then echo "Usage: make run-bootstrap PROG=example_name"; exit 1; fi
	@echo "=== Compiling $(PROG) with bootstrap compiler ==="
	$(BOOTSTRAP_STRADAC) $(EXAMPLES_DIR)/$(PROG).strada $(EXAMPLES_DIR)/$(PROG).c
	$(CC) $(CFLAGS) -o $(EXAMPLES_DIR)/$(PROG) $(EXAMPLES_DIR)/$(PROG).c $(RUNTIME_OBJ) -I$(RUNTIME_DIR) $(LDFLAGS)
	@echo ""
	@echo "=== Running $(PROG) ==="
	./$(EXAMPLES_DIR)/$(PROG)

# Build all example programs using self-hosting compiler
examples: stradac $(RUNTIME_OBJ)
	@echo "=== Building example programs with self-hosting compiler ==="
	@for f in $(EXAMPLES_DIR)/*.strada; do \
		name=$$(basename $$f .strada); \
		echo "Compiling $$name..."; \
		$(STRADAC) $$f $(EXAMPLES_DIR)/$$name.c 2>/dev/null && \
		$(CC) $(CFLAGS) -o $(EXAMPLES_DIR)/$$name $(EXAMPLES_DIR)/$$name.c $(RUNTIME_OBJ) -I$(RUNTIME_DIR) $(LDFLAGS) 2>/dev/null || \
		echo "  (skipped - compile error)"; \
	done
	@echo "✓ Examples built"

# Test that self-hosting compiler can compile itself (stage 2)
test-selfhost: stradac
	@echo "=== Testing self-hosting compiler (stage 2) ==="
	@echo "Compiling Lexer.strada with self-hosting compiler..."
	$(STRADAC) $(COMPILER_DIR)/Lexer.strada /tmp/Lexer_stage2.c
	@echo "✓ Self-hosting compiler can compile its own modules"

# Test all examples compile and link successfully
test-examples: stradac $(RUNTIME_OBJ)
	@echo "=== Testing all examples ==="
	@passed=0; failed=0; \
	for f in $(EXAMPLES_DIR)/*.strada; do \
		name=$$(basename $$f .strada); \
		printf "Testing $$name... "; \
		if timeout 5 $(STRADAC) $$f /tmp/$$name.c >/dev/null 2>&1; then \
			if $(CC) -o /tmp/$$name /tmp/$$name.c $(RUNTIME_OBJ) -I$(RUNTIME_DIR) $(LDFLAGS) 2>/dev/null; then \
				echo "OK"; \
				passed=$$((passed + 1)); \
			else \
				echo "COMPILE FAIL"; \
				failed=$$((failed + 1)); \
			fi; \
		else \
			echo "STRADA FAIL"; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	echo ""; \
	echo "Results: $$passed passed, $$failed failed"; \
	if [ $$failed -eq 0 ]; then \
		echo "✓ All examples passed"; \
	else \
		echo "✗ Some examples failed"; \
		exit 1; \
	fi

# Full test suite: runtime + self-hosting + all examples
test-all: test test-selfhost test-examples
	@echo ""
	@echo "════════════════════════════════════"
	@echo "✓ All tests passed!"
	@echo "════════════════════════════════════"

# Build the tree-walking interpreter
interpreter: stradac
	@cd interpreter && $(MAKE)

# Run interpreter compatibility tests against compiler test suite
test-interp: interpreter
	@cd interpreter && $(MAKE) test-compiler-compat

# Comprehensive test suite with output verification
# Usage: make test-suite          - Run all tests
#        make test-suite V=1      - Verbose output
#        make test-suite TAP=1    - TAP format for CI/CD
#        make test-suite FILTER=x - Run only tests matching pattern
test-suite: stradac $(RUNTIME_OBJ)
	@echo "=== Running Strada Test Suite ==="
	@OPTS=""; \
	if [ "$(V)" = "1" ]; then OPTS="$$OPTS -v"; fi; \
	if [ "$(TAP)" = "1" ]; then OPTS="$$OPTS -t"; fi; \
	if [ -n "$(FILTER)" ]; then OPTS="$$OPTS $(FILTER)"; fi; \
	./t/run_tests.sh $$OPTS

# Build tools (stradadoc, strada-soinfo, strada-md2man, strada-md2html, strada-jit, stradapp, perl2strada, xs2strada)
TOOL_BINS = tools/stradadoc tools/strada-soinfo tools/strada-md2man tools/strada-md2html tools/strada-jit tools/stradapp tools/perl2strada tools/xs2strada tools/cpan2strada

tools: $(TOOL_BINS)
	@echo ""
	@echo "✓ Tools built in tools/"

tools/stradadoc: tools/stradadoc.strada stradac
	@echo "Building stradadoc..."
	@./strada tools/stradadoc.strada -o tools/stradadoc

tools/strada-soinfo: tools/strada-soinfo.strada stradac
	@echo "Building strada-soinfo..."
	@./strada tools/strada-soinfo.strada -o tools/strada-soinfo

tools/strada-md2man: tools/strada-md2man.strada stradac
	@echo "Building strada-md2man..."
	@./strada tools/strada-md2man.strada -o tools/strada-md2man

tools/strada-md2html: tools/strada-md2html.strada stradac
	@echo "Building strada-md2html..."
	@./strada tools/strada-md2html.strada -o tools/strada-md2html

tools/strada-jit: tools/strada-jit.strada stradac $(RUNTIME_TCC_OBJ)
	@echo "Building strada-jit..."
	@./strada tools/strada-jit.strada -o tools/strada-jit -l readline

tools/stradapp: tools/stradapp.strada stradac
	@echo "Building stradapp (preprocessor)..."
	@./strada tools/stradapp.strada -o tools/stradapp

tools/perl2strada: tools/perl2strada.strada stradac
	@echo "Building perl2strada (Perl to Strada converter)..."
	@./strada tools/perl2strada.strada -o tools/perl2strada

tools/xs2strada: tools/xs2strada.strada stradac
	@echo "Building xs2strada (XS to Strada converter)..."
	@./strada tools/xs2strada.strada -o tools/xs2strada

tools/cpan2strada: tools/cpan2strada.strada stradac tools/perl2strada tools/xs2strada
	@echo "Building cpan2strada (CPAN dist to Strada converter)..."
	@./strada tools/cpan2strada.strada -o tools/cpan2strada

# =============================================================================
# Library Targets
# =============================================================================
# Build shared libraries with detected dependencies from ./configure
# Run ./configure first to generate config.mk with proper flags

# Check if configure has been run
configure-check:
	@if [ ! -f config.mk ]; then \
		echo ""; \
		echo "Warning: config.mk not found. Run ./configure first for optimal builds."; \
		echo "Using default settings (SQLite only for DBI)."; \
		echo ""; \
	fi

# Build all libraries
libs: configure-check lib-dbi lib-crypt lib-ssl lib-readline
	@echo ""
	@echo "✓ Libraries built in lib/"

# DBI library (database interface)
lib/DBI.so: lib/DBI.strada stradac configure-check
	@echo "Building DBI library..."
	@if [ "$(HAVE_MYSQL)" = "1" ] || [ "$(HAVE_POSTGRES)" = "1" ] || [ "$(HAVE_SQLITE)" = "1" ]; then \
		./strada --shared $(DBI_DEFINES) lib/DBI.strada $(DBI_LIBS) -o lib/DBI.so; \
	else \
		echo "  No database drivers detected. Run ./configure."; \
		exit 1; \
	fi
	@echo "  DBI drivers: MySQL=$(HAVE_MYSQL) SQLite=$(HAVE_SQLITE) PostgreSQL=$(HAVE_POSTGRES)"

lib-dbi: lib/DBI.so

# Crypt library (password hashing)
lib/crypt.so: lib/crypt.strada stradac
	@echo "Building crypt library..."
	./strada --shared lib/crypt.strada $(CRYPT_LIBS) -o lib/crypt.so

lib-crypt: lib/crypt.so

# SSL library
lib/ssl.so: lib/ssl.strada stradac
	@echo "Building SSL library..."
	./strada --shared lib/ssl.strada $(SSL_LIBS) -o lib/ssl.so

lib-ssl: lib/ssl.so

# Readline library
lib/readline/readline.so: lib/readline/readline.strada stradac
	@echo "Building readline library..."
	./strada --shared lib/readline/readline.strada $(READLINE_LIBS) -o lib/readline/readline.so

lib-readline: lib/readline/readline.so

# =============================================================================
# Installation
# =============================================================================

# Install to a directory (default: /usr/local)
# Usage: make install PREFIX=/path/to/install
# Expand ~ to $(HOME) so paths work in installed scripts
PREFIX_ABS = $(subst ~,$(HOME),$(PREFIX))
INSTALL_BIN = $(PREFIX_ABS)/bin
INSTALL_LIB = $(PREFIX_ABS)/lib/strada
INSTALL_DOC = $(PREFIX_ABS)/share/doc/strada
INSTALL_MAN = $(PREFIX_ABS)/share/man/man1

install: stradac $(RUNTIME_OBJ)
	@echo "=== Installing Strada to $(PREFIX) ==="
	@mkdir -p $(INSTALL_BIN)
	@mkdir -p $(INSTALL_LIB)/runtime
	@mkdir -p $(INSTALL_LIB)/lib
	@mkdir -p $(INSTALL_DOC)
	@mkdir -p $(INSTALL_MAN)
	@# Install the compiler
	install -m 755 stradac $(INSTALL_BIN)/stradac
	@# Install the wrapper script (modified for installed paths)
	@# Use LIB_PATHS_LOW so installed libs have lowest precedence (project libs can override)
	@sed -e 's|SCRIPT_DIR=.*|SCRIPT_DIR="$(INSTALL_LIB)"|' \
	     -e 's|STRADAC=.*|STRADAC="$(INSTALL_BIN)/stradac"|' \
	     -e 's|REPL_DIR=.*|REPL_DIR="$(INSTALL_BIN)"|' \
	     -e 's|LIB_PATHS_LOW=()|LIB_PATHS_LOW=("$(INSTALL_LIB)/lib")|' \
	     strada > $(INSTALL_BIN)/strada
	@chmod 755 $(INSTALL_BIN)/strada
	@# Install config.sh (generated by ./configure, needed for PCRE2 and other library flags)
	@if [ -f config.sh ]; then \
		install -m 644 config.sh $(INSTALL_LIB)/config.sh; \
	fi
	@# Install runtime files
	install -m 644 runtime/strada_runtime.c $(INSTALL_LIB)/runtime/
	install -m 644 runtime/strada_runtime.h $(INSTALL_LIB)/runtime/
	install -m 644 runtime/strada_runtime.o $(INSTALL_LIB)/runtime/
	@# Install bundled PCRE2 (static library + header for runtime compilation)
	@if [ -f vendor/pcre2/libpcre2-8.a ]; then \
		echo "Installing bundled PCRE2..."; \
		mkdir -p $(INSTALL_LIB)/vendor/pcre2/src; \
		install -m 644 vendor/pcre2/libpcre2-8.a $(INSTALL_LIB)/vendor/pcre2/; \
		install -m 644 vendor/pcre2/src/pcre2.h $(INSTALL_LIB)/vendor/pcre2/src/; \
	fi
	@# Install standard library (Strada modules and shared libraries)
	@echo "Installing standard library..."
	@if [ -d lib ]; then \
		for ext in strada so c h; do \
			find lib -name "*.$$ext" | while read f; do \
				dir=$$(dirname "$$f" | sed 's|^lib|$(INSTALL_LIB)/lib|'); \
				mkdir -p "$$dir"; \
				if [ "$$ext" = "so" ]; then \
					install -m 755 "$$f" "$$dir/"; \
				else \
					install -m 644 "$$f" "$$dir/"; \
				fi; \
			done; \
		done; \
		find lib -name "Makefile" | while read f; do \
			dir=$$(dirname "$$f" | sed 's|^lib|$(INSTALL_LIB)/lib|'); \
			mkdir -p "$$dir"; \
			install -m 644 "$$f" "$$dir/"; \
		done; \
	fi
	@# Install documentation
	@echo "Installing documentation..."
	@if [ -d docs ]; then \
		find docs -name "*.md" | while read f; do \
			install -m 644 "$$f" $(INSTALL_DOC)/; \
		done; \
	fi
	@# Build and install tools
	@echo "Installing tools..."
	@for tool in stradadoc strada-soinfo strada-md2man strada-md2html strada-jit stradapp; do \
		if [ ! -x tools/$$tool ]; then \
			echo "  Building $$tool..."; \
			./strada tools/$$tool.strada -o tools/$$tool 2>/dev/null || true; \
		fi; \
		if [ -x tools/$$tool ]; then \
			install -m 755 tools/$$tool $(INSTALL_BIN)/$$tool; \
		fi; \
	done
	@# Also install stradapp to lib dir (strada wrapper looks for it there)
	@if [ -x tools/stradapp ]; then \
		install -m 755 tools/stradapp $(INSTALL_LIB)/stradapp; \
	fi
	@# Build and install the interpreter
	@echo "Installing interpreter..."
	@if [ ! -x interpreter/strada-interp ]; then \
		echo "  Building strada-interp..."; \
		cd interpreter && $(MAKE) 2>/dev/null || true; \
		cd ..; \
	fi
	@if [ -x interpreter/strada-interp ]; then \
		install -m 755 interpreter/strada-interp $(INSTALL_BIN)/strada-interp; \
	fi
	@# Install TCC runtime for REPL
	@if [ -f $(RUNTIME_TCC_OBJ) ]; then \
		install -m 644 $(RUNTIME_TCC_OBJ) $(INSTALL_LIB)/runtime/; \
	fi
	@if [ -f $(RUNTIME_DIR)/strada_runtime_tcc.h ]; then \
		install -m 644 $(RUNTIME_DIR)/strada_runtime_tcc.h $(INSTALL_LIB)/runtime/; \
	fi
	@# Build and install man pages
	@echo "Installing man pages..."
	@if [ -x tools/strada-md2man ]; then \
		for manmd in docs/stradac.1.md docs/strada.1.md; do \
			if [ -f "$$manmd" ]; then \
				name=$$(basename "$$manmd" .1.md); \
				echo "  Generating $$name.1..."; \
				./tools/strada-md2man --name "$$name" --section 1 --center "Strada Programming Language" "$$manmd" $(INSTALL_MAN)/$$name.1; \
			fi; \
		done; \
	else \
		echo "  Warning: strada-md2man not available, skipping man pages"; \
	fi
	@echo ""
	@echo "✓ Strada installed to $(PREFIX)"
	@echo "  Compiler: $(INSTALL_BIN)/stradac"
	@echo "  Wrapper:  $(INSTALL_BIN)/strada"
	@echo "  Interp:   $(INSTALL_BIN)/strada-interp"
	@echo "  Tools:    $(INSTALL_BIN)/stradadoc, strada-soinfo, strada-md2man, strada-md2html"
	@echo "  Runtime:  $(INSTALL_LIB)/runtime/"
	@echo "  Library:  $(INSTALL_LIB)/lib/"
	@echo "  Docs:     $(INSTALL_DOC)/"
	@echo "  Man:      $(INSTALL_MAN)/"
	@echo ""
	@echo "Make sure $(INSTALL_BIN) is in your PATH"

uninstall:
	@echo "=== Uninstalling Strada from $(PREFIX) ==="
	rm -f $(INSTALL_BIN)/stradac
	rm -f $(INSTALL_BIN)/strada
	rm -f $(INSTALL_BIN)/stradadoc
	rm -f $(INSTALL_BIN)/strada-soinfo
	rm -f $(INSTALL_BIN)/strada-md2man
	rm -f $(INSTALL_BIN)/strada-md2html
	rm -f $(INSTALL_BIN)/strada-jit
	rm -f $(INSTALL_BIN)/strada-interp
	rm -rf $(INSTALL_LIB)
	rm -rf $(INSTALL_DOC)
	rm -f $(INSTALL_MAN)/stradac.1
	rm -f $(INSTALL_MAN)/strada.1
	@echo "✓ Strada uninstalled"

# Clean build artifacts
clean:
	rm -f $(RUNTIME_DIR)/test_runtime
	rm -f $(RUNTIME_OBJ)
	rm -f $(RUNTIME_DIR)/_rt_base.o
	rm -f $(RUNTIME_TCC_OBJ)
	rm -f $(BOOTSTRAP_DIR)/*.o $(BOOTSTRAP_DIR)/stradac
	rm -f $(COMPILER_DIR)/Combined.strada $(COMPILER_DIR)/Combined.c $(COMPILER_DIR)/Combined_stage1.c
	rm -f $(COMPILER_DIR)/*.o
	rm -f stradac_stage1
	rm -f $(EXAMPLES_DIR)/*.c $(EXAMPLES_DIR)/*.o
	rm -f $(EXAMPLES_DIR)/test_simple $(EXAMPLES_DIR)/test_*[!.strada]
	find $(EXAMPLES_DIR) -maxdepth 1 -type f -executable -delete
	rm -rf build/*
	rm -f stradac
	rm -f $(TOOL_BINS)
	rm -f lib/readline.so
	@if [ -f vendor/pcre2/Makefile ]; then $(MAKE) -C vendor/pcre2 clean; fi
	@echo "✓ Cleaned"

# Help target
help:
	@echo "Strada Language Build System"
	@echo ""
	@echo "Quick Start:"
	@echo "  ./configure   - Detect dependencies (MySQL, PostgreSQL, SSL, etc.)"
	@echo "  make          - Build compiler and tools"
	@echo "  make libs     - Build shared libraries (DBI, crypt, ssl, readline)"
	@echo "  make install  - Install to system"
	@echo ""
	@echo "Primary Targets:"
	@echo "  all           - Build self-hosting compiler (default)"
	@echo "  tools         - Build tools (stradadoc, strada-soinfo, etc.)"
	@echo "  libs          - Build all shared libraries"
	@echo "  install       - Install to system (default: /usr/local)"
	@echo "  uninstall     - Remove installed files"
	@echo "  run PROG=x    - Compile and run example with self-hosting compiler"
	@echo "  examples      - Build all examples with self-hosting compiler"
	@echo "  test          - Test the runtime system"
	@echo "  test-all      - Run full test suite (runtime + selfhost + examples)"
	@echo "  clean         - Remove all build artifacts"
	@echo ""
	@echo "Library Targets (run ./configure first):"
	@echo "  libs          - Build all libraries"
	@echo "  lib-dbi       - Build DBI library (MySQL/SQLite/PostgreSQL)"
	@echo "  lib-crypt     - Build crypt library (password hashing)"
	@echo "  lib-ssl       - Build SSL library (TLS support)"
	@echo "  lib-readline  - Build readline library (line editing)"
	@echo ""
	@echo "Installation:"
	@echo "  make install                    # Install to /usr/local"
	@echo "  make install PREFIX=/opt/strada # Install to custom directory"
	@echo "  make install PREFIX=~/.local    # Install to home directory"
	@echo "  sudo make uninstall             # Remove installation"
	@echo ""
	@echo "Testing Targets:"
	@echo "  test          - Test the runtime system"
	@echo "  test-selfhost - Test self-compilation (stage 2)"
	@echo "  test-examples - Test all examples compile and link"
	@echo "  test-all      - Run all tests (runtime + selfhost + examples)"
	@echo "  test-suite    - Run comprehensive test suite (82+ tests)"
	@echo "                  Options: V=1 (verbose), TAP=1 (TAP format), FILTER=pattern"
	@echo "  test-interp   - Run interpreter compatibility tests against compiler test suite"
	@echo ""
	@echo "Interpreter:"
	@echo "  interpreter   - Build the tree-walking interpreter (interpreter/strada-interp)"
	@echo ""
	@echo "Development Targets:"
	@echo "  bootstrap     - Build C bootstrap compiler"
	@echo "  run-bootstrap - Compile example with bootstrap compiler"
	@echo "  compiler      - Build self-hosting compiler"
	@echo ""
	@echo "Quick Start:"
	@echo "  make                       # Build everything"
	@echo "  ./strada -r hello.strada   # Compile and run a program"
	@echo "  make run PROG=test_simple  # Compile and run an example"
	@echo ""
	@echo "Tools (after 'make tools'):"
	@echo "  ./strada               - One-step compiler (strada -> executable)"
	@echo "  ./stradac              - Strada to C compiler"
	@echo "  tools/stradadoc        - View Strada documentation"
	@echo "  tools/strada-soinfo    - Inspect Strada shared libraries"
	@echo "  tools/strada-md2man    - Convert Markdown to man pages"
	@echo "  tools/strada-md2html   - Convert Markdown to HTML"
	@echo "  tools/strada-jit      - Interactive REPL (Read-Eval-Print Loop)"

# Additional info
info:
	@echo "Strada Language - A Strongly-Typed, Self-Hosting Language"
	@echo ""
	@echo "Architecture:"
	@echo "  1. Bootstrap Compiler (C)         -> bootstrap/stradac"
	@echo "  2. Self-Hosting Compiler (Strada) -> ./stradac"
	@echo ""
	@echo "The self-hosting compiler is the PRIMARY compiler."
	@echo "The bootstrap compiler is frozen and only used to build the self-hosting compiler."
	@echo ""
	@echo "Self-Hosting Compiler Source:"
	@echo "  compiler/AST.strada     - AST node definitions"
	@echo "  compiler/Lexer.strada   - Tokenizer"
	@echo "  compiler/Parser.strada  - Parser"
	@echo "  compiler/CodeGen.strada - C code generator"
	@echo "  compiler/Main.strada    - Entry point"
	@echo ""
	@echo "Documentation:"
	@echo "  docs/LANGUAGE_GUIDE.md        - Complete language tutorial"
	@echo "  docs/QUICK_REFERENCE.md       - Syntax cheat sheet"
	@echo "  docs/COMPILER_ARCHITECTURE.md - How the compiler works"
	@echo "  docs/RUNTIME_API.md           - Runtime library reference"
