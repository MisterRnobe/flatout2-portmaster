# macOS / Apple Silicon

This branch adds a native ARM64 macOS build of the PC recompilation. The game code is compiled to Mach-O; SDL2 handles input/audio/windowing and ANGLE runs the existing GLES renderer through Metal. CrossOver is only a possible source of the game files.

## Requirements

- Apple Silicon Mac and Xcode command line tools. The code targets macOS 13+, but installed dependencies must support your OS; this local build was validated on macOS 26.6.2 with SDL2-compat 2.32.74 built for macOS 26.
- Homebrew `llvm`, `sdl2`, `python`, plus Git and Make. The local video decoder is built from pinned FFmpeg source and does not depend on Homebrew FFmpeg.
- Python packages `capstone==5.0.7` and `pefile==2024.8.26`.
- ARM64 ANGLE `libEGL.dylib` and `libGLESv2.dylib`. Setup can copy these from an installed Visual Studio Code or Claude; alternatively pass `--angle-dir /path/to/angle`. These libraries are local build dependencies and are not committed.
- Your game installation, with `FlatOut2.exe` MD5 `1f8cae2b47276ca134750ba438aa35ca`, `d3dx9_30.dll`, `filesystem`, and the BFS archives it lists. Some current Steam installations have this compatible Zoom executable; the checksum is what matters.

## Build

```sh
brew install llvm sdl2 python
python3 -m venv work/venv
work/venv/bin/pip install capstone==5.0.7 pefile==2024.8.26
work/venv/bin/python scripts/setup_macos.py --game-dir "/path/to/FlatOut 2"
work/venv/bin/python scripts/build_video_macos.py
work/venv/bin/python scripts/lift_macos.py
work/venv/bin/python scripts/build_macos.py
./Play.command
```

Setup pins `pcrecomp` to `35548b8d0fd8aa853da67edcf7cd6134f6b9cdc7`, applies the NFSU2 recompiler patches from `manuelpllull/nfsu2-recompiled-r36s` commit `50a2fe4b8037719c8ce577fbb4a471d5f2084d6b`, then applies the local catalog-memory patch and `scripts/patches/pcrecomp-fo2.patch` (x87 flag comparisons and conditional moves required by FlatOut 2, plus correct LOOP/JECXZ transfers across recovered function boundaries). Its runtime headers come from that same pinned NFSU2 commit. All dependency checkouts, generated C, objects, libraries and executables are under ignored `work/`.

EXE and DLL analysis run concurrently. Analysis needs several passes and can take a while. Generated files are reused on subsequent builds. After changing the executable, lifter or `config/`, remove `work/gen/` before lifting again. `JOBS=4` limits compilation concurrency; the default is six compiler processes. `CC` and `CXX` can override the Homebrew LLVM compilers.

The macOS build reserves the guest address space at 1 TB through Mach VM, outside the low-address region reserved by macOS; the guest still uses its original 32-bit addresses. The executable is `work/macos/fo2`. `Play.command` selects the local ANGLE libraries and defaults to a 1280×800 window and 60 FPS. Set `ANGLE_DEFAULT_PLATFORM=gl` to use ANGLE’s OpenGL backend instead of Metal. Override `FO2_RENDER` (render size) and `NFSU2_SCREEN` (window size) as needed. F10 closes the game. Keyboard and SDL gamepads use the upstream mappings. Launch logs go to `work/macos/play.log`; the preceding log is kept as `work/macos/play.previous.log`.

`gamedata/` is a separate local copy, using APFS copy-on-write when available. Its `Savegame/` is writable and independent of the source installation. Setup never overwrites an existing imported file or save. Neither game files nor saves belong in Git.

## Validation

`work/venv/bin/python tests/test_catalog_memory.py` compares compact and full-operand discovery for branches, jump tables and callbacks. `work/venv/bin/python tests/test_lift_boundaries.py` executes generated C to verify conditional tail transfers, fallthroughs and guest stack cleanup across catalog boundaries. D3DX lifting uses `--bounded` to partition reachable blocks at registered function entries. Cold blocks beyond the catalog size estimate stay in their caller when they have no dispatch entry; explicit fallthroughs preserve control flow across gaps. The PC EXE retains the upstream lift. D3DX uses the global x87 stack and Clang `musttail` transfers, so loops across recovered entries do not grow the native stack; the game code keeps the analyzed local-window optimization. The compact cache applies only to catalog discovery; lifting retains complete operands.

```sh
./work/macos/fo2 "$PWD/gamedata" --check-runtime
```

This does not open a window or start the game. It checks the 4 GB guest address space, PE loading and imports, guest heap/thread/input/audio-cap/file-time/critical-section self-tests (tests for imports not used by the game are skipped), generated callbacks, the lifted D3DX DLL initialization and headless vertex/pixel shader compilation. It exits nonzero on failure. Interactive rendering and a full race need a logged-in macOS desktop.

Linux-only `/proc` signal sampling and the hang watchdog are disabled on macOS; normal frame timing/FPS reporting remains available. LAN uses macOS socket wrappers and adapter enumeration; interoperability needs a separate multiplayer test.

The macOS runtime check passed on the compatible Steam/Zoom executable, including D3DX DllMain, its import bindings and real vertex/pixel shader compilation. ARM64 Mach-O signing verification and an ANGLE software-OpenGL pbuffer pixel readback also passed. A desktop launch also created the D3D9 device through ANGLE Metal on an Apple M3 Pro. It then exposed an unresolved D3DX cold-block transfer at `0x00AE121F`; the bounded lift now preserves that block, with executable regression coverage for missing dispatch entries. Menu rendering and a full race still need desktop validation.

For a native crash, macOS prints a minimal `[FO2 FAULT]` record before the detailed reporter, with PC/LR and the executable image base for symbolization. Guest threads use alternate signal stacks so an exhausted normal stack can still produce the record. `./work/macos/fo2 gamedata --check-crash-handler` deliberately raises SIGILL to test this path and exits unsuccessfully; `python3 tests/test_macos_crash_report.py` verifies the expected failure and report. This is a diagnostic check, not a game launch.

`python3 tests/test_native_tail.py` checks a million transfers with constant native stack usage. `python3 tests/test_d3dx_tail_loop.py` extracts the six generated D3DX functions from the reported `0x00AD70F2`–`0x00AD7187` loop: the old transfer reproduces a stack-overflow signal on the long workload, while the guaranteed tail transfer completes 1,048,576 vector accumulations with checked numeric results and guest stack cleanup. These checks need local generated code, but no desktop or GPU.

A subsequent desktop launch passed effect compilation, texture creation and CoreAudio initialization, then exhausted its 64 MB native stack in the split D3DX vector loop. Guaranteed native tail transfers fix the reproduced overflow; the exact-loop replay and headless shader compiler checks pass. Interactive menu and race validation remain outstanding.

## Videos and networking TLS

`scripts/build_video_macos.py` builds FFmpeg n8.0.1 (`894da5ca7d742e4429ffb2af534fcda0103ef593`) with only the VP3 decoder enabled, copies the two LGPL libraries into `work/macos/lib`, fixes their loader paths and signs them. The runtime prefers this local pair over system FFmpeg; this avoids unrelated broken Homebrew codec dependencies. The launcher defaults `FO2_SKIP_LOGO=0` so the opening logo plays too. Set it to `1` to skip that logo.

`python3 tests/check_video_decode.py /path/to/video.avi` feeds up to 120 real AVI packets through the production guest-player decoder, checking changing nonblack BGR frames, output guards, frame queue and stack cleanup. Both `bg_main.avi` and `introinl.avi` from the user's game archive pass. Test samples remain under ignored `work/`.

The loader initializes the EXE's static PE TLS template and module index, with a separate block through TEB offset `0x2c` for each game/timer thread. This storage is distinct from dynamic Win32 TLS slots. Runtime checks exercise the original networking accessor at `0x005E9350` against two independent TEBs, the site that previously dereferenced guest address zero while searching internet games. Live server discovery and joining still require an interactive network test.
