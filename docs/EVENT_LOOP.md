# Async::Loop — epoll Event Loop and Green Tasks

Status: **experimental** (branch feature). Works on any POSIX system: the
readiness backend is **epoll** on Linux (detected by `./configure`;
`--without-epoll` forces the fallback) and **poll(2)** everywhere else,
behind one internal API (`evb_*` in the runtime — a kqueue backend can be
added there later). The full test suite passes on both backends.

## Why

The `async::` thread pool is fine for CPU parallelism, but a server paying one
OS thread per blocked connection cannot reach C10K. The event loop multiplexes
any number of waiting connections onto one thread; green tasks make handler
code read like plain blocking Strada.

## Layers

1. **Runtime primitives** (`core::`): `epoll_create/add/mod/del/wait`,
   `eventfd/eventfd_signal/eventfd_drain`, `socket_try_recv/try_send/try_accept`
   (non-blocking with explicit would-block results), `mono_ms`. All uniform
   `StradaValue*` functions — they work in the VM through the generic bridge.
2. **Reactor** (`lib/Async/Loop.strada`): watchers, one-shot timers, `run()`.
3. **IO-wait futures**: `async::io_wait($fd_or_sock, "r"|"w", $timeout_ms)`
   returns a future completed by a dedicated poller thread ("r"/"w" mask,
   `"timeout"`, or `"error"`). Pool workers never block on socket readiness.
4. **Green tasks** (`$loop->spawn` + `lib/Async/Task.strada`): stackful
   coroutines (ucontext). `Async::Task::recv/send/accept/sleep` look blocking
   but park the task and let the loop run others. Outside a task each falls
   back to plain blocking behavior, so the same code runs in both worlds.

## Example

```strada
use lib "lib";
use Async::Loop;
use Async::Task;

my scalar $loop = Async::Loop::new();
my scalar $listener = core::socket_server(8080);

$loop->spawn(fn () {
    while (1) {
        my scalar $conn = Async::Task::accept($listener);
        $loop->spawn(fn () {
            my str $req = Async::Task::recv($conn, 8192);
            Async::Task::send($conn, "HTTP/1.0 200 OK\r\n\r\nhello");
            core::socket_close($conn);
        });
    }
});

$loop->timer_after(250, fn () { say("tick"); });
$loop->run();   # until stop() or nothing left to wait for
```

## Callback API

- `$loop->watch($sock_or_fd, "r"|"w"|"rw", fn (int $fd, str $mask) {...})` → subscription id.
  Multiple subscriptions may share one fd (several accept-tasks on one
  listener work); the epoll registration carries the union mask.
- `$loop->unwatch_sub($sock_or_fd, $id)` — remove one subscription.
- `$loop->unwatch($sock_or_fd)` — remove all subscriptions on the fd.
- `$loop->timer_after($ms, $cb)` → id; `$loop->timer_cancel($id)`
- `$loop->stop()`; `$loop->run()` returns when stopped or drained.
- `$loop->{"on_task_error"} = fn (scalar $err) {...}` — uncaught task
  exceptions are contained at the task boundary and reported here
  (default: `warn`). One dead task never kills the loop or its siblings.

## Task API (`Async::Task`)

- `recv($sock, $max [, $timeout_ms])` → data; `""` = EOF; undef = timeout
- `readline($sock [, $timeout_ms])` → line (newline stripped); undef = EOF/timeout
- `send($sock, $data)` → bytes sent (handles partial writes); -1 = error
- `accept($listener [, $timeout_ms])` → socket; undef = timeout
- `connect($host, $port [, $timeout_ms])` → socket; undef = failure/timeout.
  Non-blocking TCP handshake; hostnames resolve on a background thread
  (`async::resolve`) with the task parking in 1ms slices, so slow DNS no
  longer stalls the loop (numeric addresses skip resolution entirely).
- `sleep($ms)`

Sockets accepted/connected through the task API get `TCP_NODELAY` (an
event loop ping-ponging small messages is Nagle's worst case).

## Green-task rules

- **try/catch works inside tasks, including across suspensions** — the
  runtime swaps the per-context try/cleanup/call-trace stacks at every
  switch (the jmp_bufs target frames on the persistent coro stack, so they
  stay valid while parked).
- **Capture regex results before suspending** — `$1`/`captures()` are
  per-OS-thread and another task may match in between.
- **One loop, one OS thread.** Tasks belong to the thread that created
  them, and calling `watch`/`timer_after`/`spawn` from another thread is
  unsupported (no locking; no wakeup of a loop parked in `epoll_wait`).
- **Transitive closure capture works**: a handler closure nested inside
  another closure captures outer variables through the enclosing closure's
  capture slots (fixed in the compiler on this branch; see
  examples/test_nested_closures.strada).

## Performance

`examples/bench_event_loop.strada` (one thread, loopback, -O2): ~34k echo
round-trips/s across 50 concurrent task connections, vs ~125k/s for a
single sequential blocking connection and ~81k raw park/wake cycles/s.
Each round-trip costs two parks (client recv + handler recv), so the loop
runs near its switching ceiling; the win is the 50-way concurrency on one
thread, not single-stream latency.

## TLS (`Async::TaskSSL`)

TLS handshake/read/write park the task on exactly the readiness OpenSSL
reports (WANT_READ/WANT_WRITE):

```strada
use Async::TaskSSL;
my scalar $h = Async::TaskSSL::connect("example.com", 443);   # verifies cert
Async::TaskSSL::write($h, "GET / HTTP/1.0\r\n\r\n");
my str $body = Async::TaskSSL::read($h, 65536, 10000);
Async::TaskSSL::close($h);

# Server side: ssl::server($port, $cert, $key) then, inside a task,
# Async::TaskSSL::accept($server) — accept + handshake without blocking.
```

`connect_insecure` skips certificate verification (test certs). Handles are
hashrefs holding the SSL session and the underlying TCP socket. The new
non-blocking primitives live in `lib/ssl.strada` (`attach_fd`,
`attach_server_fd`, `handshake_step`, `try_read`, `try_write`,
`try_accept_fd`, `want`), and `lib/ssl.strada` now declares
`link_lib "ssl"/"crypto"` so programs link OpenSSL automatically.

## Known issues (v1)

- The VM/interpreter gets the fd/socket primitives via the generic bridge,
  but coroutines (and therefore green tasks) are compiled-only.

(The "~32 bytes per task suspension" leak originally reported here turned out
to be a pre-existing runtime bug — `strada_socket_close` discarded the owned
undef returned by `strada_socket_flush`, one StradaValue per connection close,
misattributed by valgrind to watcher entries because the SV allocator recycles
freed values. Fixed on this branch; the loop, tasks, and a 100-connection echo
stress are valgrind-clean. The `STRADA_RC_TRACE` hook added during the hunt
remains available for future refcount debugging.)

## Design notes

- Scheduling policy lives entirely in `lib/Async/Loop.strada`; the runtime
  only provides epoll wrappers and ucontext switching
  (`strada_coro_create/resume/yield_io/...`).
- `epoll_wait` runs inside `cc_blocking_enter/leave`, so a blocked loop never
  stalls the cycle collector's stop-the-world.
- The poller thread (`async::io_wait`) is lazily started, registers itself as
  a GC mutator, and wakes via an eventfd.
- A kqueue backend for macOS/BSD can slot in behind the same `core::epoll_*`
  API without touching the library layer.
