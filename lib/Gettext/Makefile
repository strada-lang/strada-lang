# Makefile for Strada Gettext wrapper library
#
# This library wraps GNU gettext functions for use with Strada's extern "C".
#
# Requirements: GNU gettext (usually part of glibc on Linux)
#   Ubuntu/Debian: Usually built-in, or: apt install gettext
#   RHEL/CentOS:   Usually built-in, or: yum install gettext
#   macOS:         brew install gettext
#
# Usage (static linking - recommended):
#   make              # builds gettext_wrapper.o
#   gcc -o myapp myapp.c gettext_wrapper.o runtime/strada_runtime.c ...
#
# Usage (dynamic linking - legacy):
#   make shared       # builds libgettext_wrapper.so

CC = gcc
CFLAGS = -Wall -Wextra -O2

# Object file for static linking (recommended with extern "C")
OBJ = gettext_wrapper.o

# Shared library for dynamic linking (legacy FFI)
LIB = libgettext_wrapper.so

# Source files
SRC = gettext_wrapper.c

.PHONY: all clean shared test

# Default: build object file for static linking
all: $(OBJ)

$(OBJ): $(SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

# Build shared library (for legacy FFI usage)
shared: $(LIB)

$(LIB): $(SRC)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $<

clean:
	rm -f $(OBJ) $(LIB)

test: $(OBJ)
	@echo "Object file built successfully: $(OBJ)"
	@ls -la $(OBJ)
	@echo ""
	@echo "Symbols:"
	@nm $(OBJ) | grep ' T ' | head -20
