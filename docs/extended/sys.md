# core:: Namespace Reference

The `core::` namespace provides low-level system functions for file I/O, process control, networking, and C interop. These functions closely mirror POSIX/libc functionality.

## File I/O

### core::open

```strada
my scalar $fh = core::open($filename, $mode);
```

Open a file and return a file handle.

- `$filename` - Path to the file
- `$mode` - "r" (read), "w" (write), "a" (append), "r+" (read/write)

Returns a file handle on success, or undef on failure.

### core::close

```strada
core::close($fh);
```

Close a file handle.

### core::slurp

```strada
my str $content = core::slurp($filename);
```

Read entire file contents into a string. Returns `undef` if `$filename` is a directory.

### core::slurp_fh

```strada
my str $content = core::slurp_fh($fh);
```

Read remaining contents of a file handle into a string.

### core::slurp_fd

```strada
my str $content = core::slurp_fd($fd);
```

Read from a file descriptor into a string.

### core::spew

```strada
core::spew($filename, $content);
```

Write string to a file (overwrites existing content).

### core::spew_fh

```strada
core::spew_fh($fh, $content);
```

Write string to a file handle.

### core::spew_fd

```strada
core::spew_fd($fd, $content);
```

Write string to a file descriptor.

### core::fwrite

```strada
core::fwrite($fh, $data);
```

Write the string `$data` to a file handle. With an optional third argument,
`core::fwrite($fh, $data, $n)`, only the first `$n` bytes are written. Nothing
useful is returned.

### core::fread

```strada
my str $data = core::fread($fh);       # whole file
my str $part = core::fread($fh, $n);   # up to $n bytes
```

With one argument, read the **entire** file behind the handle (it seeks to the
start first, so the current position is ignored). With a size argument, read up
to `$n` bytes from the current position (an empty string at end of file).

### core::readline

```strada
my str $line = core::readline($fh);
```

Read a single line from a file handle. The trailing newline is stripped (same as `<$fh>`); returns `undef` at EOF.

### core::seek

```strada
core::seek($fh, $offset, $whence);
```

Seek to position in file.

- `$whence` - 0 (SEEK_SET), 1 (SEEK_CUR), 2 (SEEK_END)

### core::tell

```strada
my int $pos = core::tell($fh);
```

Get current position in file.

### core::rewind

```strada
core::rewind($fh);
```

Reset file position to beginning.

### core::eof

```strada
if (core::eof($fh)) { ... }
```

Check if at end of file. Returns 1 if at EOF, an empty string (false) otherwise.

### core::flush

```strada
core::flush($fh);
```

Flush buffered output to file.

### core::fgetc

```strada
my int $code = core::fgetc($fh);
```

Read a single byte from file handle. Returns its numeric code (use `chr()` to get the character), or -1 at EOF.

### core::fputc

```strada
core::fputc($code, $fh);
```

Write a single byte to file handle. Note the C-style argument order (value first, handle second); `$code` is the numeric byte value, e.g. `core::fputc(ord("A"), $fh)`.

### core::fgets

```strada
my str $line = core::fgets($fh, $maxlen);
```

Read up to `$maxlen - 1` bytes or until newline (the newline is kept, as with C `fgets`). Returns `undef` at EOF.

### core::fputs

```strada
core::fputs($str, $fh);
```

Write string to file handle. Note the C-style argument order (string first, handle second).

### core::ferror

```strada
if (core::ferror($fh)) { ... }
```

Check if file handle has error.

### core::fileno

```strada
my int $fd = core::fileno($fh);
```

Get file descriptor number from file handle.

### core::clearerr

```strada
core::clearerr($fh);
```

Clear error and EOF flags on file handle.

## File System

### core::unlink

```strada
core::unlink($filename);
```

Delete a file.

### core::link

```strada
core::link($oldpath, $newpath);
```

Create a hard link.

### core::symlink

```strada
core::symlink($target, $linkpath);
```

Create a symbolic link.

### core::readlink

```strada
my str $target = core::readlink($linkpath);
```

Read the target of a symbolic link.

### core::rename

```strada
core::rename($oldname, $newname);
```

Rename/move a file.

### core::mkdir

```strada
core::mkdir($path, $mode);
```

Create a directory. `$mode` is octal permissions (e.g., 0755).

### core::rmdir

```strada
core::rmdir($path);
```

Remove an empty directory.

### core::chdir

```strada
core::chdir($path);
```

Change current working directory.

### core::getcwd

```strada
my str $cwd = core::getcwd();
```

Get current working directory.

### core::chmod

```strada
core::chmod($path, $mode);
```

Change file permissions.

### core::access

```strada
my int $ok = core::access($path, $mode);
```

Check file accessibility. Returns 0 if the access is permitted, -1 otherwise (as `access(2)`).

- `$mode` - 0 (F_OK exists), 1 (X_OK exec), 2 (W_OK write), 4 (R_OK read)

### core::umask

```strada
my int $old = core::umask($mask);
```

Set file creation mask. Returns previous mask.

### core::stat

```strada
my hash %st = core::stat($path);
```

Get file status. Returns hash with keys: `dev`, `ino`, `mode`, `nlink`, `uid`, `gid`, `rdev`, `size`, `atime`, `mtime`, `ctime`, `blksize`, `blocks`. Returns `undef` if the path cannot be stat'ed.

### core::lstat

```strada
my hash %st = core::lstat($path);
```

Like `stat` but doesn't follow symlinks.

### core::readdir

```strada
my array @entries = core::readdir($path);
```

Read directory contents. Returns array of filenames (excludes `.` and `..`).

### core::opendir

```strada
my scalar $dh = core::opendir($path);
```

Open a directory for iterative reading. Returns a directory handle, or `undef` on failure. Use with `core::readdir_next()` and `core::closedir()`.

### core::readdir_next

```strada
my str $entry = core::readdir_next($dh);
```

Read the next entry from a directory handle (skips `.` and `..`). Returns `undef` when no more entries.

```strada
my scalar $dh = core::opendir(".");
my str $entry = core::readdir_next($dh);
while (defined($entry)) {
    say($entry);
    $entry = core::readdir_next($dh);
}
core::closedir($dh);
```

### core::closedir

```strada
my int $rc = core::closedir($dh);
```

Close a directory handle. Returns 0 on success.

### core::readdir_full

```strada
my array @entries = core::readdir_full($path);
```

Read directory with full path for each entry.

### core::is_dir

```strada
if (core::is_dir($path)) { ... }
```

Check if path is a directory.

### core::is_file

```strada
if (core::is_file($path)) { ... }
```

Check if path is a regular file.

### core::file_size

```strada
my int $size = core::file_size($path);
```

Get file size in bytes (0 if the file does not exist).

### core::realpath

```strada
my str $abs = core::realpath($path);
```

Resolve to absolute path (resolving symlinks).

### core::dirname

```strada
my str $dir = core::dirname($path);
```

Get directory portion of path.

### core::basename

```strada
my str $name = core::basename($path);
```

Get filename portion of path.

### core::file_ext

```strada
my str $ext = core::file_ext($path);
```

Get file extension (e.g., "txt" from "file.txt").

### core::glob

```strada
my array @files = core::glob($pattern);
```

Expand glob pattern. Returns matching filenames.

### core::fnmatch

```strada
if (core::fnmatch($pattern, $string)) { ... }
```

Match string against shell wildcard pattern.

### core::path_join

```strada
my array @parts = ($dir, $file);
my str $path = core::path_join(@parts);      # or core::path_join([$dir, $file])
```

Join path components with `/`. Takes a single array (or array reference) of components, not separate arguments.

### core::truncate

```strada
core::truncate($path, $length);
```

Truncate file to specified length.

### core::ftruncate

```strada
core::ftruncate($fd, $length);
```

Truncate open file to specified length.

### core::chown

```strada
core::chown($path, $uid, $gid);
```

Change file owner and group.

### core::lchown

```strada
core::lchown($path, $uid, $gid);
```

Like chown but doesn't follow symlinks.

### core::fchmod

```strada
core::fchmod($fd, $mode);
```

Change permissions of open file.

### core::fchown

```strada
core::fchown($fd, $uid, $gid);
```

Change owner of open file.

### core::utime

```strada
core::utime($path, $atime, $mtime);
```

Set file access and modification times.

### core::utimes

```strada
core::utimes($path, $atime, $mtime);
```

Set file times with microsecond precision.

## Temporary Files

### core::tmpfile

```strada
my scalar $fh = core::tmpfile();
```

Create a temporary file (auto-deleted on close).

### core::mkstemp

```strada
my scalar $tmp = core::mkstemp($template);
my int $fd = $tmp->[0];
my str $path = $tmp->[1];
```

Create unique temporary file. Template should end with "XXXXXX". Returns an array reference `[fd, path]` (the open file descriptor and the actual file name), or `undef` on failure.

### core::mkdtemp

```strada
my str $path = core::mkdtemp($template);
```

Create unique temporary directory.

## Process Control

### core::fork

```strada
my int $pid = core::fork();
```

Create child process. Returns 0 in child, child PID in parent.

### core::exec

```strada
core::exec($command);
```

Replace current process with a shell command (run via `/bin/sh -c`). Takes a single command string.

### core::system

```strada
my int $status = core::system($command);
```

Execute shell command. Returns the raw wait status; the exit code is `core::exit_status($status)` (or `$status >> 8`).

### core::wait

```strada
my int $pid = core::wait();
```

Wait for any child process to terminate.

### core::waitpid

```strada
my int $status = core::waitpid($pid, $options);
```

Wait for specific child process. Returns the child's raw wait status (not its pid); extract the exit code with `core::exit_status($status)`.

### core::getpid

```strada
my int $pid = core::getpid();
```

Get current process ID.

### core::getppid

```strada
my int $ppid = core::getppid();
```

Get parent process ID.

### core::kill

```strada
core::kill($pid, $signal);
```

Send signal to process.

### core::alarm

```strada
my int $remaining = core::alarm($seconds);
```

Set alarm timer. Sends SIGALRM after specified seconds.

### core::signal

```strada
core::signal($signame, \&handler);
core::signal($signame, "IGNORE");
core::signal($signame, "DEFAULT");
```

Set signal handler. `$signame` is "INT", "TERM", "HUP", etc. The handler function receives the signal number.

### core::sleep

```strada
core::sleep($seconds);
```

Sleep for specified seconds.

### core::usleep

```strada
core::usleep($microseconds);
```

Sleep for specified microseconds.

### core::nanosleep

```strada
core::nanosleep($nanoseconds);
```

High-precision sleep. Takes a single argument: the total time in nanoseconds (e.g. `core::nanosleep(1500000000)` sleeps 1.5 s).

### core::setprocname

```strada
core::setprocname($name);
```

Set process name (as shown in `ps`).

### core::getprocname

```strada
my str $name = core::getprocname();
```

Get current process name.

### core::setproctitle

```strada
core::setproctitle($title);
```

Set process title.

### core::getproctitle

```strada
my str $title = core::getproctitle();
```

Get process title.

### core::exit_status

```strada
my int $code = core::exit_status($status);
```

Extract exit code from a wait status as returned by `core::system()` / `core::waitpid()` (-1 if the process did not exit normally).

## Command Execution

### core::popen

```strada
my scalar $pipe = core::popen($command, $mode);
```

Open pipe to/from command. Returns a file handle usable with `<$pipe>`, `core::readline()`, `say($pipe, ...)`.

- `$mode` - "r" (read from command) or "w" (write to command)

### core::pclose

```strada
my int $status = core::pclose($pipe);
```

Close pipe and get exit status.

## IPC (Inter-Process Communication)

### core::pipe

```strada
my array @fds = core::pipe();
```

Create pipe. Returns the two file descriptors: (read_fd, write_fd).

### core::dup2

```strada
core::dup2($oldfd, $newfd);
```

Duplicate file descriptor.

### core::dup

```strada
my int $newfd = core::dup($fd);
```

Duplicate file descriptor to lowest available.

### core::close_fd

```strada
core::close_fd($fd);
```

Close file descriptor.

### core::read_fd

```strada
my str $data = core::read_fd($fd, $count);
```

Read from file descriptor.

### core::write_fd

```strada
my int $written = core::write_fd($fd, $data);
```

Write to file descriptor.

### core::read_all_fd

```strada
my str $data = core::read_all_fd($fd);
```

Read all available data from file descriptor.

## Environment

### core::getenv

```strada
my str $value = core::getenv($name);
```

Get environment variable.

### core::setenv

```strada
core::setenv($name, $value);
```

Set environment variable.

### core::unsetenv

```strada
core::unsetenv($name);
```

Remove environment variable.

## User/Group

### core::getuid

```strada
my int $uid = core::getuid();
```

Get real user ID.

### core::geteuid

```strada
my int $euid = core::geteuid();
```

Get effective user ID.

### core::getgid

```strada
my int $gid = core::getgid();
```

Get real group ID.

### core::getegid

```strada
my int $egid = core::getegid();
```

Get effective group ID.

### core::setuid

```strada
core::setuid($uid);
```

Set user ID.

### core::setgid

```strada
core::setgid($gid);
```

Set group ID.

### core::seteuid

```strada
core::seteuid($uid);
```

Set effective user ID.

### core::setegid

```strada
core::setegid($gid);
```

Set effective group ID.

### core::getpwnam

```strada
my hash %pw = core::getpwnam($username);
```

Get user info by name. Returns hash with: `name`, `passwd`, `uid`, `gid`, `gecos`, `dir`, `shell`.

### core::getpwuid

```strada
my hash %pw = core::getpwuid($uid);
```

Get user info by UID.

### core::getgrnam

```strada
my hash %gr = core::getgrnam($groupname);
```

Get group info by name.

### core::getgrgid

```strada
my hash %gr = core::getgrgid($gid);
```

Get group info by GID.

### core::getlogin

```strada
my str $user = core::getlogin();
```

Get login name.

### core::getgroups

```strada
my array @gids = core::getgroups();
```

Get supplementary group IDs.

## Time

### core::time

```strada
my int $epoch = core::time();
```

Get current Unix timestamp.

### core::localtime

```strada
my hash %tm = core::localtime($time);
```

Convert timestamp to local time. Returns hash with: `sec`, `min`, `hour`, `mday`, `mon`, `year`, `wday`, `yday`, `isdst`.

### core::gmtime

```strada
my hash %tm = core::gmtime($time);
```

Convert timestamp to UTC time. Same keys as `localtime` (`mon` is 0-11, `year` is years since 1900).

### core::mktime

```strada
my int $time = core::mktime(%tm);
```

Convert time hash to timestamp.

### core::strftime

```strada
my str $formatted = core::strftime($format, %tm);
```

Format time as string. The second argument must be a time hash as returned by `localtime`/`gmtime` (pass the hash itself, not a reference).

### core::ctime

```strada
my str $str = core::ctime($time);
```

Convert timestamp to string.

### core::gettimeofday

```strada
my hash %tv = core::gettimeofday();
```

Get time with microsecond precision. Returns a hash with keys `sec` and `usec`.

### core::hires_time

```strada
my num $time = core::hires_time();
```

Get high-resolution time as floating point seconds.

### core::tv_interval

```strada
my num $elapsed = core::tv_interval(%start, %end);
```

Calculate interval in seconds between two `gettimeofday` hashes.

### core::clock_gettime

```strada
my hash %ts = core::clock_gettime($clock_id);
```

Get time from specified clock. Returns a hash with keys `sec` and `nsec`.

### core::clock_getres

```strada
my hash %res = core::clock_getres($clock_id);
```

Get clock resolution. Returns a hash with keys `sec` and `nsec`.

### core::difftime

```strada
my num $diff = core::difftime($time1, $time0);
```

Calculate difference between two times.

### core::clock

```strada
my int $ticks = core::clock();
```

Get processor time.

### core::times

```strada
my hash %times = core::times();
```

Get process times. Returns a hash with keys `utime`, `stime`, `cutime`, `cstime`, `ticks`.

## Sockets

### core::socket_client

```strada
my scalar $sock = core::socket_client($host, $port);
```

Create TCP client connection.

### core::socket_server

```strada
my scalar $sock = core::socket_server($port);
```

Create TCP server socket (default backlog).

### core::socket_server_backlog

```strada
my scalar $sock = core::socket_server_backlog($port, $backlog);
```

Create TCP server socket with specified backlog.

### core::socket_server_host

```strada
my scalar $sock = core::socket_server_host($host, $port, $backlog);
```

Create a TCP listen socket bound to a specific host. The `$host` argument
selects the address family and scope:

| `$host` | Binds |
|---------|-------|
| `""`, `"*"`, `"::"` | dual-stack wildcard — IPv6 `::` with `IPV6_V6ONLY` off, so it accepts both IPv6 and IPv4 connections |
| `"0.0.0.0"` | IPv4 wildcard only |
| `"::1"` / `"127.0.0.1"` | that loopback only |
| any literal address/hostname | the family it resolves to (via `getaddrinfo`) |

> **IPv6 / dual-stack.** The whole socket layer
> (`socket_connect`/`socket_server`/`socket_accept`/`getaddrinfo_first` and the
> `udp_*` calls) resolves addresses with `getaddrinfo(AF_UNSPEC)` and is
> IPv6-aware. `socket_server` / `socket_server_backlog` prefer an IPv6
> dual-stack wildcard (falling back to IPv4); use `socket_server_host` when you
> need to pin a specific family or address.

### core::socket_accept

```strada
my scalar $client = core::socket_accept($server_sock);
```

Accept incoming connection.

### core::socket_recv

```strada
my str $data = core::socket_recv($sock, $maxlen);
```

Receive data from socket.

### core::socket_send

```strada
my int $sent = core::socket_send($sock, $data);
```

Send data on socket.

### core::socket_close

```strada
core::socket_close($sock);
```

Close socket.

### core::socket_select

```strada
my array @ready = core::socket_select(\@socks, $timeout_ms);
```

Wait until one or more of the given sockets is readable. Takes an array (reference) of sockets and a timeout in milliseconds (-1 = wait forever, 0 = poll); returns the sockets that are ready.

### core::socket_fd

```strada
my int $fd = core::socket_fd($sock);
```

Get file descriptor from socket.

### core::select_fds

```strada
my array @ready = core::select_fds(\@read_fds, $timeout_ms);
```

Select on multiple file descriptors for readability. Takes one array (reference) of fds and a timeout in milliseconds; returns the fds that are ready to read.

### core::setsockopt

```strada
core::setsockopt($sock, $level, $optname, $value);
```

Set socket option. `$sock` here (and for `getsockopt`, `shutdown`, `getpeername`, `getsockname`) is a numeric file descriptor — pass `core::socket_fd($socket)` for a socket object. `$value` is an integer.

### core::getsockopt

```strada
my scalar $value = core::getsockopt($sock, $level, $optname);
```

Get socket option.

### core::shutdown

```strada
core::shutdown($sock, $how);
```

Shutdown socket. `$how`: 0=read, 1=write, 2=both.

### core::getpeername

```strada
my hash %peer = core::getpeername($fd);
```

Get address of peer. Returns a hash with keys `addr` and `port` (`undef` on error).

### core::getsockname

```strada
my hash %local = core::getsockname($fd);
```

Get local socket address. Returns a hash with keys `addr` and `port` (`undef` on error).

### core::poll

```strada
my int $count = core::poll(\@fds, $timeout_ms);
```

Poll multiple file descriptors. `@fds` is an array of `{fd, events}` entries; returns the number of ready descriptors (-1 on error). For simple readability checks, `core::select_fds()` is easier to use.

## DNS/Network

### core::gethostbyname

```strada
my str $ip = core::gethostbyname($hostname);
```

Resolve hostname to IP address.

### core::gethostbyname_all

```strada
my array @ips = core::gethostbyname_all($hostname);
```

Resolve hostname to all IP addresses.

### core::gethostname

```strada
my str $hostname = core::gethostname();
```

Get local hostname.

### core::getaddrinfo

```strada
my str $addr = core::getaddrinfo($host, $service);
```

Resolve host and service with `getaddrinfo(3)`. Returns the first resulting address as a numeric string (e.g. `"127.0.0.1"`).

### core::inet_pton

```strada
my int $binary = core::inet_pton($af, $addr);
```

Convert IP address string to binary form, returned as an integer (for `AF_INET` (2) the 32-bit address in network byte order; for IPv6 only the first 64 bits).

### core::inet_ntop

```strada
my str $addr = core::inet_ntop($af, $binary);
```

Convert binary IP to string.

### core::inet_addr

```strada
my int $addr = core::inet_addr($ip);
```

Convert dotted-decimal IP to integer.

### core::inet_ntoa

```strada
my str $ip = core::inet_ntoa($addr);
```

Convert integer IP to dotted-decimal.

### core::htons

```strada
my int $net = core::htons($host);
```

Host to network short.

### core::htonl

```strada
my int $net = core::htonl($host);
```

Host to network long.

### core::ntohs

```strada
my int $host = core::ntohs($net);
```

Network to host short.

### core::ntohl

```strada
my int $host = core::ntohl($net);
```

Network to host long.

## Error Handling

### core::errno

```strada
my int $err = core::errno();
```

Get last error number.

### core::strerror

```strada
my str $msg = core::strerror($errno);
```

Get error message for error number.

### core::isatty

```strada
if (core::isatty($fd)) { ... }
```

Check if file descriptor is a terminal.

## Dynamic Loading (FFI)

### core::dl_open

```strada
my int $handle = core::dl_open($library);
```

Load shared library. Returns handle.

### core::dl_sym

```strada
my int $func = core::dl_sym($handle, $symbol);
```

Get symbol address from library.

### core::dl_close

```strada
core::dl_close($handle);
```

Unload shared library.

### core::dl_error

```strada
my str $err = core::dl_error();
```

Get last dynamic loading error.

### core::dl_call_sv

```strada
my scalar $result = core::dl_call_sv($func, [$arg1, $arg2]);
```

Call Strada function in shared library.

### core::dl_call_int

```strada
my int $result = core::dl_call_int($func, [$arg1, $arg2]);
```

Call C function returning int. Arguments are passed as an array reference of integers.

### core::dl_call_str

```strada
my str $result = core::dl_call_str($func, $arg);
```

Call C function taking one string argument and returning a string (`char*`).

### core::dl_call_void

```strada
core::dl_call_void($func, [$arg1, $arg2]);
```

Call C function returning void.

### core::dl_call_int_sv / core::dl_call_str_sv / core::dl_call_void_sv

Pass StradaValue* directly to C functions.

### core::dl_call_export_info

```strada
my str $info = core::dl_call_export_info($func);
```

Get export info from Strada library.

### core::dl_call_version

```strada
my str $version = core::dl_call_version($func);
```

Get version from Strada library.

## String Conversion

### core::atoi

```strada
my int $n = core::atoi($str);
```

Convert string to integer.

### core::atof

```strada
my num $n = core::atof($str);
```

Convert string to float.

### core::strtol

```strada
my scalar $r = core::strtol($str, $base);
my int $n = $r->[0];
```

Convert string to integer with base. Returns an array reference `[value, chars_consumed]`.

### core::strtod

```strada
my scalar $r = core::strtod($str);
my num $n = $r->[0];
```

Convert string to double. Returns an array reference `[value, chars_consumed]`.

## Binary/Byte Operations

### core::ord_byte

```strada
my int $byte = core::ord_byte($str);
```

Get the value of the first byte of the string.

### core::get_byte

```strada
my int $byte = core::get_byte($str, $index);
```

Get byte value at index.

### core::set_byte

```strada
my str $new = core::set_byte($str, $index, $value);
```

Set byte at index.

### core::byte_length

```strada
my int $len = core::byte_length($str);
```

Get byte length of string.

### core::byte_substr

```strada
my str $sub = core::byte_substr($str, $offset, $length);
```

Extract byte substring.

### core::pack

```strada
my str $binary = core::pack($template, @values);
```

Pack values into binary string.

### core::unpack

```strada
my array @values = core::unpack($template, $binary);
```

Unpack binary string to values.

### core::base64_encode

```strada
my str $encoded = core::base64_encode($data);
```

Encode data as base64.

### core::base64_decode

```strada
my str $data = core::base64_decode($encoded);
```

Decode base64 data.

## Memory

> For raw memory, prefer the `c::` namespace (`c::alloc($size)`, `c::realloc()`,
> `c::free()`, `c::read_*`/`c::write_*`). There is no `core::malloc`.

### core::calloc

```strada
my int $ptr = core::calloc($count, $size);
```

Allocate zeroed memory.

### core::realloc

```strada
my int $ptr = core::realloc($ptr, $size);
```

Reallocate memory.

### core::free

```strada
core::free(\$var);
```

Same as `core::release()` (below): releases the value a reference points to. It does **not** free a raw pointer from `core::calloc()`/`core::realloc()` — use `c::free($ptr)` for that.

### core::release

```strada
core::release(\$var);
```

Release the value a reference points to and set it to undef.

### core::mmap

```strada
my int $ptr = core::mmap($addr, $length, $prot, $flags, $fd, $offset);
```

Map file into memory.

### core::munmap

```strada
core::munmap($ptr, $length);
```

Unmap memory.

### core::mlock

```strada
core::mlock($ptr, $length);
```

Lock memory pages.

### core::munlock

```strada
core::munlock($ptr, $length);
```

Unlock memory pages.

## Array/Hash Utilities

### reserve

```strada
reserve(@arr, $count);   # pre-allocate array capacity
```

Pre-allocate array capacity. `core::array_reserve(@arr, $count)` is accepted as
an alias. There is no `array_capacity` or `array_shrink` function.

### core::hash_default_capacity

```strada
core::hash_default_capacity($size);
```

Set default capacity for new hashes.

## Random Numbers

### core::rand

```strada
my int $n = core::rand();
```

Get random integer.

### core::random

```strada
my int $n = core::random();
```

Get random integer (better quality).

### core::srand

```strada
core::srand($seed);
```

Seed random number generator.

### core::srandom

```strada
core::srandom($seed);
```

Seed random number generator (for random()).

## Session/Process Group

### core::setsid

```strada
my int $sid = core::setsid();
```

Create new session.

### core::getsid

```strada
my int $sid = core::getsid($pid);
```

Get session ID.

### core::setpgid

```strada
core::setpgid($pid, $pgid);
```

Set process group ID.

### core::getpgid

```strada
my int $pgid = core::getpgid($pid);
```

Get process group ID.

### core::getpgrp

```strada
my int $pgrp = core::getpgrp();
```

Get process group.

### core::setpgrp

```strada
core::setpgrp();
```

Set process group.

## Advanced Signals

### core::sigprocmask

```strada
core::sigprocmask($how, \@signals);
```

Change blocked signals. `$how` is 0 (SIG_BLOCK), 1 (SIG_UNBLOCK) or 2 (SIG_SETMASK); `@signals` holds signal numbers. Returns 0 on success.

### core::raise

```strada
core::raise($signal);
```

Send signal to self.

### core::killpg

```strada
core::killpg($pgrp, $signal);
```

Send signal to process group.

### core::pause

```strada
core::pause();
```

Wait for signal.

## Resource/Priority

### core::nice

```strada
my int $new = core::nice($inc);
```

Change process priority.

### core::getpriority

```strada
my int $pri = core::getpriority($which, $who);
```

Get process priority.

### core::setpriority

```strada
core::setpriority($which, $who, $priority);
```

Set process priority.

### core::getrusage

```strada
my hash %usage = core::getrusage($who);
```

Get resource usage. Returns a hash with keys `utime_sec`, `utime_usec`, `stime_sec`, `stime_usec`, `maxrss`, `minflt`, `majflt`, `nvcsw`, `nivcsw`.

### core::getrlimit

```strada
my hash %limit = core::getrlimit($resource);
```

Get resource limits. Returns a hash with keys `cur` (soft limit) and `max` (hard limit).

### core::setrlimit

```strada
core::setrlimit($resource, %limit);
```

Set resource limits. `%limit` is a hash with keys `cur` and `max`, as returned by `getrlimit`.

## Terminal/TTY

### core::ttyname

```strada
my str $name = core::ttyname($fd);
```

Get terminal name.

### core::tcgetattr

```strada
my hash %attr = core::tcgetattr($fd);
```

Get terminal attributes. Returns a hash with keys `iflag`, `oflag`, `cflag`, `lflag`, `ispeed`, `ospeed` (`undef` if `$fd` is not a terminal).

### core::tcsetattr

```strada
core::tcsetattr($fd, $when, %attr);
```

Set terminal attributes.

### core::cfgetospeed

```strada
my int $speed = core::cfgetospeed(%termios);
```

Get output baud rate.

### core::cfsetospeed

```strada
core::cfsetospeed(%termios, $speed);
```

Set output baud rate.

### core::cfgetispeed

```strada
my int $speed = core::cfgetispeed(%termios);
```

Get input baud rate.

### core::cfsetispeed

```strada
core::cfsetispeed(%termios, $speed);
```

Set input baud rate.

### core::term_enable_raw

```strada
core::term_enable_raw();
```

Enable terminal raw mode. Saves the current termios settings (restored by `term_disable_raw`). Disables canonical mode, echo, and signal processing. Sets minimum read to 0 bytes with 1/10 second timeout.

### core::term_disable_raw

```strada
core::term_disable_raw();
```

Restore terminal to the mode saved by `term_enable_raw()`. No-op if raw mode was never enabled.

### core::term_rows

```strada
my int $rows = core::term_rows();
```

Get terminal height in rows. Uses `ioctl(TIOCGWINSZ)`, falls back to `LINES` environment variable, then defaults to 24.

### core::term_cols

```strada
my int $cols = core::term_cols();
```

Get terminal width in columns. Uses `ioctl(TIOCGWINSZ)`, falls back to `COLUMNS` environment variable, then defaults to 80.

### core::read_byte

```strada
my int $byte = core::read_byte($fd);
```

Read a single byte from file descriptor `$fd`. Returns 0-255 on success, -1 on EOF or error.

## Advanced File Operations

### core::fcntl

```strada
my int $result = core::fcntl($fd, $cmd, $arg);
```

File control operations.

### core::flock

```strada
core::flock($fd, $operation);
```

Apply file lock.

### core::ioctl

```strada
my int $result = core::ioctl($fd, $request, $arg);
```

I/O control.

### core::statvfs

```strada
my hash %st = core::statvfs($path);
```

Get filesystem statistics.

### core::fstatvfs

```strada
my hash %st = core::fstatvfs($fd);
```

Get filesystem statistics for open file.

## Debugging

### core::stack_trace

```strada
my str $trace = core::stack_trace();
```

Get the current call stack as a string. Returns a multi-line string showing the function call chain from innermost to outermost.

Example:
```strada
func debug_here() void {
    my str $trace = core::stack_trace();
    say("Current stack:\n" . $trace);
}

func inner() void {
    debug_here();
}

func outer() void {
    inner();
}

func main() int {
    outer();
    return 0;
}
```

Output (callers show the line of the call):
```
Current stack:
  at debug_here (myprogram.strada)
  at inner (myprogram.strada:7)
  at outer (myprogram.strada:11)
  at main (myprogram.strada:15)
```

This is useful for debugging, logging, and error reporting. Note that uncaught exceptions automatically print a stack trace.

### core::set_recursion_limit

```strada
core::set_recursion_limit($limit);
```

Set the maximum recursion depth. When the call stack exceeds this limit, the program will print an error with a stack trace and exit.

- `$limit` - Maximum recursion depth (default: 1000, set to 0 to disable)

The check is part of stack-trace tracking, which the compiler only emits when
the program source uses `try`/`throw`, `caller()` or `stack_trace()` (or is
compiled with `stradac --stack-trace`). In a program without tracking the limit
is not enforced and runaway recursion ends in a native stack overflow.

Example:
```strada
# Increase limit for deeply recursive algorithms
core::set_recursion_limit(5000);

# Disable limit (not recommended)
core::set_recursion_limit(0);
```

### core::get_recursion_limit

```strada
my int $limit = core::get_recursion_limit();
```

Get the current maximum recursion depth.

Example:
```strada
my int $limit = core::get_recursion_limit();
say("Current recursion limit: " . $limit);
```

When the recursion limit is exceeded, output looks like:
```
Error: Maximum recursion depth exceeded (1000)
Stack trace:
  at recursive_func (myprogram.strada:2)
  at recursive_func (myprogram.strada:2)
  ...
  at main (myprogram.strada:7)
  -> recursive_func (myprogram.strada)

Hint: Use core::set_recursion_limit(n) to increase the limit, or 0 to disable.
```

## Call Context (Dynamic Return Type)

### core::wantarray

```strada
my int $is_array = core::wantarray();
```

Returns 1 if the function was called in array context (`my array @a = func()` or `foreach`), 0 otherwise. Only meaningful inside `dynamic` return type functions.

### core::wantscalar

```strada
my int $is_scalar = core::wantscalar();
```

Returns 1 if the function was called in scalar context (default), 0 otherwise.

### core::wanthash

```strada
my int $is_hash = core::wanthash();
```

Returns 1 if the function was called in hash context (`my hash %h = func()`), 0 otherwise.

Example:
```strada
func flexible() dynamic {
    if (core::wantarray()) {
        my array @r = (1, 2, 3);
        return @r;
    }
    if (core::wanthash()) {
        my hash %h = ();
        $h{"key"} = "value";
        return %h;
    }
    return 42;
}

my array @arr = flexible();   # (1, 2, 3)
my hash %h = flexible();      # {"key" => "value"}
my int $val = flexible();     # 42
```

## UTF-8 Functions (utf8:: namespace)

The `utf8::` namespace provides Perl-compatible UTF-8 introspection and normalization functions.

Strada strings carry an internal **UTF-8 flag** (mirroring Perl's `SVf_UTF8`, stored in the string header). When the flag is set, character-oriented built-ins — `length`, `substr`, `index`, `rindex`, `reverse`, `split`, `sprintf` field widths, and regex matching — count Unicode codepoints rather than raw bytes. The flag is set automatically by `chr(N)` for `N > 127`, by the `utf8::nfc/nfd/nfkc/nfkd/normalize` functions, and by non-ASCII string literals in a `use utf8` source file; it then propagates contagiously through concatenation, `join`, `sprintf`, substitution (`s///`), and similar operations. Pure-ASCII strings are treated as character-oriented implicitly (one byte == one codepoint). The `utf8::` functions below are mostly validation/no-op helpers — they report on or assert the flag, they do not transcode bytes.

### utf8::is_utf8

```strada
my int $valid = utf8::is_utf8($str);
```

Returns 1 if the string contains valid UTF-8 data, 0 otherwise.

### utf8::valid

```strada
my int $valid = utf8::valid($str);
```

Alias for `utf8::is_utf8()`.

### utf8::encode

```strada
my str $result = utf8::encode($str);
```

No-op in Strada (strings are already bytes). Returns the string unchanged.

### utf8::decode

```strada
my int $ok = utf8::decode($str);
```

Validates that the string is valid UTF-8. Returns 1 if valid, 0 if not.

### utf8::upgrade

```strada
my str $result = utf8::upgrade($str);
```

No-op in Strada (strings are already UTF-8 compatible). Returns the string unchanged.

### utf8::downgrade

```strada
my str $result = utf8::downgrade($str);
my scalar $result = utf8::downgrade($str, 1);
```

Returns the string if it contains only ASCII characters (bytes 0-127). If the string contains non-ASCII bytes, `utf8::downgrade()` dies with an error. With a second argument of 1 (fail_ok), it returns undef instead of dying.

### utf8::unicode_to_native

```strada
my int $code = utf8::unicode_to_native($codepoint);
```

Identity mapping on modern systems. Returns the codepoint unchanged.

### utf8::nfc / nfd / nfkc / nfkd / normalize

```strada
my str $composed   = utf8::nfc($str);          # canonical composition (NFC)
my str $decomposed = utf8::nfd($str);          # canonical decomposition (NFD)
my str $kc         = utf8::nfkc($str);         # compatibility composition (NFKC)
my str $kd         = utf8::nfkd($str);         # compatibility decomposition (NFKD)
my str $n          = utf8::normalize("NFC", $str);  # form selected by name
```

Full UAX#15 Unicode normalization backed by the Unicode 15.0.0 tables. Results
always have the UTF-8 flag set. `normalize()` takes the form name (`"NFC"`,
`"NFD"`, `"NFKC"`, `"NFKD"`) as its first argument.

### Notes

- Strada strings carry an internal UTF-8 flag (Perl's `SVf_UTF8`); see the overview at the top of this section for what sets it and which built-ins respect it.
- `chr(N)` for `N` in 0-127 produces a single ASCII byte; for `N >= 128` it UTF-8-encodes the codepoint **and sets the UTF-8 flag** (matching `use utf8` semantics).
- `utf8::encode()` and `utf8::upgrade()` are no-ops at the byte level (Strada strings are already byte strings); `utf8::decode()` validates UTF-8, and `utf8::downgrade()` asserts the string is ASCII-only.

### Example

```strada
my str $ascii = "Hello";
my str $utf8 = "Hello " . chr(233);   # "Hello " + e-acute (U+00E9)

say(utf8::is_utf8($ascii));  # 1 (ASCII is valid UTF-8)
say(utf8::is_utf8($utf8));   # 1

say(utf8::downgrade($ascii));  # "Hello" (ASCII-only, succeeds)

# utf8::downgrade($utf8);     # Would die: contains non-ASCII

my scalar $result = utf8::downgrade($utf8, 1);  # Returns undef (fail_ok)
if (!defined($result)) {
    say("String contains non-ASCII characters");
}
```

## Default Output Filehandle

### select

```strada
my scalar $prev = select($fh);
```

`select` is a bare builtin (there is no `core::select`).

Set the default filehandle for `print` and `say`. Returns the previous default filehandle.

When called with a filehandle argument, subsequent calls to `print()` and `say()` without a filehandle argument will write to the selected filehandle instead of stdout.

```strada
my scalar $prev = select($fh);         # Set $fh as default output
say("This goes to the file");          # Writes to $fh, not stdout
select($prev);                         # Restore previous default
```

When called with no arguments, returns the current default filehandle without changing it:

```strada
my scalar $current = select();
```

Example:

```strada
my scalar $log = core::open("/tmp/output.log", "w");

# Redirect all print/say output to log file
my scalar $old = select($log);
say("This goes to the log file");
say("So does this");

# Restore stdout
select($old);
say("This goes to stdout again");

core::close($log);
```

## See Also

- `math::` - Mathematical functions
- `ssl::` - SSL/TLS networking (in lib/ssl)
- `utf8::` - UTF-8 string introspection
- `LANGUAGE_GUIDE` - Complete language documentation
