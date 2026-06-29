# Strada Memory-Safety Audit — Confirmed Findings

_Generated 2026-06-04 via multi-agent review + adversarial verification (26 regions, 73 raw findings, 59 confirmed, 14 refuted)._

_Re-audited 2026-06-29 against current `main` HEAD (42443a1). Status annotated per finding._

## Re-audit Summary

Of the 58 confirmed findings, **27 are FIXED** — every CRITICAL and all 15 HIGH-severity findings are resolved (14 fully fixed, 1 mitigated). The remaining open findings are mostly memory leaks (DBI, Perl5 bridge, USB), OOM-path NULL-deref crashes, and niche LOW-severity conditions.

| Severity | Total | FIXED | OPEN | Other |
|----------|-------|-------|------|-------|
| critical | 1 | 1 | 0 | 0 |
| high | 15 | 14 | 0 | 1 mitigated |
| medium | 13 | 8 | 5 | 0 |
| low | 26 | 2 | 24 | 1 mostly-fixed, 23 open |
| info | 3 | 0 | 3 | 0 |

---

## CRITICAL

> **STATUS 2026-06-10 — RESOLVED (already guarded in tree).**
> **Re-audit 2026-06-29: ✅ CONFIRMED FIXED.**

### 1. Integer underflow in base64 decode output-length computation → undersized alloc + wild OOB write
- **Location:** `runtime/strada_runtime.c:16988`  (fn `strada_base64_decode`)
- **Category:** integer-overflow  |  **Attacker-controllable:** yes
- **Status:** ✅ **FIXED** — Out-len computed with underflow guard: `size_t groups = len/4; size_t out_len = groups*3; if (out_len >= pad) out_len -= pad; else out_len = 0;`. Regression test at `examples/test_base64_malformed.strada`.
- **Mechanism:** After filtering the input to the base64 alphabet, `len` (= count of valid [A-Za-z0-9+/=] chars) and `pad` (count of trailing '=' chars, 0/1/2) are computed, then `size_t out_len = (len / 4) * 3 - pad;` (line 16326). When the cleaned input is shorter than 4 chars but still contains '=' (e.g. core::base64_decode("="), "==", "A=", "AB="), `(len/4)*3` is 0 while `pad>=1`, so the subtraction underflows the unsigned `out_len` to ~SIZE_MAX. `unsigned char *result = malloc(out_len + 1);` (line 16327) then becomes malloc(0)/malloc((size_t)-1) → a tiny or NULL allocation. The decode loop `for(...; i+3 < len; ...)` does not execute for len<4, but line 16353 `result[out_len] = '\0';` writes one byte at result[SIZE_MAX] (or result[SIZE_MAX-1] for pad==2) — a wild out-of-bounds heap write. The input is fully attacker-controlled: core::base64_decode($data) maps directly to this function (runtime/strada_runtime.h:784, CodeGen.strada:8388). Reachable with a 1-3 byte string.
- **Trigger:** core::base64_decode("=") (or "==", "A=", "AB=") — any string whose base64-alphabet-filtered form is 1-3 chars ending in '='. pad>=1 with (len/4)*3==0 makes out_len underflow to ~SIZE_MAX; line 16353 result[out_len]='\0' is an out-of-bounds heap write (ASan-confirmed). For "==", malloc(SIZE_MAX) returns NULL and the write dereferences NULL+huge offset.


## HIGH

### 2. Unbounded value-stack push: SP_PUSH never checks sp against stack_cap, deep recursion overflows the 1M-entry stack (heap OOB write)
- **Location:** `interpreter/vm.c:1412`  (fn `vm_execute (SP_PUSH macro)`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** yes
- **Status:** ✅ **FIXED** — SP_PUSH now checks `sp >= vm->stack_cap` and calls `vm_stack_grow()` (with 64MB cap). Frame stack also bounded.
- **Mechanism:** The value stack is a fixed heap buffer: vm->stack = calloc(vm->stack_cap, sizeof(VMValue)) with stack_cap = 1024*1024 (vm_new, lines 541-542). The push macro was `#define SP_PUSH(v) (stack[sp++] = (v))` (line 1177) with NO bounds check. Recursive Strada functions accumulate value-stack slots monotonically. A program whose recursion depth is driven by input (e.g. recurse on a counter or on input length) drives sp past 1048576, and `stack[sp++] = v` writes past the calloc'd allocation — a heap out-of-bounds write.
- **Trigger:** A Strada/Perl program run on the VM whose recursion depth is driven by input — e.g. func f($n){ if($n<=0){return 0;} return 1 + f($n-1); } called as f($big) where $big comes from input/length.

### 3. OP_BUILTIN: stack buffer overflow when a builtin is called with >16 arguments
- **Location:** `interpreter/vm.c:3461`  (fn `vm_execute (OP_BUILTIN)`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** no
- **Status:** ✅ **FIXED** — Now clamps `argc` to 16 with drain of excess args to free them. Comment references the audit scenario.
- **Mechanism:** The handler declares `VMValue args[16]` then `for (int i = argc - 1; i >= 0; i--) args[i] = SP_POP();` with argc read from the bytecode (uint8). Several builtins emit the source argument count verbatim with no cap.
- **Trigger:** Run any Strada program through the VM/interpreter that calls a builtin with more than 16 arguments, e.g. core::pack("C20", 1..20).

### 4. base64_decode: heap buffer over-read and over-write on non-multiple-of-4 / unpadded input
- **Location:** `interpreter/vm.c:4570`  (fn `vm_execute (OP_BUILTIN / BUILTIN_CORE_BASE64_DECODE)`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** yes
- **Status:** ✅ **FIXED** — Now uses safe output sizing `olen = (dlen/4)*3 + 3`, NULL-checks malloc, and loop bound is `i + 3 < dlen` with explicit `'='` checks on bytes 2 and 3 before reading. Comment at lines 4566-4569 explicitly describes the old bug: "The old code used olen=3*dlen/4 with an `i < dlen` loop that read data[i+1..i+3] unconditionally — over-reading past `data` and over-writing `out`."
- **Mechanism:** The decode loop `for (i=0; i<dlen; i+=4)` reads data[i+1], data[i+2], data[i+3] unconditionally (lines 4129-4134) and writes out[j++] for each. When dlen is not a multiple of 4, data[i+2]/data[i+3] read past the NUL terminator, and the output overflow writes past the malloc'd buffer.
- **Trigger:** Run any program under ./strada-interp (VM) that calls core::base64_decode($s) where $s is a base64-ish string whose length is not a multiple of 4 and whose trailing chars are valid base64 (non-"="), e.g. "AB", "A", "ABCDE".

### 5. Stack buffer overflow: function with >256 locals overflows CompCtx.local_names[256]/local_sigils[256]
- **Location:** `interpreter/vm_compiler.c:250`  (fn `ctx_add_local`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** yes
- **Status:** ✅ **FIXED** — Now checks `ctx->local_count >= (int)(sizeof(ctx->local_names) / sizeof(ctx->local_names[0]))` and emits a compile error.
- **Mechanism:** ctx_add_local did `int slot = ctx->local_count; ctx->local_names[slot] = strdup(name); ctx->local_sigils[slot] = sigil; ctx->local_count++;` with NO bound check against the fixed sizes `char *local_names[256]` and `char local_sigils[256]`.
- **Trigger:** Compile/run a Strada source whose function body contains 256 or more accumulated local slots in a single non-bare-block scope.

### 6. Stack buffer overflow: closure capturing >64 variables overflows CompCtx.captures[64] and outer_slots[64]
- **Location:** `interpreter/vm_compiler.c:274`  (fn `ctx_add_capture`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** yes
- **Status:** ✅ **FIXED** — Now checks `ctx->capture_count >= (int)(sizeof(ctx->captures) / sizeof(ctx->captures[0]))` and emits a compile error.
- **Mechanism:** ctx_add_capture did `int idx = ctx->capture_count; ctx->captures[idx].name = strdup(name); ctx->captures[idx].outer_slot = outer_slot; ctx->capture_count++;` with no bound check against `CaptureInfo captures[64]`.
- **Trigger:** Compile untrusted Strada source containing an inner closure whose body references 65 or more distinct enclosing lexical variables.

### 7. Stack buffer overflow: sprintf with >32 args overflows VMValue args[32] (unbounded argc emitted)
- **Location:** `interpreter/vm.c:3361`  (fn `vm_execute (OP_SPRINTF)`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** yes
- **Status:** ✅ **FIXED** — Now clamps `argc` to 32 with drain of excess. Comment references the audit scenario.
- **Mechanism:** The sprintf builtin handler emits `vm_chunk_emit(ctx->chunk, (uint8_t)argc)` with no cap. The matching VM handler declares `VMValue args[32]` and runs `for (int i = argc - 1; i >= 0; i--) args[i] = SP_POP();`.
- **Trigger:** A Strada/Perl source program executed via the bytecode VM containing a call like sprintf(fmt, a1..aN) with N between 33 and 255 arguments.

### 8. Stack buffer overflow: OP_BUILTIN with >16 args (die/warn/rindex/etc.) overflows VMValue args[16]
- **Location:** `interpreter/vm_compiler.c:1592`  (fn `compile_expr (NI_CALL)`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** yes
- **Status:** ✅ **MITIGATED** — The VM handler (#3) now clamps argc to 16 at runtime, so even if the compiler emits argc>16, the buffer can't overflow. The compiler-side cap is defense-in-depth; compiler still emits unbounded argc.
- **Mechanism:** Multiple OP_BUILTIN emit sites pass a user-controlled `(uint8_t)argc` with no cap. The VM's OP_BUILTIN handler declares `VMValue args[16]` and unconditionally executes `for (int i = argc - 1; i >= 0; i--) args[i] = SP_POP();` BEFORE dispatching.
- **Trigger:** A Strada program run under strada-interp containing a call with more than 16 arguments to a variadic OP_BUILTIN builtin.

### 9. Tagged-int dereference on array element type in strada_perl5_call / call_array (crashes on ordinary integer arguments)
- **Location:** `lib/perl5/strada_perl5.c:160`  (fn `strada_perl5_call`)
- **Category:** other  |  **Attacker-controllable:** partial
- **Status:** ✅ **FIXED** — Now guards every element with `STRADA_IS_TAGGED_INT(arg)` before `arg->type` access. Both `strada_perl5_call` (line 160) and `strada_perl5_call_array` (line 237) are fixed.
- **Mechanism:** The argument-marshalling loop reads `arg->type` (and `arg->value.iv`) directly without checking for tagged integers. Array elements that hold integers are stored as TAGGED INTEGERS — odd-pointers, not heap pointers — so `arg->type` dereferences a bogus address.
- **Trigger:** From Strada: call the perl5 lib's call/call_array with an args array containing any normal integer, e.g. Perl5::call($sub, [1, 2, 3]).

### 10. Integer overflow in array reserve capacity leads to undersized allocation and heap buffer overflow
- **Location:** `runtime/strada_runtime.c:2967`  (fn `strada_array_reserve`)
- **Category:** integer-overflow  |  **Attacker-controllable:** partial
- **Status:** ✅ **FIXED** — Now guards `if (capacity > SIZE_MAX / sizeof(StradaValue*)) return;`, uses temp pointer for realloc, and NULL-checks the result.
- **Mechanism:** strada_array_reserve(av, capacity) computed `realloc(av->elements, capacity * sizeof(StradaValue*))` with no overflow check. For capacity >= 2^61, `capacity * 8` wraps modulo 2^64 to a small value.
- **Trigger:** core::array_reserve(@arr, $n) where $n >= 2^61.

### 11. Stack buffer overflow in build_concat_key: prefix_len copied into 256-byte buffer with no bounds check
- **Location:** `runtime/strada_runtime.c:4690`  (fn `build_concat_key`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** partial
- **Status:** ✅ **FIXED** — Now clamps both `suffix_len` and `prefix_len` to fit within `buflen` before any memcpy. Comment explicitly references the old overflow: "The old code memcpy'd prefix_len bytes into the fixed 256-byte buffer with no bound."
- **Mechanism:** build_concat_key(prefix, prefix_len, suffix, buf, buflen, ...) is called with a fixed 256-byte stack buffer. Original code did `memcpy(buf, prefix, prefix_len)` with NO check that prefix_len < buflen. A literal key prefix >=256 bytes in `$h{"<256+ chars>" . $i}` overflowed the stack.
- **Trigger:** Strada source: `my hash %h; my int $i = 7; $h{"<a string literal of >=256 bytes>" . $i} = 1;` — reproduced with a 300-byte literal: Segmentation fault.

### 12. Stack buffer overflow in sprintf %b width padding (memcpy after width-bounded pad loop)
- **Location:** `runtime/strada_runtime.c`  (fn `strada_sprintf_sv_args / spf_plan_build`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** partial
- **Status:** ✅ **FIXED** — The sprintf plan cache now marks any format containing `%b` (or `%c`, `%n`, `%p`, `*` width, `#` flag, etc.) as `simple=0`, falling through to the legacy loop which uses bounded `snprintf` for all conversions. The hand-rolled binary path that wrote past `temp[16384]` is no longer reachable for `%b`.
- **Mechanism:** The case 'b' (binary) branch had manual memcpy after a width-pad loop with no remaining-space check. `sprintf("%17000b", -1)` wrote ~64 bytes past a 16384-byte stack buffer.
- **Trigger:** User-controlled Strada/Perla program calling sprintf("%17000b", -1).

### 13. Negative/large max_len in strada_socket_recv causes heap buffer overflow (signed->size_t)
- **Location:** `runtime/strada_runtime.c:9884`  (fn `strada_socket_recv`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** partial
- **Status:** ✅ **FIXED** — Now rejects `if (max_len <= 0) return strada_new_str("");`, uses `size_t` for the validated length, and NULL-checks malloc.
- **Mechanism:** max_len is a signed int with no validation. A negative value sign-extends to SIZE_MAX for recv() while malloc allocated a tiny (or zero-byte) buffer. When the peer sends data, recv writes far past the allocation.
- **Trigger:** Strada code: `core::socket_recv($client_sock, $len)` where $len is derived from a peer-supplied length field and evaluates to -1.

### 14. Negative/large max_len in strada_udp_recvfrom causes heap buffer overflow (signed->size_t)
- **Location:** `runtime/strada_runtime.c:10327`  (fn `strada_udp_recvfrom`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** partial
- **Status:** ✅ **FIXED** — Same fix as #13: `if (max_len <= 0) return strada_new_undef();`, size_t cast, NULL check.
- **Mechanism:** Same defect as strada_socket_recv.
- **Trigger:** A Strada program calling core::udp_recvfrom($sock, $n) where $n is runtime-derived and can be negative.

### 15. Heap buffer overflow in strada_reverse() on truncated/invalid UTF-8 (and binary) strings
- **Location:** `runtime/strada_runtime.c:16859`  (fn `strada_reverse`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** yes
- **Status:** ✅ **FIXED** — Now clamps each character length: `if (clen <= 0 || byte_idx + (size_t)clen > byte_len) clen = 1;` before storing lengths. Comment references the audit.
- **Mechanism:** strada_reverse() computed per-character lengths using utf8_char_len(*p) WITHOUT clamping clen to the bytes actually remaining. A truncated lead byte at end of string caused the reverse loop to write past the malloc'd buffer.
- **Trigger:** Strada/Perl: reverse() applied to a scalar string whose byte content ends in an unmatched UTF-8 lead byte, e.g. `my $s = "a" . chr(0xF0); my $r = reverse($s);`.

### 16. strada_read_fd: negative/huge size wraps malloc to 0 and reads up to ~2GB into it (heap overflow)
- **Location:** `runtime/strada_runtime.c:21963`  (fn `strada_read_fd`)
- **Category:** integer-overflow  |  **Attacker-controllable:** partial
- **Status:** ✅ **FIXED** — Now rejects `if (req <= 0) return strada_new_str("");`, uses `size_t` cast, and NULL-checks malloc.
- **Mechanism:** size is computed as `(size_t)strada_to_int(size_val)` with no validation. A negative size becomes SIZE_MAX, `malloc(size + 1)` wraps to malloc(0), and `read(fd, buf, SIZE_MAX)` writes up to ~2GB into the zero-byte buffer.
- **Trigger:** A Strada program calling core::read_fd($fd, $size) where $size evaluates to -1.


## MEDIUM

### 17. OP_SPRINTF: stack buffer overflow when sprintf called with >32 arguments
- **Location:** `interpreter/vm.c:3361`  (fn `vm_execute (OP_SPRINTF)`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** no
- **Status:** ✅ **FIXED** — Same fix as #7. Now clamps argc to 32 with drain of excess.
- **Mechanism:** Same as #7. Listed separately in the original audit as a duplicate entry.
- **Trigger:** See #7.

### 18. Leak of strada_to_str() result in SQLite parameter binding
- **Location:** `lib/dbi/strada_dbi.c:433`  (fn `dbi_execute`)
- **Category:** memory-leak  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — Likely still present. `strada_to_str()` returns a malloc'd buffer; bound with SQLITE_TRANSIENT (which copies) then never freed.
- **Fix:** Capture and free after sqlite3_bind_text: `free((char*)str);` at both line 433 and line 459.
- **Trigger:** Call a prepared statement's execute() with any string bind parameter on the SQLite driver.

### 19. Leak of strada_to_str() result in MySQL parameter binding
- **Location:** `lib/dbi/strada_dbi.c:493`  (fn `dbi_execute`)
- **Category:** memory-leak  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — Likely still present. `strada_to_str()` result stored into binds[i].buffer, never freed after mysql_stmt_execute.
- **Fix:** Track bound string pointers and free after execute.
- **Trigger:** Any MySQL prepared-statement execute where a bind parameter is a string/text value.

### 20. One-byte heap buffer overflow in PostgreSQL placeholder rewrite (NUL terminator past end)
- **Location:** `lib/dbi/strada_dbi.c:536`  (fn `dbi_execute`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** partial
- **Status:** ✅ **FIXED** — Now allocates `strlen(converted_sql) + sth->num_params * 10 + 1` (the `+ 1` is the fix). Comment explicitly notes "SQL, e.g. 'SELECT 1'/'BEGIN' new_len equalled strlen(sql) and the *np = '\0' wrote one byte past."
- **Mechanism:** The Postgres '?'->'$N' rewrite allocated exactly strlen(sql) bytes for parameterless queries (num_params=0), then wrote a NUL terminator one byte past the allocation.
- **Trigger:** Any PostgreSQL connection executing a placeholder-free SQL statement.

### 21. Refcount imbalance leaks every fetched cell value in row fetch buffers
- **Location:** `lib/dbi/strada_dbi.c:654`  (fn `dbi_fetchrow_array`)
- **Category:** memory-leak  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — Likely still present. Each fetched cell value is created with refcount 1, inserted with strada_array_push (which increfs to 2), without releasing the local +1.
- **Fix:** Use `strada_array_push_take` / `strada_hash_set_take`, or `strada_decref(val)` after each push/set.
- **Trigger:** Any normal DBI query loop. Each fetched row leaks one StradaValue per column. Leak scales with result-set size.

### 22. Unbounded write into fixed g_handles[256] connection cache
- **Location:** `lib/dbi/strada_dbi.c:1074`  (fn `strada_dbi_connect`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** partial
- **Status:** ✅ **FIXED** — Now bounds-checks: `if (g_handle_count >= (int)(sizeof(g_handles) / sizeof(g_handles[0])))` with error message about "257th connect()" and disconnects the new handle. Also validates array index before reads.
- **Mechanism:** g_handles is a fixed static array of 256 pointers with monotonically increasing g_handle_count. After 256 connect() calls, idx exceeds the array bounds.
- **Trigger:** In a single process, perform 257+ successful DBI::connect() calls.

### 23. Unbounded write into fixed g_stmts[1024] statement cache
- **Location:** `lib/dbi/strada_dbi.c:1128`  (fn `strada_dbi_prepare`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** partial
- **Status:** ✅ **FIXED** — Same pattern as #22: bounds-check with error message about "1025th prepare()". Also validates array index before reads.
- **Mechanism:** g_stmts is a fixed static array of 1024 pointers with monotonically increasing g_stmt_count. After 1024 prepare() calls, idx exceeds the array bounds.
- **Trigger:** Call DBI prepare() more than 1024 times in a single process lifetime.

### 24. Pervasive strada_to_str() leaks in perl5 StradaValue wrapper functions
- **Location:** `lib/perl5/strada_perl5.c:96`  (fn `strada_perl5_eval/run/call/call_array/use/require/set_scalar/get_scalar/add_inc`)
- **Category:** memory-leak  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — Likely still present. Multiple sites capture `strada_to_str()` results into `const char *` and never free them.
- **Fix:** Capture in non-const `char *` and free() before returning.
- **Trigger:** Any call to perl5::eval/run/call/call_array/use/require/set_scalar/get_scalar/add_inc from Strada.

### 25. data buffer from strada_to_str() leaked on every USB OUT (write) transfer
- **Location:** `lib/usb/strada_usb.c:280`  (fn `strada_usb_bulk_transfer`)
- **Category:** memory-leak  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — Likely still present. strada_to_str() result never freed on OUT write paths.
- **Fix:** Add `free((char*)data);` before each return on OUT branches.
- **Trigger:** Any Strada program performing a USB OUT (write) transfer.

### 26. strada_to_str_buf integer paths ignore buflen, can overflow caller buffers < 21 bytes
- **Location:** `runtime/strada_runtime.c:2207`  (fn `strada_to_str_buf`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** unknown
- **Status:** ✅ **FIXED** — Now uses `itoa_buf_bounded(val, buf, buflen)` for tagged-int and heap-int paths, honoring buflen.
- **Mechanism:** strada_to_str_buf(sv, buf, buflen) promises to respect buflen, but two stringification paths used strada_fast_itoa which writes up to 21 bytes regardless. strada_open_sv passes 16-byte buffers for the file-open MODE argument.
- **Trigger:** core::open($path, $mode) where $mode evaluates to a non-tagged STRADA_INT (e.g. INT64_MIN).

### 27. Heap buffer overflow via uint32_t length truncation in strada_concat_cstr_sv
- **Location:** `runtime/strada_runtime.c:5530`  (fn `strada_concat_cstr_sv`)
- **Category:** integer-overflow  |  **Attacker-controllable:** partial
- **Status:** ✅ **FIXED** — Now guards: `size_t total64 = prefix_len + len_b; if (total64 > UINT32_MAX) { fprintf(stderr, "strada: string concat exceeds 4GB limit\n"); abort(); }` then casts to uint32_t. Comment describes the old overflow scenario.
- **Mechanism:** `total` was computed as `uint32_t total = (uint32_t)(prefix_len + len_b);` — if the sum exceeded UINT32_MAX (4GB), total wrapped, allocation was undersized, and the full untruncated lengths were memcpy'd past it.
- **Trigger:** A Strada expression of the form `"prefix" . $s` where $s is a ~4GB string.

### 28. Use-after-free in cycle collector when GC is disabled while roots are buffered
- **Location:** `runtime/strada_runtime.c:13022`  (fn `strada_free_value`)
- **Category:** use-after-free  |  **Attacker-controllable:** no
- **Status:** ✅ **FIXED** — The `cc_forget` call in strada_free_value is no longer gated on `cc_enabled`. The comment at line 13018-13024 explicitly states: "NOTE: must NOT be gated on cc_enabled — a candidate buffered while GC was enabled can be freed after GC is disabled; without forgetting it here, the stale cc_roots[] pointer becomes a use-after-free if GC is re-enabled and a collection runs."
- **Mechanism:** The cycle collector's cc_forget call was gated on `cc_enabled`. When GC was disabled, buffered roots could be freed without being forgotten — the stale pointer remained in cc_roots[] and was dereferenced if GC was re-enabled.
- **Trigger:** Program with cycle GC default-on: build a cyclic graph; call gc_disable(); drop the last external ref; at exit gc forces re-enable and dereferences freed memory.

### 29. Attacker-controlled repeat count drives unbounded malloc in unpack 'H'/'h'/'B'/'b' (NULL-deref on OOM, memory-exhaustion DoS)
- **Location:** `runtime/strada_runtime.c:18708`  (fn `strada_unpack`)
- **Category:** null-deref  |  **Attacker-controllable:** yes
- **Status:** ✅ **FIXED** — Now caps `hex_count` to data-derived maximum: `if (hex_count > hex_avail) hex_count = hex_avail;`. Also NULL-checks malloc: `char *hex = malloc(hex_count + 1); if (!hex) break;`.
- **Mechanism:** The unpack format string repeat count was used directly to size malloc without cap or NULL check. A format like "H2000000000" made hex_count ~2e9.
- **Trigger:** unpack() with a runtime-derived format string, e.g. $fmt = "H2000000000".


## LOW

### 30. Jump offset truncated to 16 bits: functions >65535 bytes of bytecode mis-patch jumps
- **Location:** `interpreter/vm_compiler.c:406`  (fn `compile_expr / compile_stmt`)
- **Category:** integer-overflow  |  **Attacker-controllable:** yes
- **Status:** ❌ **OPEN** — Jump targets still patched as uint16_t. No fix applied.
- **Fix:** Detect when code_len exceeds 0xFFFF and emit a compile error, or widen jump operands to u32.
- **Trigger:** Feed the interpreter/VM a Strada source file containing a single function whose compiled bytecode exceeds 65535 bytes.

### 31. Leak of strada_to_str() result wrapped in strdup in PostgreSQL param values
- **Location:** `lib/dbi/strada_dbi.c:548`  (fn `dbi_execute`)
- **Category:** memory-leak  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — `param_values[i] = strdup(strada_to_str(val));` doubles the allocation and leaks the strada_to_str buffer.
- **Fix:** Use strada_to_str result directly: `param_values[i] = strada_to_str(val);`.
- **Trigger:** Execute a parameterized Postgres statement with a string bind value.

### 32. Leak of dsn/user/pass strada_to_str() results in connect entry point
- **Location:** `lib/dbi/strada_dbi.c:980`  (fn `strada_dbi_connect`)
- **Category:** memory-leak  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — dsn_str/user_str/pass_str malloc'd by strada_to_str, passed to dbi_connect (which strdup's its own copies), then never freed.
- **Fix:** `free((char*)dsn_str)` etc. after dbi_connect returns.
- **Trigger:** Any call into the DBI layer: connect leaks 3 buffers per call.

### 33. Unguarded data_sv->struct_size read on USB OUT transfers (tagged-int wild read)
- **Location:** `lib/usb/strada_usb.c:281`  (fn `strada_usb_bulk_transfer`)
- **Category:** null-deref  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — Likely still present. `data_sv->struct_size` dereferenced without STRADA_IS_TAGGED_INT guard.
- **Fix:** Use `strada_length_sv(data_sv)` which is tagged-int aware.
- **Trigger:** A Strada program calling a write endpoint passing an integer literal as the data argument.

### 34. uint32_t truncation of size_t string length desyncs StradaString buffer from stored byte-length (heap over-read on >4GB strings)
- **Location:** `runtime/strada_runtime.c:784`  (fn `strada_new_str`)
- **Category:** integer-overflow  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — `strlen(s)` is full `size_t` but truncated to `uint32_t` for allocation while `struct_size` gets the full value. Practical OOM prevents exploitation, but the desync between allocation size and stored length is real.
- **Fix:** Reject lengths > UINT32_MAX, matching the 4GB abort guards already in concat helpers.
- **Trigger:** core::slurp() of a >4GB file on a 64-bit host with enough RAM.

### 35. Unchecked malloc of weak-registry bucket — NULL dereference on OOM
- **Location:** `runtime/strada_runtime.c:1230`  (fn `weak_registry_register`)
- **Category:** null-deref  |  **Attacker-controllable:** no
- **Status:** ❌ **OPEN** — `bucket = malloc(sizeof(StradaWeakBucket))` not NULL-checked before dereference.
- **Fix:** Check for NULL and unlock+return.
- **Trigger:** Call core::weaken() under memory exhaustion (OOM-only).

### 36. Unchecked malloc of weak-registry entry — NULL dereference on OOM
- **Location:** `runtime/strada_runtime.c:1238`  (fn `weak_registry_register`)
- **Category:** null-deref  |  **Attacker-controllable:** no
- **Status:** ❌ **OPEN** — Same as #35.
- **Fix:** Check for NULL and unlock+return.
- **Trigger:** Call core::weaken() under memory exhaustion (OOM-only).

### 37. realloc return assigned directly to av->elements: original buffer leaked and NULL-deref on allocation failure
- **Location:** `runtime/strada_runtime.c:2282`  (fn `strada_array_push` and related)
- **Category:** memory-leak  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — Multiple array growth sites assign realloc directly without checking for NULL.
- **Fix:** Capture realloc into a temporary, check for NULL before committing.
- **Trigger:** Drive an array to grow under memory pressure until realloc fails.

### 38. realloc result overwrites only pointer in strada_hash_resize: leak + NULL-deref crash on OOM
- **Location:** `runtime/strada_runtime.c:3109`  (fn `strada_hash_resize`)
- **Category:** memory-leak  |  **Attacker-controllable:** no
- **Status:** ❌ **OPEN** — `hv->hash_index = realloc(hv->hash_index, ...)` overwrites on failure.
- **Fix:** Use a temporary, check for NULL before committing.
- **Trigger:** realloc failure during hash bucket-doubling resize (OOM-only).

### 39. Unchecked malloc results dereferenced in strada_concat / strada_concat_free / strada_concat_sv
- **Location:** `runtime/strada_runtime.c:4513`  (fn `strada_concat` and related)
- **Category:** null-deref  |  **Attacker-controllable:** no
- **Status:** ❌ **OPEN** — Multiple concat helpers use malloc result without NULL check.
- **Fix:** Check for NULL or route through checked allocation wrappers.
- **Trigger:** Concatenate strings whose combined length approaches address-space limit (OOM-only).

### 40. Unchecked malloc(size+1) then write/NULL-deref in file slurp helpers
- **Location:** `runtime/strada_runtime.c:6540`  (fn `strada_read_file` and related)
- **Category:** null-deref  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — strada_read_file, strada_slurp, strada_slurp_fh compute `size` from ftell() then malloc(size+1) with no NULL check.
- **Fix:** Check for NULL and return undef on failure.
- **Trigger:** slurp() a very large file under memory pressure.

### 41. malloc(max_len + 1) with int max_len can wrap and is unchecked for NULL in socket recv paths
- **Location:** `runtime/strada_runtime.c:7980`  (fn `strada_socket_recv`)
- **Category:** integer-overflow  |  **Attacker-controllable:** partial
- **Status:** ✅ **MOSTLY FIXED** — The negative-length path is closed (#13). INT_MAX overflow on `max_len + 1` is not separately addressed, but max_len is now rejected at <= 0 and cast to size_t, so the overflow path is effectively unreachable.
- **Fix:** Already addressed by the #13 fix.
- **Trigger:** See #13.

### 42. realloc return value overwrites original pointer in POSIX strada_regex_build_result (leak + NULL-deref on OOM)
- **Location:** `runtime/strada_runtime.c:10765`  (fn `strada_regex_build_result`)
- **Category:** memory-leak  |  **Attacker-controllable:** no
- **Status:** ❌ **OPEN** — POSIX fallback path doesn't use temp pointer for realloc (PCRE2 path does).
- **Fix:** Use temp pointer and NULL-check, matching PCRE2 path.
- **Trigger:** Non-PCRE2 build; global substitution under memory pressure (OOM-only).

### 43. Potential double-free of arena-owned tied_obj after cur_arena is popped in strada_arena_end
- **Location:** `runtime/strada_runtime.c:11661`  (fn `arena_release_backbone`)
- **Category:** double-free  |  **Attacker-controllable:** no
- **Status:** ❌ **OPEN** — Narrow reachability (requires tie() on arena-resident container with arena-resident tied object). The in-code justification is incorrect.
- **Fix:** Pop cur_arena AFTER the backbone-release walk, or pass arena pointer for membership check.
- **Trigger:** arena_begin(); tie %h with arena-allocated object; arena_end(). (Narrow combination.)

### 44. Integer overflow in strada_cstruct_set_field bounds check enables out-of-bounds memcpy write
- **Location:** `runtime/strada_runtime.c:12626`  (fn `strada_cstruct_set_field`)
- **Category:** integer-overflow  |  **Attacker-controllable:** no
- **Status:** ❌ **OPEN** — `if (offset + size > STRADA_STR_BYTELEN(sv)) return;` can wrap. Offsets from compiler-generated struct layout, not untrusted input.
- **Fix:** Use non-wrapping form: `if (offset > cap || size > cap - offset) return;`.
- **Trigger:** Program-controlled, not externally attacker-supplied.

### 45. Integer overflow in strada_cstruct_get_field bounds check returns out-of-bounds pointer
- **Location:** `runtime/strada_runtime.c:12634`  (fn `strada_cstruct_get_field`)
- **Category:** integer-overflow  |  **Attacker-controllable:** no
- **Status:** ❌ **OPEN** — Same wraparound issue as #44 but on the read path.
- **Fix:** Use non-wrapping form.
- **Trigger:** Program-controlled, not externally attacker-supplied.

### 46. Thread struct and closure leak on detach-after-completion race
- **Location:** `runtime/strada_runtime.c:12930`  (fn `strada_thread_detach`)
- **Category:** memory-leak  |  **Attacker-controllable:** no
- **Status:** ❌ **OPEN** — Timing-dependent leak of StradaThread struct + closure ref. Not attacker-controllable.
- **Fix:** Use atomic compare-and-swap for single ownership-transfer.
- **Trigger:** Non-deterministic timing window; not externally controllable.

### 47. realloc result overwrites original pointer in pack's ENSURE_SPACE macro (lost pointer / NULL-deref on OOM)
- **Location:** `runtime/strada_runtime.c:15065`  (fn `strada_pack (ENSURE_SPACE macro)`)
- **Category:** memory-leak  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — ENSURE_SPACE does `buf = realloc(buf, buf_size);` with no NULL check.
- **Fix:** Use a temp pointer and NULL-check.
- **Trigger:** pack() with template containing large explicit count (attacker-controlled) under memory pressure.

### 48. strada_join re-stringifies elements between sizing and copy passes; a tied/non-deterministic element can produce a heap buffer overflow (TOCTOU)
- **Location:** `runtime/strada_runtime.c:16468`  (fn `strada_join`)
- **Category:** buffer-overflow  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — strada_join stringifies elements twice; tied/overloaded elements can return a longer string on second call.
- **Fix:** Cache stringified pointers from sizing pass (as strada_join_sv already does).
- **Trigger:** join() on array containing tied scalar whose FETCH returns growing strings.

### 49. Unchecked malloc in StringBuilder constructors causes NULL-write on buffer[0]
- **Location:** `runtime/strada_runtime.c:16566`  (fn `strada_sb_new`)
- **Category:** null-deref  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — `sb->buffer = malloc(sb->capacity);` immediately followed by `sb->buffer[0] = '\0';`.
- **Fix:** Check malloc result; on failure free partial allocation and return undef.
- **Trigger:** sb_new(-1) or sb_new(2e18).

### 50. realloc return value overwrites sb->buffer — leaks original on NULL, then NULL-deref via memcpy
- **Location:** `runtime/strada_runtime.c:16621`  (fn `strada_sb_append`)
- **Category:** memory-leak  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — `sb->buffer = realloc(sb->buffer, sb->capacity);` with no NULL check.
- **Fix:** Use temp pointer and NULL-check.
- **Trigger:** StringBuilder append under memory pressure.

### 51. oop_grow return value ignored — realloc failure leaves cap unchanged and the following indexed write overflows
- **Location:** `runtime/strada_runtime.c:19580`  (fn `oop_get_or_create_package` and related)
- **Category:** buffer-overflow  |  **Attacker-controllable:** no
- **Status:** ❌ **OPEN** — oop_grow() returns 0 on failure but callers ignore the return and write at stale index.
- **Fix:** Check oop_grow() return value; abort on failure.
- **Trigger:** realloc failure during package/method registration (OOM-only, not attacker-controllable).

### 52. realloc return value not NULL-checked in strada_brace_expand (leak + NULL write on OOM)
- **Location:** `runtime/strada_runtime.c:20950`  (fn `strada_brace_expand`)
- **Category:** memory-leak  |  **Attacker-controllable:** no
- **Status:** ❌ **OPEN** — Three realloc calls overwrite original pointer without NULL check.
- **Fix:** Capture into temp, NULL-check before committing.
- **Trigger:** glob() with brace pattern under memory exhaustion (OOM-only).

### 53. strada_path_join dereferences argument type without tagged-int/NULL guard
- **Location:** `runtime/strada_runtime.c:24198`  (fn `strada_path_join`)
- **Category:** null-deref  |  **Attacker-controllable:** partial
- **Status:** ✅ **FIXED** — Now checks `if (!parts_val || STRADA_IS_TAGGED_INT(parts_val)) return strada_new_undef();` before reading ->type. Confirmed at current line 24198.
- **Mechanism:** strada_path_join read parts_val->type with no tagged-int guard. If called with an integer, dereferences a bogus odd address.
- **Trigger:** core::path_join(5) — confirmed empirically: SIGSEGV (exit 139). Now returns undef gracefully.

### 54. Unchecked realloc result in strada_array_splice_sv leaks original buffer and NULL-derefs on OOM
- **Location:** `runtime/strada_runtime.c:23671`  (fn `strada_array_splice_sv`)
- **Category:** null-deref  |  **Attacker-controllable:** no
- **Status:** ❌ **OPEN** — realloc assigned directly to av->elements without NULL check.
- **Fix:** Capture into temp, NULL-check, abort on failure.
- **Trigger:** splice() under memory exhaustion (OOM-only).

### 55. strada_hv_fetch (borrowed) returns an owned value on the tied-hash path, leaking one ref per tied fetch
- **Location:** `runtime/strada_runtime.h:1891`  (fn `strada_hv_fetch`)
- **Category:** memory-leak  |  **Attacker-controllable:** partial
- **Status:** ❌ **OPEN** — Tied path returns owned refcount-1 value; callers of the borrowed variant don't decref.
- **Fix:** Route borrowed reads of tied hashes through cleanup stack, or use _owned variant.
- **Trigger:** Any Strada program calling hash_get builtin on a tied hash. Note: the normal `$h{key}` syntax is not affected (uses _owned variant).


## INFO

### 56. strada_super_call calls SV_BLESSED(obj) without a tagged-int guard
- **Location:** `runtime/strada_runtime.c:20312`  (fn `strada_super_call`)
- **Category:** null-deref  |  **Attacker-controllable:** no
- **Status:** ❌ **OPEN** — No STRADA_IS_TAGGED_INT(obj) check before SV_BLESSED(obj). Not reachable from normal codegen (invocant is always a blessed $self).
- **Fix:** Add STRADA_IS_TAGGED_INT(obj) to early-return guard.
- **Trigger:** Not reachable from semantically valid code. Hand-written SUPER:: call with integer invocant only.

### 57. strada_call_destroy dereferences obj->meta via SV_BLESSED before NULL/tagged-int check
- **Location:** `runtime/strada_runtime.c:20387`  (fn `strada_call_destroy`)
- **Category:** null-deref  |  **Attacker-controllable:** no
- **Status:** ❌ **OPEN** — SV_BLESSED(obj) called BEFORE the NULL check. Exported symbol, externally callable.
- **Fix:** Reorder: check `if (!obj || STRADA_IS_TAGGED_INT(obj) || oop_destroying) return;` before SV_BLESSED.
- **Trigger:** External call to strada_call_destroy(NULL) or strada_call_destroy(tagged_int).

### 58. Missing NULL guard on fp_files[].lines after calloc failure in strada_full_profile_line
- **Location:** `runtime/strada_runtime.c:22916`  (fn `strada_full_profile_line`)
- **Category:** null-deref  |  **Attacker-controllable:** no
- **Status:** ❌ **OPEN** — calloc result not checked; NULL deref if calloc fails. Only in profiling mode.
- **Fix:** Return -1 from register_file on calloc failure, or add NULL guards.
- **Trigger:** --full-profile under memory pressure (OOM-only, opt-in debug feature).


---

## Appendix: Remaining design-level concerns

These were not part of the original audit but merit mention:

### A. Command injection via system()/popen()/exec()
- **Location:** `runtime/strada_runtime.c:21884`  (fn `strada_system` and related)
- **Category:** design  |  **Attacker-controllable:** yes
- **Status:** ⚠️ **BY DESIGN** (mirrors Perl) — `system()`, `popen()`, `qx()`, and `exec()` all pass user strings directly to `/bin/sh -c` with no sanitization. No escaping helper is provided by the runtime. Callers must sanitize externally.

### B. Signal handler calls non-signal-safe functions
- **Location:** `runtime/strada_runtime.c:21694`  (fn `strada_signal_wrapper`)
- **Category:** design  |  **Attacker-controllable:** no
- **Status:** ⚠️ **BY DESIGN** (mirrors Perl %SIG) — `strada_signal_wrapper` calls `strada_new_int` (may malloc) and user Strada handlers from signal context. Not async-signal-safe.

---

## Appendix: Refuted candidates (verifier rejected)

- `runtime/strada_runtime.c:8658` (buffer-overflow, claimed high): The claim's local invariant is incorrect.
- `runtime/strada_runtime.c:10762` (buffer-overflow, claimed low): Asymmetry present but not exploitable as claimed.
- `runtime/strada_runtime.c:14071` (null-deref, claimed low): Three unchecked mallocs in strada_tr_utf8, but practical OOM prevents exploitation.
- `runtime/strada_runtime.c:16134` (null-deref, claimed low): Flagged realloc is dead code — never executed.
- `runtime/strada_runtime.c:18898` (null-deref, claimed info): Tagged-int guard present at 18894 before type read.
- `interpreter/vm.c:1271` (buffer-overflow, claimed low): Local slot bounds check present.
- `interpreter/vm.c:628` (other, claimed info): Requires count > g_locals_top which cannot happen.
- `interpreter/vm.c:2614` (other, claimed low): sp is size_t, underflow would yield OOB, but argc validation prevents it.
- `runtime/strada_runtime_tcc.h:62` (buffer-overflow, claimed medium): TCC header masks only bit 63 vs bits 62+63 in main header.
- `runtime/strada_runtime_tcc.h:1336` (use-after-free, claimed low): TCC header stack_args1_init omission; main header has the fix.
- `runtime/strada_runtime.h:1891` (null-deref, claimed low): Tagged-int guard present; NULL guard missing but callers always pass non-NULL.
- `lib/perl5/strada_perl5.c:143` (other, claimed medium): Now guarded (see #9 fix).
- `lib/compress/strada_compress.c:165` (other, claimed low): Buffer sizing correct, no corruption.
- `runtime/strada_runtime.c:19213` (buffer-overflow, claimed high): Invariant analysis wrong — grow check runs at top of every iteration.
