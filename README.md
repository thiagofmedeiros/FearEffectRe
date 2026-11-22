# FearEffectRe

A static recompilation of *Fear Effect* (PlayStation, USA, SLUS-00920/01056/01057/01058).
The game's MIPS code is translated to C++ and compiled for the host, and runs against a
small PlayStation runtime instead of an emulated CPU.

You need your own discs. Nothing from the game is stored here: the C++ is generated from
your disc image into the build directory when you build.

This project is not affiliated with or endorsed by the owners of *Fear Effect*.

## Status

The recompiled game is playable from boot into gameplay: logo and intro movies, title
screen, main menu, the load screen, the opening cinematic and controllable gameplay with
3D characters over the streamed backgrounds, HUD and pause menu. A scripted run of 50,000
frames (about 14 minutes of play on disc 1) finishes without a crash or an unimplemented
call.

| Part | State |
|---|---|
| Disc images (BIN/CUE, ISO), ISO9660 | Working |
| MIPS to C++ recompiler | Working: 1268 functions, 89,373 instructions; all four discs share one executable |
| Interpreter (fallback and test oracle) | Working |
| BIOS (A0/B0/C0 calls, events, interrupt dispatch) | The calls the game uses |
| Interrupts, DMA, root counters | Working; root counter interrupts are not implemented (the game has not asked for them) |
| CD-ROM controller | Data, streaming and XA audio sectors; disc change |
| MDEC (movies and backgrounds) | Working |
| GPU | Software renderer, 1x to 16x internal resolution, multithreaded; no dithering, no texture filtering, edge rules not pixel-exact |
| Geometry engine (GTE) | All commands; not verified against hardware test vectors |
| SPU | 24 voices, envelopes, XA audio; no reverb, noise or pitch modulation |
| Controller | Digital pad in port 1 from the keyboard or an Xbox controller |
| Memory card | Slot 1 as files in `saves/`; listing works, saving has not been exercised yet |
| Overlays loaded from the disc | Run in the interpreter; not recompiled yet |

Not verified yet: sound by ear, saving and loading a game, the disc change prompts, and
anything past the first area. Changing disc mid-game does not crash, but the game has not
been played to a real disc change.

## Building

Requires CMake 3.25+, a C++20 compiler, Ninja and Python 3.

```sh
cmake -S . -B build -G Ninja -DFE_DISC_IMAGE="D:/path/Fear Effect (USA) (Disc 1).cue"
cmake --build build
```

Without `FE_DISC_IMAGE` only the interpreter is built. With MinGW, keep the compiler's
`bin` directory on `PATH` when running the programs.

## Running

Start `build/FearEffectRe.exe` without arguments (double-click works) for the launcher:
choose the disc 1 image, a resolution (Original 640x480, 720p, 1080p, 1440p or 4K) and
fullscreen, then press Play. The choices are remembered in `FearEffectRe.ini` next to the
program. The game is drawn at that resolution: polygons, sprites and text are rasterised at a
multiple of the console's 240 lines that fills the output (3x for 720p, 5x for 1080p, 6x
for 1440p, 9x for 4K; `--scale N` overrides it), and shown as the largest 4:3 picture that
fits. Movies and the streamed backgrounds are 320x160 video on the disc, so they are
enlarged, not sharper. A size as large as
the screen is shown borderless. With more than one monitor connected the launcher also
asks which display to use (`--display N` on the command line, 1 is the primary).

```sh
build/FearEffectRe "D:/path/Fear Effect (USA) (Disc 1).cue" --resolution 1080p [--fullscreen] [--display 2]
```

starts the game directly. Keys: arrows or WASD move, E/Enter is cross, Q/Esc circle,
F square, Space triangle, Tab L1, R R1, 1 L2, 4 R2, P start, Backspace select. F1
fast-forwards, F2 to F5 insert disc 1 to 4. An Xbox controller (XInput) works too: A is
cross, B circle, X square, Y triangle, LB/RB are L1/R1, LT/RT are L2/R2, Start and Back are
start and select, and the d-pad or left stick moves. F2 to F5 insert disc 1 to 4 (the other discs are found next to the first by
their `(Disc N)` name). Saves are kept in `saves/`; use `--saves dir` to choose another
folder or `--saves none` to play without a memory card.

For testing without a window:

```sh
build/FearEffectRe disc.cue --frames 3000 --input "500:start,1300:start,2000:up*300" --shots out --every 250
```

`--frames N` runs headless for N frames and prints a report: where it stopped, the game's
console output, calls and devices that are not implemented, and the busiest hardware
registers. `--input` presses buttons at given frames (`*n` holds for n frames), `--shots`
writes a PNG every `--every` frames, `--dump` and `--dump-vram` write the last picture and
video memory, `--wav` writes the sound, `--swap frame:disc` changes disc. `--interpreter`
runs without the recompiled code; `--log-bios`, `--log-cd N` and `--log-sio N` trace calls
and register accesses.

## Tests

```sh
ctest --test-dir build
```

`fe_difftest` runs every recompiled function and the interpreter from the same
pseudo-random registers and memory. Calls and hardware accesses are recorded instead of
executed. Both sides must record the same sequence and finish with the same registers and
memory. Cases that do not finish within the cycle budget are reported as inconclusive.

## Layout

- `tools/psxdisc.py` lists and extracts files from a disc image.
- `tools/recomp/` is the recompiler: `mips.py` decodes instructions, `analyze.py` finds
  functions and jump tables, `emit.py` writes the C++.
- `src/core/disc` reads disc images. `src/core/psx` is the machine: memory, interpreter,
  BIOS, CD-ROM, GPU, GTE, MDEC, SPU and the other hardware. `src/core/recomp/Runtime.h` is what generated code
  is written against.
- `src/app` is the program: the headless runner and the Win32 window. `tests` holds the
  differential test.

## How the recompiled code runs

Each guest function is a C++ function taking the CPU state. Branches inside a function
are gotos, delay slots are folded into their branch, direct calls are C++ calls, and
`jalr`, BIOS vectors and unresolved jumps go through a dispatcher that falls back to the
interpreter for code that was not recompiled. Cycles are charged per straight-line block;
when a time slice runs out the machine advances hardware and delivers interrupts. An
interrupt saves the CPU state, runs the game's handler to `ReturnFromException`, and
restores it.

A function normally returns to the address its caller put in `ra`. The game's movie
bitstream decoder is hand-written and uses `ra` as a continuation; when a call comes back
at a different address the interpreter carries on from there until execution reaches the
call site's return address.

Known simplifications: load delay slots and arithmetic overflow traps are not modelled
(the executable was scanned and does not depend on load delays), every instruction costs
two cycles, and `longjmp` is not supported.
