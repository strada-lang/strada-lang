# Strada Installation Guide

This guide covers building Strada from source on Unix-like systems (Linux, macOS).

## Prerequisites

### Required

- **GCC** (GNU Compiler Collection) version 7.0 or later
- **GNU Make** version 3.81 or later
- **Standard C library** with POSIX support

### Optional (for specific features)

- **pthreads** - For multithreading (usually included with glibc)
- **libdl** - For dynamic loading/FFI (usually included)
- **libm** - Math library (usually included)
- **OpenSSL** - For SSL/TLS support in networking
- **Perl 5** development headers - For Perl integration (`libperl-dev`)

### Checking Prerequisites

```bash
# Check GCC
gcc --version

# Check Make
make --version

# Check for required libraries
ldconfig -p | grep -E "(libdl|libm|libpthread)"
```

## Quick Install

```bash
# Clone the repository
git clone https://github.com/yourusername/strada.git
cd strada

# Detect system dependencies (MySQL, PostgreSQL, OpenSSL, etc.)
./configure

# Build the compiler
make

# Build libraries (DBI, crypt, ssl, readline)
make libs

# Verify it works
./strada -r examples/test_simple.strada
```

## Detailed Build Process

### Step 1: Configure (Recommended)

Run the configure script to detect available libraries:

```bash
./configure
```

This detects:
- **Database drivers**: MySQL, SQLite, PostgreSQL
- **Cryptography**: libcrypt, OpenSSL
- **Other**: readline, zlib, libusb

Output:
```
Configuring Strada...

Checking for dependencies...

  Checking for MySQL... yes (mysql_config)
  Checking for SQLite... yes
  Checking for PostgreSQL... no
  Checking for libcrypt... yes
  Checking for readline... yes
  Checking for OpenSSL... yes
  Checking for zlib... yes
  Checking for libusb... yes

Configuration complete!
```

Configure options:
```bash
./configure --help              # Show all options
./configure --with-mysql        # Require MySQL (fail if not found)
./configure --without-postgres  # Skip PostgreSQL detection
./configure --prefix=/opt/strada # Set installation prefix
```

### Step 2: Build the Self-Hosting Compiler

The Makefile handles the full bootstrap process:

```bash
make
```

This does the following:
1. Compiles the bootstrap compiler (C code in `bootstrap/`)
2. Uses bootstrap to compile the self-hosting compiler (`compiler/*.strada`)
3. Produces `./stradac` - the Strada compiler

### Step 3: Build Libraries (Optional)

Build the standard libraries with detected dependencies:

```bash
make libs
```

This builds:
- `lib/DBI.so` - Database interface (MySQL/SQLite/PostgreSQL based on configure)
- `lib/crypt.so` - Password hashing
- `lib/ssl.so` - SSL/TLS support
- `lib/readline/readline.so` - Line editing for REPL

Individual library targets:
```bash
make lib-dbi       # Build DBI only
make lib-crypt     # Build crypt only
make lib-ssl       # Build ssl only
make lib-readline  # Build readline only
```

### Step 4: Verify the Build

```bash
# Run the test suite
make test

# Verify self-hosting (compiler compiles itself)
make test-selfhost

# Build all examples
make examples
```

### Step 5: Install (Optional)

For system-wide installation:

```bash
# Create installation directories
sudo mkdir -p /usr/local/bin
sudo mkdir -p /usr/local/lib/strada
sudo mkdir -p /usr/local/include/strada

# Install compiler and wrapper
sudo cp stradac /usr/local/bin/
sudo cp strada /usr/local/bin/
sudo chmod +x /usr/local/bin/strada

# Install runtime
sudo cp runtime/strada_runtime.c /usr/local/lib/strada/
sudo cp runtime/strada_runtime.h /usr/local/include/strada/

# Add to PATH (add to your .bashrc or .zshrc)
export PATH="/usr/local/bin:$PATH"
export STRADA_HOME="/usr/local/lib/strada"
```

## Build Targets

| Target | Description |
|--------|-------------|
| `./configure` | Detect system dependencies |
| `make` | Build the self-hosting compiler |
| `make libs` | Build all shared libraries (DBI, crypt, ssl, readline) |
| `make lib-dbi` | Build DBI library with detected database drivers |
| `make lib-crypt` | Build crypt library |
| `make lib-ssl` | Build SSL library |
| `make lib-readline` | Build readline library |
| `make interpreter` | Build the tree-walking interpreter (`strada-interp`) |
| `make tools` | Build tools (stradadoc, strada-soinfo, etc.) |
| `make run PROG=name` | Compile and run `examples/name.strada` |
| `make test` | Run runtime tests |
| `make test-selfhost` | Verify compiler can compile itself |
| `make test-suite` | Run comprehensive test suite |
| `make examples` | Build all example programs |
| `make install` | Install to system (default: /usr/local) |
| `make clean` | Remove all build artifacts |
| `make help` | Show all available targets |

## Compilation Options

### Using the Wrapper Script

The `./strada` wrapper script provides a convenient interface:

```bash
# Compile to executable (default)
./strada program.strada          # Creates ./program

# Compile and run immediately
./strada -r program.strada

# Keep the generated C code
./strada -c program.strada       # Creates program.c and ./program

# Include debug symbols
./strada -g program.strada       # Adds -g to gcc

# Specify output name
./strada -o myapp program.strada
```

### Using stradac Directly

For more control:

```bash
# Compile Strada to C
./stradac program.strada program.c

# Compile C to executable
gcc -o program program.c runtime/strada_runtime.c -Iruntime -ldl -lm

# With threading support
gcc -o program program.c runtime/strada_runtime.c -Iruntime -ldl -lm -lpthread

# With debug symbols
gcc -g -o program program.c runtime/strada_runtime.c -Iruntime -ldl -lm

# For FFI with rdynamic
gcc -rdynamic -o program program.c runtime/strada_runtime.c -Iruntime -ldl -lm
```

## Directory Structure

After building:

```
strada/
├── stradac              # Self-hosting compiler executable
├── strada               # Wrapper script for easy compilation
├── runtime/
│   ├── strada_runtime.c # Runtime library (link with programs)
│   └── strada_runtime.h # Runtime header
├── compiler/
│   ├── AST.strada       # Compiler source (AST definitions)
│   ├── Lexer.strada     # Compiler source (tokenizer)
│   ├── Parser.strada    # Compiler source (parser)
│   ├── CodeGen.strada   # Compiler source (code generator)
│   └── Main.strada      # Compiler source (entry point)
├── interpreter/
│   ├── Main.strada      # Interpreter driver (REPL + file execution)
│   └── Combined.strada  # Combined source (generated)
├── bootstrap/
│   └── stradac          # Bootstrap compiler (frozen)
├── build/               # Build artifacts (generated)
├── examples/            # Example programs
├── lib/                 # Standard library modules
│   └── Strada/
│       ├── Interpreter.strada  # Tree-walking interpreter library
│       └── JIT.strada          # JIT eval library
└── docs/                # Documentation
```

## Platform-Specific Notes

### Linux

Standard build should work. Ensure development packages are installed:

```bash
# Debian/Ubuntu
sudo apt-get install build-essential

# Fedora/RHEL
sudo dnf install gcc make

# Arch
sudo pacman -S base-devel
```

For Perl integration:
```bash
# Debian/Ubuntu
sudo apt-get install libperl-dev

# Fedora/RHEL
sudo dnf install perl-devel
```

### macOS

Install Xcode Command Line Tools:

```bash
xcode-select --install
```

Note: Use `libSystem` instead of explicit `-ldl` (it's included automatically).

### WSL (Windows Subsystem for Linux)

Follow Linux instructions. WSL2 recommended for best performance.

## Troubleshooting

### "gcc: command not found"

Install GCC:
```bash
# Ubuntu/Debian
sudo apt-get install gcc

# macOS
xcode-select --install
```

### "cannot find -ldl"

On some systems, libdl is part of libc:
```bash
# Try without -ldl
gcc -o program program.c runtime/strada_runtime.c -Iruntime -lm
```

### "strada_runtime.h: No such file"

Ensure you're including the runtime directory:
```bash
gcc -o program program.c runtime/strada_runtime.c -Iruntime -ldl -lm
```

### Bootstrap compiler fails

Try rebuilding from scratch:
```bash
make clean
cd bootstrap && make clean && make
cd .. && make
```

### Self-hosting verification fails

This may indicate a bug. Try:
```bash
# Use bootstrap compiler directly
./bootstrap/stradac program.strada output.c
gcc -o program output.c runtime/strada_runtime.c -Iruntime -ldl -lm
```

## Building Optional Components

The `./configure` script detects available libraries automatically. After running configure, use `make libs` to build all detected libraries.

### Database Support (DBI)

```bash
# Install database development files
sudo apt-get install libmysqlclient-dev  # MySQL
sudo apt-get install libsqlite3-dev      # SQLite (usually pre-installed)
sudo apt-get install libpq-dev           # PostgreSQL

# Run configure to detect databases
./configure

# Build the DBI library with detected drivers
make lib-dbi

# Check which drivers were enabled
# (shown during make lib-dbi output)
```

### Perl 5 Integration

```bash
# Install Perl development files
sudo apt-get install libperl-dev  # Debian/Ubuntu

# Build the Perl FFI library
cd lib/perl5
make

# Test
cd ../..
./strada -r examples/test_perl5.strada
```

### SSL/TLS Support

```bash
# Install OpenSSL development files
sudo apt-get install libssl-dev  # Debian/Ubuntu

# Run configure (detects OpenSSL)
./configure

# Build the SSL library
make lib-ssl

# Test
./strada -r examples/test_ssl.strada
```

### Password Hashing (crypt)

```bash
# Usually available by default on Linux
# Build the crypt library
make lib-crypt
```

### Readline (for REPL)

```bash
# Install readline development files
sudo apt-get install libreadline-dev  # Debian/Ubuntu

# Run configure and build
./configure
make lib-readline
```

## Verifying Installation

Run the test suite to verify everything works:

```bash
# Quick smoke test
./strada -r examples/test_simple.strada

# Full test suite
./t/run_tests.sh

# Expected output: All tests should pass
```

Create a test program:

```bash
cat > hello.strada << 'EOF'
func main() int {
    say("Strada is installed correctly!");
    return 0;
}
EOF

./strada -r hello.strada
rm hello.strada hello
```

## Updating

To update to the latest version:

```bash
git pull
make clean
make
make test
```

## Uninstalling

If you installed system-wide:

```bash
sudo rm /usr/local/bin/stradac
sudo rm /usr/local/bin/strada
sudo rm -rf /usr/local/lib/strada
sudo rm -rf /usr/local/include/strada
```

For local installation, simply delete the repository:

```bash
rm -rf strada/
```
