# Strada Built-in Function Reference

Complete reference for every built-in function available in Strada. Organized
by namespace.

- `core::` — system / libc functions (this is what you should write)
- `math::` — math functions
- `async::` — async / threading
- `c::` — low-level memory / FFI

Bare built-ins (no namespace) and Perl-compatibility helpers are listed at the
end.

> **Note on `sys::`** — `sys::` is the legacy alias for `core::` and still
> compiles (the compiler normalizes `core::` to `sys::` internally), but new
> code should use `core::`.

For the C runtime API (the `strada_*` functions the generated C calls into),
see [RUNTIME_API.md](RUNTIME_API.md).

---

## math:: — Math functions

All math functions take and return scalars (int or num). Most are libm wrappers.

| Function | Signature | Description |
|---|---|---|
| `math::abs(x)` | `num → num` | Absolute value (float). |
| `math::acos(x)` | `num → num` | Arc cosine, result in [0, π]. |
| `math::asin(x)` | `num → num` | Arc sine, result in [-π/2, π/2]. |
| `math::atan(x)` | `num → num` | Arc tangent of x. |
| `math::cbrt(x)` | `num → num` | Cube root. |
| `math::ceil(x)` | `num → num` | Smallest integer ≥ x. |
| `math::copysign(x, y)` | `num, num → num` | x with sign of y. |
| `math::cos(x)` | `num → num` | Cosine (radians). |
| `math::cosh(x)` | `num → num` | Hyperbolic cosine. |
| `math::exp(x)` | `num → num` | e^x. |
| `math::fabs(x)` | `num → num` | Absolute value (alias for abs). |
| `math::floor(x)` | `num → num` | Largest integer ≤ x. |
| `math::fmax(x, y)` | `num, num → num` | Maximum of two values. |
| `math::fmin(x, y)` | `num, num → num` | Minimum of two values. |
| `math::fmod(x, y)` | `num, num → num` | Floating-point remainder of x/y. |
| `math::frexp(x)` | `num → array` | Mantissa and exponent: returns `[m, e]` where x = m × 2^e. |
| `math::hypot(x, y)` | `num, num → num` | sqrt(x² + y²) without overflow. |
| `math::isfinite(x)` | `num → int` | 1 if x is finite, 0 otherwise. |
| `math::isinf(x)` | `num → int` | 1 if x is infinity. |
| `math::isnan(x)` | `num → int` | 1 if x is NaN. |
| `math::ldexp(x, e)` | `num, int → num` | x × 2^e. |
| `math::log(x)` | `num → num` | Natural logarithm (base e). |
| `math::modf(x)` | `num → array` | Splits into integer and fractional parts: `[ipart, fpart]`. |
| `math::pow(x, y)` | `num, num → num` | x^y. |
| `math::rand()` | `→ num` | Random float in [0, 1). |
| `math::remainder(x, y)` | `num, num → num` | IEEE 754 remainder. |
| `math::round(x)` | `num → num` | Round to nearest integer (away from zero on halfway). |
| `math::scalbn(x, e)` | `num, int → num` | x × 2^e (integer exponent). |
| `math::sin(x)` | `num → num` | Sine (radians). |
| `math::sinh(x)` | `num → num` | Hyperbolic sine. |
| `math::sqrt(x)` | `num → num` | Square root. |
| `math::tan(x)` | `num → num` | Tangent (radians). |
| `math::tanh(x)` | `num → num` | Hyperbolic tangent. |
| `math::trunc(x)` | `num → num` | Truncate towards zero. |

---

## async:: — Async, Threading, IPC

Strada uses a thread-pool backend (default 4 workers). Most async primitives
are based on POSIX threads underneath.

### Futures

| Function | Signature | Description |
|---|---|---|
| `async::all(@futures)` | `array → array` | Wait for all futures; returns array of results in order. |
| `async::race(@futures)` | `array → scalar` | Wait for first future to complete; returns its value. |
| `async::timeout(future, ms)` | `scalar, int → scalar` | Wait up to ms milliseconds; returns value or undef on timeout. |
| `async::cancel(future)` | `scalar → int` | Request cancellation. Returns 1 if it was running, 0 otherwise. |
| `async::is_cancelled(future)` | `scalar → int` | 1 if cancellation has been requested. |
| `async::is_done(future)` | `scalar → int` | 1 if future has completed (success or failure). |
| `async::pool_init(n)` | `int → void` | Initialize the thread pool with n workers. |
| `async::pool_shutdown()` | `→ void` | Drain pending work and shut down the pool. |

### Channels

| Function | Signature | Description |
|---|---|---|
| `async::channel(capacity)` | `int → scalar` | Create a buffered channel. capacity 0 = unbuffered. |
| `async::send(ch, value)` | `scalar, any → int` | Send value; blocks if channel is full. Returns 1 on success. |
| `async::recv(ch)` | `scalar → any` | Receive a value; blocks if channel is empty. Returns undef on close. |
| `async::try_send(ch, value)` | `scalar, any → int` | Non-blocking send; returns 0 if would block. |
| `async::try_recv(ch)` | `scalar → any` | Non-blocking receive; returns undef if would block. |
| `async::close(ch)` | `scalar → void` | Close channel; subsequent recv returns undef. |
| `async::is_closed(ch)` | `scalar → int` | 1 if closed. |
| `async::len(ch)` | `scalar → int` | Number of items currently in the channel. |

### Mutexes

| Function | Signature | Description |
|---|---|---|
| `async::mutex()` | `→ scalar` | Create a new mutex. |
| `async::lock(mtx)` | `scalar → void` | Acquire the mutex (blocks). |
| `async::try_lock(mtx)` | `scalar → int` | Non-blocking lock; returns 1 if acquired, 0 otherwise. |
| `async::unlock(mtx)` | `scalar → void` | Release the mutex. |
| `async::mutex_destroy(mtx)` | `scalar → void` | Free mutex resources. |

### Atomics

| Function | Signature | Description |
|---|---|---|
| `async::atomic(val)` | `int → scalar` | Create an atomic int initialized to val. |
| `async::atomic_load(at)` | `scalar → int` | Read current value (memory_order_seq_cst). |
| `async::atomic_store(at, v)` | `scalar, int → void` | Write value. |
| `async::atomic_add(at, n)` | `scalar, int → int` | Add n; returns previous value. |
| `async::atomic_sub(at, n)` | `scalar, int → int` | Subtract n; returns previous value. |
| `async::atomic_inc(at)` | `scalar → int` | Increment by 1; returns previous. |
| `async::atomic_dec(at)` | `scalar → int` | Decrement by 1; returns previous. |
| `async::atomic_cas(at, expected, new)` | `scalar, int, int → int` | Compare-and-swap; returns 1 if swap happened. |

---

## c:: — Low-level memory and FFI

| Function | Signature | Description |
|---|---|---|
| `c::alloc(size)` | `int → scalar` | malloc(size) wrapper; returns an opaque pointer. |
| `c::realloc(ptr, size)` | `scalar, int → scalar` | realloc; returns possibly-new pointer. |
| `c::free(ptr)` | `scalar → void` | free() the pointer. |
| `c::null()` | `→ scalar` | NULL pointer literal. |
| `c::is_null(ptr)` | `scalar → int` | 1 if ptr is NULL. |
| `c::memcpy(dst, src, n)` | `scalar, scalar, int → void` | Copy n bytes. |
| `c::memset(ptr, byte, n)` | `scalar, int, int → void` | Fill n bytes with byte value. |
| `c::ptr_add(ptr, offset)` | `scalar, int → scalar` | Pointer arithmetic: ptr + offset bytes. |
| `c::ptr_to_str(ptr)` | `scalar → str` | Treat ptr as NUL-terminated C string; copy into a Strada str. |
| `c::ptr_to_str_n(ptr, n)` | `scalar, int → str` | Copy n bytes starting at ptr. |
| `c::str_to_ptr(s)` | `str → scalar` | Return a pointer to the string's underlying bytes. |
| `c::read_float(ptr)` | `scalar → num` | Read a float at ptr. |
| `c::read_double(ptr)` | `scalar → num` | Read a double at ptr. |
| `c::read_ptr(ptr)` | `scalar → scalar` | Dereference a pointer-to-pointer. |
| `c::write_float(ptr, val)` | `scalar, num → void` | Write a float at ptr. |
| `c::write_double(ptr, val)` | `scalar, num → void` | Write a double at ptr. |
| `c::write_ptr(ptr, val)` | `scalar, scalar → void` | Write a pointer at ptr. |
| `c::sizeof_int()` | `→ int` | sizeof(int) on this platform (usually 4). |
| `c::sizeof_long()` | `→ int` | sizeof(long) (8 on 64-bit Linux/macOS). |
| `c::sizeof_ptr()` | `→ int` | sizeof(void*) (8 on 64-bit). |
| `c::sizeof_size_t()` | `→ int` | sizeof(size_t) (8 on 64-bit). |

---

## core:: — System interface

Use `core::` in user code. `sys::` is a legacy alias (the compiler normalizes
`core::` to `sys::` internally), but new code should use `core::`.

### Process control

| Function | Signature | Description |
|---|---|---|
| `core::fork()` | `→ int` | fork(2); returns child pid in parent, 0 in child, -1 on error. |
| `core::exec(cmd)` | `str → int` | execlp; replaces current process. |
| `core::exec_argv(@argv)` | `array → int` | execvp with argument list. |
| `core::system(cmd)` | `str → int` | Shell-out via /bin/sh -c; returns exit status. |
| `core::system_argv(@argv)` | `array → int` | system() with argv (no shell). |
| `core::wait()` | `→ int` | wait(2); returns child pid; updates $?. |
| `core::waitpid(pid, flags)` | `int, int → int` | waitpid(2). |
| `core::kill(sig, pid)` | `str/int, int → int` | Send signal. Sig may be name ("USR1") or number. |
| `core::killpg(pgid, sig)` | `int, int → int` | killpg(2). |
| `core::exit(code)` | `int → void` | Run END blocks then exit(code). |
| `core::_exit(code)` | `int → void` | _exit(2); skips END blocks. |
| `core::exit_status(status)` | `int → int` | WEXITSTATUS-equivalent extraction. |
| `core::getpid()` | `→ int` | Current process id. |
| `core::getppid()` | `→ int` | Parent process id. |
| `core::getpgid(pid)` | `int → int` | Process group id. |
| `core::getpgrp()` | `→ int` | Process group id of current process. |
| `core::setpgid(pid, pgid)` | `int, int → int` | Set process group. |
| `core::setpgrp()` | `→ int` | Make current process a process group leader. |
| `core::setsid()` | `→ int` | Create a new session. |
| `core::getsid(pid)` | `int → int` | Session id. |
| `core::nice(inc)` | `int → int` | Adjust scheduling priority. |
| `core::getpriority(which, who)` | `int, int → int` | getpriority(2). |
| `core::setpriority(which, who, prio)` | `int, int, int → int` | setpriority(2). |
| `core::pause()` | `→ int` | pause(2) — sleep until signal. |
| `core::alarm(sec)` | `int → int` | alarm(2). |
| `core::umask(mask)` | `int → int` | umask(2). |
| `core::chroot(dir)` | `str → int` | chroot(2). |
| `core::chdir(dir)` | `str → int` | chdir(2). |
| `core::getcwd()` | `→ str` | getcwd(3). |
| `core::times()` | `→ array` | Returns [user, system, cuser, csystem] CPU times. |
| `core::getrlimit(resource)` | `int → array` | getrlimit; returns [soft, hard]. |
| `core::setrlimit(resource, soft, hard)` | `int, int, int → int` | setrlimit. |
| `core::getrusage(who)` | `int → array` | getrusage; returns assorted accounting fields. |

### User / group

| Function | Signature | Description |
|---|---|---|
| `core::getuid()` | `→ int` | Real user id. |
| `core::geteuid()` | `→ int` | Effective user id. |
| `core::setuid(uid)` | `int → int` | setuid(2). |
| `core::seteuid(uid)` | `int → int` | seteuid(2). |
| `core::setreuid(real, eff)` | `int, int → int` | setreuid(2). |
| `core::getgid()` | `→ int` | Real group id. |
| `core::getegid()` | `→ int` | Effective group id. |
| `core::setgid(gid)` | `int → int` | setgid(2). |
| `core::setegid(gid)` | `int → int` | setegid(2). |
| `core::setregid(real, eff)` | `int, int → int` | setregid(2). |
| `core::getgroups()` | `→ array` | Supplementary groups. |
| `core::getpwuid(uid)` | `int → array` | Password entry by uid. |
| `core::getpwnam(name)` | `str → array` | Password entry by name. |
| `core::getgrgid(gid)` | `int → array` | Group entry by gid. |
| `core::getgrnam(name)` | `str → array` | Group entry by name. |
| `core::getlogin()` | `→ str` | Login name. |

### File I/O

| Function | Signature | Description |
|---|---|---|
| `core::open(path, mode)` | `str, str → scalar` | Open file; mode ∈ {"r","w","a","rb","<",">",">>","+>"}. Returns filehandle. |
| `core::close(fh)` | `scalar → int` | Close filehandle. |
| `core::open_fd(fd, mode)` | `int, str → scalar` | fdopen(3); wrap an fd as a filehandle. |
| `core::open_str(ref)` | `scalar → scalar` | In-memory open over a scalar ref. |
| `core::fdopen_read(fd)` | `int → scalar` | Open fd for reading. |
| `core::fdopen_write(fd)` | `int → scalar` | Open fd for writing. |
| `core::close_fd(fd)` | `int → int` | close(2). |
| `core::readline(fh)` | `scalar → str` | Read one line (or undef at EOF). |
| `core::read_fd(fd, n)` | `int, int → str` | read(2) up to n bytes. |
| `core::read_all_fd(fd)` | `int → str` | Read until EOF. |
| `core::read_byte(fh)` | `scalar → int` | Read one byte (-1 on EOF). |
| `core::write_fd(fd, data)` | `int, str → int` | write(2). |
| `core::seek(fh, offset, whence)` | `scalar, int, int → int` | fseek; whence ∈ {SEEK_SET=0, SEEK_CUR=1, SEEK_END=2}. |
| `core::tell(fh)` | `scalar → int` | ftell(3). |
| `core::eof(fh)` | `scalar → int` | feof(3). |
| `core::flush(fh)` | `scalar → int` | fflush(3). |
| `core::autoflush(fh, on)` | `scalar, int → int` | setvbuf to `_IONBF` (unbuffered) when on, else `_IOFBF`. Matches perl `$|=1`. |
| `core::rewind(fh)` | `scalar → int` | rewind(3). |
| `core::clearerr(fh)` | `scalar → void` | clearerr(3). |
| `core::ferror(fh)` | `scalar → int` | ferror(3). |
| `core::fileno(fh)` | `scalar → int` | Underlying fd. |
| `core::fread(fh, len)` | `scalar, int → str` | Read len bytes from fh. |
| `core::fwrite(fh, data)` | `scalar, str → int` | Write data to fh. |
| `core::fgetc(fh)` | `scalar → int` | Read one char (-1 on EOF). |
| `core::fputc(c, fh)` | `int, scalar → int` | Write one char. |
| `core::fgets(fh, max)` | `scalar, int → str` | Read up to max bytes or newline. |
| `core::fputs(s, fh)` | `str, scalar → int` | Write string to fh. |
| `core::dup(fd)` | `int → int` | dup(2). |
| `core::pipe()` | `→ array` | Returns [read_fd, write_fd]. |
| `core::popen(cmd, mode)` | `str, str → scalar` | popen(3). |
| `core::pclose(fh)` | `scalar → int` | pclose(3). |
| `core::tmpfile()` | `→ scalar` | tmpfile(3) — auto-deleted filehandle. |
| `core::mkstemp(template)` | `str → array` | mkstemp(3); returns [fd, actual_path]. |
| `core::mkdtemp(template)` | `str → str` | mkdtemp(3); returns path. |
| `core::slurp(path)` | `str → str` | Read entire file into a string. |
| `core::slurp_fd(fd)` | `int → str` | Slurp from open fd. |
| `core::slurp_fh(fh)` | `scalar → str` | Slurp from open filehandle. |
| `core::spew(path, data)` | `str, str → int` | Write entire string to file (truncate). |
| `core::spew_fd(fd, data)` | `int, str → int` | Write to fd. |
| `core::spew_fh(fh, data)` | `scalar, str → int` | Write to filehandle. |
| `core::str_from_fh(fh)` | `scalar → str` | Slurp filehandle to string. |
| `core::qx(cmd)` | `str → str` | Capture command output (shell). |
| `core::flock(fh, op)` | `scalar, int → int` | flock(2); op ∈ {LOCK_SH=1, LOCK_EX=2, LOCK_UN=8, LOCK_NB=4}. |

### Filesystem

| Function | Signature | Description |
|---|---|---|
| `core::file_exists(path)` | `str → int` | Existence check (any type). |
| `core::is_file(path)` | `str → int` | Regular file? |
| `core::is_dir(path)` | `str → int` | Directory? |
| `core::is_symlink(path)` | `str → int` | Symbolic link? |
| `core::is_readable(path)` | `str → int` | Readable by current user? |
| `core::is_writable(path)` | `str → int` | Writable? |
| `core::is_executable(path)` | `str → int` | Executable? |
| `core::is_zero_size(path)` | `str → int` | Zero-byte file? |
| `core::dir_exists(path)` | `str → int` | Directory existence (alias for is_dir). |
| `core::file_size(path)` | `str → int` | Bytes; -1 on error. |
| `core::file_mtime(path)` | `str → int` | Modification time (epoch seconds). |
| `core::file_ext(path)` | `str → str` | Filename extension (without leading "."). |
| `core::basename(path)` | `str → str` | Basename (last path component). |
| `core::dirname(path)` | `str → str` | Directory part of path. |
| `core::path_join(@parts)` | `array → str` | Join with platform separator. |
| `core::realpath(path)` | `str → str` | Resolve symlinks, return absolute path. |
| `core::readlink(path)` | `str → str` | readlink(2). |
| `core::symlink(target, link)` | `str, str → int` | symlink(2). |
| `core::link(src, dst)` | `str, str → int` | link(2) (hard link). |
| `core::unlink(path)` | `str → int` | Remove a file. |
| `core::rename(old, new)` | `str, str → int` | rename(2). |
| `core::mkdir(path, mode)` | `str, int → int` | mkdir(2). |
| `core::rmdir(path)` | `str → int` | rmdir(2). |
| `core::chmod(mode, path)` | `int, str → int` | chmod(2). |
| `core::fchmod(fh, mode)` | `scalar, int → int` | fchmod(2). |
| `core::chown(uid, gid, path)` | `int, int, str → int` | chown(2). |
| `core::fchown(fh, uid, gid)` | `scalar, int, int → int` | fchown(2). |
| `core::lchown(uid, gid, path)` | `int, int, str → int` | lchown(2). |
| `core::access(path, mode)` | `str, int → int` | access(2). |
| `core::truncate(path, len)` | `str, int → int` | truncate(2). |
| `core::ftruncate(fh, len)` | `scalar, int → int` | ftruncate(2). |
| `core::stat(path)` | `str → array` | stat(2); returns 13-element list. |
| `core::lstat(path)` | `str → array` | lstat(2). |
| `core::statvfs(path)` | `str → array` | statvfs(3). |
| `core::fstatvfs(fh)` | `scalar → array` | fstatvfs(3). |
| `core::utime(atime, mtime, @paths)` | `int, int, array → int` | utime(2). |
| `core::utimes(atime, mtime, path)` | `int, int, str → int` | utimes(2). |
| `core::glob(pattern)` | `str → array` | Filename globbing. |
| `core::fnmatch(pattern, name)` | `str, str → int` | fnmatch(3). |
| `core::opendir(path)` | `str → scalar` | Open directory. |
| `core::readdir(dh)` | `scalar → array` | All entries. |
| `core::readdir_next(dh)` | `scalar → str` | One entry (undef when done). |
| `core::readdir_full(dh)` | `scalar → array` | Entries with type info. |
| `core::closedir(dh)` | `scalar → int` | Close directory. |

### Time / dates

| Function | Signature | Description |
|---|---|---|
| `core::time()` | `→ int` | Seconds since epoch. |
| `core::hires_time()` | `→ num` | Sub-second-precision seconds since epoch. |
| `core::gettimeofday()` | `→ array` | [sec, usec]. |
| `core::tv_interval(t1, t2)` | `array, array → num` | Difference in seconds. |
| `core::nanosleep(sec)` | `num → int` | Sleep with sub-second precision. |
| `core::sleep(sec)` | `int → int` | Sleep whole seconds. |
| `core::usleep(usec)` | `int → int` | Sleep microseconds. |
| `core::localtime(epoch)` | `int → array` | Local time as 9-tuple. |
| `core::gmtime(epoch)` | `int → array` | UTC time as 9-tuple. |
| `core::mktime(@parts)` | `array → int` | Inverse of localtime. |
| `core::strftime(fmt, @parts)` | `str, array → str` | Format time. |
| `core::ctime(epoch)` | `int → str` | "Day Mon DD HH:MM:SS YYYY". |
| `core::difftime(t2, t1)` | `int, int → num` | t2 - t1 in seconds. |
| `core::clock()` | `→ int` | CPU clock ticks. |
| `core::clock_gettime(clk)` | `int → array` | clock_gettime(2). |
| `core::clock_getres(clk)` | `int → array` | Resolution. |

### Network / sockets

| Function | Signature | Description |
|---|---|---|
| `core::socket_client(host, port)` | `str, int → scalar` | TCP connect. |
| `core::socket_server(host, port)` | `str, int → scalar` | TCP listen socket. |
| `core::socket_server_backlog(host, port, backlog)` | `str, int, int → scalar` | TCP listen with explicit backlog. |
| `core::socket_server_host(host, port, backlog)` | `str, int, int → scalar` | TCP listen bound to `host`. `""`/`"*"`/`"::"` → dual-stack; `"0.0.0.0"` → IPv4 wildcard; literal → its family. IPv6-aware. |
| `core::socket_accept(srv)` | `scalar → scalar` | Accept a connection (IPv6/IPv4). |
| `core::socket_send(sock, data)` | `scalar, str → int` | Send bytes. |
| `core::socket_recv(sock, n)` | `scalar, int → str` | Receive up to n bytes. |
| `core::socket_close(sock)` | `scalar → int` | Close. |
| `core::socket_flush(sock)` | `scalar → int` | Flush write buffer. |
| `core::socket_select(@socks)` | `array → array` | Indexes of ready sockets. |
| `core::socket_fd(sock)` | `scalar → int` | Underlying fd. |
| `core::socket_set_nonblocking(sock)` | `scalar → int` | Set O_NONBLOCK. |
| `core::shutdown(sock, how)` | `scalar, int → int` | shutdown(2); how ∈ {0,1,2}. |
| `core::getsockname(sock)` | `scalar → array` | Local [addr, port]. |
| `core::getpeername(sock)` | `scalar → array` | Remote [addr, port]. |
| `core::getsockopt(sock, level, opt)` | `scalar, int, int → int` | getsockopt(2). |
| `core::setsockopt(sock, level, opt, val)` | `scalar, int, int, int → int` | setsockopt(2). |
| `core::udp_socket()` | `→ scalar` | New UDP socket. |
| `core::udp_bind(sock, host, port)` | `scalar, str, int → int` | bind UDP socket. |
| `core::udp_server(host, port)` | `str, int → scalar` | UDP server (socket + bind). |
| `core::udp_sendto(sock, data, host, port)` | `scalar, str, str, int → int` | sendto. |
| `core::udp_recvfrom(sock, max)` | `scalar, int → array` | Returns [data, from_addr, from_port]. |
| `core::gethostname()` | `→ str` | gethostname(3). |
| `core::gethostbyname(host)` | `str → str` | First IP for hostname. |
| `core::gethostbyname_all(host)` | `str → array` | All IPs. |
| `core::getaddrinfo(host, port)` | `str, int → array` | getaddrinfo(3) results. |
| `core::inet_addr(ip)` | `str → int` | "1.2.3.4" → 32-bit int. |
| `core::inet_ntoa(addr)` | `int → str` | int → "1.2.3.4". |
| `core::inet_pton(af, str)` | `int, str → str` | Generic address presentation→binary. |
| `core::inet_ntop(af, bin)` | `int, str → str` | Binary→presentation. |
| `core::htonl(n)` | `int → int` | Host → network long. |
| `core::ntohl(n)` | `int → int` | Network → host long. |
| `core::htons(n)` | `int → int` | Host → network short. |
| `core::ntohs(n)` | `int → int` | Network → host short. |

### Process I/O

| Function | Signature | Description |
|---|---|---|
| `core::ioctl(fd, req, arg)` | `int, int, scalar → int` | ioctl(2). |
| `core::fcntl(fd, cmd, arg)` | `int, int, scalar → int` | fcntl(2). |
| `core::poll(@fds, timeout)` | `array, int → array` | poll(2). |
| `core::select_fds(rfds, wfds, efds, timeout)` | `array, array, array, num → int` | select(2). |
| `core::isatty(fd)` | `int → int` | isatty(3). |
| `core::ttyname(fd)` | `int → str` | ttyname(3). |
| `core::term_rows()` | `→ int` | Terminal height. |
| `core::term_cols()` | `→ int` | Terminal width. |
| `core::term_enable_raw()` | `→ int` | Put terminal into raw mode. |
| `core::term_disable_raw()` | `→ int` | Restore terminal mode. |
| `core::tcgetattr(fd)` | `int → array` | Read terminal attributes. |
| `core::tcsetattr(fd, attrs)` | `int, array → int` | Write terminal attributes. |
| `core::tcdrain(fd)` | `int → int` | tcdrain(2). |
| `core::tcflush(fd, q)` | `int, int → int` | tcflush(2). |
| `core::cfgetispeed(attrs)` | `array → int` | Input speed. |
| `core::cfgetospeed(attrs)` | `array → int` | Output speed. |
| `core::cfsetispeed(attrs, speed)` | `array, int → int` | Set input speed. |
| `core::cfsetospeed(attrs, speed)` | `array, int → int` | Set output speed. |
| `core::serial_open(path)` | `str → scalar` | Open serial port. |

### Signals

| Function | Signature | Description |
|---|---|---|
| `core::signal(name, handler)` | `str, scalar → int` | Install signal handler. handler can be a code ref or "IGNORE"/"DEFAULT". |
| `core::raise(sig)` | `int → int` | raise(3). |
| `core::sigprocmask(how, set)` | `int, str → str` | sigprocmask(2). |

### Environment

| Function | Signature | Description |
|---|---|---|
| `core::getenv(name)` | `str → str` | Get env variable (or undef). |
| `core::setenv(name, val)` | `str, str → int` | setenv(3). |
| `core::unsetenv(name)` | `str → int` | unsetenv(3). |
| `core::argv()` | `→ array` | Command-line args (without program name). |
| `core::getprocname()` | `→ str` | Current process name (argv[0]). |
| `core::setprocname(name)` | `str → int` | Set process name. |
| `core::getproctitle()` | `→ str` | Full process title (incl. argv). |
| `core::setproctitle(title)` | `str → int` | Override process title. |

### Memory / DL

| Function | Signature | Description |
|---|---|---|
| `core::mmap(addr, len, prot, flags, fd, off)` | many → scalar | mmap(2). |
| `core::munmap(addr, len)` | `scalar, int → int` | munmap(2). |
| `core::mlock(addr, len)` | `scalar, int → int` | mlock(2). |
| `core::munlock(addr, len)` | `scalar, int → int` | munlock(2). |
| `core::calloc(n, sz)` | `int, int → scalar` | calloc(3). |
| `core::realloc(ptr, sz)` | `scalar, int → scalar` | realloc(3). |
| `core::free(ptr)` | `scalar → void` | free(3). |
| `core::dl_open(path)` | `str → scalar` | dlopen(3). |
| `core::dl_close(handle)` | `scalar → int` | dlclose. |
| `core::dl_sym(handle, name)` | `scalar, str → scalar` | dlsym. |
| `core::dl_error()` | `→ str` | dlerror. |
| `core::dl_call_void(sym, @args)` | `scalar, ... → void` | Call a `void(int...)` function. |
| `core::dl_call_int(sym, @args)` | `scalar, ... → int` | Call a `int(int...)` function. |
| `core::dl_call_num(sym, @args)` | `scalar, ... → num` | Call a `double(int...)` function. |
| `core::dl_call_str(sym, @args)` | `scalar, ... → str` | Returns char*. |
| `core::dl_call_void_sv(sym, @args)` | `scalar, ... → void` | Pass StradaValue* args. |
| `core::dl_call_int_sv(sym, @args)` | `scalar, ... → int` | StradaValue args, int return. |
| `core::dl_call_str_sv(sym, @args)` | `scalar, ... → str` | StradaValue args, str return. |
| `core::dl_call_sv(sym, @args)` | `scalar, ... → scalar` | StradaValue args + return. |
| `core::dl_call_export_info(sym)` | `scalar → array` | Read __strada_export_info from a loaded shared lib. |
| `core::dl_call_version(sym)` | `scalar → str` | Module version string. |

### Strings / encoding

| Function | Signature | Description |
|---|---|---|
| `core::pack(fmt, @vals)` | `str, ... → str` | Binary pack (see format codes below). |
| `core::unpack(fmt, data)` | `str, str → array` | Binary unpack (same format codes). |
| `core::vec_get(s, off, bits)` | `str, int, int → int` | Perl `vec()` rvalue: read `bits`-wide field at `off`. |
| `core::vec_set(s, off, bits, val)` | `str, int, int, int → str` | Perl `vec()` lvalue: set field, extending `s` as needed. |
| `core::ord_byte(s)` | `str → int` | First byte ordinal. |
| `core::byte_length(s)` | `str → int` | Length in bytes. |
| `core::byte_substr(s, off, len)` | `str, int, int → str` | Byte-offset substring. |
| `core::get_byte(s, pos)` | `str, int → int` | Byte at position. |
| `core::set_byte(s, pos, val)` | `str, int, int → str` | Return s with byte at pos replaced. |
| `core::read_byte(fh)` | `scalar → int` | Read one byte. |
| `core::hex(s)` | `str → int` | Parse hex string. |
| `core::random_bytes(n)` | `int → str` | Cryptographic random bytes. |
| `core::random_bytes_hex(n)` | `int → str` | Random bytes as hex. |
| `core::strerror(errno)` | `int → str` | Errno → message. |
| `core::strtod(s)` | `str → num` | strtod(3). |
| `core::strtol(s, base)` | `str, int → int` | strtol(3). |
| `core::atof(s)` | `str → num` | atof(3). |
| `core::atoi(s)` | `str → int` | atoi(3). |
| `core::quotemeta(s)` | `str → str` | Escape regex metacharacters. |

#### `pack` / `unpack` format codes

A count suffix (`N`) or `*` (consume all remaining) follows each code. Endianness modifiers `<` (little) and `>` (big) apply to the native-width integer codes.

| Code | Meaning |
|---|---|
| `c` / `C` | Signed / unsigned 8-bit char. |
| `s` / `S` | Signed / unsigned 16-bit, native endian. |
| `l` / `L` | Signed / unsigned 32-bit, native endian. |
| `i` / `I` | Signed / unsigned `int`, native endian. |
| `q` / `Q` | Signed / unsigned 64-bit, native endian. |
| `n` / `N` | Unsigned 16- / 32-bit, **big-endian** (network order). |
| `v` / `V` | Unsigned 16- / 32-bit, **little-endian**. |
| `f` / `d` | Single- / double-precision float, native. |
| `a` / `A` / `Z` | String: NUL-padded / space-padded / NUL-terminated. |
| `H` / `h` | Hex string, high / low nibble first. |
| `B` / `b` | Bit string, high / low bit first. |
| `x` / `X` | Insert a NUL byte / back up one byte. |
| `w` | BER-compressed integer (7-bit groups, MSB continuation). |
| `u` | uuencoded string (45-byte lines). |
| `U` | Unicode codepoint → UTF-8 bytes (sets the UTF-8 flag). |

### Errors / introspection

| Function | Signature | Description |
|---|---|---|
| `core::errno()` | `→ int` | Current errno. |
| `core::caller(level)` | `int → array` | Returns hash with function/file/line. level=0 is immediate caller. |
| `core::stack_trace()` | `→ str` | Format current call stack. |
| `core::wantarray()` | `→ int` | 1 in list context, 0 in scalar, undef in void. |
| `core::wanthash()` | `→ int` | 1 if caller expects a hash. |
| `core::wantscalar()` | `→ int` | 1 in scalar context. |
| `core::set_recursion_limit(n)` | `int → int` | Set max sub recursion depth. |
| `core::get_recursion_limit()` | `→ int` | Current limit. |
| `core::isweak(ref)` | `scalar → int` | 1 if a weak reference. |
| `core::weaken(ref)` | `scalar → void` | Make a weak reference. |
| `core::release(ref)` | `scalar → void` | Force-free a reference. |

### Globals / package storage

| Function | Signature | Description |
|---|---|---|
| `core::global_get(name)` | `str → scalar` | Read package-scoped global. |
| `core::global_set(name, val)` | `str, scalar → void` | Write global. |
| `core::global_exists(name)` | `str → int` | Existence check. |
| `core::global_delete(name)` | `str → scalar` | Remove and return. |
| `core::global_keys()` | `→ array` | All package-global keys. |

### Profiling

| Function | Signature | Description |
|---|---|---|
| `core::full_profile_start(path)` | `str → int` | Begin line-level profiling, write to path. |
| `core::full_profile_stop()` | `→ int` | Stop profiling. |
| `core::memprof_enable()` | `→ int` | Enable allocation tracking. |
| `core::memprof_disable()` | `→ int` | Disable. |
| `core::memprof_report()` | `→ array` | Report counts/sizes by type. |
| `core::memprof_reset()` | `→ int` | Zero counters. |

### Misc utilities

| Function | Signature | Description |
|---|---|---|
| `core::rand()` | `→ num` | Random in [0, 1). |
| `core::srand(seed)` | `int → int` | Seed RNG. |
| `core::random()` | `→ int` | random(3). |
| `core::srandom(seed)` | `int → void` | Seed random(3). |
| `core::idiv(a, b)` | `int, int → int` | Truncated integer division. |
| `core::times()` | `→ array` | CPU times. |
| `core::stack_trace()` | `→ str` | Stack trace (as string). |
| `core::hash_default_capacity(n)` | `int → int` | Set hash preallocation default. |
| `core::array_default_capacity(n)` | `int → int` | Set array preallocation default. |

### Tuning capacity hints

| Function | Signature | Description |
|---|---|---|
| `core::hash_default_capacity(n)` | `int → int` | Default capacity for new hashes. |
| `core::array_default_capacity(n)` | `int → int` | Default capacity for new arrays. |

### C struct interop

| Function | Signature | Description |
|---|---|---|
| `core::cstruct_new(layout)` | `str → scalar` | Allocate a struct described by layout. |
| `core::cstruct_get_int(cs, field)` | `scalar, str → int` | Read int field. |
| `core::cstruct_get_double(cs, field)` | `scalar, str → num` | Read double field. |
| `core::cstruct_get_string(cs, field)` | `scalar, str → str` | Read string field. |
| `core::cstruct_set_int(cs, field, v)` | `scalar, str, int → void` | Write int field. |
| `core::cstruct_set_double(cs, field, v)` | `scalar, str, num → void` | Write double field. |
| `core::cstruct_set_string(cs, field, v)` | `scalar, str, str → void` | Write string field. |
| `core::cstruct_ptr(cs)` | `scalar → scalar` | Raw pointer to the struct. |
| `core::int_ptr(v)` | `int → scalar` | Pointer to an int holding v. |
| `core::num_ptr(v)` | `num → scalar` | Pointer to a double holding v. |
| `core::str_ptr(v)` | `str → scalar` | Pointer to a C string. |
| `core::ptr_deref_int(ptr)` | `scalar → int` | Read int through pointer. |
| `core::ptr_deref_num(ptr)` | `scalar → num` | Read num through pointer. |
| `core::ptr_deref_str(ptr)` | `scalar → str` | Read C string through pointer. |
| `core::ptr_set_int(ptr, v)` | `scalar, int → void` | Write int through pointer. |
| `core::ptr_set_num(ptr, v)` | `scalar, num → void` | Write num through pointer. |

---

## utf8:: — Unicode / UTF-8

| Function | Type | Description |
|----------|------|-------------|
| `utf8::is_utf8(s)` | `str → int` | Returns 1 if `s` is well-formed UTF-8. |
| `utf8::valid(s)` | `str → int` | Alias for `utf8::is_utf8`. |
| `utf8::encode(s)` | `str → str` | No-op (strada strings are already bytes). |
| `utf8::decode(s)` | `str → int` | Validates UTF-8; 1 if valid, 0 if not. |
| `utf8::upgrade(s)` | `str → str` | No-op; returns `s`. |
| `utf8::downgrade(s)` | `str → str` | Returns `s` if ASCII-only, dies otherwise. |
| `utf8::downgrade(s, 1)` | `str, int → scalar` | Returns `s` if ASCII, undef if not (fail_ok). |
| `utf8::unicode_to_native(cp)` | `int → int` | Identity on Unix. |
| `utf8::nfc(s)` | `str → str` | UAX#15 canonical composition (NFC). |
| `utf8::nfd(s)` | `str → str` | UAX#15 canonical decomposition (NFD). |
| `utf8::nfkc(s)` | `str → str` | UAX#15 compatibility composition (NFKC). |
| `utf8::nfkd(s)` | `str → str` | UAX#15 compatibility decomposition (NFKD). |
| `utf8::normalize(form, s)` | `str, str → str` | Generic entry; `form` = `"NFC"`/`"NFD"`/`"NFKC"`/`"NFKD"`. |

Normalization runs the full UAX#15 pipeline (recursive decomposition → canonical reorder by combining class → optional canonical composition) backed by Unicode 15.0.0 tables. Hangul is handled algorithmically.

---

## Bare built-ins (no namespace)

These match Perl built-ins one-to-one in most cases.

### Strings

Strada strings are **UTF-8 character oriented** — `length`, `substr`,
`index`, `rindex`, and `reverse` all count Unicode codepoints, not
bytes. Use `core::byte_*` (next section) when you need byte access
for binary data.

| Function | Description |
|---|---|
| `length(s)` | UTF-8 codepoint count. |
| `lc(s)`, `lower(s)` | Lowercase (handles Latin-1 range). |
| `uc(s)`, `upper(s)` | Uppercase (handles Latin-1 range). |
| `lcfirst(s)` | Lowercase first character. |
| `ucfirst(s)` | Uppercase first character. |
| `chomp(var)` | Remove trailing newline; mutates in place. |
| `chop(var)` | Remove last character; mutates in place. |
| `chr(n)` | Codepoint → string. 0-255 = single byte, ≥256 = UTF-8 multi-byte. |
| `ord(s)` | First-character codepoint. |
| `reverse(s)` or `reverse(@arr)` | Reverse string (codepoint-aware) in scalar context, array in list context. |
| `substr(s, off [, len [, repl]])` | Substring extract (3-arg, codepoint offsets) or replace (4-arg lvalue). |
| `index(s, needle [, off])` | First codepoint position of needle. |
| `rindex(s, needle [, off])` | Last codepoint position. |
| `sprintf(fmt, ...)` | Format string (printf-style). |
| `printf(fh, fmt, ...)` | Print formatted to filehandle. |
| `join(sep, @list)` | Join with separator. |
| `split(pat, s [, limit])` | Split on pattern. |
| `trim(s)`, `ltrim(s)`, `rtrim(s)` | Strip whitespace. |
| `quotemeta(s)` | Escape regex metacharacters. |
| `match(s, pat)` | Regex match returning captures. |
| `replace(s, pat, repl)` | Single replacement. |
| `replace_all(s, pat, repl)` | All replacements. |
| `captures()` | Last regex capture group list. |
| `named_captures()` | Last regex named captures as hash. |
| `pos(var)` | Current /g position. |
| `pack(fmt, ...)` | Binary pack. |
| `unpack(fmt, data)` | Binary unpack. |

### Numbers

| Function | Description |
|---|---|
| `abs(n)` | Absolute value. |
| `int(n)` | Truncate to integer. |
| `chr(n)` | Codepoint → char. |
| `ord(s)` | Char → codepoint. |
| `hex(s)` | Parse hex string. |
| `rand([n])` | Random float in [0, n) (default n=1). |
| `srand([seed])` | Seed RNG. |

### Arrays

| Function | Description |
|---|---|
| `push(@arr, ...)` | Append. |
| `pop(@arr)` | Remove and return last element. |
| `shift(@arr)` | Remove and return first element. |
| `unshift(@arr, ...)` | Prepend. |
| `splice(@arr, off [, len [, @repl]])` | In-place edit; returns removed. |
| `each(@arr)` | Iterator: [index, value] tuples. |
| `sort([{block,}] @arr)` | Sort. |
| `nsort(@arr)` | Numeric sort. |
| `reverse(@arr)` | Reverse list. |
| `scalar(@arr)` | Array count. |
| `map { ... } @arr` | Transform. |
| `grep { ... } @arr` | Filter. |
| `array_new()` | New empty array. |
| `clone(ref)` | Deep clone. |
| `reserve(@arr, n)` | Preallocate capacity. |
| `deref(ref)` | Dereference. |
| `derefto(ref, type)` | Type-checked deref. |
| `deref_array(ref)`, `deref_hash(ref)` | Typed deref helpers. |
| `deref_set(ref, val)` | Set through a scalar ref. |

### Hashes

| Function | Description |
|---|---|
| `keys(%h)` | List of keys. |
| `values(%h)` | List of values. |
| `each(%h)` | Iterator: [key, value] tuples. |
| `exists($h{k})` | Key existence. |
| `delete($h{k})` | Remove key; returns its value. |
| `hash_new()` | New empty hash. |
| `hash_get(%h, k)`, `hash_set(%h, k, v)` | Direct accessors. |
| `tie(var, "Class", ...)` | Tie variable to class. |
| `untie(var)` | Remove tie. |
| `tied(var)` | Currently tied object (or undef). |

### References / OOP

| Function | Description |
|---|---|
| `ref(v)` | Reference type ("ARRAY", "HASH", "CODE", or pkg name). |
| `reftype(v)` | Like ref, ignoring bless. |
| `is_ref(v)`, `is_refto(v, type)` | Bool ref checks. |
| `bless($ref, "Class")` | Bless ref into class. |
| `blessed(v)` | Package name or undef. |
| `isa($obj, "Class")` | Inheritance check. |
| `can($obj, "method")` | Method existence check. |
| `UNIVERSAL::isa($obj, "C")`, `UNIVERSAL::can(...)` | Function-form. |
| `inherit("Parent")` | Add to @ISA. |
| `package` | Set current package (statement, not function). |

### Misc

| Function | Description |
|---|---|
| `defined(v)` | 1 if not undef. |
| `print([fh,] ...)` | Print without trailing newline. |
| `say([fh,] ...)` | Print with newline. |
| `printf(...)` | Format and print. |
| `warn(...)` | Print to STDERR. |
| `die(...)` | Throw exception. |
| `eval { ... }` | Exception-catching block. |
| `caller([level])` | Caller info. |
| `time()`, `gmtime()`, `localtime()` | Time helpers. |
| `wait()`, `waitpid()` | Process waiting. |
| `fork()`, `exec()`, `exit()`, `system()` | Process control. |
| `getpid()` | Current PID. |
| `__PACKAGE__`, `__FILE__`, `__LINE__` | Compile-time constants. |
| `__program_name` | Program name (= $0 in Perl). |
| `dumper($v)` | Pretty-print value (returns string). |
| `dumper_str($v)` | Variant: shorter format. |
| `clone($v)` | Deep clone (Storable::dclone equivalent). |
| `goto LABEL` | Jump within sub. |
| `last`, `next`, `redo` | Loop control. |
| `return` | Return from sub. |
| `cast_int(v)`, `cast_num(v)`, `cast_str(v)` | Explicit type cast. |

### Type-decl keywords (not functions)

`int`, `num`, `str`, `scalar`, `array`, `hash`, `void`, `undef`, `dynamic`,
`int8`, `int16`, `uint8`, `byte`, `uint16`, `uint32`, `uint64`, `size_t`,
`char`, `float`, `double`, `long`, `short`, `ssize_t`, `time_t`, `pid_t`, etc.

### Statement keywords (not functions)

`if`, `unless`, `elsif`, `else`, `while`, `until`, `for`, `foreach`, `do`,
`my`, `our`, `local`, `state`, `package`, `use`, `require`, `import_lib`,
`import_object`, `import_archive`, `version`, `func`, `fn`, `sub`,
`private`, `extern`, `inline`, `auto`, `async`, `await`, `try`, `catch`,
`finally`, `throw`, `redo`, `last`, `next`, `goto`, `return`, `break`,
`continue`, `case`, `default`, `switch`, `enum`, `const`, `BEGIN`, `END`,
`__C__`, `__STRADA__`.

---

## See also

- [LANGUAGE_GUIDE.md](LANGUAGE_GUIDE.md) — narrative walk-through of the language.
- [QUICK_REFERENCE.md](QUICK_REFERENCE.md) — concise cheat-sheet by topic.
- [RUNTIME_API.md](RUNTIME_API.md) — C-level API the generated code calls.
- [OOP_GUIDE.md](OOP_GUIDE.md) — classes, roles, modifiers, overloading.
- [FFI_GUIDE.md](FFI_GUIDE.md) — `__C__` blocks and C interop.
