# Makefile for Strada crypt library
#
# Build: make
# Clean: make clean

CC = gcc
CFLAGS = -Wall -Wextra -fPIC -O2 -I../../runtime
LDFLAGS = -shared -lcrypt

TARGET = libstrada_crypt.so
SRC = strada_crypt.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $(TARGET)"
	@echo ""
	@echo "Usage in Strada:"
	@echo "  use lib \"lib\";"
	@echo "  use crypt;"
	@echo ""
	@echo "  my str \$$hash = crypt::hash_password(\"mypassword\");"
	@echo "  if (crypt::verify(\"mypassword\", \$$hash)) { ... }"

clean:
	rm -f $(TARGET) *.o
