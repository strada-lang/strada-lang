# Makefile for bundled PCRE2-8 static library
# Builds libpcre2-8.a with minimal config (8-bit only, no JIT, no PCRE2-16/32)

CC ?= gcc
CFLAGS ?= -O2
CFLAGS += -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8 -DPCRE2_STATIC -I. -Isrc
AR ?= ar

# Core PCRE2-8 source files (no JIT, no POSIX wrapper, no fuzzer, no dftables)
SRCS = src/pcre2_auto_possess.c \
       src/pcre2_chkdint.c \
       src/pcre2_compile.c \
       src/pcre2_config.c \
       src/pcre2_context.c \
       src/pcre2_convert.c \
       src/pcre2_dfa_match.c \
       src/pcre2_error.c \
       src/pcre2_extuni.c \
       src/pcre2_find_bracket.c \
       src/pcre2_maketables.c \
       src/pcre2_match.c \
       src/pcre2_match_data.c \
       src/pcre2_newline.c \
       src/pcre2_ord2utf.c \
       src/pcre2_pattern_info.c \
       src/pcre2_script_run.c \
       src/pcre2_serialize.c \
       src/pcre2_string_utils.c \
       src/pcre2_study.c \
       src/pcre2_substitute.c \
       src/pcre2_substring.c \
       src/pcre2_tables.c \
       src/pcre2_ucd.c \
       src/pcre2_ucptables.c \
       src/pcre2_valid_utf.c \
       src/pcre2_xclass.c \
       src/pcre2_chartables.c

OBJS = $(SRCS:.c=.o)

all: setup libpcre2-8.a

# Copy generic headers/tables if not already present
setup:
	@if [ ! -f config.h ]; then cp src/config.h.generic config.h; fi
	@if [ ! -f src/pcre2.h ]; then cp src/pcre2.h.generic src/pcre2.h; fi
	@if [ ! -f src/pcre2_chartables.c ]; then cp src/pcre2_chartables.c.dist src/pcre2_chartables.c; fi

libpcre2-8.a: setup $(OBJS)
	$(AR) rcs $@ $(OBJS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) libpcre2-8.a

distclean: clean
	rm -f config.h src/pcre2.h src/pcre2_chartables.c

.PHONY: all setup clean distclean
