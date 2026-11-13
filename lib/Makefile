# Makefile for building Strada libraries in lib/
#
# Usage:
#   make        # build configured core libraries (via top-level Makefile)
#   make perl5  # build Perl 5 integration C library (requires libperl-dev)
#   make usb    # build USB C library (requires libusb-1.0-0-dev)
#   make clean  # clean built library artifacts

SUBDIRS = Digest perl5 usb readline tcc
STRADA = ../strada

.PHONY: all clean $(SUBDIRS)

all:
	$(MAKE) -C .. libs

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	@rm -f *.so *.o
