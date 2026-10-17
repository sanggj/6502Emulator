# 6502 IDE

A Windows desktop IDE, emulator, debugger and assembler for the MOS 6502 and
WDC 65C02 CPUs. C++17, Win32 GUI, headless CLI with JSON output.

## Requirements

- Windows 10+
- clang++ 22 (MSYS2 `ucrt64` toolchain) on PATH
- `windres` (MSYS2 `mingw-w64-ucrt-x86_64-binutils`) for the app resource

## Build

```
build-clang.bat
```

Produces `build\6502_ide.exe` (GUI + headless CLI, `-Wall -Wextra -Werror`),
`build\6502_cli.exe` (console-only CLI, same flags, `src\main_cli.cpp`), then
`build\test_core.exe` (also `-Werror`) and runs the test suite. The script
exits non-zero if compilation or any test fails.

## Tests

`build\test_core.exe` runs four suites (1328 checks): CPU semantics (including
exhaustive 256x256x2 ADC/SBC sweeps with an independent reference model),
assembler encodings (full 151-opcode chart matrix + branch-offset sweeps),
platform/remote/CLI integration, and core integration (savestates, iNES
parsing, APU, memory). Golden fixtures under `tests\data\`:

- `6502_functional_test.bin` - Klaus Dormann functional test (MD5
  `9905EF74087A2FFB67B9D632D0ACD6B1`, success trap `$3469` at ~30.6M
  instructions).
- `nestest.nes` + `nestest.log` - kevtris nestest: NROM load checks plus a
  full trace comparison (PC/registers/flags/cycles per instruction) against
  the golden log's 5003 official-opcode lines, automated entry at `$C000`.

If a fixture is absent the suite prints a loud `SKIP:` line instead of
failing.

Sanitizer runs: MSYS2 `ucrt64` clang ships no ASan/UBSan runtimes; the suite
was therefore built and run with the `clang64` toolchain
(`pacman -S mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-libc++`,
`-fsanitize=address` and `-fsanitize=undefined -fno-sanitize-recover=undefined`,
link `-Wl,--stack,8388608`): **all 1328 checks pass under both, zero reports**.

## Headless CLI

```
build\6502_ide.exe --headless --asm prog.asm --run --instr 100000
```

Flags: `--headless`, `--asm FILE`, `--link FILES`, `--image FILE`,
`--base ADDR`, `--run`, `--instr N`, `--cycles N`, `--core 6502|65c02`,
`-D sym[=val]`, `--screenshot FILE`, `--waverec FILE`, `--remote PORT`,
`--map`, `--xref`, `--prg FILE`, `--nes FILE`, `--mesen-trace FILE`.

A JSON result line is printed (all strings escaped); exit code 0 on success,
2 on usage/load/assembly errors. `--instr`/`--cycles` bound execution
(`--cycles` overshoots by at most one instruction). Output is deterministic
for identical inputs. Invalid flags, missing flag values and malformed
numbers are rejected.

## Remote server

The remote debug server binds `127.0.0.1` ONLY (never an external interface),
is disabled by default, and has NO authentication: any local process can
control the emulator when enabled (`--remote PORT` or the ini setting). Client
buffers are capped at 64 KB and connections are dropped on malformed input.

## Save states

Current format v3; v1 (payload only) and v2 (shorter trailer) files still
load. CPU registers, RAM, APU state, interrupt lines/edges, core variant,
instruction counters and the NMOS poll-suppression/WAI flags are persisted.
Corrupt, truncated or oversized files are rejected before any emulator state
is mutated. Files whose APU blob does not match the current layout load with
the APU left untouched (external IRQ state defaults to false).

## iNES support

Mapper 0 (NROM) only, PRG up to 32 KB, trainer bit handled. Mapper 1/2/3 and
larger images are rejected with explicit errors (no bank switching
implemented). **There is no PPU**: `tests`, rendering, OAM DMA and controller
reads are not emulated, so games load but cannot display video. The memory
model is a flat 64 KB (no NES RAM mirroring). This is a 6502 development
environment, not a full NES emulator.

## APU

Frame sequencer and frame IRQ are implemented (`$4015`/`$4017`, 4-step and
5-step modes); audio backends are optional - the sequencer advances even
without an audio device. Channel synthesis exists but is not
cycle-exact-verified against hardware.

## Known limitations / unverified

- Win32 GUI paths (windows, accelerators, dialogs) are compile-verified
  (`-Werror`) plus a manual smoke test (launch, main-window title, clean
  close), but not interaction-tested by automation.
- No CI configuration.
- NMOS interrupt-deferred-poll and 65C02 decimal/timing behavior follow
  published documentation, not silicon-level (visual6502) verification.

## Performance

Reference: `examples\plasma.asm`, `--run --instr 5000000`, GUI-less headless
build, median of 8 runs ≈ 4.06 s (~1.25 M instructions/s), identical cycle
count across runs (18,333,342 with the 7-cycle reset accounting).
