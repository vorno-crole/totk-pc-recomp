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
| ASTC | no | the compute decoder runs, after a change for Metal's compiler, see [ASTC](#the-astc-compute-decoder-and-the-metal-compiler) |
| Graphics pipeline library | no | existing whole-pipeline path, the default on AMD RDNA 1/2 on Windows |
| Subgroup size 32 | yes, fixed at 32, but only in the fragment, compute and tessellation control stages (`supportedStages` 0x32) | the pin works; vertex shaders that use subgroup operations get one-lane modules, see [Fused-weapon icons](#fused-weapon-icons) |
| `minStorageBufferOffsetAlignment` 4 | 16 | existing rounding path (`vk_caps.c`) |
| 16-bit packed formats (RGB565, RGB5A1, RGBA4) | **no**, not even the mandatory stand-ins: Metal has them on Apple GPUs only | stored as RGBA8, each texel expanded on upload, see [Fused-weapon icons](#fused-weapon-icons) |
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
| `imageView2DOn3DImage` (portability) | no | draws into 3D images go through 2D array stand-ins, see [What else the first runs showed](#what-else-the-first-runs-showed) |
| `minVertexInputBindingStrideAlignment` (portability) | 4 | `vk_valign.c` copies streams that break it, see [Vertex streams](#vertex-streams) |
| `triangleFans` (portability) | true but slow | MoltenVK emulates fans with compute passes, see below |
| `maxMemoryAllocationSize`, `maxBufferSize` | 3.5 GiB | one pool or buffer above it fails |
| Samplers per stage, direct binding | 16 | layouts above it are plain sets, see [Sampler limit](#sampler-limit) |
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

### The ASTC compute decoder and the Metal compiler

`vkCreateComputePipelines` failed for `astc_decode.comp`: `Compiler encountered an internal error`. It is the compiler for
AMD GPUs, reproduced outside the game with `newLibraryWithSource` and `newComputePipelineStateWithFunction` on the MSL
that `MVK_CONFIG_DEBUG=1` printed. What did not help:

| Change | Result |
|---|---|
| Fast math on or off | fails after 0.3 to 0.8 s off, about 30 s on |
| `MTLLibraryOptimizationLevelSize` | fails after about 30 s |
| Workgroup arrays made thread-local | fails, so workgroup memory is not the trigger |
| Group barriers removed | fails after 140 s |
| Another decoder of the same lineage: Weeping of the Empire's `engine/runtime/nvn/vulkan/astc_decode.comp` (MIT), one invocation per block, no workgroup memory, no barriers | fails the same way |

The size of the shader suggested a compiler limit; it was one expression. Building variants of the shader as compute
pipelines through MoltenVK 1.4.2, each with one part replaced by a stub, narrowed it down. The pipeline builds when both
integer-sequence decodes (`ise_at`) are stubbed and fails with either one. Inside it, it builds without the trit
decode, and the quint decode is innocent. Inside `trit_at` it is the last branch of the low-trit decode,
`(C & 1u) & ~(C >> 1) & 1u`: written `(C & 1u) & ((C >> 1) ^ 1u) & 1u`, the same bit, the pipeline builds. Both
`~` uses in `trit_at` are now written that way. The other decoder has the same trit decode, which is why it failed too.

With that change the shipped SPIR-V (`glslc -O`, `runtime/astc_decode_spv.h`) builds in 0.5 s, and `--probe-astc`
passes: 1,740,602 texels of 14 footprints, linear and sRGB, bit-exact with the CPU decoder. A 2048x2048 4x4 texture
decodes in 7.7 ms of batch time against 18.6 ms on the CPU worker pool. In two scripted runs from the save, standing
still on Great Sky Island, the steady frame rate is the same with either decoder (25.7-27.4 fps GPU, 25.4-29.8 CPU):
nothing streams in. The difference is at loading: with the CPU decoder the worst frames of the first minute were
1,055, 991 and 344 ms, two of them with 389 and 151 ms of ASTC decode; with the GPU decoder 445, 299 and 107 ms, with
under 1 ms of ASTC each, and 7 frames over 100 ms against 11. The gain while moving through the world, where textures
stream, has not been measured.

`TOTK_ASTC=cpu` still selects the CPU decoder. BC3 re-encoding after the decode (`TOTK_ASTC_BC`) stays off by itself on
this 8 GB card.

### Thread interruption

A port of Linux's signal handshake stopped only 551 of 2,000 tries inside its 200 us window, and would have left
asynchronous preemption nearly idle. `thread_suspend` with `thread_get_state` and `thread_set_state` stopped 2,000 of
2,000 and redirected 200 of 200.

A suspended thread still finishes its work in the kernel. A game thread stopped on a write-watch fault may have the
call of its signal handler set up after `plat_intr_stop` read its state (rip in `_sigtramp`, the handler's arguments
in the registers, rsp on the signal stack). `plat_intr_resume` used to read the state again and change only rip, so
`kern_ap_stub` saved that state and went back into game code with it, without running the handler: the crash dump
of `tests/intr_fault_macos.c` showed the fault handler's address in rdi, `UC_FLAVOR` (30) in rsi, `SIGBUS` (10) in rdx
and stack addresses in rcx and r8, at the store of the writer's loop. `plat_intr_resume` now writes back the state
the stop read, with rip changed: a handler call set up in between is dropped, the stub returns to the faulting
instruction, and that faults again. The dropped call leaves the kernel's on-signal-stack mark set, so `fault_handler`
sets `uc_onstack` from whether the interrupted rsp is on the signal stack. `thread_info`'s run state does not tell the
two cases apart: a thread stopped in user code and one stopped inside a fault both report `WAITING`.

Taking the faults on a Mach exception port instead, where the faulting thread waits with its registers untouched, also
removes the race, but a fault then costs 13-14 us instead of about 6 us, and the game fell from 60 to 25 fps in
a scripted run, so the port was not kept.

`tests/intr_fault_macos.c` stops and redirects a thread that stores to watched pages while the watch is reset. With
10 s runs and 20 or 5 us between stops, the old resume crashed in every setting; the new one passed each, up to
131,083 redirects with 0 words wrong. A fault costs the same as before (6.6 us).

### Stress test

`tests/plat_stress_macos.c` runs the platform file without game files. Three runs passed: 1,200,000 contended lock
increments with readers alongside, a timed wait of 20.03 ms for 20 ms, 1,000 fiber switches, and the interruption numbers
above. In its write-watch part three threads write random pages while the main thread syncs a copy every 2 ms: about
500,000 pages over 800 rounds in 3 s, and 0 words differ at the end. Then it reads a 4 MiB file into watched
memory while the main thread resets the watch (see the `EFAULT` note under "What differs from Linux"). Its last part
writes one page before each of 20 resets, then stops: the page is reported 20 times, then 48 times without a write
(`hot_keep`, [Frame rate](#frame-rate)), and not again until the next write. With `TOTK_WW_HOT=0` it is reported 0
times without a write.

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
| Write watch | userfaultfd | page bitmap, `mprotect`, fault handler; a page written between every two resets stays writable and is reported at each ([Frame rate](#frame-rate)) |
| Queue submits | the driver's | MoltenVK encodes on its own queue thread (`MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0`, set by `vk_device.c` `moltenvk_settings`) |
| Reserving | `MAP_FIXED_NOREPLACE` | `mach_vm_allocate` with `VM_FLAGS_FIXED`, which fails on taken addresses |
| Reading memory safely | `process_vm_readv` | `mach_vm_read_overwrite` |
| Affinity | `sched_setaffinity` | none on macOS: the calls report failure |
| Priorities | nice values | `pthread_setschedparam` around the default of 31 |
| Rename without replacing | `renameat2` | `renamex_np` with `RENAME_EXCL` |
| Unbuffered reads | `O_DIRECT` | `fcntl(F_NOCACHE)` |
| Window | a thread of its own | AppKit needs the main thread: the main thread runs the window and the game runs on another |
| Vulkan | system driver | MoltenVK beside the executable, `VK_DRIVER_FILES`, `VK_KHR_portability_enumeration`, `VK_KHR_portability_subset` |
| Memory mode | shadow copies | imports, as on Windows (`vk_memory.c`); mirror on, since the GPU is discrete |
| Thread-locals across fiber switches | the thread register gives each access the running thread's variable | the address comes from a call clang merges within a function, so a fiber resumed on another thread kept the first thread's; `PLAT_TLS_VAR` looks it up at each use |
| Overlay GPU numbers | sysfs `gpu_busy_percent`, `mem_info_vram_used`; ADL for the rest | the `PerformanceStatistics` of the GPU's IOAccelerator: `Device Utilization %`, `inUseVidMemoryBytes`, `Temperature(C)`, `Total Power(W)`, `Core Clock(MHz)`. MoltenVK's `deviceLUID` is Metal's `registryID` big-endian, the accelerator's registry entry ID, which picks the GPU |
| Local IPC (Discord) | Unix socket, `SOCK_CLOEXEC`, `MSG_NOSIGNAL` | the same socket in `$TMPDIR`, where Discord's macOS client listens; `FD_CLOEXEC` and `SO_NOSIGPIPE` instead of the two flags |
| 3D render targets | 2D array views of the 3D image's depth planes (`2D_ARRAY_COMPATIBLE`) | MoltenVK makes such images only in Metal placement heaps, which it leaves off on AMD (`imageView2DOn3DImage` 0), so each drawn level gets a 2D array stand-in that every pass end copies back (`vk_exec.c` `vol_target`) |

`runtime/platform/macos.c` is `linux.c` section by section. `check_platform.py` still reports no stray Win32 uses.

A kernel write into a page the tracker holds read-only fails with `EFAULT` instead of faulting. `plat_file_pread` therefore
touches the buffer's pages first, so each one is marked written and made writable before the read. That alone is a race:
the render thread resets watched ranges every frame, and a reset between the touch and the read protects a page again.
The first build crashed this way about a minute into the cave (`RomFS: read of 142572 bytes ... refused: the buffer is
not writable`, then `FATAL: RomFS read failure`). The read now runs in 256 KiB parts, each touched just before it and
read again after an `EFAULT`. With 4 MiB reads against a reset every 2 ms, the stress test failed 399 of 399 reads
before and fails 0 of 355 after.

A later run died after 72 minutes, idle in the menu, with a macOS crash report and no `FATAL` line in the log: a
`SIGBUS` on an ordinary watched heap page (read-only, its neighbours read-write) written by lifted code. Had the fault
handler declined that fault, the crash hook would have logged it, so the handler was not in place. The likely path is
the decline path, which copied `linux.c`: for a fault in code outside the executable it set the default action and
returned. If the access then succeeded (a page another thread had just made accessible), the default action stayed set
for the whole process, and the next write-watch fault on any thread ended the game silently. The handler now retries a
fault whose page allows the access by the time it checks (`mach_vm_region`), up to 1,000 times at one address, and
otherwise logs a `platform: unhandled ... fault` line (address, error code, page state, watched, written bit, and the
library) and ends the process through the crash hook wherever the code is. This cause is inferred, not reproduced; the
new line will tell if it happens again.

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

1. Frame rate through MoltenVK: 27 to 42 fps on Great Sky Island so far, see [Frame rate](#frame-rate).
2. Shader compile time. MoltenVK converts SPIR-V to MSL and Metal compiles it, there is no graphics pipeline library, and
   the extended-dynamic-state gaps add pipeline variants. The list holds 29,587 variants for 1.4.3.
3. Shaders SPIRV-Cross or Metal refuses, like the ASTC decoder. Every shader can be tested offline: run the census
   output through the conversion and a Metal compile before booting the game.
4. The visible risks in the table above.
5. Write fault cost, above.

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
its `header_key` and `master_key_00` to `0f` equal the older set's, and it decrypted a 1.4.3 update's ExeFS: `main`,
`sdk`, `subsdk0`, `rtld` and `main.npdm` all match `expected-v1.4.3.json`.

A 1.4.3 update NSP checked out: version 720896, the file table complete, every NCA's SHA-256 matching its name, and a
common ticket. Its RomFS merged with the base to the `romfs` hash of `expected-v1.4.3.json`.

## First boot

The full pipeline ran on this machine with 1.4.3, nothing on another OS:

| Step | Result |
|---|---|
| `tools/build_hactool.py`, `tools/build_xlate.py`, `tools/xlate_smoke.py` | pass with no macOS changes beyond the `osx-x64` name; translator built in 36 s |
| `tools/extract.py` | ExeFS and RomFS of 1.0.0 and the merged 1.4.3 match every recorded hash; 304,243 files |
| `recomp/gen.py` | 283,000 functions, 0 unsupported instructions, 78 s |
| `tools/build_game.py` | compile 538 s on 8 cores, link 1.6 s, 237 MiB executable |

The game reaches the title screen and animates it. On the title, about 44 fps, GPU 10 to 18 ms a frame. The write watch cost
0.5 ms a frame there (394 pages found): far from the worry above, but a menu writes little, so measure it in the world.

`--seconds S` counts world time from the save's first frame, so at the title it never ends, and `--shot` never fires. Use
`TOTK_SHOTS=300-3300:600 TOTK_SHOTS_DIR=<dir> TOTK_SHOTS_FULL=1`.

### Vertex streams

The first title screen had long streaks fanning out from the centre, and it looked plausible until the user reported it. Zelda
Wiki's capture of the real screen shows a flat, bright mural wall. MoltenVK's limit is the cause: on this GPU family
`vertexStrideAlignment` is 4, and Metal wants attribute offsets in multiples of 4 as well. The title's skinned meshes have
strides of 6, 10 and 14 with bone indices and weights at +6, +9 and +10. MoltenVK reports a bad stride only when it is static.
The runtime's is dynamic, so nothing warned, and Metal read garbage. Vertices flew off, and the triangles they stretched smeared
their textures.

`runtime/vk_valign.c` gives such a stream a copy with aligned attributes, and fills in the `w` of RGB16 positions, which the
runtime maps to RGBA16. The title needs 32 copies, 0.2 MiB.

Two wrong turns to avoid repeating: a smooth depth buffer does not prove the geometry right (it was smooth and wrong), and
checking only the UV stream's alignment missed the skinning streams. Compare with a reference early. Forcing 16x anisotropy
changed nothing, and the texture data was correct throughout.

The title was still darker and less orange than the reference. The 3D render targets below were failing then, the sky's
volume among them; the title has not been compared with the reference again since they were fixed.

### Sampler limit

Play starts: Continue loads, and the opening cave runs. It showed black blocks around Link and Zelda, made of 32-pixel screen tiles.
The game lights its picture at half resolution through a tile mask, with one pipeline per material class. The class for
characters needs more than 16 samplers, Metal's limit for direct binding, which is how MoltenVK binds a push descriptor set.
That pipeline did not compile, draws with it were skipped, and those tiles stayed black.

`vk_pipeline.c` `layout_get` now makes such a layout a plain descriptor set, which MoltenVK puts in an argument buffer. Only
pipelines over the limit change. Finding the pass took one script: the block's position is known, so measure the brightness step
along its edge in every dumped image, and the first pass with a step is the one to read.

To reach a scene without a person: `TOTK_INPUT_SCRIPT` with lines such as `900 A 10`. For a save, presses at ticks 900, 1300,
1700 and 2100 get through Continue and the load prompt, and the world is on screen by frame 5000.

### What else the first runs showed

- **3D render targets failed.** Four kinds of 3D image (`vk_texture.c`, `2D_ARRAY_COMPATIBLE`) failed with `MTLHeap must be
  enabled to create 2D-on-3D or block texel view compatible images`, retried every frame. They include the sky's 256x32x16
  scattering volume, R8 volumes of the sizes of the BC4 volumes drawn at the logo, and the grading LUT of the menu's background blur; without
  them the sky was a murky green-grey. MoltenVK 1.4.2 enables placement heaps on AMD only with `MVK_CONFIG_USE_MTLHEAP=2`
  (MoltenVK, `MVKDevice.mm`: "AMD support for MTLHeap is buggy"), and that would move every allocation into heaps. The
  images are now made without the flag when the portability subset reports no `imageView2DOn3DImage`, and draws into them
  go through 2D array stand-ins, as 3D BC targets already did ([table](#what-differs-from-linux)). A scripted run with the
  camera on the sky shows a blue sky and clouds where the build before showed green.
- **Audio frame events came in bursts.** `--probe-ipc` expects the audio renderer's frame event every 5 ms (240
  samples at 48 kHz) and measured 12.2-13.7 ms, worst gap 25-26 ms; its SDK loop measured 18-19 ms. The renderer kept
  time (554 frames rendered over 547 periods), so frames were rendered but signalled together. The built-in output
  pulls 1024-sample periods, about 4.3 frames each, and `audren.c` `render_thread` rendered them back to back; the
  waiting thread woke once per burst. This is the PipeWire case: on Linux, device-paced frames stay 4/5 of a frame
  apart (`gap_ns`), because without the gap AUX effects read stale return samples and click at frame seams. Timed waits
  are not the cause: the stress test waits 20.03 ms for 20 ms.

  A smaller device period does not help. With `SDL_AUDIO_DEVICE_SAMPLE_FRAMES` at 240, 256 or 480 SDL reports that
  period, but the probe's thread still woke every 10.2-10.5 ms, two frames at a time. The gap does: with it on macOS as
  well, the event comes every 5.01-5.02 ms and the SDK loop every 4.90-4.92 ms in three runs, and 150 s of the game's
  title screen rendered 1,000 frames each 5 s with 0 underruns and 0-1 frames a window under 1 ms after the one before
  (`TOTK_TRACE=audio`, `TOTK_AUDIO_DUMP`). The title screen was silent, so reverb seams in play are not checked yet.
  The probe's check of frames rendered against time now starts after 100 warm-up frames: after Start the renderer fills
  the output ring to its target once, 8.5 frames ahead with 1024-sample periods, and with the gap that fill falls
  inside the timed second.
- **One GPU hang, not reproduced.** About 20 minutes into a session, as Link received Fuse, the GPU stopped making
  progress; after 5 s the kernel restarted it (`IOAccelEventMachine2` in `log show`) and the game ended with
  `VK_ERROR_DEVICE_LOST`. A full-screen pass had waited 1.3 s for its pipeline just before. None of the shaders
  translated in that second has a loop, and the ASTC decoder had not logged work for some time. Replays of the same save
  did not hang. MoltenVK has neither `VK_AMD_buffer_marker` nor the NVIDIA checkpoints, so the breadcrumbs that name
  the hung pass were off; `TOTK_PASS_CRUMBS=1` now writes them with `vkCmdFillBuffer` (`vk_crumbs.c`). Each fill is
  a Metal encoder of its own, which cost about a third of the frame rate in play, so it stays off unless asked for. If
  the hang comes back, replay with it on; if it then stops hanging, a mark per submission alone is the cheaper next
  step.

### Fused-weapon icons

Fused weapons showed a dark square in the inventory where their icon belongs. The game captures each icon on the GPU
and keeps it as a PNG in its cache storage (`build/profile-v143/cache/0/icon/Weapon_000.png` and so on, 144x144); the
bad ones were a flat dark grey. The cause was a texture format: Metal on AMD and Intel GPUs has none of the 16-bit
packed formats (RGB565, RGB5A1, RGBA4), and neither the exact Vulkan formats nor the mandatory stand-ins of
`vk_formats.c` are there, so every log said `texture format 0x3b: VkFormat 8 lacks features 0xc001` (and `0x3c`) and the
textures were never created; a shader sampling one read zeros. These formats also hold the terrain's normal and blend
maps. Such a texture is now RGBA8 (`TexVkFormat.pack16`): each texel is expanded on upload as the table's Vulkan format
defines its fields, the view swizzle stays the table's, and a read-back packs it again (`vk_texture.c`
`expand_pack16`, `shrink_pack16`; all 65,536 values of each format survive the round trip). `--probe-vkmem` no longer
lists the eight formats as unsupported, and weapons fused since show their icons.

On the way a second fault turned up, in every session's log: one pipeline failed with `vkCreateGraphicsPipelines
failed (-3)` and its draws were skipped. The failure line now names the pipeline's shaders and state (`vk_pipeline.c`
`create_graphics`), and with `MVK_CONFIG_LOG_LEVEL=2` MoltenVK said the vertex function did not compile. MoltenVK
reports subgroup operations for the fragment, compute and tessellation control stages only, as Metal has no SIMD-group
functions in vertex functions on these GPUs, but the translator is told 32 lanes for every stage. 21 of the 5,140
cached vertex shaders use them: 18 a vote, 3 a ballot and the lane index, where the lane with the ballot's highest bit
adds the ballot's bit count to an atomic counter. `vk_pipeline.c` `spirv_one_lane` now rewrites such a module for a
stage the driver lists without subgroups as a one-lane subgroup (a ballot of p is (p, 0, 0, 0), a vote of p is p, the
lane index 0, a shuffle the invocation's own value), which gives the counter the same total; `make_module` applies it
only to modules that use the operations. Over the whole cache, the 50 modules it changes pass `spirv-val` and the rest
come back unchanged, and in play the pipeline builds. The failing pipeline drew points into no target, and fixing it
alone did not bring the icons back.

An icon captured while the bug was there stays in the cache, and the game shows it instead of capturing it again: an
axe picked up on the first day stayed dark after the fix until its `Weapon_000.png` was removed, then the game captured
it again correctly. The same holds for any later rendering fix: a bad cached icon goes when its file goes.

## Frame rate

Measured with a muted scripted run that continues the save and stands still on Great Sky Island, looking over a wide
view (`TOTK_TRACE=perf`, frames 3,000 to 6,300; the scene moves with the in-game clock, so runs vary by about 1 fps).
Before the changes below: 22.9 and 24.7 fps in two runs, the replay thread busy 22-25 ms a frame.

`sample` showed the replay thread busy 73% of the time and the game's main thread waiting for GPU fences for half of
its time, so the replay held the frame back. Half of the replay's time was MoltenVK encoding Metal commands inside
`vkQueueSubmit2`: MoltenVK does that on the calling thread unless `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS` is 0.
Every part of a split submission (`vk_exec.c` `maybe_split`, about 8 a frame here) was encoded before the replay went
on. `vk_device.c` `moltenvk_settings` now sets it to 0 before the driver loads, unless the environment has a value:
the replay fell to 12 ms a frame, and the frame rate rose to 25.7 and 26.5 fps.

After that nothing was saturated. The replay thread was idle 63% of the time, MoltenVK's encoding thread busy 53%, and
the GPU's `GPU Activity(%)` 40-60%, while the game's main thread still waited for fences for 47% of its time. Each frame
is a chain: the game records, the replay turns that into Vulkan, MoltenVK encodes it, the GPU runs it, and the game
waits for that fence before it goes on. The frame rate now depends on the length of the chain more than on any one part
of it. Submitting smaller parts did not help: parts of 300 us and 32 draws (17 a frame) gave 26.0 fps, and no splitting
at all 18.3 fps, so the defaults stay.

The write watch was a cost on that chain: about 940 pages a frame were found written and protected again, and each
faults on its next write, 6-14 us of a game thread. 71% of the pages a reset reported had been reported 30-60 ms
before, the previous frame. `macos.c` `hot_keep` now leaves a page that was reported at 4 resets in a row, no more than
100 ms apart, writable with its bit set, so it is reported at every reset without faulting, and protects it again every
64 reports to find out whether it went quiet. A report too many costs the consumer a copy of the page. Two runs each:
27.4 and 27.3 fps against 26.5 and 26.8 with `TOTK_WW_HOT=0`. The replay's write-watch queries fell from 1.05 to
0.1-0.17 ms a frame, and screenshots show the same scene.

The per-draw check of the vertex layouts for Metal's 4-byte alignment (`vk_exec_draw.c` `valign_streams`) is now kept
until the attribute state changes. It cost 6% of the replay thread; its effect on the frame rate is inside the noise.

`TOTK_ONE_PER_CORE` has nothing to act on here: the i7-9700K has no SMT, and macOS has no affinity calls.

All three together, with no environment set: 26.9 and 27.4 fps, the replay 10-11 ms a frame, against 22.9 and 24.7
before, about 15% more.

The fence wait came next. Two `TOTK_TRACE=perf` lines measure it: `perf: nvnSyncWait` sorts the game's sync waits by
how many submissions the waited value lies behind the newest (`nvn_api_core.c`), and `perf: fence chain` times each
value from `vkQueueSubmit2` to the completion thread seeing it signalled, and from there to the waiting threads waking
(`vk_sync.c`). The game waits once a frame for the submission it has just made (0 behind), so the frame cannot overlap
with the GPU's work on it; submit to signalled took 27 ms, signalled to done 0.12 ms. The GPU's own time was the
length of the chain. A Metal System Trace (`xcrun xctrace record --template 'Metal System Trace' --attach <pid>`)
showed about 270 render encoders and 244 blit encoders of one small copy each per frame: every `UpdateUniformBuffer`
(about 12,000 a second, 170 bytes on average) ended the render pass to record a `vkCmdCopyBuffer` into the uniform
buffer, and MoltenVK turns each pass and each copy into a Metal encoder of its own.

Keeping the updates inside the pass removes those encoders. This branch first did it with a setting of its own,
`TOTK_UB_PATCH`, which wrote each update into the submission's uniform snapshot (`vk_usnap.c`), bound a patched copy to
later draws and batched the buffer copies where the pass ended anyway. In interleaved runs on Great Sky Island, with
other applications using the GPU: 23.9 and 24.3 fps without it, 33.4 and 36.6 with it; GPU time per frame 36-42 ms
against 11-17, submit to signalled 27 ms against 13. Upstream's `ub_overlay` (v0.3.12, pull request #743; on by
default on every platform) does the same with scratch overlays whose copies go behind one barrier at the next full
barrier (`vk_exec_draw.c` `vk_exec_ub_flush`), so the rebase onto v0.3.12 dropped `TOTK_UB_PATCH` for it. The branch
also carries upstream's #896 from `dev`, which orders partly overlapping overlay copies with a transfer barrier so the
newer update lands last. An hour of play on v0.3.12 with `ub_overlay` on (58 per-minute `frametime` lines in the log,
Hyrule and Great Sky Island, other applications open): a mean of 30 to 53 fps a minute, median 42, against the 33-37
fps `TOTK_UB_PATCH` gave standing on Great Sky Island; 50 of the 58 minutes had at most 4 frames over 100 ms, the
pipelines of areas seen before being built at start (6,434 in about 1.2 s after the device was ready). Not an
interleaved comparison, but nothing was lost by taking upstream's version.

Two pipelines failed in that hour, both depth-only (no colour target, `D16_UNORM`, triangles), with `-3`
(`VK_ERROR_INITIALIZATION_FAILED`) from MoltenVK: vertex shaders `80896ea6afda6580` and `9939bf5180be64a6`, fragment
shaders `6b102e6094e75417` and `3a664940fb78f69d`. The four modules pass `spirv-val`, the vertex shaders use no subgroup
operations, the fragment inputs match the vertex outputs, and the interlock capability they declare is declared by
every fragment shader in the cache. Both vertex shaders read a `uint4` attribute at location 6; Metal refuses a
pipeline whose vertex format cannot be read as the shader's type, which the failure line does not show yet.

Profile-guided optimisation does not help yet. An instrumented build (`-fprofile-instr-generate` on the generated code
and the runtime; `LLVM_PROFILE_FILE` with `%c`, continuous mode, because `plat_exit` skips the exit handlers that
would write the profile) trained on the same scripted run, and the build with `-fprofile-instr-use` gave 32.3-33.6
fps in two runs, against 32.7-33.5 for the normal build in two runs interleaved with them. The replay was 7.5-7.6 ms a
frame in both. The game's threads wait for GPU fences for about 21 ms of each 30 ms frame, so the CPU side is not
what limits this scene, and a faster CPU side cannot show. Single runs drift by up to 5 fps between sessions (26 to 33
fps for one build on the same day), so compare builds only in runs interleaved in one session.

## Status

The game builds and plays on this machine: title screen, saves, the opening cave ([First boot](#first-boot)), standing
still on Great Sky Island in scripted runs, and an hour of play on v0.3.11 from Great Sky Island down to Hyrule, Fuse and
its weapon icons included.

v0.3.7 ran the replay on its own thread by default. A scripted run on Great Sky Island with it on and off: 42.5 and
41.9 fps mean, 169 and 160 frames over 50 ms, no crash in either. v0.3.10 turned it off by default again for uneven
motion at the 60 fps cap on a GTX 1660 Ti (#659); as on and off measured the same here, macOS follows that default, and
the measurements in [Frame rate](#frame-rate) were taken with it on. A new translator changes every shader key, so the
first start after an update that changes it rebuilds every pipeline before the window opens: 2,668 in 132 s on v0.3.7,
3,678 in 410 s on v0.3.10; the next start took 1.5 s. On v0.3.10 the shader write merge (`wb_merge.comp`) runs in
mirror mode, 4-6 dispatches a frame on the title screen, which holds 59 fps.

On a heavier view of the island the frame rate is 27 fps after the changes in [Frame rate](#frame-rate), 23-25 before.

The way down to Hyrule stalled on pipeline builds, as any first visit does: in that hour 107 frames spent over 100 ms
building pipelines, the worst 1.6 s, and no pipeline failed. MoltenVK has no graphics pipeline library, so a draw whose
pipeline is missing builds it whole; draws into per-frame passes and full-screen draws are skipped meanwhile
(`pipe_skip = fullscreen`, the default), the others wait, and two background workers build them. Every new pipeline is
recorded in `pipeline-keys.bin` and built at the next start (4,505 in about 5 s), so a second visit is smooth.

`display = n` in `[display]` (`TOTK_DISPLAY`, `gfx_window_sdl.c` `move_to_display`) opens the window on the n-th display
of the log's `gfx: display` lines, and fullscreen then covers that display; without it macOS opens the window on the
display with the menu bar. A display above 60 Hz paces a 60 fps limit unevenly: set it to 60 Hz ("Pacing" above).

| Check | Result |
|---|---|
| `tools/build_game.py --version v143` | passes |
| `tests/plat_stress_macos.c`, `tests/intr_fault_macos.c` | pass |
| `--selftest`, `--probe-loader`, `--probe-kernel`, `--probe-fs`, `--probe-nvn`, `--probe-movie` | pass |
| `--probe-services` | 10 of 3,187 checks fail on v0.3.7: the 5.1 speaker texts (#438) have no Brazilian Portuguese or Chinese yet. Not a macOS difference |
| `--probe-crash-call`, `--probe-crash-null` | pass: `libsystem_platform.dylib+0xe32` for the fault in `strlen`, `outside every module` for the null call |
| `--probe-discord` | skips without a Discord client |
| `--probe-cfg`, `--probe-gfx-abi` (55 checks, write watch included), `--probe-vkdevice`, `--probe-nvn-tex` | pass |
| `--probe-vkmem` (41 GPU checks through imports, 0 validation errors) | passes |
| `--probe-window` | window, swapchain and present work; fails on +1 of 255 in each channel of the first frame's clear colour, also with the upscaler off and with MoltenVK fast math off or on. Cause unknown; invisible but unexplained |
| `--probe-astc` | passes, since the change to `trit_at` ([ASTC](#the-astc-compute-decoder-and-the-metal-compiler)) |
| `--probe-ipc` | passes, since the audio frame gap applies on macOS too ([What else the first runs showed](#what-else-the-first-runs-showed)). The `lm` fatal it prints is a deliberate check |
| `--probe-vkexec`, `--probe-shader` | need the shader census, not run |
| Windows and Linux builds | not rebuilt: there is no such machine here. The shared edits are behind `__APPLE__` or expand to the same text, but they need a build on each |

| Phase | Done when | State |
|---|---|---|
| 0. Assessment | requirements checked against this GPU, memory and fault costs measured | done |
| 1. Runtime builds | the runtime links and the game-free probes run | done, apart from `--probe-window` |
| 2. Tools | `tools/extract.py`, hactool, translator on macOS | done |
| 3. Shader census | every variant converts and compiles for Metal | not started |
| 4. Boot | title screen | done |
| 5. Play | a stable 60 fps in the open world | started: the opening cave runs |

Crashes in play on 2026-11-03, a few minutes in each, all on the same game thread (under `main+0xacf8dc` and
`main+0x27d2b2c`): a write through a garbage table pointer in `main+0xa2ea20`, a jump to a stack address, and a write
to a page that the instruction before had read and that was gone at the write. With `TOTK_ASYNC_PREEMPT=0` the game
played on, so the asynchronous preemption caused them; see [Thread interruption](#thread-interruption) for the race
and the fix.

A second macOS-only hazard turned up on the way and is fixed too: a thread-local is reached through a lookup call clang
treats as pure, so a function computes its address once and keeps it. `fiber_main` (`kernel.c`) kept `&tcur` in a
register across `rt_call`, the guest fiber's whole run, and a fiber that came back on another host thread would switch
fibers with the first thread's state. `PLAT_TLS_VAR` (`platform.h`) looks the address up at every use on macOS, for
`tcur` and `ipc.c`'s `t_req`; the other thread-locals are not used across a fiber switch.

The base game, a 1.4.3 update and keys are verified, the merged RomFS included ([Game files](#game-files)).

### Work left

In the order it is likely to pay off:

1. **Play past the cave.** Long sessions since the preemption fix, the surface and other sky islands, with
   `TOTK_TRACE=perf`. These runs give the first figures for texture streaming and written pages a frame in the world.
   Include a `TOTK_AUDIO_DUMP` where the game uses reverb (a cave, a shrine) to check the frame seams.
2. **ASTC while moving.** The GPU decoder runs now; measure what it saves while textures stream in, against
   `TOTK_ASTC=cpu`.
3. **Two depth-only pipelines MoltenVK refuses** ([Frame rate](#frame-rate)). Log the vertex attribute formats with
   the failure and get MoltenVK's own message (`MVK_CONFIG_LOG_LEVEL=2`, which goes to the terminal, not `totk.log`).
   Then an interleaved run against `ub_overlay=0` and a new Metal System Trace to count the encoders a frame now.
4. **Shader census.** Unblocks `--probe-vkexec` and `--probe-shader`, finds shaders Metal refuses before play, and lets
   `tools/prepare_shaders.py` build pipelines at install time.
5. **Windows and Linux builds** of this branch, before any of it goes upstream. CI runs only the content check.
6. **Profile-guided optimisation** again once the CPU side limits the frame ([Frame rate](#frame-rate)).
7. If a trace or compile failure points there: triangle fans as lists (`vk_exec_draw.c` `op_draw`), `Coherent` in
   `vk_pipeline.c` `spirv_fixup`, the title colour against the reference, `--probe-window`'s +1/255.
