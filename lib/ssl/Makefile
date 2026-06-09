# Makefile for Strada SSL library
#
# Requirements: OpenSSL development headers
#   Ubuntu/Debian: apt install libssl-dev
#   RHEL/CentOS:   yum install openssl-devel
#   macOS:         brew install openssl

CC = gcc
CFLAGS = -Wall -Wextra -fPIC -O2 -I../../runtime
LDFLAGS = -shared -lssl -lcrypto

# Output library
LIB = libstrada_ssl.so

# Source files
SRC = strada_ssl.c

.PHONY: all clean install

all: $(LIB)

$(LIB): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(LIB) *.o

# Install to lib directory (relative to strada root)
install: $(LIB)
	cp $(LIB) ../

# Test compilation
test: $(LIB)
	@echo "Library built successfully: $(LIB)"
	@ls -la $(LIB)
