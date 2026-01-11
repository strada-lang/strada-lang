# Makefile for stradapm - Strada Package Manager

STRADA = ../../strada
STRADAC = ../../stradac
RUNTIME = ../../runtime

# Default target
all: stradapm

# Build the package manager
stradapm: stradapm.strada SemVer.strada Manifest.strada Lock.strada Fetch.strada Resolver.strada Build.strada
	@echo "Building stradapm..."
	$(STRADA) -L . stradapm.strada -o stradapm

# Install to project tools directory
install: stradapm
	@echo "Installing stradapm..."
	cp stradapm ../../tools/
	@echo "Installed to ../../tools/stradapm"

# Clean build artifacts
clean:
	rm -f stradapm stradapm.c *.o

# Test version parsing
test-version: SemVer.strada
	@echo "Testing version module..."
	$(STRADA) -r -L . -e 'use SemVer; \
		say("Parse 1.2.3: " . StradaPM::SemVer::to_string(StradaPM::SemVer::parse("1.2.3"))); \
		say("Compare 1.0.0 vs 2.0.0: " . StradaPM::SemVer::compare("1.0.0", "2.0.0")); \
		say("1.5.0 satisfies ^1.0.0: " . StradaPM::SemVer::satisfies("1.5.0", "^1.0.0")); \
		say("2.0.0 satisfies ^1.0.0: " . StradaPM::SemVer::satisfies("2.0.0", "^1.0.0"));'

# Test manifest parsing
test-manifest: Manifest.strada
	@echo "Testing manifest module..."
	@echo '[package]' > /tmp/test_manifest.toml
	@echo 'name = "test-pkg"' >> /tmp/test_manifest.toml
	@echo 'version = "1.0.0"' >> /tmp/test_manifest.toml
	@echo '' >> /tmp/test_manifest.toml
	@echo '[dependencies]' >> /tmp/test_manifest.toml
	@echo 'json = "^1.0"' >> /tmp/test_manifest.toml
	$(STRADA) -r -L . -e 'use Manifest; \
		my scalar $$m = StradaPM::Manifest::parse("/tmp/test_manifest.toml"); \
		say("Name: " . StradaPM::Manifest::get_name($$m)); \
		say("Version: " . StradaPM::Manifest::get_version($$m));'
	@rm -f /tmp/test_manifest.toml

.PHONY: all install clean test-version test-manifest
