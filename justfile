# thin dispatcher: real recipes live in firmware/justfile. Exists so `just
# <recipe>` still works from the repo root now that firmware/ is a subdir
# (`just` only searches the cwd + ancestors for a justfile, not subdirs).
firmware_just := "just --justfile firmware/justfile --working-directory firmware"

build:
    {{firmware_just}} build

test:
    {{firmware_just}} test

fresh:
    {{firmware_just}} fresh

flash:
    {{firmware_just}} flash

debug:
    {{firmware_just}} debug

flash-remote:
    {{firmware_just}} flash-remote

debug-remote:
    {{firmware_just}} debug-remote

ensure-forwards:
    {{firmware_just}} ensure-forwards

ensure-server:
    {{firmware_just}} ensure-server
