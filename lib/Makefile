# Makefile for building Strada shared libraries in lib/
#
# Usage:
#   make        # build all shared libraries
#   make dbi    # build DBI library only
#   make ssl    # build SSL library only
#   make perl5  # build Perl 5 integration library only
#   make clean  # clean all libraries

SUBDIRS = dbi ssl perl5
STRADA = ../strada
LIB_STRADA = $(wildcard *.strada)
LIB_SO = $(LIB_STRADA:.strada=.so)

.PHONY: all clean strada-libs $(SUBDIRS)

all: $(SUBDIRS) strada-libs

$(SUBDIRS):
	$(MAKE) -C $@

strada-libs: $(LIB_SO)

%.so: %.strada
	$(STRADA) --shared $< -o $@

clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	@rm -f $(LIB_SO) *.o
