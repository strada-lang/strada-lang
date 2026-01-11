# Makefile for Strada Digest::MD5 library
#
# Build: make
# Clean: make clean

CC = gcc
CFLAGS = -Wall -Wextra -fPIC -O2 -I../../runtime
LDFLAGS = -shared

TARGET = libstrada_md5.so
SRC = strada_md5.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $(TARGET)"
	@echo ""
	@echo "Usage in Strada:"
	@echo "  use lib \"lib\";"
	@echo "  use Digest::MD5;"
	@echo ""
	@echo "  my str \$$hex = Digest::MD5::md5_hex(\"Hello, World!\");"
	@echo "  say(\$$hex);  # 65a8e27d8879283831b664bd8b7f0ad4"

clean:
	rm -f $(TARGET) *.o
