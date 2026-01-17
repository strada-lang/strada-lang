# Cannoli - Preforking Web Server & Framework for Strada
#
# Build: make
# Test:  make test
# Clean: make clean

STRADA_DIR := ..
STRADAC := $(STRADA_DIR)/stradac
RUNTIME_SRC := $(STRADA_DIR)/runtime/strada_runtime.c
RUNTIME_OBJ := $(STRADA_DIR)/runtime/strada_runtime.o
RUNTIME_INC := $(STRADA_DIR)/runtime

SRC_DIR := src
BUILD_DIR := build

CC := gcc
CFLAGS := -Wall -Wextra -Wno-unused-variable -Wno-unused-parameter -O2
LDFLAGS := -ldl -lm -lpthread -lz -rdynamic

# Source files in order of dependency
# Note: compress.strada has 'package compress;' so must come after files that
# don't have package declarations (otherwise they inherit the compress package)
SOURCES := \
	$(SRC_DIR)/config.strada \
	$(SRC_DIR)/mime.strada \
	$(SRC_DIR)/session.strada \
	$(SRC_DIR)/template.strada \
	$(SRC_DIR)/validation.strada \
	$(SRC_DIR)/request.strada \
	$(SRC_DIR)/log.strada \
	$(SRC_DIR)/response.strada \
	$(SRC_DIR)/router.strada \
	$(SRC_DIR)/static.strada \
	$(SRC_DIR)/server.strada \
	$(SRC_DIR)/fastcgi.strada \
	$(SRC_DIR)/app.strada \
	$(SRC_DIR)/main.strada \
	$(STRADA_DIR)/lib/compress.strada \
	$(SRC_DIR)/cannoli_obj.strada

# Library sources (without main.strada) - for building example apps
LIB_SOURCES := \
	$(SRC_DIR)/config.strada \
	$(SRC_DIR)/log.strada \
	$(SRC_DIR)/mime.strada \
	$(SRC_DIR)/session.strada \
	$(SRC_DIR)/template.strada \
	$(SRC_DIR)/validation.strada \
	$(SRC_DIR)/request.strada \
	$(SRC_DIR)/response.strada \
	$(SRC_DIR)/router.strada \
	$(SRC_DIR)/static.strada \
	$(SRC_DIR)/server.strada \
	$(SRC_DIR)/fastcgi.strada \
	$(SRC_DIR)/app.strada \
	$(STRADA_DIR)/lib/compress.strada \
	$(SRC_DIR)/cannoli_obj.strada

# Combined source file
COMBINED := $(BUILD_DIR)/cannoli.strada
COMBINED_C := $(BUILD_DIR)/cannoli.c

# Target binary
TARGET := cannoli

.PHONY: all clean test examples

all: $(TARGET)

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Check that Strada compiler exists
$(STRADAC):
	@echo "Error: Strada compiler not found at $(STRADAC)"
	@echo "Run 'make' in the parent directory first"
	@exit 1

# Combine all source files
$(COMBINED): $(SOURCES) | $(BUILD_DIR)
	@echo "Combining source files..."
	@cat $(SOURCES) > $(COMBINED)

# Compile Strada to C
$(COMBINED_C): $(COMBINED) $(STRADAC)
	@echo "Compiling Strada to C..."
	@$(STRADAC) $(COMBINED) $(COMBINED_C)

# Build pre-compiled runtime if needed
$(RUNTIME_OBJ): $(RUNTIME_SRC)
	@echo "Building pre-compiled runtime..."
	$(CC) -O2 -std=c99 -c $(RUNTIME_SRC) -I$(RUNTIME_INC) -o $(RUNTIME_OBJ)

# Build the binary
$(TARGET): $(COMBINED_C) $(RUNTIME_OBJ)
	@echo "Building $(TARGET)..."
	@$(CC) $(CFLAGS) -o $(TARGET) $(COMBINED_C) $(RUNTIME_OBJ) -I$(RUNTIME_INC) $(LDFLAGS)
	@echo "Built: ./$(TARGET)"

# Run tests
test: $(TARGET)
	@chmod +x t/run_tests.sh build_test.sh
	@./t/run_tests.sh

# Build example applications
examples: $(TARGET) $(RUNTIME_OBJ)
	@echo "Building examples..."
	@for ex in examples/*.strada; do \
		name=$$(basename $$ex .strada); \
		echo "  Building $$name..."; \
		cat $(LIB_SOURCES) $$ex > $(BUILD_DIR)/$$name.strada; \
		$(STRADAC) $(BUILD_DIR)/$$name.strada $(BUILD_DIR)/$$name.c 2>/dev/null || true; \
		if [ -f $(BUILD_DIR)/$$name.c ]; then \
			$(CC) $(CFLAGS) -o $(BUILD_DIR)/$$name $(BUILD_DIR)/$$name.c $(RUNTIME_OBJ) -I$(RUNTIME_INC) $(LDFLAGS) 2>/dev/null || true; \
		fi \
	done
	@echo "Examples built in $(BUILD_DIR)/"

# Build example shared library
example-lib:
	@echo "Building example library..."
	$(CC) -shared -fPIC -o examples/myapp.so examples/myapp.c
	@echo "Built: examples/myapp.so"
	@echo ""
	@echo "To use: ./cannoli --library examples/myapp.so --dev"
	@echo ""
	@echo "Note: cannoli must be compiled with -rdynamic for the library"
	@echo "      to access strada_to_str(). This is automatic with ./strada wrapper."

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET)
	rm -f /tmp/cannoli_*

# Install to /usr/local/bin
install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

# Development: run with auto-reload
dev: $(TARGET)
	./$(TARGET) --dev

# Help
help:
	@echo "Cannoli Makefile targets:"
	@echo "  make          - Build the cannoli binary"
	@echo "  make test     - Run the test suite"
	@echo "  make examples - Build example applications"
	@echo "  make clean    - Remove build artifacts"
	@echo "  make install  - Install to /usr/local/bin"
	@echo "  make dev      - Run in development mode"
	@echo ""
	@echo "After building, run: ./cannoli --help"
