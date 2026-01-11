# Makefile for Strada libtcc wrapper
# Requires: libtcc-dev (sudo apt install libtcc-dev)

STRADA = ../../strada
CC = gcc
CFLAGS = -fPIC -O2 -I../../runtime

.PHONY: all clean install

all: tcc.so

tcc.so: tcc.strada
	$(STRADA) --shared tcc.strada -l tcc -l dl
	@echo "Built tcc.so"
	@echo "Install libtcc-dev if you get linker errors:"
	@echo "  sudo apt install libtcc-dev"

clean:
	rm -f tcc.so tcc.c tcc.o

install: tcc.so
	cp tcc.so ..

test: tcc.so
	$(STRADA) -r example.strada
