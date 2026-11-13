# Makefile for Strada USB library
#
# Requirements: libusb-1.0 development headers
#   Ubuntu/Debian: apt install libusb-1.0-0-dev
#   RHEL/CentOS:   yum install libusb1-devel
#   macOS:         brew install libusb

-include ../../config.mk

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    SHARED_LDFLAGS = -dynamiclib -Wl,-undefined,dynamic_lookup
else
    SHARED_LDFLAGS = -shared
endif

CC ?= gcc
CFLAGS = -Wall -Wextra -fPIC -O2 -I../../runtime $(LIBUSB_CFLAGS)
LIBUSB_LIBS ?= -lusb-1.0
LDFLAGS = $(SHARED_LDFLAGS) $(LIBUSB_LIBS)

# Output library
LIB = libstrada_usb.so

# Source files
SRC = strada_usb.c

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
