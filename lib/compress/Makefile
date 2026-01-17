# Makefile for Strada compression library

CC = gcc
CFLAGS = -shared -fPIC -Wall -O2
LDFLAGS = -lz

TARGET = libstrada_compress.so
SOURCES = strada_compress.c

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o

test: $(TARGET)
	@echo "Compression library built: $(TARGET)"
	@ls -la $(TARGET)

.PHONY: all clean test
