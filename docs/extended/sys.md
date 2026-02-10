# sys:: Namespace Reference

The `sys::` namespace provides low-level system functions for file I/O, process control, networking, and C interop. These functions closely mirror POSIX/libc functionality.

## File I/O

### sys::open

```strada
my scalar $fh = sys::open($filename, $mode);
```

Open a file and return a file handle.

- `$filename` - Path to the file
- `$mode` - "r" (read), "w" (write), "a" (append), "r+" (read/write)

Returns a file handle on success, or undef on failure.

### sys::close

```strada
sys::close($fh);
```

Close a file handle.

### sys::slurp

```strada
my str $content = sys::slurp($filename);
```

Read entire file contents into a string.

### sys::slurp_fh

```strada
my str $content = sys::slurp_fh($fh);
```

Read remaining contents of a file handle into a string.

### sys::slurp_fd

```strada
my str $content = sys::slurp_fd($fd);
```

Read from a file descriptor into a string.

### sys::spew

```strada
sys::spew($filename, $content);
```

Write string to a file (overwrites existing content).

### sys::spew_fh

```strada
sys::spew_fh($fh, $content);
```

Write string to a file handle.

### sys::spew_fd

```strada
sys::spew_fd($fd, $content);
```

Write string to a file descriptor.

### sys::fwrite

```strada
my int $written = sys::fwrite($fh, $data, $size);
```

Write binary data to a file handle. Returns number of bytes written.

### sys::fread

```strada
my str $data = sys::fread($fh, $size);
```

Read binary data from a file handle.

### sys::readline

```strada
my str $line = sys::readline($fh);
```

Read a single line from a file handle (includes newline).

### sys::seek

```strada
sys::seek($fh, $offset, $whence);
```

Seek to position in file.

- `$whence` - 0 (SEEK_SET), 1 (SEEK_CUR), 2 (SEEK_END)

### sys::tell

```strada
my int $pos = sys::tell($fh);
```

Get current position in file.

### sys::rewind

```strada
sys::rewind($fh);
```

Reset file position to beginning.

### sys::eof

```strada
if (sys::eof($fh)) { ... }
```

Check if at end of file. Returns 1 if at EOF, 0 otherwise.

### sys::flush

```strada
sys::flush($fh);
```

Flush buffered output to file.

### sys::fgetc

```strada
my str $ch = sys::fgetc($fh);
```

Read a single character from file handle.

### sys::fputc

```strada
sys::fputc($fh, $char);
```

Write a single character to file handle.

### sys::fgets

```strada
my str $line = sys::fgets($fh, $maxlen);
```

Read up to `$maxlen` characters or until newline.

### sys::fputs

```strada
sys::fputs($fh, $str);
```

Write string to file handle.

### sys::ferror

```strada
if (sys::ferror($fh)) { ... }
```

Check if file handle has error.

### sys::fileno

```strada
my int $fd = sys::fileno($fh);
```

Get file descriptor number from file handle.

### sys::clearerr

```strada
sys::clearerr($fh);
```

Clear error and EOF flags on file handle.

## File System

### sys::unlink

```strada
sys::unlink($filename);
```

Delete a file.

### sys::link

```strada
sys::link($oldpath, $newpath);
```

Create a hard link.

### sys::symlink

```strada
sys::symlink($target, $linkpath);
```

Create a symbolic link.

### sys::readlink

```strada
my str $target = sys::readlink($linkpath);
```

Read the target of a symbolic link.

### sys::rename

```strada
sys::rename($oldname, $newname);
```

Rename/move a file.

### sys::mkdir

```strada
sys::mkdir($path, $mode);
```

Create a directory. `$mode` is octal permissions (e.g., 0755).

### sys::rmdir

```strada
sys::rmdir($path);
```

Remove an empty directory.

### sys::chdir

```strada
sys::chdir($path);
```

Change current working directory.

### sys::getcwd

```strada
my str $cwd = sys::getcwd();
```

Get current working directory.

### sys::chmod

```strada
sys::chmod($path, $mode);
```

Change file permissions.

### sys::access

```strada
my int $ok = sys::access($path, $mode);
```

Check file accessibility.

- `$mode` - 0 (F_OK exists), 1 (X_OK exec), 2 (W_OK write), 4 (R_OK read)

### sys::umask

```strada
my int $old = sys::umask($mask);
```

Set file creation mask. Returns previous mask.

### sys::stat

```strada
my hash %st = sys::stat($path);
```

Get file status. Returns hash with keys: `dev`, `ino`, `mode`, `nlink`, `uid`, `gid`, `size`, `atime`, `mtime`, `ctime`.

### sys::lstat

```strada
my hash %st = sys::lstat($path);
```

Like `stat` but doesn't follow symlinks.

### sys::readdir

```strada
my array @entries = sys::readdir($path);
```

Read directory contents. Returns array of filenames.

### sys::readdir_full

```strada
my array @entries = sys::readdir_full($path);
```

Read directory with full path for each entry.

### sys::is_dir

```strada
if (sys::is_dir($path)) { ... }
```

Check if path is a directory.

### sys::is_file

```strada
if (sys::is_file($path)) { ... }
```

Check if path is a regular file.

### sys::file_size

```strada
my int $size = sys::file_size($path);
```

Get file size in bytes.

### sys::realpath

```strada
my str $abs = sys::realpath($path);
```

Resolve to absolute path (resolving symlinks).

### sys::dirname

```strada
my str $dir = sys::dirname($path);
```

Get directory portion of path.

### sys::basename

```strada
my str $name = sys::basename($path);
```

Get filename portion of path.

### sys::file_ext

```strada
my str $ext = sys::file_ext($path);
```

Get file extension (e.g., "txt" from "file.txt").

### sys::glob

```strada
my array @files = sys::glob($pattern);
```

Expand glob pattern. Returns matching filenames.

### sys::fnmatch

```strada
if (sys::fnmatch($pattern, $string)) { ... }
```

Match string against shell wildcard pattern.

### sys::path_join

```strada
my str $path = sys::path_join($dir, $file);
```

Join path components.

### sys::truncate

```strada
sys::truncate($path, $length);
```

Truncate file to specified length.

### sys::ftruncate

```strada
sys::ftruncate($fd, $length);
```

Truncate open file to specified length.

### sys::chown

```strada
sys::chown($path, $uid, $gid);
```

Change file owner and group.

### sys::lchown

```strada
sys::lchown($path, $uid, $gid);
```

Like chown but doesn't follow symlinks.

### sys::fchmod

```strada
sys::fchmod($fd, $mode);
```

Change permissions of open file.

### sys::fchown

```strada
sys::fchown($fd, $uid, $gid);
```

Change owner of open file.

### sys::utime

```strada
sys::utime($path, $atime, $mtime);
```

Set file access and modification times.

### sys::utimes

```strada
sys::utimes($path, $atime, $mtime);
```

Set file times with microsecond precision.

## Temporary Files

### sys::tmpfile

```strada
my scalar $fh = sys::tmpfile();
```

Create a temporary file (auto-deleted on close).

### sys::mkstemp

```strada
my str $path = sys::mkstemp($template);
```

Create unique temporary file. Template should end with "XXXXXX".

### sys::mkdtemp

```strada
my str $path = sys::mkdtemp($template);
```

Create unique temporary directory.

## Process Control

### sys::fork

```strada
my int $pid = sys::fork();
```

Create child process. Returns 0 in child, child PID in parent.

### sys::exec

```strada
sys::exec($program, @args);
```

Replace current process with new program.

### sys::system

```strada
my int $status = sys::system($command);
```

Execute shell command. Returns exit status.

### sys::wait

```strada
my int $pid = sys::wait();
```

Wait for any child process to terminate.

### sys::waitpid

```strada
my int $status = sys::waitpid($pid, $options);
```

Wait for specific child process.

### sys::getpid

```strada
my int $pid = sys::getpid();
```

Get current process ID.

### sys::getppid

```strada
my int $ppid = sys::getppid();
```

Get parent process ID.

### sys::kill

```strada
sys::kill($pid, $signal);
```

Send signal to process.

### sys::alarm

```strada
my int $remaining = sys::alarm($seconds);
```

Set alarm timer. Sends SIGALRM after specified seconds.

### sys::signal

```strada
sys::signal($signame, \&handler);
sys::signal($signame, "IGNORE");
sys::signal($signame, "DEFAULT");
```

Set signal handler. `$signame` is "INT", "TERM", "HUP", etc.

### sys::sleep

```strada
sys::sleep($seconds);
```

Sleep for specified seconds.

### sys::usleep

```strada
sys::usleep($microseconds);
```

Sleep for specified microseconds.

### sys::nanosleep

```strada
sys::nanosleep($seconds, $nanoseconds);
```

High-precision sleep.

### sys::setprocname

```strada
sys::setprocname($name);
```

Set process name (as shown in `ps`).

### sys::getprocname

```strada
my str $name = sys::getprocname();
```

Get current process name.

### sys::setproctitle

```strada
sys::setproctitle($title);
```

Set process title.

### sys::getproctitle

```strada
my str $title = sys::getproctitle();
```

Get process title.

### sys::exit_status

```strada
my int $code = sys::exit_status($status);
```

Extract exit code from wait status.

## Command Execution

### sys::popen

```strada
my int $pipe = sys::popen($command, $mode);
```

Open pipe to/from command.

- `$mode` - "r" (read from command) or "w" (write to command)

### sys::pclose

```strada
my int $status = sys::pclose($pipe);
```

Close pipe and get exit status.

## IPC (Inter-Process Communication)

### sys::pipe

```strada
my array @fds = sys::pipe();
```

Create pipe. Returns [read_fd, write_fd].

### sys::dup2

```strada
sys::dup2($oldfd, $newfd);
```

Duplicate file descriptor.

### sys::dup

```strada
my int $newfd = sys::dup($fd);
```

Duplicate file descriptor to lowest available.

### sys::close_fd

```strada
sys::close_fd($fd);
```

Close file descriptor.

### sys::read_fd

```strada
my str $data = sys::read_fd($fd, $count);
```

Read from file descriptor.

### sys::write_fd

```strada
my int $written = sys::write_fd($fd, $data);
```

Write to file descriptor.

### sys::read_all_fd

```strada
my str $data = sys::read_all_fd($fd);
```

Read all available data from file descriptor.

## Environment

### sys::getenv

```strada
my str $value = sys::getenv($name);
```

Get environment variable.

### sys::setenv

```strada
sys::setenv($name, $value);
```

Set environment variable.

### sys::unsetenv

```strada
sys::unsetenv($name);
```

Remove environment variable.

## User/Group

### sys::getuid

```strada
my int $uid = sys::getuid();
```

Get real user ID.

### sys::geteuid

```strada
my int $euid = sys::geteuid();
```

Get effective user ID.

### sys::getgid

```strada
my int $gid = sys::getgid();
```

Get real group ID.

### sys::getegid

```strada
my int $egid = sys::getegid();
```

Get effective group ID.

### sys::setuid

```strada
sys::setuid($uid);
```

Set user ID.

### sys::setgid

```strada
sys::setgid($gid);
```

Set group ID.

### sys::seteuid

```strada
sys::seteuid($uid);
```

Set effective user ID.

### sys::setegid

```strada
sys::setegid($gid);
```

Set effective group ID.

### sys::getpwnam

```strada
my hash %pw = sys::getpwnam($username);
```

Get user info by name. Returns hash with: `name`, `passwd`, `uid`, `gid`, `gecos`, `dir`, `shell`.

### sys::getpwuid

```strada
my hash %pw = sys::getpwuid($uid);
```

Get user info by UID.

### sys::getgrnam

```strada
my hash %gr = sys::getgrnam($groupname);
```

Get group info by name.

### sys::getgrgid

```strada
my hash %gr = sys::getgrgid($gid);
```

Get group info by GID.

### sys::getlogin

```strada
my str $user = sys::getlogin();
```

Get login name.

### sys::getgroups

```strada
my array @gids = sys::getgroups();
```

Get supplementary group IDs.

## Time

### sys::time

```strada
my int $epoch = sys::time();
```

Get current Unix timestamp.

### sys::localtime

```strada
my hash %tm = sys::localtime($time);
```

Convert timestamp to local time. Returns hash with: `sec`, `min`, `hour`, `mday`, `mon`, `year`, `wday`, `yday`, `isdst`.

### sys::gmtime

```strada
my hash %tm = sys::gmtime($time);
```

Convert timestamp to UTC time.

### sys::mktime

```strada
my int $time = sys::mktime(%tm);
```

Convert time hash to timestamp.

### sys::strftime

```strada
my str $formatted = sys::strftime($format, %tm);
```

Format time as string.

### sys::ctime

```strada
my str $str = sys::ctime($time);
```

Convert timestamp to string.

### sys::gettimeofday

```strada
my array @tv = sys::gettimeofday();
```

Get time with microsecond precision. Returns [seconds, microseconds].

### sys::hires_time

```strada
my num $time = sys::hires_time();
```

Get high-resolution time as floating point seconds.

### sys::tv_interval

```strada
my num $elapsed = sys::tv_interval(@start, @end);
```

Calculate interval between two gettimeofday results.

### sys::clock_gettime

```strada
my array @ts = sys::clock_gettime($clock_id);
```

Get time from specified clock.

### sys::clock_getres

```strada
my array @res = sys::clock_getres($clock_id);
```

Get clock resolution.

### sys::difftime

```strada
my num $diff = sys::difftime($time1, $time0);
```

Calculate difference between two times.

### sys::clock

```strada
my int $ticks = sys::clock();
```

Get processor time.

### sys::times

```strada
my hash %times = sys::times();
```

Get process times.

## Sockets

### sys::socket_client

```strada
my int $sock = sys::socket_client($host, $port);
```

Create TCP client connection.

### sys::socket_server

```strada
my int $sock = sys::socket_server($port);
```

Create TCP server socket (default backlog).

### sys::socket_server_backlog

```strada
my int $sock = sys::socket_server_backlog($port, $backlog);
```

Create TCP server socket with specified backlog.

### sys::socket_accept

```strada
my int $client = sys::socket_accept($server_sock);
```

Accept incoming connection.

### sys::socket_recv

```strada
my str $data = sys::socket_recv($sock, $maxlen);
```

Receive data from socket.

### sys::socket_send

```strada
my int $sent = sys::socket_send($sock, $data);
```

Send data on socket.

### sys::socket_close

```strada
sys::socket_close($sock);
```

Close socket.

### sys::socket_select

```strada
my int $ready = sys::socket_select($sock, $timeout_ms);
```

Wait for socket to be ready.

### sys::socket_fd

```strada
my int $fd = sys::socket_fd($sock);
```

Get file descriptor from socket.

### sys::select_fds

```strada
my array @ready = sys::select_fds(@read_fds, @write_fds, $timeout_ms);
```

Select on multiple file descriptors.

### sys::setsockopt

```strada
sys::setsockopt($sock, $level, $optname, $value);
```

Set socket option.

### sys::getsockopt

```strada
my scalar $value = sys::getsockopt($sock, $level, $optname);
```

Get socket option.

### sys::shutdown

```strada
sys::shutdown($sock, $how);
```

Shutdown socket. `$how`: 0=read, 1=write, 2=both.

### sys::getpeername

```strada
my str $addr = sys::getpeername($sock);
```

Get address of peer.

### sys::getsockname

```strada
my str $addr = sys::getsockname($sock);
```

Get local socket address.

### sys::poll

```strada
my array @ready = sys::poll(@fds, $timeout_ms);
```

Poll multiple file descriptors.

## DNS/Network

### sys::gethostbyname

```strada
my str $ip = sys::gethostbyname($hostname);
```

Resolve hostname to IP address.

### sys::gethostbyname_all

```strada
my array @ips = sys::gethostbyname_all($hostname);
```

Resolve hostname to all IP addresses.

### sys::gethostname

```strada
my str $hostname = sys::gethostname();
```

Get local hostname.

### sys::getaddrinfo

```strada
my array @addrs = sys::getaddrinfo($host, $service);
```

Get address info for host and service.

### sys::inet_pton

```strada
my str $binary = sys::inet_pton($af, $addr);
```

Convert IP address string to binary.

### sys::inet_ntop

```strada
my str $addr = sys::inet_ntop($af, $binary);
```

Convert binary IP to string.

### sys::inet_addr

```strada
my int $addr = sys::inet_addr($ip);
```

Convert dotted-decimal IP to integer.

### sys::inet_ntoa

```strada
my str $ip = sys::inet_ntoa($addr);
```

Convert integer IP to dotted-decimal.

### sys::htons

```strada
my int $net = sys::htons($host);
```

Host to network short.

### sys::htonl

```strada
my int $net = sys::htonl($host);
```

Host to network long.

### sys::ntohs

```strada
my int $host = sys::ntohs($net);
```

Network to host short.

### sys::ntohl

```strada
my int $host = sys::ntohl($net);
```

Network to host long.

## Error Handling

### sys::errno

```strada
my int $err = sys::errno();
```

Get last error number.

### sys::strerror

```strada
my str $msg = sys::strerror($errno);
```

Get error message for error number.

### sys::isatty

```strada
if (sys::isatty($fd)) { ... }
```

Check if file descriptor is a terminal.

## Dynamic Loading (FFI)

### sys::dl_open

```strada
my int $handle = sys::dl_open($library);
```

Load shared library. Returns handle.

### sys::dl_sym

```strada
my int $func = sys::dl_sym($handle, $symbol);
```

Get symbol address from library.

### sys::dl_close

```strada
sys::dl_close($handle);
```

Unload shared library.

### sys::dl_error

```strada
my str $err = sys::dl_error();
```

Get last dynamic loading error.

### sys::dl_call_sv

```strada
my scalar $result = sys::dl_call_sv($func, [$arg1, $arg2, ...]);
```

Call Strada function in shared library.

### sys::dl_call_int

```strada
my int $result = sys::dl_call_int($func, $arg1, $arg2);
```

Call C function returning int.

### sys::dl_call_str

```strada
my str $result = sys::dl_call_str($func, $arg1, $arg2);
```

Call C function returning string.

### sys::dl_call_void

```strada
sys::dl_call_void($func, $arg1, $arg2);
```

Call C function returning void.

### sys::dl_call_int_sv / sys::dl_call_str_sv / sys::dl_call_void_sv

Pass StradaValue* directly to C functions.

### sys::dl_call_export_info

```strada
my str $info = sys::dl_call_export_info($func);
```

Get export info from Strada library.

### sys::dl_call_version

```strada
my str $version = sys::dl_call_version($func);
```

Get version from Strada library.

## String Conversion

### sys::atoi

```strada
my int $n = sys::atoi($str);
```

Convert string to integer.

### sys::atof

```strada
my num $n = sys::atof($str);
```

Convert string to float.

### sys::strtol

```strada
my int $n = sys::strtol($str, $base);
```

Convert string to integer with base.

### sys::strtod

```strada
my num $n = sys::strtod($str);
```

Convert string to double.

## Binary/Byte Operations

### sys::ord_byte

```strada
my int $byte = sys::ord_byte($str, $index);
```

Get byte value at index.

### sys::get_byte

```strada
my int $byte = sys::get_byte($str, $index);
```

Alias for ord_byte.

### sys::set_byte

```strada
my str $new = sys::set_byte($str, $index, $value);
```

Set byte at index.

### sys::byte_length

```strada
my int $len = sys::byte_length($str);
```

Get byte length of string.

### sys::byte_substr

```strada
my str $sub = sys::byte_substr($str, $offset, $length);
```

Extract byte substring.

### sys::pack

```strada
my str $binary = sys::pack($template, @values);
```

Pack values into binary string.

### sys::unpack

```strada
my array @values = sys::unpack($template, $binary);
```

Unpack binary string to values.

### sys::base64_encode

```strada
my str $encoded = sys::base64_encode($data);
```

Encode data as base64.

### sys::base64_decode

```strada
my str $data = sys::base64_decode($encoded);
```

Decode base64 data.

## Memory

### sys::malloc

```strada
my int $ptr = sys::malloc($size);
```

Allocate memory. Returns pointer.

### sys::calloc

```strada
my int $ptr = sys::calloc($count, $size);
```

Allocate zeroed memory.

### sys::realloc

```strada
my int $ptr = sys::realloc($ptr, $size);
```

Reallocate memory.

### sys::free

```strada
sys::free($ptr);
```

Free allocated memory.

### sys::release

```strada
sys::release($sv);
```

Release StradaValue reference.

### sys::mmap

```strada
my int $ptr = sys::mmap($addr, $length, $prot, $flags, $fd, $offset);
```

Map file into memory.

### sys::munmap

```strada
sys::munmap($ptr, $length);
```

Unmap memory.

### sys::mlock

```strada
sys::mlock($ptr, $length);
```

Lock memory pages.

### sys::munlock

```strada
sys::munlock($ptr, $length);
```

Unlock memory pages.

## Array/Hash Utilities

### sys::array_capacity

```strada
my int $cap = sys::array_capacity(@arr);
```

Get array's allocated capacity.

### sys::array_reserve

```strada
sys::array_reserve(@arr, $count);
```

Pre-allocate array capacity.

### sys::array_shrink

```strada
sys::array_shrink(@arr);
```

Shrink array capacity to match length.

### sys::hash_default_capacity

```strada
sys::hash_default_capacity($size);
```

Set default capacity for new hashes.

## Random Numbers

### sys::rand

```strada
my int $n = sys::rand();
```

Get random integer.

### sys::random

```strada
my int $n = sys::random();
```

Get random integer (better quality).

### sys::srand

```strada
sys::srand($seed);
```

Seed random number generator.

### sys::srandom

```strada
sys::srandom($seed);
```

Seed random number generator (for random()).

## Session/Process Group

### sys::setsid

```strada
my int $sid = sys::setsid();
```

Create new session.

### sys::getsid

```strada
my int $sid = sys::getsid($pid);
```

Get session ID.

### sys::setpgid

```strada
sys::setpgid($pid, $pgid);
```

Set process group ID.

### sys::getpgid

```strada
my int $pgid = sys::getpgid($pid);
```

Get process group ID.

### sys::getpgrp

```strada
my int $pgrp = sys::getpgrp();
```

Get process group.

### sys::setpgrp

```strada
sys::setpgrp();
```

Set process group.

## Advanced Signals

### sys::sigaction

```strada
sys::sigaction($signum, \&handler);
```

Set signal action.

### sys::sigprocmask

```strada
sys::sigprocmask($how, @signals);
```

Change blocked signals.

### sys::raise

```strada
sys::raise($signal);
```

Send signal to self.

### sys::killpg

```strada
sys::killpg($pgrp, $signal);
```

Send signal to process group.

### sys::pause

```strada
sys::pause();
```

Wait for signal.

### sys::sigsuspend

```strada
sys::sigsuspend(@signals);
```

Wait for signal with mask.

## Resource/Priority

### sys::nice

```strada
my int $new = sys::nice($inc);
```

Change process priority.

### sys::getpriority

```strada
my int $pri = sys::getpriority($which, $who);
```

Get process priority.

### sys::setpriority

```strada
sys::setpriority($which, $who, $priority);
```

Set process priority.

### sys::getrusage

```strada
my hash %usage = sys::getrusage($who);
```

Get resource usage.

### sys::getrlimit

```strada
my hash %limit = sys::getrlimit($resource);
```

Get resource limits.

### sys::setrlimit

```strada
sys::setrlimit($resource, $soft, $hard);
```

Set resource limits.

## Terminal/TTY

### sys::ttyname

```strada
my str $name = sys::ttyname($fd);
```

Get terminal name.

### sys::tcgetattr

```strada
my hash %attr = sys::tcgetattr($fd);
```

Get terminal attributes.

### sys::tcsetattr

```strada
sys::tcsetattr($fd, $when, %attr);
```

Set terminal attributes.

### sys::cfgetospeed

```strada
my int $speed = sys::cfgetospeed(%termios);
```

Get output baud rate.

### sys::cfsetospeed

```strada
sys::cfsetospeed(%termios, $speed);
```

Set output baud rate.

### sys::cfgetispeed

```strada
my int $speed = sys::cfgetispeed(%termios);
```

Get input baud rate.

### sys::cfsetispeed

```strada
sys::cfsetispeed(%termios, $speed);
```

Set input baud rate.

## Advanced File Operations

### sys::fcntl

```strada
my int $result = sys::fcntl($fd, $cmd, $arg);
```

File control operations.

### sys::flock

```strada
sys::flock($fd, $operation);
```

Apply file lock.

### sys::ioctl

```strada
my int $result = sys::ioctl($fd, $request, $arg);
```

I/O control.

### sys::statvfs

```strada
my hash %st = sys::statvfs($path);
```

Get filesystem statistics.

### sys::fstatvfs

```strada
my hash %st = sys::fstatvfs($fd);
```

Get filesystem statistics for open file.

## Debugging

### sys::stack_trace

```strada
my str $trace = sys::stack_trace();
```

Get the current call stack as a string. Returns a multi-line string showing the function call chain from innermost to outermost.

Example:
```strada
func debug_here() void {
    my str $trace = sys::stack_trace();
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

### sys::set_recursion_limit

```strada
sys::set_recursion_limit($limit);
```

Set the maximum recursion depth. When the call stack exceeds this limit, the program will print an error with a stack trace and exit.

- `$limit` - Maximum recursion depth (default: 1000, set to 0 to disable)

Example:
```strada
# Increase limit for deeply recursive algorithms
sys::set_recursion_limit(5000);

# Disable limit (not recommended)
sys::set_recursion_limit(0);
```

### sys::get_recursion_limit

```strada
my int $limit = sys::get_recursion_limit();
```

Get the current maximum recursion depth.

Example:
```strada
my int $limit = sys::get_recursion_limit();
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

Hint: Use sys::set_recursion_limit(n) to increase the limit, or 0 to disable.
```

## See Also

- `math::` - Mathematical functions
- `ssl::` - SSL/TLS networking (in lib/ssl)
- `LANGUAGE_GUIDE` - Complete language documentation
