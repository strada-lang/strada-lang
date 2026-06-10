# Makefile for building HTML documentation from Markdown
#
# Usage:
#   make          - Build all HTML files
#   make clean    - Remove generated HTML files
#   make html/INDEX.html  - Build a specific file

# The strada-md2html tool (built from tools/strada-md2html.strada)
MD2HTML := ../tools/strada-md2html

# Find all markdown files
MD_FILES := $(wildcard *.md)
HTML_FILES := $(patsubst %.md,html/%.html,$(MD_FILES))

# Default target: build all HTML files
all: $(MD2HTML) html $(HTML_FILES)

# Build the strada-md2html tool if needed
$(MD2HTML): ../tools/strada-md2html.strada
	@echo "Building strada-md2html tool..."
	cd .. && ./strada tools/strada-md2html.strada -o tools/strada-md2html

# Create the html output directory
html:
	@mkdir -p html

# Pattern rule: convert .md to .html
html/%.html: %.md $(MD2HTML)
	@echo "Converting $< -> $@"
	@$(MD2HTML) $< $@

# Clean up generated files
clean:
	rm -rf html

# Rebuild everything
rebuild: clean all

# List available targets
help:
	@echo "Strada Documentation Builder"
	@echo ""
	@echo "Targets:"
	@echo "  make          - Build all HTML documentation"
	@echo "  make clean    - Remove generated HTML files"
	@echo "  make rebuild  - Clean and rebuild everything"
	@echo "  make help     - Show this help"
	@echo ""
	@echo "Source files:"
	@for f in $(MD_FILES); do echo "  $$f"; done
	@echo ""
	@echo "Output files (in html/):"
	@for f in $(HTML_FILES); do echo "  $$f"; done

.PHONY: all clean rebuild help html
