# stm32u5-mpack-rpc

## Overview

This project is a bare metal rpc over uart implementation for the STM32U5 series. It contains only the firmware meant to run on the STM32U5 Microcontroller. 
Chip configuration is done largely with STMCubeMX using the Low Level Drivers with one exception for the RX DMA. The STM32U5 uses linked list for GPDMA, and CubeMX only offers the configuration for this in the HAL drivers.  

## Why I built this

The Arduino Uno Q is an interesting product because it combines a STM32U585 MCU with a Qualcomm QRB2210 MPU on the same 
board. This architecture lends itself well to tasks that require both computationally "heavy" tasks (heavy data processing, edge AI, service hosting, etc.) 
with a need for timing-critical I/O (Interfacing with sensors, controlling motors, etc.). Additionally, it is quite affordable. 
When originally launched, the Uno Q priced in at \$44USD for the 2gb version, recently increased to \$59USD. 

I was initially interested in this board with the idea of using it for my projects in which the MPU side would be free to run
a webserver acting as a user interface for my project, using any framework of my choosing (Django or Flask for Python, Node.js or 
Bun for JavaScript or TypeScript) and leaving the mcu to handle any timing critical aspectsion of my project. 

As someone who is trying to learn more about embedded systems development, the Arduino IDE hides too much complexity. As a result, I wanted to write 
myself a library to interface with the RPC protocol that Arduino ships on the MPU side, but using only the STM32 Low Level 
drivers. This allows me the full flexibility (and complexity) to write my own C code for the microcontroller without having to 
reimplement the RPC stack on the MPU side as well. 



## Architecture

![router-bridge-architecture.svg](/Images/router-bridge-architecture.svg)

The Arduino Uno Q pairs its two processors over a private internal UART. On the
Linux (MPU) side, Arduino ships a background daemon, `arduino-router`, that owns
this link and acts as a broker. Local clients (Python, C++, etc.) connect to it
over a Unix domain socket, and it multiplexes their calls onto the single serial
line to the MCU using a MessagePack-RPC protocol. This firmware implements the
other end of that link. It runs entirely on the STM32U585 and speaks the
router's protocol directly, so the stock MPU-side stack is reused unchanged and
no custom Linux code is required. The link is reserved by the platform as
`/dev/ttyHS1` on Linux and `Serial1` on the MCU.

The firmware is built entirely on the STM32 Low Level (LL) drivers and is
organized as a small stack of layers, from the wire upward:

- **LPUART + GPDMA**, the hardware transport. Receive uses DMA into a buffer
  with an idle-line interrupt to mark end-of-burst; transmit is DMA-driven from
  an outbound buffer. Keeping the CPU off the byte-by-byte path leaves the
  application layer free for timing-critical work.
- **RX / TX ring buffers**, which decouple interrupt context from the main
  loop. The DMA/ISR side only moves bytes. All parsing and encoding happen
  later in thread context, so ISRs stay short and bursts get absorbed without
  dropping data.
- **MessagePack streaming parser**, which reassembles complete RPC messages
  from the byte stream. A UART has no inherent message boundaries, so this
  layer is the framing: it consumes bytes incrementally and emits a decoded
  message only once a full MessagePack value has arrived, regardless of how
  DMA chunked it.
- **RPC dispatch and registration**, which interprets each decoded message,
  routes requests to the correct handler, matches responses to pending calls,
  and runs the registration handshake that advertises the MCU's callable
  methods to the router.
- **Application handlers**, the user-provided procedures (e.g. `set_led`,
  `read_sensor`). These are the functions exposed over RPC, and the only layer
  a consuming project normally touches.

Data flows in both directions through the same stack. An inbound call climbs
it: bytes to ring buffer to parser to dispatch to handler. The handler's
return value descends it: encode to TX ring to DMA to UART, back to the
router, which returns the result to the calling client. The link is
symmetric, so the MCU can also originate calls to methods registered by
MPU-side services and emit notifications upward.

Wire-level message formats are described under [Wire Protocol](#wire-protocol);
connection loss, re-registration, and back-pressure are covered under
[Fault Handling](#fault-handling).

## Wire Protocol

### Framing

The link carries no framing beyond MessagePack's own self-describing encoding.
There are no length prefixes, delimiters, or sentinel bytes on top of the
protocol. A message's end is implicit in the MessagePack structure itself
(e.g. an array of the declared length is complete once all its elements have
arrived). That's why the streaming parser sits directly on top of the ring
buffer and acts as the framing layer: it consumes bytes incrementally and
emits a decoded message only once a complete MessagePack value has been
parsed.

### Message shapes

Every message is a MessagePack array. The first element is a small integer
tag identifying the message type:

| Type | Tag | Shape                              | Expects a response? |
|------|-----|-------------------------------------|----------------------|
| Request  | `0` | `[0, msgid, method, params]`    | Yes |
| Response | `1` | `[1, msgid, error, result]`      | No (is one) |
| Notify   | `2` | `[2, method, params]`            | No |

- `msgid` is a per-request identifier chosen by the caller. A response echoes
  the same `msgid` so the caller can match it to the original request.
- `method` is a string naming the remote procedure.
- `params` is an array of positional arguments.
- `error` in a response is `nil` on success, or a non-nil value describing
  the failure. `result` is `nil` on failure, or the return value on success.
- Notifications carry no `msgid` and receive no response. They're
  fire-and-forget.

### Registration

Method dispatch is not static. On connect, and after every reconnect, this
firmware advertises the procedures it handles by issuing a `$/register` call
for each registered method, one at a time, to the router. It's a call, not a
notification: it carries a `msgid` and is retried until it gets a response.
The router keeps this mapping in memory only, so a fresh connection always
starts from an empty table on the router's side, and registration is how the
MCU repopulates it. Calls to a method the MCU has registered are forwarded to
it by the router; the MCU can equally call methods that MPU-side clients have
registered, using the same request/response shapes in the other direction.

Registration is driven by `registration_tick()` in `Core/Src/rpc.c`: it
walks the method table one entry at a time, sends `$/register` with the
method name as the sole element of `params`, and only advances to the next
method once a response arrives (or retries after `REG_RETRY_MS` if the call
times out). While a registration call is outstanding, the onboard LED
(`GPIOH` pin 10) is lit as a visual "still registering" indicator.

### Encoding specifics

Wire-level parsing is implemented in `Core/Src/mpack.c` (vendored,
unmodified [mpack](https://github.com/ludocode/mpack)); this firmware's
side of the parsing/dispatch loop is in `Core/Src/rpc.c`. Encoding for
outbound messages is also in `Core/Src/rpc.c` (`encode_outgoing()`,
`send_response()`), using mpack's writer API.

- **Integers** are written with `mpack_write_uint()`/`mpack_write_int()`,
  which pick the smallest MessagePack representation that fits (fixint up
  through int64). There's no fixed-width convention imposed on top of the
  library's default.
- **Strings only.** Method names and any string parameters are written with
  `mpack_write_cstr()`/`mpack_write_str()` (MessagePack `str`). The `bin`
  family is never used; there's currently no binary payload path. Only
  `MPACK_READER`, `MPACK_WRITER`, and `MPACK_NODE` are compiled in
  (`Core/Inc/mpack-config.h`). `MPACK_EXPECT` and `MPACK_EXTENSIONS` are off,
  so extension types aren't available even on the wire.
- **Streaming in, fixed-size out.** Inbound messages are parsed
  incrementally off the wire by `mpack_tree_init_stream()` (`rx_tree` in
  `rpc.c`), bounded to 32 nodes per message
  (`RPC_MAX_PENDING`/`mpack_tree_init_stream` args). Outbound messages are
  built in one shot into a fixed 128-byte stack buffer (`RPC_TX_BUF`). There
  is no outbound streaming path, so a single encoded message (method name +
  params) is assumed to fit in 128 bytes.
- **No deviations from the MessagePack spec.** Messages are plain
  MessagePack arrays; the `[type, ...]` shapes in the table above are an
  application-level convention (matching `msgpack-rpc`), not a wire-format
  extension.

## Fault Handling

Fault state is persisted across a warm reset using the RTC backup registers
(`TAMP->BKP0R`/`BKP1R`). These survive a software reset or IWDG timeout but
are cleared on power loss, which is exactly the boundary needed: "why did we
just reset" should survive the reset that answers the question, but doesn't
need to survive unplugging the board. This is implemented in
`Core/Src/fault.c` / `Core/Inc/fault.h`.

- `fault_init()` runs once at boot, as early as possible (right after the
  `PWR` clock is enabled, before anything else that could fault). It reads
  and clears whatever the previous boot left behind, so
  `fault_get_last_reset_reason()` and `fault_was_watchdog_reset()` describe
  the prior run, not the current one. A magic value (`0x4641554C`, `'FAUL'`)
  distinguishes "we wrote this" from power-on-reset garbage/zero.
- `fault_record_and_reset(reason)` writes `reason` to the backup register and
  calls `NVIC_SystemReset()`; it does not return. If a debugger is attached
  (`CoreDebug->DHCSR`), it breaks (`__BKPT`) instead of resetting, so the
  attach-and-inspect workflow during development still works.
- `fault_was_watchdog_reset()` reports whether the hardware IWDG reset flag
  was set at boot. This is independent of the recorded reason and a useful
  cross-check against it: a bare watchdog timeout means the device hung badly
  enough that no fault handler ever ran to record why.

**Currently wired up.** Only two of the seven `fault_reason_t` values are
actually triggered anywhere. `FAULT_REASON_RPC_FAULT` comes from
`rpc_fault()` in `Core/Src/app.c`, called from `Core/Src/rpc.c` when the
mpack tree parser reports `mpack_error_memory` (a message can't fit in the
configured node budget) or when the built-in `ping` method fails to
register. `FAULT_REASON_ERROR_HANDLER` comes from the generic
CubeMX-generated `Error_Handler()` in `Core/Src/main.c`.
`FAULT_REASON_HARDFAULT`, `_MEMMANAGE`, `_BUSFAULT`, `_USAGEFAULT`, and
`_NMI` are defined but currently unused; see
[Known Issues](#known-issues--history).

**Connection loss / re-registration.** If the mpack tree parser hits any
error other than `mpack_error_memory` (for example, resync noise after a
dropped connection), `rpc_poll()` destroys and reinitializes the parse tree
(`Core/Src/rpc.c`) instead of resetting the MCU, so a bad byte sequence on
the wire is recoverable without a reboot. Registration re-runs whenever the
router side of the link comes back and the MCU's `$/register` calls start
timing out and retrying (`registration_tick()`, `REG_RETRY_MS` = 500ms). The
router's method table is memory-only, so it always needs repopulating after
it restarts.

**Back-pressure.** Outbound writes (`lpuart_write()` / `Core/Src/lpuart.c`)
go through a 256-byte TX ring buffer drained by DMA. If the ring buffer is
full, `rpc_send()` (`Core/Src/rpc.c`) spin-retries for up to
`RPC_TX_RETRY_MS` (20ms, several times the worst-case drain time of a full
ring buffer at LPUART's baud rate) before giving up. A call that never made
it onto the wire returns `0` from `rpc_call()` and never occupies a
pending-call slot, so the caller isn't left waiting on a response that will
never arrive. Inbound bytes are staged into a 512-byte ring buffer
(`rx_staging`) by the RX ISR. If that fills faster than the main loop drains
it, the write is dropped and counted in `rx_overflow`
(`Core/Src/lpuart.c`) instead of blocking the ISR.

## Build & Flash

Requires CMake ≥ 3.22, Ninja, and the `arm-none-eabi-gcc` toolchain
(CI pins `13.2.Rel1`) on `PATH`.

```sh
cmake --preset Debug      # or Release
cmake --build --preset Debug
```

The `Debug`/`Release` presets (`CMakePresets.json`) both go through
`cmake/gcc-arm-none-eabi.cmake`, which targets `cortex-m33` with a hard-float
FPU ABI and links against `STM32U585xx_FLASH.ld`. Debug builds with `-O0
-g3`; Release with `-Os -g0`. Compiler warnings are treated as errors
(`-Wall -Wextra -Werror`).

Flashing targets an Uno Q board over `adb`. `openocd` runs on-device
(`arduino-debug`), its GDB port is forwarded back to the host, and
`arm-none-eabi-gdb` drives the load. The `justfile` wraps this:

```sh
just flash   # build (Debug), forward the openocd port over adb, load, reset, run
just debug   # same, but stop at main() under an interactive gdb session
```


## Testing

`tests/test_ring_buff.c` is a native, host-side unit test suite for the ring
buffer (`Core/Src/ring_buff.c`). No ARM toolchain or hardware needed. It
covers init, basic round-trips, empty/zero-length reads and writes, capacity
boundaries, rejecting writes that would overflow, wraparound reads/writes
that straddle the buffer end, and `rb_get_linear_block()`/`rb_skip()`
behavior in both wrapped and unwrapped states.

```sh
just test
# or directly:
cc -std=c11 -Wall -Wextra -ICore/Inc -o build/test_ring_buff tests/test_ring_buff.c Core/Src/ring_buff.c
./build/test_ring_buff
```

There is currently no automated test coverage for the RPC layer (`rpc.c`),
the mpack streaming integration, or the LPUART/DMA transport. Those have so
far been validated by manual soak testing against the real router (see
[Known Issues / History](#known-issues--history)).

## CI

`.github/workflows/ci.yml` runs three jobs on every push and pull request:

- **Ring buffer unit tests (native)** builds and runs
  `tests/test_ring_buff.c` with the host's `cc`, `-Wall -Wextra -Werror`.
- **Firmware build (STM32U5, arm-none-eabi)** installs `arm-none-eabi-gcc`
  13.2.Rel1 and Ninja, then configures and builds the `Debug` CMake preset:
  a full cross-compile of the firmware.
- **cppcheck (Core/Src)** runs `cppcheck` (`warning,performance,portability`,
  `--error-exitcode=1`) over `app.c`, `lpuart.c`, `ring_buff.c`, `rpc.c`, and
  `fault.c`.

None of the jobs flash real hardware. CI validates that the code builds and
that the host-testable pieces (currently just the ring buffer) behave
correctly.

## Known Issues / History

- **Soak-test RPC timeouts (fixed).** An early version of `rpc_poll()`
  called `mpack_tree_try_parse()` unconditionally on every main-loop
  iteration. The main loop spins many orders of magnitude faster than bytes
  arrive at 115200 baud, so a single in-flight message could get re-parsed
  thousands of times before it fully arrived. An internal mpack accounting
  counter (`node_count`) incremented on each of those attempts, including
  ones that immediately bailed out because the message wasn't fully received
  yet, and it was never rolled back. That silently exhausted the configured
  node budget (`max_nodes = 32`) in microseconds, long before any real
  message would have. The fix, now in place, is `rx_staged_events`: a
  counter bumped once per RX DMA staging event in ISR context, so
  `rpc_poll()` only attempts a parse when new bytes have actually arrived.
  Confirmed with a soak test sustaining 5,800+ consecutive RPC calls at
  ~135 msg/s with zero failures.
- **Cortex-M fault handlers are still stock CubeMX stubs.**
  `HardFault_Handler`, `MemManage_Handler`, `BusFault_Handler`, and
  `UsageFault_Handler` (`Core/Src/stm32u5xx_it.c`) each just spin in an
  infinite `while (1)`. None of them call `fault_record_and_reset()`, even
  though `fault_reason_t` already has dedicated values for each of them
  (`FAULT_REASON_HARDFAULT`, etc.), and `NMI_Handler` is in the same state.
  In practice a genuine CPU fault currently hangs the board silently instead
  of resetting with a recorded reason.
- **IWDG is not enabled.** `fault_was_watchdog_reset()` and the backup-domain
  logic in `fault.c` are already written to detect and report an IWDG-caused
  reset, but `HAL_IWDG_MODULE_ENABLED` is commented out
  (`Core/Inc/stm32u5xx_hal_conf.h`) and nothing in the codebase initializes
  or feeds an IWDG instance. Combined with the point above, a hang in one of
  the stock fault handlers currently has no hardware backstop to reset the
  board.

## TODO / Open Questions

- Wire up the Cortex-M fault handlers (`HardFault_Handler` and friends) to
  `fault_record_and_reset()` so a real CPU fault reboots with a recorded
  reason instead of hanging in the stub's `while (1)`.
- Enable and feed an IWDG instance so a hang anywhere (including inside an
  unhandled fault) is recoverable without a manual power cycle.
- Add automated test coverage for `rpc.c` and the mpack streaming
  integration. Currently only `ring_buff.c` has unit tests; the RPC/parser
  layer has only been exercised via manual soak testing.
- `RPC_MAX_METHODS` is 8 and `RPC_METHOD_NAME_MAX` is 16 (`Core/Src/rpc.c`).
  That's fine for the current `ping`-only built-in surface, but worth
  revisiting once real application handlers are added.
