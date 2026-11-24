# iOS port handoff

This is the current macOS Metal source checkpoint, prepared for a separate iOS agent. It is **not an iOS application yet**: there is no iOS Xcode target or signing configuration. Develop the iOS target on `ios-port`; keep shared fixes reviewable against `macos-metal` so later macOS commits can be rebased or cherry-picked.

## Starting point

- Fork: `https://github.com/MisterRnobe/flatout2-portmaster`.
- `macos-metal`: current macOS implementation, including native Metal, SSR, hardware RT prototypes, Darwin networking and packaging tools.
- `ios-port`: starts at that checkpoint; intended for iOS-only adaptation.
- The original macOS working directory is a separate project. Do not change its files or its `work/` tree from the iOS task.
- Read `MACOS.md` for detailed architecture, validation history and launcher behavior.

The local prepared copy additionally contains ignored, private inputs: `gamedata/`, generated `work/gen/`, lift catalogs, patched lift dependencies and generated headers. These are independent file copies, not symlinks to the macOS project. Never add game archives, EXEs/DLLs, generated recompilation output, saves, device backups or diagnostic captures to GitHub. A GitHub-only clone needs its owner's game data and regeneration of the ignored inputs; the local prepared copy avoids that initial regeneration.

## What already works on macOS

The x86 game and D3DX are statically lifted to C and compiled for ARM64. The guest-facing Win32/D3D9/DirectInput/DirectSound ABI remains a compatibility interface. Native rendering uses Metal directly; the Metal backend does not require ANGLE/GLES. SDL supplies the window/input/audio host integration. OpenSpy browsing/joining was tested in gameplay; Darwin socket layouts and socket polling have explicit adaptations.

Relevant code:

- `runtime/host/main.cpp`: current desktop application lifecycle and input.
- `runtime/host/macos_compat.h`: desktop compatibility definitions; audit rather than copying its assumptions into iOS.
- `runtime/guest/guest_runtime.cpp`, `guest_memory.h`, `arm64_guest.h`, `guest_threads.inc`: guest address space, CPU state, callbacks and scheduling.
- `runtime/guest/d3d9_bridge.cpp`, `d3d9_render_thread.inc`: guest COM bridge, owned command payloads and render thread.
- `runtime/d3d9_metal/`: native renderer, translated shader integration, SSR and hardware RT.
- `runtime/render/`: renderer-independent owned commands, materials and geometry collectors.
- `runtime/native/fo2_video.c`: optional FFmpeg decoder with desktop dynamic-library discovery that must be adapted or disabled initially on iOS.
- `scripts/build_macos.py`: current complete source/define/library inventory. **It is a macOS build script**, not an iOS cross-compiler.

## First iOS milestones

1. Add a real iOS application target with an iPhoneOS SDK/device ARM64 build and an appropriate simulator target. Build UIKit/SDL host integration and a CAMetalLayer; package compatible SDL and any optional codec libraries for iOS. macOS dylibs and existing macOS object files cannot be reused for an iOS target.
2. Prove guest memory reservation and the headless runtime self-tests on the target device before attempting gameplay. The current guest has a 4 GiB virtual space, with a compile-time fixed native base (`0x10000000000` normally, a different base in macOS ASan builds). `GuestMemory` uses fixed Mach VM reservation on Apple platforms. Audit iOS address-space availability, device memory limits and any required entitlements. A changed base must agree in every guest and host translation unit. Do not silently replace existing mappings or merely shrink the guest address space.
3. Bring up the existing Metal rendering/resource fixtures, then a menu, then a single race. Start with SSR and hardware RT disabled, with an explicit in-app toggle later. Query hardware capabilities on the actual device; an ARM64 CPU does not imply hardware ray tracing.
4. Move writable game state and logs into the app sandbox; use bundled/imported game assets read-only. Provide a reproducible import path for user-owned assets and preserve saves across upgrades. Audit all desktop paths, environment-based options and dlopen behavior.
5. Add touch/controller input, app foreground/background handling, audio-session handling, orientation/resize behavior and an iOS-visible way to change graphics options. F7/F8 desktop shortcuts are not an iPhone UI.
6. Verify internet/LAN networking and the required iOS privacy declarations on-device. Keep the existing explicit Winsock/Darwin address conversions and poll semantics.

This implementation does not request native executable permissions for guest x86 pages: `GuestMemory::protect` treats x86 execute flags as guest metadata. Generated x86 callbacks use the runtime translation/interpreter path. Preserve that distinction and audit the complete runtime rather than introducing a JIT dependency by default.

## Rendering state and pending acceptance

SSR works in gameplay. Hardware RT currently has incomplete geometry/material coverage and uses SSR fallback. One inspected car-body shader pair can shade actual hit materials, including offscreen hits; this passed GPU fixtures and desktop gameplay on 2026-11-23. It is not a full physically based renderer, and water refraction, complete driver geometry and reflected-view lighting remain unfinished.

The **latest** addition is a bounded ray-only population of car scene-root meshes omitted by the primary camera. It was built and passed CPU/ASan/UBSan and real lifted mesh/batch ABI tests, but **its new desktop GPU/gameplay acceptance is still pending**. See `runtime/guest/offcamera_car_scene.h`, `offcamera_car_runtime.inc`, and the last section of `MACOS.md`. Do not treat the previous offscreen-material acceptance as acceptance of this population change. For an initial iOS baseline keep `FO2_RT_WATER=0`; `FO2_RT_CAR_SCENE=0` independently disables the new population path when investigating RT.

The population hooks use five verified EXE entry addresses in three generated EXE translation units. `scripts/build_macos.py` marks those units with `FO2_RT_SCENE_OBSERVER=1`; preserve the equivalent build defines or intentionally disable the population feature on iOS. Preserve per-command ray-only provenance and scoped cleanup so hidden geometry cannot suppress the HUD or visible draws.

Subsequent macOS work also includes independent loose/released part roots from the car's part inventory, with owner/parent validation and a distinct ray-only provenance value (2). This addition likewise awaits gameplay/GPU acceptance; do not collapse the source-kind value to a boolean when porting command transport. See the final section of `MACOS.md`. The prepared iOS branch starts at its published checkpoint; later shared refinements arrive by rebasing onto `macos-metal`.

The Nov 24 user run subsequently exercised detached geometry and passed the GPU material fixture, but did not demonstrate detached reflections in gameplay. Later shared changes prioritize part material capture under the existing budget and use a direct primitive acceleration structure for synchronous water RT (no identity-only TLAS); those changes await a new GPU/gameplay run. Preserve the primitive-AS shader/host pairing when merging. SSR-only iOS bring-up remains independent of hardware RT support.

Later RT material shading adds per-material GPU hit worklists (histogram/prefix/scatter plus indirect dispatch) within the same RT command buffer. Preserve the shader buffer slots 8–10 and host worklist allocation/ordering together; the default can be disabled with `FO2_RT_CAR_WORKLIST=0`. Its exact GPU comparison fixture and new gameplay acceptance are pending. This optional RT-only optimization is not needed for initial SSR-only iOS bring-up. See the latest `MACOS.md` section for memory budgets and diagnostics.

The next macOS user run passed the exact worklist GPU comparison. A subsequent capture change protects nearby visible body materials against offcamera population starvation, with proximity diagnostics in effects slot 60 and RT counter 10 (11 total counters). Merge the material source and counter allocation together. This newer nearby-material policy still awaits GPU/gameplay acceptance and does not identify the local player explicitly.

## Useful baseline checks

For a local macOS comparison, the private inputs allow a full rebuild without lifting again:

```sh
python3 scripts/build_macos.py --renderer metal
./work/metal/fo2-metal gamedata --check-runtime
./work/metal/fo2-metal --metal-check-memory
```

The guest runtime check includes actual lifted mesh enqueue, hidden flags, stack/register restoration and ray-only scope cleanup. Other useful fixtures are `--metal-check`, `--metal-check-raytracing`, and `--metal-check-raytracing-water`; GPU unavailability exits 77 and is not a GPU pass. CPU shader corpus checks need a local `work/metal/shaders/` capture, which is not part of GitHub or the minimal private input copy.

Native CPU tests under `tests/` include geometry/caching/ownership, car-material capture, offcamera selection/provenance, socket ABI/polling and launcher behavior. Keep the macOS checks passing when editing shared code, and add device-specific checks for the iOS host. The build agent's shell may not expose a Metal device; report on-device acceptance separately from successful compilation.

## Suggested first message to the iOS agent

> Read IOS_HANDOFF.md and MACOS.md. Work only in this iOS clone on ios-port. Create and build a real iOS target using the existing statically recompiled guest and native Metal renderer. First prove guest memory reservation and runtime/renderer fixtures on the target iPhone, then reach the menu and a single race with SSR/RT initially off. Keep iOS host/build changes separate from shared renderer/runtime fixes so later macos-metal commits can be rebased. Do not modify the sibling macOS project or upload private game/generated/save data.
