# macOS / Apple Silicon

This branch adds a native ARM64 macOS build of the PC recompilation. The game code is compiled to Mach-O; SDL2 handles input/audio/windowing and ANGLE runs the existing GLES renderer through Metal. CrossOver is only a possible source of the game files.

## Requirements

- Apple Silicon Mac and Xcode command line tools. The code targets macOS 13+, but installed dependencies must support your OS; this local build was validated on macOS 26.6.2 with SDL2-compat 2.32.74 built for macOS 26.
- Homebrew `llvm`, `sdl2`, `python` (and optionally `ffmpeg` for native VP3 menu videos).
- Python packages `capstone==5.0.7` and `pefile==2024.8.26`.
- ARM64 ANGLE `libEGL.dylib` and `libGLESv2.dylib`. Setup can copy these from an installed Visual Studio Code or Claude; alternatively pass `--angle-dir /path/to/angle`. These libraries are local build dependencies and are not committed.
- Your game installation, with `FlatOut2.exe` MD5 `1f8cae2b47276ca134750ba438aa35ca`, `d3dx9_30.dll`, `filesystem`, and the BFS archives it lists. Some current Steam installations have this compatible Zoom executable; the checksum is what matters.

## Build

```sh
brew install llvm sdl2 python ffmpeg
python3 -m venv work/venv
work/venv/bin/pip install capstone==5.0.7 pefile==2024.8.26
work/venv/bin/python scripts/setup_macos.py --game-dir "/path/to/FlatOut 2"
work/venv/bin/python scripts/lift_macos.py
work/venv/bin/python scripts/build_macos.py
./Play.command
```

Setup pins `pcrecomp` to `35548b8d0fd8aa853da67edcf7cd6134f6b9cdc7`, applies the NFSU2 recompiler patches from `manuelpllull/nfsu2-recompiled-r36s` commit `50a2fe4b8037719c8ce577fbb4a471d5f2084d6b`, then applies the local catalog-memory patch and `scripts/patches/pcrecomp-fo2.patch` (x87 flag comparisons and conditional moves required by FlatOut 2, plus correct LOOP/JECXZ transfers across recovered function boundaries). Its runtime headers come from that same pinned NFSU2 commit. All dependency checkouts, generated C, objects, libraries and executables are under ignored `work/`.

EXE and DLL analysis run concurrently. Analysis needs several passes and can take a while. Generated files are reused on subsequent builds. After changing the executable, lifter or `config/`, remove `work/gen/` before lifting again. `JOBS=4` limits compilation concurrency; the default is six compiler processes. `CC` and `CXX` can override the Homebrew LLVM compilers.

The macOS build reserves the guest address space at 1 TB through Mach VM, outside the low-address region reserved by macOS; the guest still uses its original 32-bit addresses. The executable is `work/macos/fo2`. `Play.command` selects the local ANGLE libraries and defaults to a 1280×800 window and 60 FPS. Set `ANGLE_DEFAULT_PLATFORM=gl` to use ANGLE’s OpenGL backend instead of Metal. Override `FO2_RENDER` (render size) and `NFSU2_SCREEN` (window size) as needed. F10 closes the game. Keyboard and SDL gamepads use the upstream mappings. Launch logs go to `work/macos/play.log`.

`gamedata/` is a separate local copy, using APFS copy-on-write when available. Its `Savegame/` is writable and independent of the source installation. Setup never overwrites an existing imported file or save. Neither game files nor saves belong in Git.

## Validation

`work/venv/bin/python tests/test_catalog_memory.py` compares compact and full-operand discovery for branches, jump tables and callbacks. `work/venv/bin/python tests/test_lift_boundaries.py` executes generated C to verify conditional tail transfers, fallthroughs and guest stack cleanup across catalog boundaries. D3DX lifting uses `--bounded` to avoid copying blocks beyond a function’s catalog extent; the PC EXE retains the upstream lift. D3DX uses the global x87 stack; the game code keeps the analyzed local-window optimization. The compact cache applies only to catalog discovery; lifting retains complete operands.

```sh
./work/macos/fo2 "$PWD/gamedata" --check-runtime
```

This does not open a window or start the game. It checks the 4 GB guest address space, PE loading and imports, guest heap/thread/input/audio-cap/file-time/critical-section self-tests (tests for imports not used by the game are skipped), generated callbacks and the lifted D3DX DLL initialization. It exits nonzero on failure. Interactive rendering and a full race need a logged-in macOS desktop.

Linux-only `/proc` signal sampling and the hang watchdog are disabled on macOS; normal frame timing/FPS reporting remains available. LAN uses macOS socket wrappers and adapter enumeration; interoperability needs a separate multiplayer test.

The macOS runtime check passed on the compatible Steam/Zoom executable, including D3DX DllMain and its import bindings. ARM64 Mach-O signing verification and an ANGLE software-OpenGL pbuffer pixel readback also passed. Interactive launch was blocked by this build environment’s LaunchServices/display access, so Metal rendering and a full race still need desktop validation.
