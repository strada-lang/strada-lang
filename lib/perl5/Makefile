# Makefile for Strada Perl 5 FFI module
#
# Build: make
# Clean: make clean
#
# Requirements: libperl-dev (Debian/Ubuntu) or perl-devel (RHEL/Fedora)

CC = gcc
CFLAGS = -Wall -Wextra -fPIC -O2

# Get Perl embedding flags
PERL_CFLAGS = $(shell perl -MExtUtils::Embed -e ccopts 2>/dev/null)
PERL_LDFLAGS = $(shell perl -MExtUtils::Embed -e ldopts 2>/dev/null)

# Strada runtime include path
RUNTIME_INC = -I../../runtime

# Target
TARGET = libstrada_perl5.so

all: $(TARGET)

$(TARGET): strada_perl5.c
	@if [ -z "$(PERL_CFLAGS)" ]; then \
		echo "Error: Perl embedding not available. Install libperl-dev or perl-devel."; \
		exit 1; \
	fi
	$(CC) -shared $(CFLAGS) -o $@ $< $(RUNTIME_INC) $(PERL_CFLAGS) $(PERL_LDFLAGS)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET) *.o

test: $(TARGET)
	@echo "Library built successfully. Test with examples/test_perl5.strada"

.PHONY: all clean test
