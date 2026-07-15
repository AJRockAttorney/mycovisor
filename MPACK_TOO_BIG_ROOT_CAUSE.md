# Root cause: soak-test RPC timeouts

Short reference summary. Full chronological investigation (including the retracted
`arduino-router` theory) is in `SOAK_DEBUG_LOG.md`.

## The bug

`rpc_poll()` (`Core/Src/rpc.c`) called `mpack_tree_try_parse(&rx_tree)` on every main-loop
iteration, unconditionally.

`mpack`'s tree parser (vendored, unmodified `Core/Src/mpack.c`) is designed to be resumable:
if an array/map's children haven't fully arrived yet, `mpack_tree_try_parse()` returns `false`
and picks back up on the same node next call. That resumability is correct. The problem is a
side effect of it: `mpack_tree_parse_children()` increments `tree->node_count` by the
container's child count on *every* call — including calls that immediately fail their own
`mpack_tree_reserve_bytes()` check because the rest of the message hasn't arrived over UART
yet. That increment is never rolled back.

Our main loop spins many orders of magnitude faster than bytes arrive at 115200 baud
(~87µs/byte). So while a single ~33-byte response was still trickling in, the same
not-yet-satisfied array node got re-entered (and `node_count` re-bumped) thousands of times,
blowing past `max_nodes = 32` (set in `rpc_init()`'s `mpack_tree_init_stream(...)` call) in
microseconds — long before the message was even half received.

Once `mpack_error_too_big` fires, `rpc_poll()`'s (correct, pre-existing) error handling —
`mpack_tree_destroy(&rx_tree); rpc_init();` — throws away the tree, including whatever prefix
bytes (critically, the array header bytes) had already been consumed out of `rx_staging` into
the now-freed buffer. The freshly reinitialized tree then reparses whatever's left of that
*same* message, still sitting in `rx_staging`, as brand-new headerless top-level scalar
values. That's what every downstream symptom traced back to: dispatch getting called
repeatedly with wrong-shaped "messages," registration never completing, calls timing out.

## Why it looked like a transport bug for so long

Every previous hypothesis (TX ring buffer corruption, RX hardware errors, a short write in
`arduino-router`'s `go.bug.st/serial` dependency) was a reasonable read of the symptoms and
each was individually falsified with hard evidence — see `SOAK_DEBUG_LOG.md` items 1–9. The
actual bug lived one layer further in: not in what bytes arrived, but in how many times our own
polling loop re-examined them before they'd fully arrived.

GDB breakpoint-based debugging made the bug disappear, which delayed finding it: halting the
CPU doesn't halt DMA, so bytes kept accumulating in the background during each halt. By the
time single-stepped execution resumed, whole messages tended to already be fully buffered,
skipping the multi-retry window entirely.

## How it was confirmed

Added temporary instrumentation directly in `rpc.c` (never touching `mpack.c`/`mpack.h`) to
snapshot `tree->error` / `tree->node_count` / `tree->max_nodes` at the moment `rpc_poll()`
detects a non-`mpack_ok` error. Every recorded event showed the same signature:

```
error = mpack_error_too_big
node_count = 33
max_nodes  = 32
size_at_error = 0   // i.e. no real content had been committed yet
```

## The fix

Added `rx_staged_events` (`Core/Src/lpuart.c` / `Core/Inc/lpuart.h`): a monotonic counter
bumped once per RX staging event, inside `rx_stage_handler()` (ISR context). `rpc_poll()` now
only calls `mpack_tree_try_parse()` when that counter has changed since the last poll:

```c
uint32_t staged_now = rx_staged_events;
if (staged_now != last_staged_seen) {
    last_staged_seen = staged_now;
    if (mpack_tree_try_parse(&rx_tree)) {
        dispatch(mpack_tree_root(&rx_tree));
    }
}
```

This bounds retries to real UART arrival cadence instead of main-loop speed, so `node_count`
never has a chance to run away. No changes were made to `mpack.c`/`mpack.h`.

## Result

Soak test sustained 5,800+ consecutive RPC calls at ~135 msg/s with zero failures (previously:
5/9/19-message failure pattern). On-chip counters after the run: zero tree errors, both test
methods (`ping`, `getcount`) registered, request/response counts matched exactly for the whole
run.
