# Makefile for Strada Language
# 
# Build order:
#   1. Bootstrap compiler (C) - builds bootstrap/stradac
#   2. Self-hosting compiler (Strada) - uses bootstrap to compile itself
#   3. Examples use self-hosting compiler (./stradac)
#

CC = gcc
CFLAGS = -Wall -Wextra -Wno-unused-variable -Wno-return-type -Wno-unused-but-set-variable -Wno-unused-result -Wno-comment -O2 -std=c99
LDFLAGS = -ldl -lm -lpthread
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

.PHONY: all clean test test-all test-examples test-selfhost test-suite runtime bootstrap compiler examples run run-bootstrap help info selfhost install uninstall tools

# Default: build everything including self-hosting compiler
all: stradac $(RUNTIME_OBJ)
	@echo ""
	@echo "✓ Build complete!"
	@echo "  Primary compiler: ./stradac (self-hosting, written in Strada)"
	@echo "  Bootstrap compiler: ./bootstrap/stradac (written in C)"
	@echo ""
	@echo "Usage: ./stradac input.strada output.c"
	@echo "   or: make run PROG=test_simple"

# Pre-compiled runtime object (for faster program compilation)
$(RUNTIME_OBJ): $(RUNTIME_SRC) $(RUNTIME_HDR)
	@echo "=== Building pre-compiled runtime ==="
	$(CC) $(CFLAGS) -c $(RUNTIME_SRC) -I$(RUNTIME_DIR) -o $(RUNTIME_OBJ)

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

# Compile combined source to C using bootstrap compiler
$(COMPILER_DIR)/Combined.c: $(COMPILER_DIR)/Combined.strada bootstrap
	@echo "=== Compiling self-hosting compiler (Strada -> C) ==="
	$(BOOTSTRAP_STRADAC) $(COMPILER_DIR)/Combined.strada $(COMPILER_DIR)/Combined.c

# Build the self-hosting compiler executable
stradac: $(COMPILER_DIR)/Combined.c $(RUNTIME_OBJ)
	@echo "=== Building self-hosting compiler executable ==="
	$(CC) $(CFLAGS) -o stradac $(COMPILER_DIR)/Combined.c $(RUNTIME_OBJ) -I$(RUNTIME_DIR) $(LDFLAGS)
	@echo "✓ Self-hosting compiler built: ./stradac"

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

# Build tools (stradadoc, strada-soinfo, strada-md2man, strada-md2html)
TOOLS = stradadoc strada-soinfo strada-md2man strada-md2html

tools: all
	@echo "=== Building tools ==="
	@for tool in $(TOOLS); do \
		echo "Building $$tool..."; \
		./strada tools/$$tool.strada -o tools/$$tool; \
	done
	@echo ""
	@echo "✓ Tools built in tools/"

# Install to a directory (default: /usr/local)
# Usage: make install PREFIX=/path/to/install
PREFIX ?= /usr/local
INSTALL_BIN = $(PREFIX)/bin
INSTALL_LIB = $(PREFIX)/lib/strada
INSTALL_DOC = $(PREFIX)/share/doc/strada
INSTALL_MAN = $(PREFIX)/share/man/man1

install: all
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
	     -e 's|LIB_PATHS_LOW=()|LIB_PATHS_LOW=("$(INSTALL_LIB)/lib")|' \
	     strada > $(INSTALL_BIN)/strada
	@chmod 755 $(INSTALL_BIN)/strada
	@# Install runtime files
	install -m 644 runtime/strada_runtime.c $(INSTALL_LIB)/runtime/
	install -m 644 runtime/strada_runtime.h $(INSTALL_LIB)/runtime/
	install -m 644 runtime/strada_runtime.o $(INSTALL_LIB)/runtime/
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
	@for tool in stradadoc strada-soinfo strada-md2man strada-md2html; do \
		if [ ! -x tools/$$tool ]; then \
			echo "  Building $$tool..."; \
			./strada tools/$$tool.strada -o tools/$$tool 2>/dev/null || true; \
		fi; \
		if [ -x tools/$$tool ]; then \
			install -m 755 tools/$$tool $(INSTALL_BIN)/$$tool; \
		fi; \
	done
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
	rm -rf $(INSTALL_LIB)
	rm -rf $(INSTALL_DOC)
	rm -f $(INSTALL_MAN)/stradac.1
	rm -f $(INSTALL_MAN)/strada.1
	@echo "✓ Strada uninstalled"

# Clean build artifacts
clean:
	rm -f $(RUNTIME_DIR)/test_runtime
	rm -f $(RUNTIME_OBJ)
	rm -f $(BOOTSTRAP_DIR)/*.o $(BOOTSTRAP_DIR)/stradac
	rm -f $(COMPILER_DIR)/Combined.strada $(COMPILER_DIR)/Combined.c
	rm -f $(COMPILER_DIR)/*.o
	rm -f $(EXAMPLES_DIR)/*.c $(EXAMPLES_DIR)/*.o
	rm -f $(EXAMPLES_DIR)/test_simple $(EXAMPLES_DIR)/test_*[!.strada]
	find $(EXAMPLES_DIR) -maxdepth 1 -type f -executable -delete
	rm -rf build/*
	rm -f stradac
	@echo "✓ Cleaned"

# Help target
help:
	@echo "Strada Language Build System"
	@echo ""
	@echo "Primary Targets:"
	@echo "  all           - Build self-hosting compiler (default)"
	@echo "  tools         - Build tools (stradadoc, strada-soinfo, etc.)"
	@echo "  install       - Install to system (default: /usr/local)"
	@echo "  uninstall     - Remove installed files"
	@echo "  run PROG=x    - Compile and run example with self-hosting compiler"
	@echo "  examples      - Build all examples with self-hosting compiler"
	@echo "  test          - Test the runtime system"
	@echo "  test-all      - Run full test suite (runtime + selfhost + examples)"
	@echo "  clean         - Remove all build artifacts"
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
