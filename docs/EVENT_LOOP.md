# Async::Loop — epoll Event Loop and Green Tasks

Status: **experimental** (branch feature). Linux-only — requires epoll, detected
by `./configure` ("Checking for epoll (event loop)"); `--without-epoll` disables
it. On platforms without epoll the runtime primitives compile as stubs and
`Async::Loop::new()` throws `"Async::Loop requires epoll (Linux)"`.

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

- `$loop->watch($sock_or_fd, "r"|"w"|"rw", fn (int $fd, str $mask) {...})`
- `$loop->unwatch($sock_or_fd)`
- `$loop->timer_after($ms, $cb)` → id; `$loop->timer_cancel($id)`
- `$loop->stop()`; `$loop->run()` returns when stopped or drained.

## Green-task rules (v1)

- **No suspending inside `try{}`** — `Async::Task::*` throws
  `"cannot suspend inside try{}"` (runtime-enforced). The per-thread exception
  stacks cannot yet be swapped safely across context switches.
- **Capture regex results before suspending** — `$1`/`captures()` are
  per-OS-thread and another task may match in between.
- Tasks belong to the OS thread that created them.
- The runtime swaps the pending-cleanup and call-trace stacks at every
  switch, so temporaries and `core::stack_trace()` behave per-task.

## Known issues (v1)

- **Small per-suspension leak (~32 bytes/park).** Each task suspension
  registers a watcher entry whose final reference is not released — an
  interaction between the closure-call cleanup protocol and coroutine context
  switches that is still being chased (the identical callback-only code path
  leaks nothing; see the `STRADA_RC_TRACE` hook in `strada_runtime.h`, built
  with `-DSTRADA_RC_TRACE`, which exists precisely to finish this hunt). A
  server doing 1M request-parks leaks ~64 MB, so fix before production use;
  callback-style (`watch`/`timer_after`) has no such leak.
- SSL sockets are not yet loop-aware (blocking handshake/IO).
- The VM/interpreter gets the epoll primitives via the generic bridge, but
  coroutines (and therefore green tasks) are compiled-only.

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
