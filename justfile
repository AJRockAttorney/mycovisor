elf := "build/Debug/UnoQBlinky.elf"
gdb := "arm-none-eabi-gdb"

# auto-ensure the gdb forward exists (idempotent)
ensure-forwards:
    @adb forward --list | grep -q "tcp:3333.*tcp:3333" || adb forward tcp:3333 tcp:3333

# auto-ensure openocd is running on the board, backgrounded
ensure-server:
    @adb shell 'pgrep -x openocd > /dev/null || (nohup arduino-debug > /tmp/openocd.log 2>&1 &)'
    @while ! nc -z localhost 3333 2>/dev/null; do sleep 0.1; done

# build via cmake (Debug preset)
build:
    cmake --build build/Debug

# the common case: build + flash + run free, one shot
flash: build ensure-forwards ensure-server
    {{gdb}} -nx --batch \
      -ex "target extended-remote localhost:3333" \
      -ex "monitor reset halt" \
      -ex "load" \
      -ex "monitor reset" \
      {{elf}}

# build + flash + drop into interactive debug at main
debug: build ensure-forwards ensure-server
    {{gdb}} \
      -ex "target extended-remote localhost:3333" \
      -ex "monitor reset halt" \
      -ex "load" \
      -ex "break main" \
      -ex "monitor reset halt" \
      -ex "continue" \
      {{elf}}

# server utilities, carried over unchanged
server:
    adb shell arduino-debug
server-log:
    adb shell tail -f /tmp/openocd.log
server-kill:
    adb shell pkill -x openocd

fresh:
    cmake --build build/Debug --clean-first