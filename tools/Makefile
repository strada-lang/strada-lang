# Makefile for Strada tools
#
# Usage:
#   make              - Build all tools
#   make strada-soinfo - Build strada-soinfo tool
#   make clean        - Remove built tools

STRADA = ../strada

# All tools to build
TOOLS = strada-soinfo strada-md2html strada-md2man stradadoc perl2strada strada2perl

.PHONY: all clean $(TOOLS)

all: $(TOOLS)

strada-soinfo: strada-soinfo.strada
	$(STRADA) strada-soinfo.strada
	@echo "Built: strada-soinfo"

strada-md2html: strada-md2html.strada
	$(STRADA) strada-md2html.strada
	@echo "Built: strada-md2html"

strada-md2man: strada-md2man.strada
	$(STRADA) strada-md2man.strada
	@echo "Built: strada-md2man"

stradadoc: stradadoc.strada
	$(STRADA) stradadoc.strada
	@echo "Built: stradadoc"

perl2strada: perl2strada.strada
	$(STRADA) perl2strada.strada
	@echo "Built: perl2strada"

strada2perl: strada2perl.strada
	$(STRADA) strada2perl.strada
	@echo "Built: strada2perl"

clean:
	rm -f $(TOOLS) *.o *.c
	@echo "Cleaned tools directory"
