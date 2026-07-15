# Soak-test debugging log

Investigating: `bridge_soak.py` (on the UNO Q's Linux side) intermittently/deterministically
times out on `Bridge.call("ping"/"getcount")` after a small, fixed number of messages
(observed: 5, 9, 19 — varies by run, ~9-18 msg/s).

Each entry is a change made to the STM32 firmware during the investigation, why it was made,
and what it told us. Ordered chronologically. Not all entries turned out to be the root cause —
that's noted where relevant.

---

## 1. `rpc.c`: stop ignoring `lpuart_write()`'s return value

**Files:** `Core/Src/rpc.c`

**What changed:** `rpc_call`, `rpc_notify`, and `send_response` all called `lpuart_write()`
without checking its return value. `lpuart_write()` can return `false` when the 256-byte TX
ring buffer (`tx_rb`) is momentarily full. Added `rpc_send()`, a bounded (20ms) spin-retry
wrapper, and made all three call sites use it:
- `rpc_call`: only reserves a `pending[]` slot if the message actually got sent. Previously it
  marked the slot `in_use` and returned a valid msgid *before* attempting the write, so a
  dropped write would pin a slot for the full 3s timeout for a call that was never transmitted.
- `send_response`: only counts `dbg_resp_sent++` on an actual successful send; added
  `dbg_resp_send_failed` counter, and wired up the existing (previously dead) `dbg_resp_destroy_fail`
  counter.
- `rpc_notify`: added `dbg_notify_send_failed` counter (currently dead code — `rpc_notify` is
  unused anywhere in the firmware).

**Why:** initial hypothesis was that ping responses were silently dropped when send it under
load, causing `Bridge.call()` timeouts and TX buffer contention was suspected as the mechanism.

**Result:** real bug, worth keeping, but **did not fix the soak-test failure**. Re-ran
`bridge_soak.py` after flashing — identical failure pattern (5/9/19 msg, ~9-18 msg/s). At that
rate the 256-byte TX buffer is nowhere near saturation, so this wasn't the actual mechanism.
Ruled out via on-chip counters (see #3): `dbg_resp_send_failed` stayed at 0 through a full failing
run — no write ever actually failed during the real test.

---

## 2. On-chip debug counters revealed the failure isn't in `rpc.c`'s pending-slot lifecycle

**No code change** — read `pending[]`/`reap_timeouts()`/`registration_tick()` and confirmed:
- `pending[]` (`RPC_MAX_PENDING = 10`) is only used for calls *this chip originates*
  (`$/register`, one at a time, serialized by `reg_inflight`). `ping`/`getcount` are inbound
  requests handled via `send_response()` and never touch `pending[]`.
- `reap_timeouts()` always frees a slot (sets `in_use = false`) before invoking its callback,
  on both the response-match and timeout paths — no leak possible in the current code.

**Why this mattered:** the leading hypothesis at the time was "ping responses never complete,
so each ping call leaks a `pending[]` slot until the table's exhausted." Since the soak script
only ever has one call in flight (`Bridge.call()` is synchronous) and STM32-side registration
is capped to 1-in-flight, this table structurally cannot be exhausted by this test. Redirected
the search away from `pending[]` lifecycle bugs.

---

## 3. Read `arduino/app_utils/bridge.py` (MPU-side `Bridge` client)

**No firmware change** — pulled the source via `adb shell` from
`/home/arduino/serial-env/arduino/app_utils/bridge.py` on the board.

**Finding:** `ClientServer.call()`'s timeout path (`except queue.Empty`) correctly pops its own
`callbacks` dict before raising `TimeoutError` — no leak on the Python client side either. Also
noted it fires `Bridge.notify("$/cancelRequest", msgid)` on timeout, which the firmware silently
ignores (`$/cancelRequest` isn't a registered method) — harmless, but adds traffic exactly when
the link is already struggling.

**Why this mattered:** ruled out the Python client's own pending-call bookkeeping as a leak
source. Left the router process (`arduino-router`, a separate compiled daemon bridging
`unix:///var/run/arduino-router.sock` to the UART) as the only remaining unexamined layer
between the two ends we do control.

---

## 4. Live on-chip counters during an actual failing run (first hard evidence STM32 is healthy)

**No code change** — attached GDB (`arm-none-eabi-gdb ... target extended-remote localhost:3333`)
after a failing 3-run soak and read `dbg_req_seen`, `dbg_resp_sent`, `dbg_alloc_fail`,
`rx_overflow`, `reg_idx`, `reg_inflight`.

**Finding:** `dbg_req_seen == dbg_resp_sent == 90` — the firmware answered *every single request
it received* with zero internal errors of any kind. But the Python script only issued 37 calls
that session, and `dbg_last_req_msgid` was 876 — far beyond what 37 client calls could produce.
More device-level request/response traffic was happening than the client accounted for.

Also found, via `journalctl -u arduino-router`, a prior log line:
`ERROR Error in connection err="invalid packet, expected array, got: int8"`. Initially treated
this as evidence of transport-level frame corruption from the STM32 — **this was premature**; the
timestamp predates the actual test window and a `torn.py` script already present in
`serial-env/` suggests it's from a separate, intentional malformed-frame test, not from
`bridge_soak.py`. Flagged as retracted in the loop but not yet definitively ruled out.

---

## 5. `lpuart.c`: on-chip CRC16 tracing across the TX pipeline (queued vs. armed)

**Files:** `Core/Src/lpuart.c`

**What changed:** added `crc16_ccitt()` (CRC16-CCITT, bit-banged — diagnostic use only, never
put on the wire). Extended the existing (already-present but underused) `tx_trace[]`/`tx_arm[]`
ring buffers with `crc`, `seq`, and `tick` fields:
- `lpuart_write()` computes a CRC over exactly what the caller asked to send, before it touches
  the ring buffer ("intent" / queued side).
- `lpuart_start_tx()` computes a CRC over exactly what's read back out of the ring buffer right
  before `LL_DMA_EnableChannel` ("armed" / about-to-transmit side).
- `tx_frame_seq`, a monotonic counter, ties queued writes to the arm events that watermark past
  them, so the two trace buffers can be cross-referenced after the fact via GDB.

**Why:** to directly test whether the ring buffer / DMA-arming logic (previously reasoned about
but not empirically verified) was corrupting frames, independent of whether the router receives
them correctly.

**Result:** ran a failing soak test, dumped both trace buffers. **Every queued frame's CRC
exactly matched its corresponding armed frame's CRC** (cross-referenced 7 distinct frames, all
matched, e.g. seq 24 → crc 64906 on both sides). Conclusive: the ring buffer, wraparound
splitting, and DMA-arming logic are byte-perfect. **This firmware's TX pipeline is not the
source of corruption.**

---

## 6. `lpuart.c` + `Core/Inc/lpuart.h` + `stm32u5xx_it.c`: RX hardware error instrumentation

**Files:** `Core/Src/lpuart.c`, `Core/Inc/lpuart.h`, `Core/Src/stm32u5xx_it.c`

**What changed:**
- Added `rx_ore_count`, `rx_fe_count`, `rx_ne_count` (LPUART overrun / framing / noise error
  counters) and `rx_dma_err_count` (GPDMA1 channel 3 data-transfer-error counter).
- Added `lpuart_rx_error_check()` — checks/clears/counts ORE, FE, NE flags on LPUART1 — and
  `lpuart_rx_dma_error_check()` for GPDMA1 channel 3's DTE flag.
- Enabled `LL_LPUART_EnableIT_ERROR()` (CR3.EIE) and `LL_DMA_EnableIT_DTE()` at init, so these
  conditions actually raise an interrupt instead of just setting a flag that nothing polls.
  Wired both checks into `LPUART1_IRQHandler` / `GPDMA1_Channel3_IRQHandler` in
  `stm32u5xx_it.c`.

**Why:** these are hardware-level error conditions that happen *upstream* of every
software-visible counter (ring buffer, mpack parser, etc.) — if the LPUART peripheral itself
were dropping or corrupting incoming bytes (e.g. an overrun because the RX DMA wasn't serviced
in time), none of the existing instrumentation could ever see it. ORE in particular can silently
stall further reception on some STM32 USART/LPUART configs until cleared, so this also doubles
as a potential functional fix, not just visibility.

**Result:** ran a failing soak test, dumped counters. All zero: `rx_ore_count = rx_fe_count =
rx_ne_count = rx_dma_err_count = rx_overflow = 0`, and `dbg_req_seen == dbg_resp_sent` again
(30/30, zero failures). **RX hardware path is also provably clean.**

---

## Where this leaves things

Both directions of the STM32's UART pipeline — ring buffers, DMA arm/drain (CRC-verified), and
the LPUART/GPDMA hardware error flags — are clean during an actual failing `bridge_soak.py` run.
The firmware answers 100% of what it receives, correctly, every time. The failure is not in this
codebase. Current suspicion (unconfirmed): the `arduino-router` daemon
(`/usr/bin/arduino-router`, systemd unit `arduino-router.service`) — it's the one layer between
the Python client and this chip that hasn't been instrumented, and item #4's msgid/request-count
mismatch (90 device-level requests vs. 37 client-issued calls) is still unexplained.

**Not yet done:** examining `arduino-router` itself (binary, not source we control) — e.g. its
logs during an actual failing run (not just the stale one from #4), or a raw capture of the
`/dev/ttyHS1` traffic independent of the router's own parsing.

---

## 7. Captured router logs during a live failing run — found the actual loss point

**No firmware change.** The systemd unit's `ExecStart` (`--verbose`) is overridden by a
generator drop-in (`/run/systemd/generator/arduino-router.service.d/*.conf`) that drops
`--verbose` entirely — the live router had never been logging at request granularity. With the
user's help (`sudo systemctl stop arduino-router`), started a manual instance as the `arduino`
user: `arduino-router --verbose --unix-port /home/arduino/arduino-router-verbose.sock
--serial-port /dev/ttyHS1 --serial-baudrate 115200` (used a home-directory socket path since
`/var/run` isn't writable without root; pointed `bridge_soak.py` at it via `APP_SOCKET`).

**Finding:**
- Log shows the router issues **6 separate `write()` calls per outgoing MessagePack
  frame** (one per field: array header, type, msgid, string header, string bytes, params) rather
  than one buffered write — a real hazard on a raw serial fd.
- Across a 3-run soak (133 requests sent), exactly **3 requests never got a matching response**
  (`133 "Received request"` vs `130 "Received response"`, and exactly 3 `$/cancelRequest`
  notifications logged — one per failed run). Confirmed via `grep -c` on the log.
- For the last such case (msgid 131, method `ping`), the router's log shows the full 6-syscall
  write sequence completed with **no error logged**. On-chip, `dbg_last_req_msgid` was **130** —
  one less than 131 — and `dbg_req_seen == dbg_resp_sent` (128/128, zero drops on the chip's own
  accounting). Combined with #6's RX-hardware-error counters (all zero, checked again after this
  run): **the STM32 never received that request's bytes at all.** Not corruption the peripheral
  could detect — the bytes never arrived.

**Conclusion so far:** the loss is happening between the router's `write()` syscalls and the
physical UART — either in the router process itself (e.g. a short write it doesn't check for) or
in the Linux kernel's serial driver for `/dev/ttyHS1` under this particular 6-tiny-writes-per-frame
pattern. This is outside both codebases we control (firmware and `bridge_soak.py`).

**Tried and blocked:** wanted to `strace` the router process to see actual `write()` return
values (would settle router-bug vs. driver-bug definitively) — `strace` isn't installed on the
board, and installing it needs a password-interactive `sudo` we don't have. `dmesg` was also a
dead end — the kernel ring buffer had already rotated past the test window (only boot-time
messages remained).

---

## 8. Ruled out "undocumented protocol quirk"; found the likely actual mechanism in `go.bug.st/serial`

**No firmware change.** User asked to confirm this is genuinely a router/transport-level bug and
not our firmware failing to conform to some undocumented expectation of the real protocol.
Compared against two things:

1. **Arduino's own reference MCU-side implementation, `Arduino_RPClite`** — found on the board at
   `/home/arduino/.arduino15/internal/Arduino_RPClite_0.2.1_.../Arduino_RPClite/src/` (installed
   as part of the Arduino IDE/CLI toolchain; not something we'd have thought to look for without
   the user pointing at it). Read `client.h`, `server.h`, `decoder.h`, `SerialTransport.h`: same
   array shapes (request=4, response=4, notify=3), same message-type integers, same
   one-buffered-write-per-frame semantics. Our `rpc.c` conforms to this reference; no protocol
   deviation found.

2. **The router binary's actual dependencies**, via `strings /usr/bin/arduino-router`: it's built
   on the public Go library `go.bug.st/serial` v1.6.4 for the physical serial I/O
   (`go.bug.st/serial.(*unixPort).Write` appears in the symbol table). Pulled that library's
   source locally (`go get go.bug.st/serial@v1.6.4`, inspected via
   `$GOMODCACHE/go.bug.st/serial@v1.6.4/serial_unix.go`):

   ```go
   func (port *unixPort) Write(p []byte) (n int, err error) {
       n, err = unix.Write(port.handle, p)
       if n < 0 { n = 0 }
       return
   }
   ```

   A single raw `write(2)` syscall, **no retry loop for short writes**. This violates Go's
   `io.Writer` contract (must return non-nil `err` if `n < len(p)`) — a short write from the
   kernel returns silently with `err == nil`. Confirmed this is still the case in the latest
   release (v1.7.1) — not a since-fixed bug, current behavior.

**Why this fits:** if the router's calling code trusts the `io.Writer` contract (the idiomatic
assumption in Go) rather than independently checking the returned `n`, any short write silently
drops the untransmitted tail with no error anywhere — matching exactly what #7 captured: 6 tiny
`write()`s per frame (maximizing the chance of a short write under load), a request that vanishes
with zero error in the router's own log, and zero corresponding evidence on the STM32 (which is
consistent with #5/#6's clean on-chip instrumentation).

**Confidence, stated precisely:** this identifies a real, verifiable footgun in the router's
serial transport dependency that matches the symptom exactly. It is not yet proven with a trace
showing `n < len(p)` at the literal moment of a drop (would need `strace` or router source, both
still blocked — see #7). Treat as the leading explanation, not a closed case.

---

## 9. Retracted #8 — got `strace` working after all, and it exonerated the router

**No firmware change.** User was skeptical that a Go stdlib-adjacent library would ship this kind
of bug, and asked to look at Arduino's own reference implementation for anything we'd missed
before accepting #8's theory.

**Reference-implementation comparison:** pulled `Arduino_RPClite` (the official MCU-side library;
found on the board under `~/.arduino15/internal/...`) via `adb`, including files not examined in
item #8 (`decoder.h`, `SerialTransport.h`, `serial_client.py`/`serial_server.py`). Its `send()`
also loops on short writes, under an explicit comment: *"blocking send, under the assumption
`_transport->write` will always succeed eventually."* That assumption is safe on the MCU side only
because Arduino's `Stream::write()` blocks until everything is sent. This didn't overturn #8 —
if anything it explained *why* the protocol has zero resync/checksum tolerance (nobody designing
against Arduino's `Stream` ever needed it) — but it also didn't prove #8 either.

**Getting real evidence:** found an unused, already-built `LD_PRELOAD` shim at
`~/serial-env/write_shim.c`/`.so`, written earlier specifically to catch a short `write(2)` on the
`ttyHS1` fd, but never actually run. Running it revealed why it could never have worked:
`arduino-router` is a **statically-linked Go binary** (`file`/`ldd` confirm no dynamic linking) —
Go's runtime issues raw syscalls directly, bypassing libc entirely, so an `LD_PRELOAD` hook on
libc's `write()` symbol can never see anything it does. This wasn't a fixable instrumentation gap;
it ruled out that whole approach.

Got a real `strace` onto the board without root by downloading the Debian `arm64` `.deb` package
directly (`ar x` + `tar xf data.tar.xz`, no `dpkg-deb` needed), pushing the extracted binary via
`adb push`, and running the router as an unprivileged child of `strace` (tracing your own child
needs no special permission). Stopped the stock `arduino-router.service` **and its `.path` unit**
(path-activation would otherwise respawn the stock router), and ran a manual
`strace -f -e trace=write,read -o ... arduino-router --unix-port ~/arduino-router-shim.sock ...`
instance instead, redirecting `bridge_soak.py`/`stresstest.py` at it via `APP_SOCKET`.

**Result: every `write()` to the `ttyHS1` fd, at the syscall level, returns the full requested
length, every single time — no short write, ever**, across many repeated `$/register` response
writes (each broken into 6+ tiny single-byte `write()`s, all fully successful). This directly
contradicts #8: the syscalls are not short. **Retracting #8.**

---

## 10. Found the real bug: this firmware was discarding its own correctly-received data

**Files:** `Core/Src/rpc.c`, `Core/Src/lpuart.c`, `Core/Inc/lpuart.h` (temporary trace
instrumentation, since removed — see below).

**First clue:** with `strace` confirming clean writes, attached GDB (`arm-none-eabi-gdb ...
target extended-remote localhost:3333`, a debug probe already running on the operator's machine)
and found `reg_idx` permanently stuck at `0` — the device was retrying `$/register("ping")` every
~3s forever, never advancing, even though the router's response for the *very first* attempt was
independently confirmed correct and fully written over the wire.

**Byte-level trace:** added a temporary ring-buffer trace of every message the mpack tree
successfully parsed. Correlated against the true wire bytes for a single response
(`[1, msgid, [5, "route already exists: ping"], nil]`, 33 bytes total): instead of one coherent
4-element array, the firmware was parsing **5 separate top-level messages** — the type byte, the
msgid byte, the error code byte, the string, and the nil — each individually, as if the two array
headers (`0x94` outer, `0x92` inner) had simply never existed. Every other byte, in the correct
order, with correct values, was accounted for exactly once.

**Root cause, confirmed via a second trace on `mpack_tree_error`/`node_count`/`max_nodes` added
directly in `rpc.c` (mpack.c itself was never modified):** `rpc_poll()` called
`mpack_tree_try_parse()` unconditionally on every main-loop iteration, with no gate on whether new
bytes had actually arrived. `mpack`'s own `mpack_tree_parse_children()` (in the vendored,
untouched `mpack.c`) increments `tree->node_count` by an array's child count on *every* call,
including calls that immediately fail their own reservation because the rest of the message hasn't
arrived over UART yet — and that increment is never rolled back on such a retry. Because the
bare-metal main loop iterates far faster than bytes arrive at 115200 baud (~87µs/byte), the same
not-yet-satisfied array node was being re-entered thousands of times while its bytes trickled in,
running `node_count` (max 32, set in `rpc_init()`'s `mpack_tree_init_stream(...)`) past its limit
in a handful of microseconds — long before the 33-byte message was even half received. Confirmed
directly: `error_trace[]` showed `error = mpack_error_too_big`, `node_count = 33`, `max_nodes =
32`, `size_at_error = 0` on every single occurrence.

Once that error fires, `rpc_poll()`'s existing (correct, from item #1) error handling —
`mpack_tree_destroy(&rx_tree); rpc_init();` — discards the tree, including whatever prefix bytes
(the array headers) had already been pulled out of `rx_staging` into the now-freed buffer. The
fresh tree then reparses whatever's left of that same message, still sitting in `rx_staging`, as
brand-new headerless top-level scalars — exactly the corruption pattern observed.

**This also fully explains why GDB single-stepping earlier made the bug disappear**: halting the
CPU on a breakpoint doesn't halt DMA, so bytes kept accumulating in the background; by the time
execution resumed, whole messages tended to already be fully buffered, skipping the multi-retry
window where the bug lives.

**Fix (no changes to `mpack.c`/`mpack.h`):** added `rx_staged_events`, a monotonic counter bumped
once per RX staging event in `lpuart.c`'s `rx_stage_handler()`, and gated `rpc_poll()`'s call to
`mpack_tree_try_parse()` on that counter having changed since the last poll. This bounds retries to
real UART arrival cadence instead of main-loop speed, so `node_count` never runs away.

**Result:** soak test sustained 5,800+ consecutive calls at ~135 msg/s with zero failures (vs. the
original 5/9/19-message failure pattern). On-chip counters confirmed: `error_seq = 0` (zero tree
errors of any kind), `reg_idx = 2` (both test methods registered), `dbg_req_seen == dbg_resp_sent`
for the whole run. The `arduino-router`/`go.bug.st/serial` theory (#8) is fully retracted — the
router was never the problem; this firmware was discarding its own correctly-received data.

All temporary trace instrumentation added during this investigation (`parse_trace`, `fill_trace`,
`error_trace` in `rpc.c`; the CRC16 TX tracing and per-message RX position tracing in `lpuart.c`;
the various one-off `dbg_*` counters) has been removed. The one addition that's a real, permanent
part of the fix is `rx_staged_events` (`lpuart.c`/`lpuart.h`) and the gating check in `rpc_poll()`.

**See also:** `MPACK_TOO_BIG_ROOT_CAUSE.md` for a short standalone summary of this root cause,
written for future reference without needing to read the full chronological log.
