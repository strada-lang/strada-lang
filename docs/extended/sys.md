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

Read entire file contents into a string.

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
my int $written = core::fwrite($fh, $data, $size);
```

Write binary data to a file handle. Returns number of bytes written.

### core::fread

```strada
my str $data = core::fread($fh, $size);
```

Read binary data from a file handle.

### core::readline

```strada
my str $line = core::readline($fh);
```

Read a single line from a file handle (includes newline).

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

Check if at end of file. Returns 1 if at EOF, 0 otherwise.

### core::flush

```strada
core::flush($fh);
```

Flush buffered output to file.

### core::fgetc

```strada
my str $ch = core::fgetc($fh);
```

Read a single character from file handle.

### core::fputc

```strada
core::fputc($fh, $char);
```

Write a single character to file handle.

### core::fgets

```strada
my str $line = core::fgets($fh, $maxlen);
```

Read up to `$maxlen` characters or until newline.

### core::fputs

```strada
core::fputs($fh, $str);
```

Write string to file handle.

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

Check file accessibility.

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

Get file status. Returns hash with keys: `dev`, `ino`, `mode`, `nlink`, `uid`, `gid`, `size`, `atime`, `mtime`, `ctime`.

### core::lstat

```strada
my hash %st = core::lstat($path);
```

Like `stat` but doesn't follow symlinks.

### core::readdir

```strada
my array @entries = core::readdir($path);
```

Read directory contents. Returns array of filenames.

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

Get file size in bytes.

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
my str $path = core::path_join($dir, $file);
```

Join path components.

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
my str $path = core::mkstemp($template);
```

Create unique temporary file. Template should end with "XXXXXX".

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
core::exec($program, @args);
```

Replace current process with new program.

### core::system

```strada
my int $status = core::system($command);
```

Execute shell command. Returns exit status.

### core::wait

```strada
my int $pid = core::wait();
```

Wait for any child process to terminate.

### core::waitpid

```strada
my int $status = core::waitpid($pid, $options);
```

Wait for specific child process.

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

Set signal handler. `$signame` is "INT", "TERM", "HUP", etc.

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
core::nanosleep($seconds, $nanoseconds);
```

High-precision sleep.

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

Extract exit code from wait status.

## Command Execution

### core::popen

```strada
my int $pipe = core::popen($command, $mode);
```

Open pipe to/from command.

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

Create pipe. Returns [read_fd, write_fd].

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

Convert timestamp to UTC time.

### core::mktime

```strada
my int $time = core::mktime(%tm);
```

Convert time hash to timestamp.

### core::strftime

```strada
my str $formatted = core::strftime($format, %tm);
```

Format time as string.

### core::ctime

```strada
my str $str = core::ctime($time);
```

Convert timestamp to string.

### core::gettimeofday

```strada
my array @tv = core::gettimeofday();
```

Get time with microsecond precision. Returns [seconds, microseconds].

### core::hires_time

```strada
my num $time = core::hires_time();
```

Get high-resolution time as floating point seconds.

### core::tv_interval

```strada
my num $elapsed = core::tv_interval(@start, @end);
```

Calculate interval between two gettimeofday results.

### core::clock_gettime

```strada
my array @ts = core::clock_gettime($clock_id);
```

Get time from specified clock.

### core::clock_getres

```strada
my array @res = core::clock_getres($clock_id);
```

Get clock resolution.

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

Get process times.

## Sockets

### core::socket_client

```strada
my int $sock = core::socket_client($host, $port);
```

Create TCP client connection.

### core::socket_server

```strada
my int $sock = core::socket_server($port);
```

Create TCP server socket (default backlog).

### core::socket_server_backlog

```strada
my int $sock = core::socket_server_backlog($port, $backlog);
```

Create TCP server socket with specified backlog.

### core::socket_accept

```strada
my int $client = core::socket_accept($server_sock);
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
my int $ready = core::socket_select($sock, $timeout_ms);
```

Wait for socket to be ready.

### core::socket_fd

```strada
my int $fd = core::socket_fd($sock);
```

Get file descriptor from socket.

### core::select_fds

```strada
my array @ready = core::select_fds(@read_fds, @write_fds, $timeout_ms);
```

Select on multiple file descriptors.

### core::setsockopt

```strada
core::setsockopt($sock, $level, $optname, $value);
```

Set socket option.

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
my str $addr = core::getpeername($sock);
```

Get address of peer.

### core::getsockname

```strada
my str $addr = core::getsockname($sock);
```

Get local socket address.

### core::poll

```strada
my array @ready = core::poll(@fds, $timeout_ms);
```

Poll multiple file descriptors.

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
my array @addrs = core::getaddrinfo($host, $service);
```

Get address info for host and service.

### core::inet_pton

```strada
my str $binary = core::inet_pton($af, $addr);
```

Convert IP address string to binary.

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
my scalar $result = core::dl_call_sv($func, [$arg1, $arg2, ...]);
```

Call Strada function in shared library.

### core::dl_call_int

```strada
my int $result = core::dl_call_int($func, $arg1, $arg2);
```

Call C function returning int.

### core::dl_call_str

```strada
my str $result = core::dl_call_str($func, $arg1, $arg2);
```

Call C function returning string.

### core::dl_call_void

```strada
core::dl_call_void($func, $arg1, $arg2);
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
my int $n = core::strtol($str, $base);
```

Convert string to integer with base.

### core::strtod

```strada
my num $n = core::strtod($str);
```

Convert string to double.

## Binary/Byte Operations

### core::ord_byte

```strada
my int $byte = core::ord_byte($str, $index);
```

Get byte value at index.

### core::get_byte

```strada
my int $byte = core::get_byte($str, $index);
```

Alias for ord_byte.

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

### core::malloc

```strada
my int $ptr = core::malloc($size);
```

Allocate memory. Returns pointer.

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
core::free($ptr);
```

Free allocated memory.

### core::release

```strada
core::release($sv);
```

Release StradaValue reference.

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

### core::array_capacity

```strada
my int $cap = core::array_capacity(@arr);
```

Get array's allocated capacity.

### core::array_reserve

```strada
core::array_reserve(@arr, $count);
```

Pre-allocate array capacity.

### core::array_shrink

```strada
core::array_shrink(@arr);
```

Shrink array capacity to match length.

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

### core::sigaction

```strada
core::sigaction($signum, \&handler);
```

Set signal action.

### core::sigprocmask

```strada
core::sigprocmask($how, @signals);
```

Change blocked signals.

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

### core::sigsuspend

```strada
core::sigsuspend(@signals);
```

Wait for signal with mask.

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

Get resource usage.

### core::getrlimit

```strada
my hash %limit = core::getrlimit($resource);
```

Get resource limits.

### core::setrlimit

```strada
core::setrlimit($resource, $soft, $hard);
```

Set resource limits.

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

Get terminal attributes.

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

Output:
```
Current stack:
  at debug_here (myprogram.strada)
  at inner (myprogram.strada)
  at outer (myprogram.strada)
  at main (myprogram.strada)
```

This is useful for debugging, logging, and error reporting. Note that uncaught exceptions automatically print a stack trace.

### core::set_recursion_limit

```strada
core::set_recursion_limit($limit);
```

Set the maximum recursion depth. When the call stack exceeds this limit, the program will print an error with a stack trace and exit.

- `$limit` - Maximum recursion depth (default: 1000, set to 0 to disable)

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
  at recursive_func (myprogram.strada)
  at recursive_func (myprogram.strada)
  ...
  at main (myprogram.strada)
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

The `utf8::` namespace provides Perl-compatible UTF-8 introspection functions. In Strada, all strings are stored as raw bytes (typically UTF-8). There is no separate "UTF-8 flag" like Perl has.

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

### Notes

- In Strada, all strings are stored as raw bytes. There is no internal "UTF-8 flag" like Perl.
- `chr()` for values 0-255 produces raw bytes (Latin-1), not UTF-8 encoded. Use `chr()` with values >= 256 for multibyte UTF-8.
- `utf8::encode()` and `utf8::upgrade()` are no-ops because Strada strings are already byte strings.

### Example

```strada
my str $ascii = "Hello";
my str $utf8 = "Hello \x{C3}\x{A9}";  # "Hello " + UTF-8 e-acute

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

### core::select

```strada
my scalar $prev = core::select($fh);
```

Set the default filehandle for `print` and `say`. Returns the previous default filehandle.

When called with a filehandle argument, subsequent calls to `print()` and `say()` without a filehandle argument will write to the selected filehandle instead of stdout.

```strada
my scalar $prev = core::select($fh);   # Set $fh as default output
say("This goes to $fh");               # Writes to $fh, not stdout
core::select($prev);                    # Restore previous default
```

When called with no arguments, returns the current default filehandle without changing it:

```strada
my scalar $current = core::select();
```

Example:

```strada
my scalar $log = core::open("/tmp/output.log", "w");

# Redirect all print/say output to log file
my scalar $old = core::select($log);
say("This goes to the log file");
say("So does this");

# Restore stdout
core::select($old);
say("This goes to stdout again");

core::close($log);
```

## See Also

- `math::` - Mathematical functions
- `ssl::` - SSL/TLS networking (in lib/ssl)
- `utf8::` - UTF-8 string introspection
- `LANGUAGE_GUIDE` - Complete language documentation
