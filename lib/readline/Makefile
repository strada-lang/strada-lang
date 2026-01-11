# Makefile for Strada readline library
# Requires: libreadline-dev (apt install libreadline-dev)

STRADA = ../../strada
CC = gcc
CFLAGS = -fPIC -O2 -I../../runtime
LDFLAGS = -shared -lreadline

.PHONY: all clean install

all: readline.so

readline.so: readline.strada
	$(STRADA) --shared readline.strada
	@echo "Built readline.so"
	@echo "Install libreadline-dev if you get linker errors:"
	@echo "  sudo apt install libreadline-dev"

clean:
	rm -f readline.so readline.c readline.o

install: readline.so
	cp readline.so ..

# Test the library
test: readline.so
	$(STRADA) -r example.strada
