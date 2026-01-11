# File Handles in Strada

Strada provides Perl-like file handle support for reading and writing files and sockets. This guide covers all aspects of file handle I/O.

## Overview

Strada has two types of I/O handles:

| Type | Internal | Use Case |
|------|----------|----------|
| `STRADA_FILEHANDLE` | `FILE*` | Regular files |
| `STRADA_SOCKET` | Buffered socket | Network connections |

Both types support the same high-level operations: diamond operator `<$fh>` for reading and `say()`/`print()` for writing.

## Opening Files

```strada
# Open for reading
my scalar $fh = core::open("input.txt", "r");
if (!defined($fh)) {
    die("Cannot open file");
}

# Open for writing (creates/truncates)
my scalar $out = core::open("output.txt", "w");

# Open for appending
my scalar $log = core::open("app.log", "a");

# Open for read/write
my scalar $rw = core::open("data.txt", "r+");
```

### File Modes

| Mode | Description |
|------|-------------|
| `"r"` | Read only (file must exist) |
| `"w"` | Write only (creates/truncates) |
| `"a"` | Append (creates if needed) |
| `"r+"` | Read and write (file must exist) |
| `"w+"` | Read and write (creates/truncates) |
| `"a+"` | Read and append |

## Reading from Files

### Single Line (Scalar Context)

The diamond operator `<$fh>` reads one line at a time:

```strada
my scalar $fh = core::open("input.txt", "r");
my str $line = <$fh>;

while (defined($line)) {
    say("Got: " . $line);
    $line = <$fh>;
}

core::close($fh);
```

**Note:** The newline is automatically stripped from each line.

### All Lines (Array Context)

When assigned to an array, the diamond operator reads ALL lines:

```strada
my scalar $fh = core::open("input.txt", "r");
my array @lines = <$fh>;  # Reads entire file!
core::close($fh);

say("Read " . scalar(@lines) . " lines");

foreach my str $line (@lines) {
    say($line);
}
```

This also works with assignment to existing arrays:

```strada
my array @data = ();
my scalar $fh = core::open("file.txt", "r");
@data = <$fh>;  # Replaces contents with all lines
core::close($fh);
```

### Entire File as String

Use `core::slurp()` to read the entire file as a single string:

```strada
my str $content = core::slurp("file.txt");
```

Or from an open filehandle:

```strada
my scalar $fh = core::open("file.txt", "r");
my str $content = core::slurp_fh($fh);
core::close($fh);
```

## Writing to Files

### Using say() and print()

The `say()` and `print()` functions accept a filehandle as the first argument:

```strada
my scalar $fh = core::open("output.txt", "w");

say($fh, "This line has a newline");     # Adds \n
print($fh, "No newline here");           # No \n
print($fh, " - continued\n");            # Manual \n

core::close($fh);
```

### Using core::spew()

Write an entire string to a file:

```strada
core::spew("output.txt", "File contents here");
```

Or to an open filehandle:

```strada
my scalar $fh = core::open("output.txt", "w");
core::spew_fh($fh, "Content");
core::close($fh);
```

## Closing Files

Always close files when done:

```strada
core::close($fh);
```

Files are also closed when the filehandle goes out of scope (reference counting), but explicit closing is recommended.

## Socket I/O

Sockets work identically to files with the diamond operator and say/print:

```strada
# Connect to server
my scalar $sock = core::socket_client("example.com", 80);

# Send HTTP request
say($sock, "GET / HTTP/1.0");
say($sock, "Host: example.com");
say($sock, "");  # Empty line ends headers

# Read response line by line
my str $line = <$sock>;
while (defined($line)) {
    say($line);
    $line = <$sock>;
}

core::socket_close($sock);
```

### Reading All Lines from Socket

```strada
my scalar $sock = core::socket_client("example.com", 80);
say($sock, "GET / HTTP/1.0");
say($sock, "Host: example.com");
say($sock, "");

# Read entire response
my array @response = <$sock>;
core::socket_close($sock);

foreach my str $line (@response) {
    say($line);
}
```

## Buffering

### File Buffering

Files use stdio's internal buffering (`FILE*`):
- Typically 4KB-8KB buffer managed by the C library
- Very efficient for sequential access
- Use `core::flush($fh)` to force write

### Socket Buffering

Sockets use Strada's custom 8KB buffers:

```
StradaSocketBuffer:
  - read_buf[8192]   : Input buffer
  - read_pos         : Current read position
  - read_len         : Amount of data in buffer
  - write_buf[8192]  : Output buffer
  - write_len        : Amount buffered for writing
```

**Read behavior:**
- `recv()` fills the 8KB buffer
- Lines are extracted from the buffer
- Leftover data stays for next read

**Write behavior:**
- Data is buffered until newline or buffer full
- `say()` always flushes (line-buffered)
- `print()` flushes only if data ends with `\n`
- Use `core::socket_flush($sock)` for explicit flush

### CRLF Handling

Socket reads automatically strip `\r` (carriage return), so CRLF line endings from network protocols are handled correctly:

```strada
# Server sends: "HTTP/1.1 200 OK\r\n"
my str $line = <$sock>;
# $line contains: "HTTP/1.1 200 OK" (no \r\n)
```

## Server Example

```strada
func handle_client(scalar $client) void {
    say($client, "Welcome to the server!");

    my str $line = <$client>;
    while (defined($line)) {
        if ($line eq "quit") {
            say($client, "Goodbye!");
            last;
        }
        say($client, "Echo: " . $line);
        $line = <$client>;
    }

    core::socket_close($client);
}

func main() int {
    my scalar $server = core::socket_server(8080);
    say("Server listening on port 8080");

    while (1) {
        my scalar $client = core::socket_accept($server);
        if (defined($client)) {
            handle_client($client);
        }
    }

    return 0;
}
```

## File Position

```strada
my scalar $fh = core::open("file.txt", "r+");

# Get current position
my int $pos = core::tell($fh);

# Seek to position
core::seek($fh, 0, 0);   # Beginning (SEEK_SET)
core::seek($fh, 10, 1);  # Forward 10 bytes (SEEK_CUR)
core::seek($fh, -5, 2);  # 5 bytes from end (SEEK_END)

# Rewind to beginning
core::rewind($fh);

# Check for end of file
if (core::eof($fh)) {
    say("At end of file");
}

core::close($fh);
```

## Low-Level I/O

For advanced use cases, low-level file descriptor operations are available:

```strada
# Open with file descriptor
my int $fd = core::open_fd("file.txt", 0);  # O_RDONLY

# Read raw bytes
my str $data = core::read_fd($fd, 1024);

# Write raw bytes
core::write_fd($fd, "data");

# Close
core::close_fd($fd);
```

## Default Output Filehandle (`select`)

The `select($fh)` function sets the default output filehandle. When set, `say()` and `print()` without an explicit filehandle argument write to the selected filehandle instead of STDOUT.

```strada
my scalar $log = core::open("app.log", "w");

# Set default output to log file
select($log);

say("This goes to app.log");    # Writes to $log, not STDOUT
print("Also to log");           # Writes to $log

# Restore default to STDOUT
select(STDOUT);

say("Back to terminal");        # Writes to STDOUT again
core::close($log);
```

This is useful for redirecting output in libraries or temporarily capturing output to a file.

## In-Memory I/O

Strada supports reading from and writing to strings using standard file handle operations. In-memory handles are real `FILE*` pointers (via POSIX `fmemopen`/`open_memstream`), so **all** existing I/O functions work transparently: `<$fh>`, `say()`, `print()`, `core::seek()`, `core::tell()`, `core::eof()`, `core::flush()`, etc.

### Reading from a String

Use `core::open_str()` to create a read handle from string content:

```strada
my scalar $fh = core::open_str("line1\nline2\nline3\n", "r");

my str $line = <$fh>;
while (defined($line)) {
    say($line);
    $line = <$fh>;
}

core::close($fh);
```

### Writing to a String Buffer

Use `core::open_str()` with `"w"` mode to create a write handle, then `core::str_from_fh()` to extract the accumulated output:

```strada
my scalar $wfh = core::open_str("", "w");
say($wfh, "hello");
say($wfh, "world");
print($wfh, "no newline");

my str $result = core::str_from_fh($wfh);  # "hello\nworld\nno newline"
core::close($wfh);
```

You can call `core::str_from_fh()` multiple times — it snapshots the current buffer without closing the handle.

### Append Mode

Pass existing content as the first argument with `"a"` mode:

```strada
my scalar $afh = core::open_str("existing\n", "a");
say($afh, "appended");
my str $all = core::str_from_fh($afh);  # "existing\nappended\n"
core::close($afh);
```

### Reference-Style (Perl Compatibility)

Like Perl's `open(my $fh, '<', \$string)`, you can pass a reference to `core::open()`. For write/append modes, the referenced variable is updated when the handle is closed:

```strada
# Read from a string variable
my str $data = "alpha\nbeta\ngamma\n";
my scalar $rfh = core::open(\$data, "r");
my str $first = <$rfh>;   # "alpha"
my str $second = <$rfh>;  # "beta"
core::close($rfh);

# Write to a string variable (updated on close)
my str $output = "";
my scalar $wfh = core::open(\$output, "w");
say($wfh, "written via ref");
core::close($wfh);
# $output is now "written via ref\n"

# Append to a string variable
my str $buf = "start\n";
my scalar $afh = core::open(\$buf, "a");
say($afh, "more");
core::close($afh);
# $buf is now "start\nmore\n"
```

### In-Memory I/O Functions

| Function | Description |
|----------|-------------|
| `core::open_str($content, $mode)` | Open in-memory handle. Modes: `"r"`, `"w"`, `"a"` |
| `core::open(\$var, $mode)` | Open in-memory handle backed by variable. Write/append modes update `$var` on close |
| `core::str_from_fh($fh)` | Extract accumulated string from a write-mode memstream (without closing) |

### Auto-Close

In-memory handles are reference-counted like regular file handles. They are automatically closed and their buffers freed when the handle goes out of scope:

```strada
{
    my scalar $fh = core::open_str("temporary\n", "r");
    my str $line = <$fh>;
    # $fh goes out of scope here — handle closed, buffer freed automatically
}
```

### Use Cases

- **Testing**: Capture output in tests without writing to disk
- **String processing**: Use line-oriented I/O on string data
- **Template engines**: Build output with standard print/say, extract at the end
- **Perl migration**: Direct translation of `open(my $fh, '<', \$string)` patterns

## Best Practices

1. **Always close files** - Don't rely on garbage collection
2. **Check for errors** - Test `defined($fh)` after opening
3. **Use array context for small files** - `my array @lines = <$fh>` is convenient
4. **Use scalar context for large files** - Process line by line to save memory
5. **Flush sockets when needed** - Use `core::socket_flush()` for non-line protocols
6. **Use say() for line-oriented I/O** - It handles newlines and flushing
7. **Use in-memory I/O for string processing** - Avoids temporary files

## Summary

| Operation | Syntax |
|-----------|--------|
| Open file | `core::open($path, $mode)` |
| Open in-memory (string) | `core::open_str($content, $mode)` |
| Open in-memory (ref) | `core::open(\$var, $mode)` |
| Extract memstream output | `core::str_from_fh($fh)` |
| Close file | `core::close($fh)` |
| Read line | `my str $line = <$fh>` |
| Read all lines | `my array @lines = <$fh>` |
| Read entire file | `core::slurp($path)` |
| Write with newline | `say($fh, $text)` |
| Write without newline | `print($fh, $text)` |
| Write entire file | `core::spew($path, $content)` |
| Flush buffer | `core::flush($fh)` or `core::socket_flush($sock)` |
| Socket connect | `core::socket_client($host, $port)` |
| Socket server | `core::socket_server($port)` |
| Socket accept | `core::socket_accept($server)` |
| Socket close | `core::socket_close($sock)` |
| Select default output | `select($fh)` |
