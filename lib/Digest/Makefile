# Makefile for Strada Digest libraries (Digest::MD5, Digest::SHA)
#
# Build: make
# Clean: make clean

STRADA = ../../strada
CC ?= gcc
TARGETS = MD5.o SHA.o

.PHONY: all clean

all: $(TARGETS)

%.o: %.strada
	$(STRADA) -M $<
	@echo "Built: $@"

clean:
	rm -f $(TARGETS) *.so *.c
