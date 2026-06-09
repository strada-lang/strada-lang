# Makefile for pacco - Strada Package Manager

STRADA = ../../strada
STRADAC = ../../stradac
RUNTIME = ../../runtime

# Default target
all: pacco

# Build the package manager
pacco: pacco.strada SemVer.strada Manifest.strada Lock.strada Fetch.strada Resolver.strada Build.strada
	@echo "Building pacco..."
	$(STRADA) -L . pacco.strada -o pacco

# Install to project tools directory
install: pacco
	@echo "Installing pacco..."
	cp pacco ../../tools/
	@echo "Installed to ../../tools/pacco"

# Clean build artifacts
clean:
	rm -f pacco pacco.c *.o

# Test version parsing
test-version: SemVer.strada
	@echo "Testing version module..."
	$(STRADA) -r -L . -e 'use SemVer; \
		say("Parse 1.2.3: " . Pacco::SemVer::to_string(Pacco::SemVer::parse("1.2.3"))); \
		say("Compare 1.0.0 vs 2.0.0: " . Pacco::SemVer::compare("1.0.0", "2.0.0")); \
		say("1.5.0 satisfies ^1.0.0: " . Pacco::SemVer::satisfies("1.5.0", "^1.0.0")); \
		say("2.0.0 satisfies ^1.0.0: " . Pacco::SemVer::satisfies("2.0.0", "^1.0.0"));'

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
		my scalar $$m = Pacco::Manifest::parse("/tmp/test_manifest.toml"); \
		say("Name: " . Pacco::Manifest::get_name($$m)); \
		say("Version: " . Pacco::Manifest::get_version($$m));'
	@rm -f /tmp/test_manifest.toml

.PHONY: all install clean test-version test-manifest
