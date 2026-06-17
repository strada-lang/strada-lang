# Makefile for Strada Bootstrap Compiler

CC = gcc
CFLAGS = -Wall -Wextra -Wno-unused-variable -Wno-return-type -Wno-unused-but-set-variable -Wno-unused-result -Wno-comment -O2 -std=c99 -I../runtime
LDFLAGS = 

SOURCES = main.c lexer.c parser.c ast.c codegen.c
OBJECTS = $(SOURCES:.c=.o)
TARGET = stradac

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)
	rm -f ../examples/*.c ../examples/test_simple

test: $(TARGET)
	@echo "=== Testing Bootstrap Compiler ==="
	./$(TARGET) ../examples/test_simple.strada ../examples/test_simple.c
	@echo ""
	@echo "=== Compiling Generated C Code ==="
	$(CC) -o ../examples/test_simple ../examples/test_simple.c ../runtime/strada_runtime.c -I../runtime
	@echo ""
	@echo "=== Running Compiled Program ==="
	../examples/test_simple
	@echo ""
	@echo "✓ Bootstrap compiler test passed!"

.PHONY: help
help:
	@echo "Strada Bootstrap Compiler Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all    - Build the bootstrap compiler"
	@echo "  test   - Test the compiler"
	@echo "  clean  - Remove build files"
	@echo "  help   - Show this message"
