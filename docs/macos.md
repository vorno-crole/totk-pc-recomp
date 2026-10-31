# macOS port (Intel Mac, AMD GPU)

The port of the runtime to an Intel Mac through MoltenVK. This file records what was measured, what was decided and
why, and what is left. The approach is the Linux port's: the same lifter, runtime and Vulkan back end, with one more
platform file and `__APPLE__` branches beside the existing ones. Windows and Linux behaviour stays as it is.

Status and the work left are at the end, in [Status](#status).

## Target machine

| | |
|---|---|
| Model | Hackintosh reporting as iMac19,1, macOS 15.4 (24E248) |
| CPU | Intel Core i7-9700K, 8 cores, 8 threads; AVX2, BMI1/2, FMA, MOVBE, so `-march=x86-64-v3` holds |
| GPU | AMD Radeon RX 6600 XT, 8 GB, Metal 3, PCIe x16 |
| RAM | 32 GB |
| Displays | KG271U 2560x1440 at 60 Hz (main), KG272U 2560x1440 at 74 Hz, S24R35x 1920x1080 at 60 Hz |
| Homebrew | Intel, `/usr/local`; LLVM and lld 23.1.4, SDL3, Opus, libvpx, zstd, Vulkan headers and loader 1.4.363 |

Two earlier ports ran on this same machine and their notes apply here: `dragons-crown-pro-pc` (`docs/MACOS.md`, a PS4
game running x86-64 code directly) and `speedbreaker` (`HANDOVER-intel-mac.md`, an Xbox 360 recompilation).

## Why it is feasible

The lifted code is C with GNU extensions and targets x86-64, so the CPU half needs no new work: the generator's output
compiles for this machine as it does for Linux. Page size is 4 KiB, which the runtime assumes. An Apple silicon build
is a different project: it would need Rosetta or an arm64 build of the SIMD lowering, and the page size is 16 KiB.

MacNativeGames, the source of the NVN front end, is a Mac port of the same game but is no shortcut. It runs the ARM64
code natively and its back end is Metal 4, which this GPU and macOS version do not have. Its Metal notes stay useful as
a list of what the game needs from Metal.

## Vulkan on this GPU

Khronos MoltenVK 1.4.2, not Homebrew's Intel bottle (1.2.11, Vulkan 1.2). `vulkaninfo --text --show-formats` through the
portability enumeration reports Vulkan 1.4.357, driver id MoltenVK. What each runtime requirement found:

| Runtime needs | On this GPU | What the runtime does |
|---|---|---|
| Vulkan 1.3, timeline semaphores, dynamic rendering, synchronization2 | yes | |
| `VK_KHR_push_descriptor`, `VK_EXT_depth_clip_control` | yes (`depthClipControl` true) | |
| `VK_EXT_external_memory_host` | yes, and it works, see below | |
| `shaderInt64`, `shaderStorageImageWriteWithoutFormat`, BC textures, anisotropy, independent blend | yes | |
| `geometryShader` | **no** | required by `vk_device.c` `create_device`, but no stage uses it: pipelines are vertex and fragment only, so the check is dropped on macOS |
| `shaderFloat64` | no | existing softfloat64 path (`vk_caps.c`) |
| ASTC | no | the compute decoder does not compile here, see [ASTC](#the-astc-compute-decoder-does-not-compile), so the CPU decoder runs |
| Graphics pipeline library | no | existing whole-pipeline path, the default on AMD RDNA 1/2 on Windows |
| Subgroup size 32 | yes, fixed at 32 | the pin works |
| `minStorageBufferOffsetAlignment` 4 | 16 | existing rounding path (`vk_caps.c`) |
| `VK_EXT_attachment_feedback_loop_layout` | no | existing `VK_FEEDBACK_RESTART` path |
| `VK_EXT_vertex_input_dynamic_state` | no | vertex input goes into the pipeline key |
| `VK_EXT_extended_dynamic_state3` | only depth clamp, polygon mode and a few others | blend, write mask and alpha-to-coverage go into the pipeline key: more pipelines |
| `VK_EXT_custom_border_color` | no | **visible risk**: Metal has three fixed border colours |
| `VK_EXT_provoking_vertex` | no | **visible risk**: flat outputs come from the first vertex, NVN uses the last (`vk_pipeline.c` logs this) |
| `samplerMipLodBias` (portability) | no | **visible risk**: LOD bias has no effect |
| `samplerFilterMinmax` | no | **visible risk** if the game uses min/max filtering; not known yet |
| `logicOp`, `wideLines`, `shaderCullDistance`, `pipelineStatisticsQuery` | no | check when a shader or state needs them |
| `robustBufferAccess` | yes | |
| `VK_EXT_robustness2` | extension listed, all three features false | `nullDescriptor` is not used by the runtime |
| `VK_EXT_conditional_rendering`, `VK_EXT_transform_feedback` | no | only buffer usage flags depend on them |
| `VK_EXT_device_fault`, `VK_AMD_buffer_marker` | no | a lost device gives no report |
| `imageView2DOn3DImage` (portability) | no | check `vk_texture.c` views of 3D images |
| `triangleFans` (portability) | true but slow | MoltenVK emulates fans with compute passes, see below |
| `maxMemoryAllocationSize`, `maxBufferSize` | 3.5 GiB | one pool or buffer above it fails |
| `maxPerStageDescriptorUniformBuffers`, `StorageBuffers` | 31 | Metal's 31 buffer slots per stage are shared with vertex buffers |
| `maxSamplerAllocationCount` | 2048 | |

Memory types: 0 device-local; 1 host visible, coherent and cached (system RAM, 32 GiB heap); 2 device-local, host visible
and cached but **not coherent** (Metal managed storage). `VK_EXT_memory_budget` is present, and so is
`VK_KHR_present_wait`, which SpeedBreaker lacked on MoltenVK 1.2.12.

## Measurements

All on this machine, from throwaway C programs, MoltenVK 1.4.2.

### Host memory import works

The two earlier ports recorded that a discrete GPU refuses imported host memory; that was MoltenVK 1.2.x. On 1.4.2 a
16 MiB and a 256 MiB range at guest address `0x7000000000` went through
`vkGetMemoryHostPointerPropertiesEXT` (memory types 1 and 2), `vkCreateBuffer`, `vkAllocateMemory` and
`vkBindBufferMemory`, and a GPU round trip through `vkCmdCopyBuffer` behaved as follows:

| Test | Result |
|---|---|
| GPU reads CPU data from the import | 0 bad words of 67,108,864 |
| CPU writes after the import, then `mprotect` read-only, GPU reads | no stale pages |
| Read-only back to read-write, CPU write, GPU reads | new value |
| GPU writes into the import, CPU reads | new value |
| GPU writes into a range the CPU holds read-only | succeeds, CPU sees it |
| **`mmap(MAP_FIXED)` over pages inside a live import** | **GPU keeps reading the old pages, CPU sees the new ones** |

The last row matters: Metal holds the physical pages it was given. Replacing pages inside a live import silently splits
the two views. A pool must be released before its pages are decommitted, which is also what Windows needs.

256 MiB copies take about 39 ms in each direction between system RAM and VRAM (about 6.5 GiB/s), the same for imported
and ordinary host memory. The first use of an import took 192 ms against 39 ms after it, so the first bind wires pages.

Not tested yet: an import over a range with uncommitted pages.

### Write tracking has to be built, and costs more than on the other hosts

Windows has `GetWriteWatch` and Linux has userfaultfd with `PAGEMAP_SCAN`. macOS has neither. `mach_vm_page_range_query`
reports a page's state but offers no way to reset it, so the only option is `mprotect` and a fault handler.

| Operation | Cost |
|---|---|
| Write fault on a 4 KiB page, handler unprotects it | about 6.3 us (100 MiB of pages: 157 ms) |
| Same with a 64 KiB grain | 100 MiB: 35 ms, 1,600 faults |
| Same with a 2 MiB grain | 100 MiB: 26 ms, 50 faults |
| `mprotect` read-only over 1 GiB, one VM entry | about 1.5 ms |
| `mprotect` read-only over 50,000 split entries | about 36 ms (0.7 us per entry) |

Most of the coarse-grain cost is the kernel walking page table entries, so a coarser grain has a floor of about 0.26 ms
per MiB. Protection faults arrive as `SIGBUS`, and the write bit is `uc_mcontext->__es.__err & 2`.

For scale, the comments in `vk_memory.c` record about 257 MiB queried a frame in a fight on Windows. Queried is not
written, so the cost on this machine depends on how many pages are written per frame, which is the first number to take
from a real run. If it is high, the answer is to leave pages written in recent frames unprotected and count them as
written every time, since a page copy is far cheaper than a fault.

A commit leaves watched pages read-only, so the first write of every page costs one of these faults on top of its zero
fill. The whole 3.4 GiB application pool would be about 5 s of CPU spread over its loading threads, if every page were
written. A first version left committed pages writable and marked them written, which saved those faults, but it
reported pages nobody wrote and `--probe-gfx-abi` rightly failed it.

### The ASTC compute decoder does not compile

`vkCreateComputePipelines` fails for `astc_decode.comp`: `Compiler encountered an internal error`. It is the compiler for
AMD GPUs, reproduced outside the game with `newLibraryWithSource` and `newComputePipelineStateWithFunction` on the MSL
that `MVK_CONFIG_DEBUG=1` printed. What did not help:

| Change | Result |
|---|---|
| Fast math on or off | fails after 0.3 to 0.8 s off, about 30 s on |
| `MTLLibraryOptimizationLevelSize` | fails after about 30 s |
| Workgroup arrays made thread-local | fails, so workgroup memory is not the trigger |
| Group barriers removed | fails after 140 s |

The MSL has no `coherent`, so this is not Dragon's Crown's bug. The size of the shader (2,361 lines of MSL, everything
inlined) and the long failures suggest a limit in the compiler rather than one bad construct, but that is not proven.
The runtime falls back by itself to the CPU decoder, which is correct and tested (`--probe-vkmem`, 186,758 blocks) but
slow for a game with this many ASTC textures. `--probe-astc` fails here for this reason. The next things to try are
splitting the shader into smaller functions Metal does not inline, or transcoding ASTC to BC at install time.

### Thread interruption

A port of Linux's signal handshake stopped only 551 of 2,000 tries inside its 200 us window, and would have left
asynchronous preemption nearly idle. `thread_suspend` with `thread_get_state` and `thread_set_state` stopped 2,000 of
2,000 and redirected 200 of 200.

### Stress test

`tests/plat_stress_macos.c` runs the platform file without game files. Three runs passed: 1,200,000 contended lock
increments with readers alongside, a timed wait of 20.03 ms for 20 ms, 1,000 fiber switches, and the interruption numbers
above. In its write-watch part three threads write random pages while the main thread syncs a copy every 2 ms: about
500,000 pages over 800 rounds in 3 s, and 0 words differ at the end.

### Address space

These all reserve at the fixed addresses in a process linked as a normal PIE: 8 GiB at `0x7000000000`, 6 GiB at
`0x200000000`, 4 GiB at `0x300000000`, 64 GiB at `0x400000000`. The dyld shared cache and malloc's zones sit far above
2^39 on x86-64 macOS. The executable loads at `0x100000000` plus a slide; `memory.c` searches for its low block from
`LOW_SEARCH_FROM`, so it moves past that.

### Compile check

`clang -fsyntax-only -march=x86-64-v3` with Homebrew LLVM 23 over `runtime/*.c` and `runtime/ui/*.cpp`, Windows files left
out: 75 of 85 files pass. Nine of the ten failures are the one `RT_HOST_SECTION` macro in `host.h` (Mach-O sections are
named `segment,section`), and the tenth is `romfs_config.h`, which the build generates. Syntax checks do not reach the
assembler, so `kernel.c`'s stubs needed their own branch.

## What differs from Linux

| Area | Linux | macOS |
|---|---|---|
| Executable | `-no-pie`, ELF | PIE, Mach-O. Dispatch slots are 32-bit offsets from an anchor, so the load address does not matter |
| Override tables | `__start_`/`__stop_` symbols | `section$start$__DATA$totk_ovr` symbols |
| Assembly stubs | `.type`, `.size` | no such directives; C symbols take a leading underscore |
| Locks | futex | `os_sync_wait_on_address` (public since macOS 14.4) on the same 32-bit words, so the lock logic is unchanged |
| Faults | `SIGSEGV`, `gregs[REG_RIP]` | `SIGBUS` as well, `uc_mcontext->__ss.__rip`. A watched reservation installs the handler itself |
| Interrupting a thread | signal handshake | `thread_suspend`, `thread_get_state`, `thread_set_state`, as on Windows. A signal handshake stopped only 551 of 2,000 tries inside its 200 us window here |
| Write watch | userfaultfd | page bitmap, `mprotect`, fault handler |
| Reserving | `MAP_FIXED_NOREPLACE` | `mach_vm_allocate` with `VM_FLAGS_FIXED`, which fails on taken addresses |
| Reading memory safely | `process_vm_readv` | `mach_vm_read_overwrite` |
| Affinity | `sched_setaffinity` | none on macOS: the calls report failure |
| Priorities | nice values | `pthread_setschedparam` around the default of 31 |
| Rename without replacing | `renameat2` | `renamex_np` with `RENAME_EXCL` |
| Unbuffered reads | `O_DIRECT` | `fcntl(F_NOCACHE)` |
| Window | a thread of its own | AppKit needs the main thread: the main thread runs the window and the game runs on another |
| Vulkan | system driver | MoltenVK beside the executable, `VK_DRIVER_FILES`, `VK_KHR_portability_enumeration`, `VK_KHR_portability_subset` |
| Memory mode | shadow copies | imports, as on Windows (`vk_memory.c`); mirror on, since the GPU is discrete |

`runtime/platform/macos.c` is `linux.c` section by section. `check_platform.py` still reports no stray Win32 uses.

A kernel write into a page the tracker holds read-only fails with `EFAULT` instead of faulting. `plat_file_pread` therefore
touches the buffer's pages first, so each one is marked written and made writable before the read.

## Lessons carried over from the earlier ports

These cost days there. Check them here before looking elsewhere:

- **Triangle fans.** MoltenVK turned each fan draw into compute passes; Dragon's Crown went from 20 to 60 fps by drawing
  fans as lists. `vk_exec_draw.c` `op_draw` already rewrites quads the same way; fans and polygons belong beside it.
- **Metal's `coherent`.** Apple's compiler for AMD GPUs fails on MSL `coherent device`. A buffer decorated `Coherent`
  needs the decoration dropped, in `vk_pipeline.c` `spirv_fixup`.
- **Features, not extensions.** `VK_EXT_robustness2` is listed with every feature off. Read the feature bits.
- **Sparse resources.** Metal has none; this runtime does not need them.
- **`R8_UNORM`.** MoltenVK 1.2 sampled it as zero on this GPU; 1.4.2 fixed it (SpeedBreaker `a7edf3e`).
- **Hidden costs.** Find them with `MVK_CONFIG_PERFORMANCE_TRACKING=1` and
  `xcrun xctrace record --template 'Metal System Trace' --attach <pid> --time-limit 5s`. Measure before fixing: two
  plausible guesses about Dragon's Crown's frame rate were wrong.
- **Shader failures.** `MVK_CONFIG_DEBUG=1` prints the MSL. `xcrun metal` runs only the front end; GPU-specific failures
  need `newLibraryWithSource` and a pipeline state in a few lines of Objective-C.
- **Pacing.** Keep the game on a 60 Hz display.

## Risks

In the order they are likely to matter:

1. ASTC on the CPU, above. It may be the first thing that limits play.
2. Frame rate through MoltenVK. Nothing has been measured with this game's draw counts.
3. Shader compile time. MoltenVK converts SPIR-V to MSL and Metal compiles it, there is no graphics pipeline library, and
   the extended-dynamic-state gaps add pipeline variants. The list holds 29,587 variants for 1.4.3.
4. Shaders SPIRV-Cross or Metal refuses, like the ASTC decoder. Every shader can be tested offline: run the census
   output through the conversion and a Metal compile before booting the game.
5. The visible risks in the table above.
6. Write fault cost, above.

## Game files

Results from a real game card dump, read on this machine without changing it. No game data or card identifiers are here.

A MIG Flash backup is a folder named `<id>.xci`, holding `ROM.xci` and four small card files (card ID set, card UID,
certificate, initial data). The tools want `ROM.xci` itself: give that file as `base`, not the folder. The small files
are unique to the card, the build needs none of them, and they should not be copied or shared.

| Check | Result |
|---|---|
| `ROM.xci` size against `card_image_size` of `expected-v1.0.0.json` | equal: 17,536,620,544 bytes, an untrimmed dump |
| HFS0 hash chain from the card header (no keys needed) | root and all four partitions verify |
| Program NCA `9d827368992a2faafa9062aa5458b7ec.nca` in the secure partition | present |
| SHA-256 of the whole NCA against its name | matches, 17.1 GB in 211 s over SMB |
| `extract_exefs.py` with `prod.keys`, then ExeFS hashes | `main`, `sdk`, `subsdk0`, `rtld` and `main.npdm` all match |

The RomFS was not hashed against `romfs.sha256`. The NCA hash already covers those bytes.

`extract_exefs.py` runs on macOS with Python 3.15 and `cryptography` 50.0.2, which is the first of the Windows-side tools
to run here. Its hash check is the quickest full test of a dump and keys together: it reads little of the card.

The base alone does not build. Only 1.1.1, 1.2.1 and 1.4.3 have patch sets (`recomp/patches-*.txt`), so a cumulative
update NSP is needed as well, and an update on a game card does not count: the `update` partition of a card holds system
firmware. The keys file must also reach the update's key generation. 1.0.0 and 1.2.1 share their SDK, so keys that open
the base should open 1.2.1; 1.4.3 uses a much newer SDK (20.5.22) and needs newer master keys than a set that stops at
`master_key_0f` (firmware 16). A set reaching `master_key_15` also decrypted the base's ExeFS to the verified hashes, and
its `header_key` and `master_key_00` to `0f` equal the older set's. That shows the set is sound; only extracting a 1.4.3
update will prove the generation that update uses.

## Status

The runtime builds, links and runs on this machine; game code has never run here. There is no game on the machine, so
`build_game.py` went as far as `--make-runtime-lib`, and the executable below was linked by hand against stub module
glue in a scratch folder, not by `build_game.py`. The steps that link a real game on macOS, MoltenVK's copy beside the
executable among them, have not run yet.

| Check | Result |
|---|---|
| `tools/build_game.py --make-runtime-lib` | passes |
| Hand link with lld, stub layout | passes |
| `tests/plat_stress_macos.c` | passes |
| `--probe-cfg`, `--probe-gfx-abi` (55 checks, write watch included), `--probe-vkdevice`, `--probe-nvn-tex` | pass |
| `--probe-vkmem` (41 GPU checks through imports, 0 validation errors) | passes |
| `--probe-window` | window, swapchain and present work; fails on +1 of 255 in each channel of the first frame's clear colour, also with the upscaler off and with MoltenVK fast math off or on. Cause unknown; invisible but unexplained |
| `--probe-astc` | fails: the compiler bug above |
| `--selftest`, `--probe-kernel`, `--probe-ipc`, `--probe-services` | need lifted modules and ExeFS hashes, not run |
| `--probe-vkexec` | needs the shader census, not run |
| Windows and Linux builds | not rebuilt: there is no such machine here. The shared edits are behind `__APPLE__` or expand to the same text, but they need a build on each |

| Phase | Done when | State |
|---|---|---|
| 0. Assessment | requirements checked against this GPU, memory and fault costs measured | done |
| 1. Runtime builds | the runtime links and the game-free probes run | done, apart from the two open probes |
| 2. Tools | `tools/extract.py`, hactool, translator on macOS | `build_xlate.py` names `osx-x64`; none of it has run, `dotnet` and `uv` are not installed |
| 3. Shader census | every variant converts and compiles for Metal | needs game files |
| 4. Boot | title screen | needs game files |
| 5. Play | a stable 60 fps in the open world | needs game files |

The base game dump and keys are verified ([Game files](#game-files)), but no update is on hand. Phases 3 to 5 wait for a
1.2.1 or 1.4.3 update NSP. Before boot, these are known work: draw triangle fans as lists, and check
`spirv_fixup` for `Coherent`.
