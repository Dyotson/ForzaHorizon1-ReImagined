# ReXGlue SDK patches we carry

`patches/rexglue-sdk/` holds this project's patches to the ReXGlue SDK (nightly 0.10.0.24,
commit `bd833a2`); `tools/build_rexglue_sdk.sh` applies them to the clone
`~/rexglue-sdk-src-forza` and installs the patched GPU plugin, runtime and codegen tool into
the overlay `~/rexglue-sdk-patched-forza`. Patches 0001-0035 came from RecompiledHaloReach,
whose `docs/sdk_patches.md` describes each one (the numbers match; this repo leaves out
0010, 0012, 0014, 0024, 0026 and 0027). Patches written here (0036, 0038 and 0039 for both
projects, measured in this game's races; RecompiledHaloReach `docs/perf.md` has the profile; 0040, 0042, 0043, 0048, 0049 and 0050 are this
project's alone):

| Patch | Area | What changes | Why |
| --- | --- | --- | --- |
| 0086 | Startup pipelines (Vulkan, Direct3D 12) | Reports existing blocking warm-up progress. Vulkan keeps the driver's compiled pipeline data beside the shader description cache, validated and GPU/driver-specific, one entry per stored pipeline description: each pipeline is created with a `VkPipelineCache` of its own, seeded with its entry. An entry the driver did not use (pipeline creation feedback, or without it, data grown past 1.5 times a fresh entry) is rebuilt from an empty cache; entries of descriptions no longer stored are dropped; files of an earlier driver or game version on the same GPU are deleted. A save over 512 MiB is skipped with a warning. `include/rex/graphics/vulkan/driver_cache_file.h`, `tools/test_driver_pipeline_cache.cpp` | Makes startup preparation visible and lets the driver reuse locally compiled pipeline data. The first version seeded one cache with all of the last session's data and saved it back. Drivers never drop data from a cache, so every pipeline the driver did not find in it again was stored a second time at every start, and dead data stayed: about 16 MB more per start on an RX 6700 XT (AMD, Windows), until saves stopped silently at 64 MiB (#247). Only previously encountered pipelines can be warmed; see [Startup shader warm-up](shader_warmup.md) |
| 0102 | Pipeline store (Vulkan) | At start, when records were dropped from the pipeline description store (`<title>.fbo.vk.xpso`), it is rewritten with exactly the kept records, in order. Dropped: records the device cannot create (`ArePipelineRequirementsMet`), all records from the first one failing its hash, a partial last record. The log names what was dropped and why. The store is replaced through a temporary file (in place if none can be made). Its header (format 2, 20 bytes) holds the description and shader modification versions separately; a store with the original 12-byte header and the same versions is kept and converted. `include/rex/graphics/pipeline_store_file.h`, `tools/test_pipeline_store.cpp` | Upstream kept the device's records in memory, then truncated the file to their count: the rejected records stayed, and as many valid records as were rejected were cut from the end. Those are the newest pipelines: compiled again in play, appended, and cut again at the next start, until the cache was deleted (#247, #167). Any GPU change, driver with fewer features, or imported pipeline list triggers it. The original header holds only the larger of the two versions, so raising the smaller one went unnoticed. Direct3D 12 has no device filter: a damaged record ends its store and the file is cut there, which keeps every valid record, so its code is unchanged |
| 0091 | HDR presentation and HUD coverage | Opt-in scRGB swapchain, bounded SDR highlight expansion, pre-HUD scene snapshots, conservative HUD mask, calibration and diagnostic float readback; retains the semaphore lifetime handling from 0076 | Port the PinyonShift HDR presenter without a wholesale SDK/executor update; keep HUD white controlled (#33, [setup and validation](hdr.md)) |
| 0036 | Vulkan shared memory uploads | An upload of vertex or index data the CPU rewrote no longer ends the current render pass when no draw in it has read the uploaded pages: it is recorded before the render pass, and the uploads hoisted there share one barrier pair (`vulkan_hoist_shared_memory_uploads`) | Races at 144 ran 52,000 render pass instances and 133,000 barriers a second, nearly all for these uploads (10,600 and 27,000 with the patch) |
| 0037 | Video mode (runtime) | The guest refresh rate (`video_mode_refresh_rate`, its range and `VdQueryVideoMode`'s clamp) may go up to 480 Hz instead of 240 | 144 fps (and other rates above 120) can keep the game's divider of two: a 288 Hz guest vblank. With a 144 Hz vblank and no divider, the emulated GPU's swap waits (which end on a vblank, patch 0030) turn every frame that misses 6.9 ms into a 13.9 ms one: free roam made 72 fps at a 144 fps target. `docs/framerate.md` |
| 0038 | Vulkan submission | The deferred command buffer is replayed into Vulkan and submitted on a "Vulkan Submission" thread instead of the GPU command processor thread (`vulkan_threaded_submission`) | The replay took 11% of the command processor thread, which bounds the races above 60 fps |
| 0039 | Shared memory | `clear_memory_page_state` off by default, as in Xenia (`--clear_memory_page_state=true` restores the SDK's default) | On, every page the CPU wrote and the GPU read was uploaded again after every frame: 74,000 uploads (2 GB) a second in a race at 144, 1,800 without |
| 0040 | Keyboard and mouse (runtime) | The left stick's keys ramp (`mnk_stick_ramp`, default 0.2 s: 60 % at once, full lock over the ramp, released in half of it); the mouse drives the right stick by its speed, low-passed over the game's polls (1000 px/s is half the stick at sensitivity 1, five times the SDK's old scale); `Shift`, `Control` and `Alt` work as bindings on their own | Steering with keys was full lock at once; a mouse moved at normal speeds stayed inside the game's dead zone of the look stick; a key bound to a modifier never fired. `docs/playing.md` |
| 0042 | Vulkan render targets (debug) | `vulkan_render_target_initial_fill` (default 0, unchanged behaviour): 1 clears a new host color render target to zero before its first use, 2 fills it with magenta (64, 0, 64, 1); each fill is logged | A render target the guest never wrote holds whatever the driver's memory had; filling it shows whether the game reads such memory (driver-dependent flicker, #34; `docs/rendering.md` > "Screening for driver-dependent bugs") |
| 0043 | Presenter (runtime) | `present_effect` accepts `cas` and `fsr` (AMD FSR 1: EASU upscaling, repeated up to 4x, then RCAS) on Linux and Windows, Vulkan and Direct3D 12: `REX_HAS_FIDELITYFX_SDK` is defined for the whole SDK build, no FidelityFX library needed. `fsr2` / `fsr3` stay out unless the SDK is built with the FidelityFX runtime | The nightly SDK is built without the `REXGLUE_ENABLE_FIDELITYFX` option, so `present_effect` allowed only `bilinear` and the launcher's cas / fsr / fsr2 / fsr3 entries did nothing. The CAS and FSR 1 shaders are precompiled in the SDK source (`src/ui/shaders`); only the temporal path (FSR 2/3) needs the FidelityFX library (#44; `docs/rendering.md` > "Upscaling") |
| 0048 | Resolve readback | The readback buffers of a resolve are keyed by the written range plus the copy's source (depth or color) and destination format (format, number format, endian, swap, array/slice), not the range alone. Exponent bias, EDRAM base and size stay out of the key | Photo mode (#54): the saved photo is a 1280x720 color resolve into the range a depth resolve fills every frame. With the range as key, `some` treated it as an old key and never copied it back (the CPU encoded whatever an earlier copy left there), and `fast` handed the CPU the previous resolve of that range, the depth buffer. With the copy's kind in the key it is a first-seen key: copied back after a GPU wait. A key is first-seen whenever the range alone was, so everything `some` copied before (car thumbnails) is still copied; race fps at 144 unchanged (`docs/perf.md`) |
| 0049 | Draws (Vulkan, Direct3D 12) | The memexport readback check after a draw looks at the draw's memexport ranges first, and only then at the setting | The setting check looks the legacy setting up by name (the registry lock, two `std::string`s and a hash lookup) and ran on every draw, memexport or not: in a race 8-15% of the command processor thread, most of its `malloc` and `memcpy` (#45, `docs/perf.md`) |
| 0050 | Timer queue (runtime) | On Linux and macOS the timer thread blocks on a condition variable until the next timer is due or a new one comes (disruptorplus' blocking wait strategy, whose two timed waits passed their arguments in the wrong order and did not compile); its timer slack is 1 us. Windows keeps the spin strategy (whole-tick deadlines there) | The spin strategy polled between timers (pause loops, `sched_yield` 19 times in 20, then a 1 ms sleep): with the 1 ms timestamp timer, 5-7% of a core in a race, and up to 1 ms late in its sleep phase (#45) |
| 0051 | Pipelines (Vulkan) | New setting `sync_pipeline_creation` (default off): while set, a draw whose pipeline is a placeholder waits for the creation thread's real pipeline (up to 10 s) instead of being skipped, and new pipelines are created at once, whatever `async_shader_compilation` says. Direct3D 12 not covered | A render made once and read back loses every draw skipped for a placeholder for good: with an empty pipeline cache the car's picture saved at a purchase had no car (#74). The game sets it around the picture and the livery renders of a car load (`src/hooks/thumbnail_sync.cpp`, #77); nothing else uses it |
| 0052 | Textures (Vulkan, Direct3D 12) | New setting `scaled_resolve_min_coverage_percent` (default 50; 0 = old behaviour): with a resolution scale above 1, a texture is loaded from the scaled resolve buffer only if resolves wrote at least that share of its guest memory range, not on any overlap of one page | A resolve that overlapped only the last mip pages of a 1024x1024 DXT5 texture made the whole texture a "scaled resolve" texture, read from a buffer that is zero where nothing was resolved: black (#77). Resolve targets are covered by their own resolve and stay scaled |
| 0095 | Codegen: multiply-add | `fmadd`, `fmsub`, `fnmadd`, `fnmsub` and their single forms call `ppc_fma` instead of `std::fma`. It is defined in the generated `forza_pch.h` (template `codegen/_ppc_fma.inja`): the inline FMA instruction when the code is built with FMA; otherwise `std::fma` on a CPU with FMA (results unchanged) and a multiply, then an add, on one without (CPUID FMA and OSXSAVE, XCR0 AVX state; checked once at startup). `REX_PPC_FMA=0` forces the multiply-add, `REX_PPC_FMA=1` `std::fma` | On CPUs without FMA (Ivy Bridge and older, #160, #112) `std::fma` is a software routine: about 30 ns a call in the Windows CRT (1.4 ns with FMA), 9.6 ns in glibc. A race makes 30-40 million calls a second, most on the audio thread: the sound slowed down and frames stalled. The multiply-add may differ from the fused result in the last bit. The helper is in generated code, not the SDK headers, because Linux builds use the stock nightly headers |
| 0096 | Codegen: headers as build outputs | The codegen custom command in the `generated/rexglue.cmake` template also lists the generated `<project>_pch.h` and `<project>_init.h` as outputs | The precompiled header and `main.cpp` knew those headers only from their depfiles, so ninja judged them up to date before codegen ran. When a new codegen tool changed `forza_pch.h` (0095), it was rewritten mid-build and every source compiled after it failed: "forza_pch.h has been modified since the precompiled header was built". A second build worked. Codegen rewrites `generated/rexglue.cmake` from this template on every run, so the fix belongs in the template |
| 0097 | File system: host errors | `HostPathEntry::CreateEntryInternal` creates a guest directory with the `std::error_code` form of `create_directories` and logs a refusal (`cannot create directory '<host path>': <reason> (<code>)`); `NtCreateFile` then fails with `X_STATUS_ACCESS_DENIED`, as when the directory cannot be made for any other reason. `VirtualFileSystem::OpenFile` checks that a cached entry still exists on the host the same way, and keeps the entry if the check fails | The throwing forms let `std::filesystem::filesystem_error` out of `NtCreateFile` into guest code, which ended the game (#211: Windows, saving a photo). The photo's folder is named after the file name the player types. `NtCreateFile` accepts `:` in a name, and Windows refuses it in a directory name (`ERROR_DIRECTORY`); a host path past `MAX_PATH`, or a file where the directory goes, threw too. The game handles a failed `CreateDirectoryA`. `tools/test_vfs_host_errors.cpp` |
| 0101 | Racing wheels: the game's force feedback (#219) | The kernel's `XInputdFF*` exports (0x282-0x289, stubs before) play on the wheel driver's SDL haptics: `SetEffect`/`UpdateEffect` define effect blocks (constant force, spring, damper, inertia, friction), `EffectOperation` starts and stops them, `DeviceControl` stops, resets, pauses them or turns the actuators off, `SetRumble` drives the vibration, `GetDeviceInfo` reports 16 effects; scaled by `wheel_ffb_strength`, off with `wheel_ffb`, the steering force reversed with `wheel_ffb_reverse`. A spring on a wheel without a spring effect (Linux G25/G27/G29 driver) becomes its self-centring, taken from all the effect blocks so it drops to 0 when the spring stops, is reset or becomes another type. `XamInputGetUserVibrationLevel` answers 1 (0 with `vibration` off). `wheel_steer_offset` applies only with `wheel_900_steering` off. The G29/G923 map puts the paddles on the bumpers (`rb=4,lb=5`, circle B, square X). #256: the game's forces and rumble fade out (0.25 s) while the game says the player is not driving and back in (0.5 s) when they are, through the C export `ForzaWheelPlayerDriving(int)` (found at run time; until its first call nothing fades); `wheel_ffb_spring_strength` scales the spring and damper (and the autocentre made from the spring), `wheel_rumble_engine_strength` and `wheel_rumble_road_strength` the right and left motor, each in percent of `wheel_ffb_strength` (100 by default: as before); the buttons go by model (`ButtonLayoutOf`: the G920 and G923 for Xbox have Menu 6, View 7, RSB 8, LSB 9), and a button the shifter uses (`ShifterButtons`, wheel_shifter.h) presses no pad button whatever the defaults or `wheel_buttons` say. Effect decoding and state: `include/rex/input/wheel/wheel_ff.h`, buttons: `wheel_buttons.h` (no SDL; `forza-recomp/tests/wheel_ff_test.cpp`, `wheel_buttons_test.cpp`); `wheel_ff_probe.cpp` drives the driver with SDL on a uinput wheel (Linux), the fade and the spring strength included | Once the game recognises a wheel (the capabilities fix in `input_autopress.cpp`) it looks these exports up at run time (`XexGetProcedureAddress`, so they are not in the import table; `sub_831CD7F8` and its wrappers) and sends all its wheel forces through them (`sub_82CBB2D8`, `sub_82CB76B8`, `sub_82CBB790`, tuned by `media/ControllerFFB.ini`): stubs made the wheel go silent. The game's wheel layouts shift with the bumpers, and full-range steering has no inner deadzone for the offset to cancel: it only made the car twitch around the centre. #256 (a G923 for Xbox owner): in the pause menu the game sets its spring to full and keeps it and the damper on, rewinds play the steering force and rumble of the car going backwards and replays keep the last spring, so the wheel pulled and buzzed; the game (`forza-recomp/src/platform/wheel_bridge.cpp`) reports driving from its physics substeps, which stop in all of those (its rumble does not reach `XamInputSetState` with a wheel). Near the redline the engine motor drowned out the road, and the speed-dependent spring made small turns heavy. On the Xbox wheels the generic stick presses were the Xbox button and the shifter's reverse (11): reverse opened the friends screen. [Playing](playing.md#racing-wheels-issue-4) |
| 0103 | Audio output (runtime) | The SDL audio system creates a `FallbackAudioDriver` (private, `src/audio/sdl/`; `sdl_audio_driver.h` is unchanged). It asks for the default output with 48 kHz float 5.1 (as before), then the device's own format (`SDL_GetAudioDeviceFormat`: its own rate, so WASAPI initializes the shared stream with the mix format as it is instead of with automatic conversion), then 48 kHz float stereo and 16-bit stereo; each refusal is logged with SDL's reason, and the device's format once. The game's frames always enter the stream as 48 kHz float, 5.1 or stereo (`SDL_SetAudioStreamFormat`, in place of the close and reopen for a stereo device). After 0.5 s it asks once more. When nothing opens it logs `No audio output could be opened (<reason>); the game runs without sound.` and a silent output takes the device's place: a thread pulls the driver's unbound stream 1024 sample frames every 21.3 ms of the steady clock, so the same callback takes the game's frames and releases the client semaphore at 48 kHz, and every 5 s it asks for the device again (the device then takes over the frame queue). Without any SDL audio driver it runs silent without asking again. `REX_AUDIO_FAIL_OPEN=N` (or `all`) refuses the first N attempts, for tests | A device Windows refused to open (#244: `WASAPI can't initialize audio client`) failed `XAudioRegisterRenderDriverClient`, and the game stopped in `KeBugCheckEx` (exit 0x80000003): a device another program holds in exclusive mode, or a format WASAPI refuses with automatic conversion. Linux, forced refusals: boots to the menu, free roam and a race at 60 fps as with the device, 187.5 audio frames a second, the silent thread 0.03 % of a core; with the device unchanged. `forza-recomp/tests/overlay-sdk/audio_output_fallback_test.cpp` |
| 0104 | Vulkan device loss and presentation stalls: diagnostics (#221) | A `VK_ERROR_DEVICE_LOST` from the command processor's submission fence check is logged with the submission and frame, and the device-loss handler logs `Graphics device lost` at critical level before it stops the game (both went only to the abort, whose message reached stderr alone). `vkAcquireNextImageKHR` and `vkQueuePresentKHR` calls that block for 1 s or more, command-processor waits of 2 s or more for the GPU, and window focus, minimise and restore changes are logged. `FORZA_TEST_GPU_LOSS_AT_FRAME=<n>` reports a device loss from frame n on, to test that path | #221 aborted with nothing about it in forza.log. The stack shows the NVIDIA driver reporting the device lost to the fence check at the start of a frame, under a second after a 5.6 to 6.5 s stall with the game in the background (twice); a lost Vulkan device can't be used again, and the SDK can't create a new one. The next report shows whether presentation or the GPU stalled, and when focus changed |
| 0105 | XMA decoder: unreadable input and locked contexts (#228) | `XmaContext::Work` checks again under the context's lock that the context is still allocated and enabled, and `Release` drops a kick not yet worked. An input buffer marked valid with no address, no packets or packets past the end of guest physical memory, or a read offset past its last packet, is dropped as consumed. A frame size shorter than the 15-bit frame header, or longer than the two packets the decoder copies, skips to the first frame of the next packet. Each of these is logged once. A decode pass that reads no input and writes no output ends the loop. `BitStream::Copy` copies fewer bits than are left in the first byte without wrapping around, and never copies past the end of the stream. `tools/test_xma_invalid_input.cpp` (Linux SDK CI job) | The game crashed during the opening drive with "read fault at host 0x0" in `memcpy` under `BitStream::Copy` and `XmaContext::Decode`. FFmpeg had just rejected a frame ("would have to skip"), so the read offset was off the frames. The next frame size read can only have been under 8 bits: `Copy`'s unsigned bit count wrapped around, and `memcpy` got 2^61 bytes. The other cases made `Work` loop forever on the game thread that kicked the context, or read past guest memory. The check under the lock closes a race: a `Work` that saw the context enabled, then waited for its lock while the game locked it (`Disable`, then `Block` until the lock is free), decoded it while the game rewrote it, and wrote stale read offsets over the game's. [Details](#xma-decoder-crash-228) |
| 0112 | Vulkan render target memory (#241) | The Vulkan render target cache counts the device memory of its images. At a frame end, every 30 frames, the command processor reads the device-local usage and budget (`VK_EXT_memory_budget`); at `vulkan_render_target_trim_percent` (default 90, 0 = off) of the budget, or after a render target image or scaled-resolve memory allocation failed, it waits for the GPU and frees the cached render targets that hold no EDRAM data (the existing `ClearCache` rule; render passes are kept) in both render target caches, then waits at least 5 s (1 s after a failure, 30 s when under 256 MB was freed). `ClearCache` also drops the accumulated render target bindings, which pointed at freed objects. Policy in `src/graphics/vulkan/fh1_render_target_trim.h`, `tools/test_render_target_trim.cpp` (Linux SDK CI job) | The cache kept one image per EDRAM layout for the whole session, about 90 MB each at render scale 3: 11.4 GB after the paint shop, 13.2 GB after switching decal sides (RX 9060 XT, 720p). A player at 4K and scale 3 (16 GB) ran out after two hours: render targets and scaled resolves failed and a car's picture was saved from stale memory |
| 0113 | Shader prebuild (Vulkan, #226) | At start each Vulkan pipeline cache also reads the launcher's "Prebuild shaders" files in `<cache root>/shaders/prebuild/`: `<title>.<fbo|fsi>.vk.xpl`, the pipelines of the shipped list (format in `include/rex/graphics/pipeline_prebuild_file.h`: header with the render target path and both description versions, FNV-1a checksum; per pipeline the shader hashes and modifications, the render partitions, and the 58 decoded bit fields by name, one byte each), and `<title>.xsh`, the shaders the launcher rebuilt from the player's own game files. It keeps the pipelines of its render partition (`main`, or `ui` and `scene` with split rendering), drops the FH1 fractional raster variant (modification bit 63) when its raster is not scaled, values outside an enum, records `ArePipelineRequirementsMet` rejects and those whose shaders are missing, and adds the rest, assembled field by field, to the stored descriptions before translation: the warm-up creates them with the stored ones and their driver data (0086) is kept. They are never written to the store. A list for other versions or the other path, or damaged, is ignored with a warning. `shader_prebuild_only` exits after the warm-up (driver data saved) before the guest starts: `SHADER_PREBUILD: complete, exiting`. The partition comes from `VulkanCommandProcessor::InitializeShaderStorage` (`SetShaderPrebuildSource`, plugin-internal `pipeline_cache.h`). `tools/test_pipeline_prebuild.cpp` | Pipelines the player has not met yet are compiled when first drawn: new places, cars and cameras hitch ([shader prebuild](shader_prebuild.md)). Stored records are compiler-specific (MSVC packs the description in 60 bytes, Clang/GCC in 58), so the shipped list has decoded fields and the test checks the assembled bytes in both layouts. The list holds no game data; the shader microcode comes from the player's files. The render pass key holds the guest's depth format on every device (`depth_format` comes from the render target's key), so D24S8 and D24FS8 records need no remapping |
| 0114 | Waits (runtime, Linux and macOS) | A thread blocked on several handles, or in an alertable wait, registers itself with each handle it waits on and with its own user-callback queue: a state change wakes only the threads registered with that handle, and a queued user callback (APC) wakes the thread it is queued to. Alertable waits and alertable sleeps block until then instead of re-checking every 1 ms. A zero-timeout alertable wait looks at the handle before it times out. Each handle's list of waiting threads has its own lock, so a thread that exits inside such a wait (`Thread::Exit` from a user callback) leaves the lists without the handles' locks: a `WaitAll` on that thread and one of its handles holds those while it joins the thread (the first version hung there, #286). Replaces 0033's process-wide condition variable. `tools/test_wait_wakeups.cpp` (Linux SDK CI job, the exit case included) | With 0033 every signal in the process (about 4,000 a second in free roam, 5,000 in a race) woke every thread in a multi-handle wait: the SDK's Audio Worker and two of the game's audio threads woke 4,000-6,000 times a second each, and every alertable wait (five threads) 1,000 times a second, for nothing. On four cores those wakeups queue up behind the game's threads (#45, #263). [Details](perf.md#low-end-cpus-45-263-268-2026-10-22) |
| 0115 | Command processor writes to guest memory (Linux, Windows) | After each word the command processor writes to guest memory (the scratch register and read pointer write-backs, `MEM_WRITE`, `COND_WRITE`, `EVENT_WRITE_SHD`, `REG_TO_MEM`) it wakes the threads blocked on that word: a futex wake on Linux, `WakeByAddressAll` on Windows, at the word's address in the physical view (`TranslatePhysical`) | The game's waits for the GPU poll such a word; `src/hooks/gpu_wait.cpp` now blocks on it until it changes instead of waking every 50 us (Linux) or yielding without pause (Windows; only with the setting `gpu_wait_blocking`, off by default there until tested). A futex wake that finds no waiter costs one system call; `WakeByAddressAll` without waiters does not enter the kernel. [Details](perf.md#low-end-cpus-45-263-268-2026-10-22) |
| 0116 | Controller remapping (SDL, #279) | `controller_remap` (new cvar, written by the launcher's Controls page: Bindings): profiles separated by `;`, each `GUID:target=source,...`. A pad uses the profile with its SDL GUID, else one of the same model (vendor and product from the GUID, so USB and Bluetooth share it), else `*`. Targets are the XInput controls (`a b x y lb rb back start guide ls rs up down left right lt rt lx ly rx ry`); a target left out keeps its own input. Sources: a pad button (also `misc1`, `paddle1`-`4`, `touchpad`) or `none`; `lt`, `rt`; a stick axis (`-lx` inverted) or one direction of it (`lx+` right, `ly+` up), `@0.3` for how far an axis must move to press a button (default half way). Button to button, button to trigger (a full press), trigger or stick direction to trigger, axis to button past the threshold, stick axis to stick axis (swaps, inversion); other pairs are dropped with a warning. A pad with a profile keeps its raw SDL state (by joystick id, under the controllers lock) and its XInput state is rebuilt from it on every event (`src/input/sdl/gamepad_remap.h`, no SDL; `tools/test_gamepad_remap.cpp`, also in CI); held inputs are read when it connects. Without a profile the driver is unchanged, and no rules give exactly the old mapping (tested over random states). Wheels keep their own driver. No public header changes | An SDL gamepad went straight to the game's XInput state: a player could not put the throttle on A or swap the sticks. [Playing](playing.md#remapping-a-controller-279) |

0037 is in `librexruntime.so`, so the overlay carries a patched runtime (as for 0028).
Without it the game falls back on its own: `src/hooks/frame_time.cpp` reads the setting's
range and uses a divider of one when twice the frame rate does not fit.

0044 fixes native clang-cl SDK builds on Windows. 0045 records the Vulkan
presenter's output-pipeline format so an unchanged swapchain does not rebuild
that pipeline every paint.

0047 uses hardware implicit LOD for ordinary automatic cubemap samples on NVIDIA,
preserving mip bias and filtering. The captured traffic-car reflection shader
produced a checker pattern with manually emitted coarse gradients; implicit LOD
matched the reference result. Explicit guest gradients and LOD, and other GPU
vendors, retain their existing path. See [Rendering issues](rendering.md#nvidia-car-reflection-sampling-2026-10-15).

0064 flushes pending Vulkan shader/pipeline storage writes before sleeping and drains queued records on shutdown. See [Performance](perf.md#flush-pending-vulkan-shader-cache-writes-before-waiting-49).

0065 skips repeated Vulkan GPU-to-host resolve work after a successful CPU copy in `some` mode, preserving the first copy and cache eviction. Other modes are unchanged. See [Performance](perf.md#nvidia-vulkan-resolve-readback-overhead-49-2026-10-15).

0063 adds native ultrawide rasterization, proportional HUD layout/edge anchors and square reflection cube faces. It applies independently of DLSS; see [Ultrawide](ultrawide.md).

0053 refreshes known reused car-thumbnail readback surfaces on Vulkan; see
[Rendering issues](rendering.md#reused-car-thumbnail-readbacks-vulkan).

0067 enables the SDK's existing direct clock-conversion fallback on Linux libc++
when C++20 clock conversions are unavailable, using the standard-library feature
macro rather than an Apple-only platform check. Native libstdc++ and MSVC
conversions remain unchanged.


0055 preserves the source stencil value when copying depth/stencil ownership
without shader stencil export. Previously the stencil-only shader skipped its
bit test and filled the destination with 255, rejecting dynamic shadows (#75).
See [Rendering issues](rendering.md#missing-dynamic-shadows-in-the-stencil-copy-fallback-75).

0068 binds a window of the scaled resolve buffer, not the whole buffer, for the readback downscale
compute pass when the buffer (512 MB x scale^2) is larger than `maxStorageBufferRange`. At render
scale 3 on AMD the whole-buffer binding was invalid and the car thumbnail was saved black (#102).
Smaller buffers (scale 1 and 2 on AMD) are unchanged. See
[Rendering issues](rendering.md#black-thumbnail-at-render-scale-3-102).

0069 limits the default Vulkan frame queue to two frames on NVIDIA, retaining
three on other vendors. This shortens blocking resolve-readback waits without
skipping CPU copies or changing readback-cache eviction. The three-slot resource
ring and fence-based retirement remain in place. Set
`vulkan_max_frames_in_flight=3` to restore the previous queue depth; nonpositive
values select the vendor default, and positive values are limited to 1-3.
See [Performance](perf.md#nvidia-readback-cache-miss-stalls-49-2026-10-16).

0070 adds FIFO_LATEST_READY (`VK_EXT_present_mode_fifo_latest_ready`, or its KHR promotion) to the
Vulkan presenter: enabled with the `presentModeFifoLatestReady` feature when the device has it, and
chosen over plain FIFO when the surface offers it (after mailbox, immediate and relaxed FIFO, which
keep their cvars). New cvar `vulkan_allow_present_mode_fifo_latest_ready` (default true); false or an
unsupported driver gives plain FIFO exactly as before. The chosen mode is logged once. It evens out
the frame pacing of FIFO at high frame rates on drivers without MAILBOX (#80). See
[Frame rate](framerate.md).

0066 averages supersampled RGBA8 car-thumbnail pixels during readback, keeping other formats and resolves unchanged. Requires the known-thumbnail refresh rule already on main (0053); see [Rendering issues](rendering.md#filter-supersampled-car-thumbnails-50).

0072 adds the AMD FidelityFX FSR 3.1 upscaler (no frame generation) as a second backend of
0046's scene/UI split: `fh1_upscaler=fsr3_quality|fsr3_balanced|fsr3_performance|fsr3_native`
on any Vulkan GPU. CMake fetches the FidelityFX SDK v1.1.4 GLSL passes and headers (MIT; pinned
commit and per-file SHA-256; `REXGLUE_FSR3_SDK`, `REXGLUE_USE_FSR3`) and embeds them; the
renderer compiles them with its glslang at start-up (`CompileFh1ComputeReflected`, with an
include handler and reflection for the bindings) and `fh1_fsr3.cpp` dispatches the SDK's pass
sequence. The DLSS state (inputs, depth/motion pass, jitter, status file) is no longer NGX-only;
jitter uses FSR's phase count in FSR modes. The MIT notice is installed under
`share/licenses/amd-fidelityfx-fsr3` and copied beside the GPU library. See
[Rendering issues](rendering.md#temporal-upscaling-with-amd-fsr-3-51-2026-10-16).

0073 raises the FSR 3 and DLSS scene input to at least the game's native 1280x720 (never above the
output), logged once: FSR 3 Performance at a 1080p output (960x540) crashed a player's game (#110).
See [Rendering issues](rendering.md#temporal-upscaling-with-amd-fsr-3-51-2026-10-16).

0046 adds native Windows Vulkan DLSS/DLAA with separate scene/UI rendering.
See [Native DLSS](native_dlss.md) for runtime setup, licensing and validation limits.

0057/0058 keep DLSS viewport jitter consistent across EDRAM tiles and rotate
depth, motion vectors and NGX jitter into the final color orientation. 0062
synchronizes unscaled scene resolves into the scaled native HUD cache (1440p
Performance uses a 1x scene and 2x HUD). All three belong to the DLSS integration
and apply without the ultrawide patch series.

0071 loads eligible scaled, tiled 32-bit textures directly from the scaled resolve
buffer into an R32_UINT storage view on NVIDIA. The shader preserves the original
texel bits and endian conversion, while removing the intermediate untile buffer
and buffer-to-image copy. It handles single-layer 2D base levels only; other
formats, mip chains, arrays, 3D layouts, unsupported storage formats, and other
vendors retain the original path. Image allocation/view failures also fall back.
`vulkan_direct_scaled_texture_upload=false` restores the original uploads. The
storage descriptors use the existing frame retirement rules, and image writes
are ordered before subsequent sampling or uploads. See [Performance](perf.md#scaled-texture-upload-cost-101).

0074 adds a racing wheel input driver (`src/input/wheel/`, issue #4). An SDL joystick that is a wheel (SDL
type, a wheel-like name, or any controller without a gamepad mapping with `wheel_mode = on`) is presented as
the Xbox 360 Wireless Racing Wheel: SubType 2 with the force feedback and wireless flags (FH1 checks exactly
that when it reads the capabilities), steering on thumb LX, pedals on the triggers, buttons and hat on the pad
buttons. Known Logitech wheels (by VID:PID) get fixed axes, pedal direction and, for the G29/G923, buttons; other wheels have their axes detected from their resting values (a pedal counts only after it has been seen at rest), with overrides and curves (`wheel_*` cvars). `wheel_steer_offset` pre-compensates the game's inner steering deadzone. The
motors' vibration drives SDL haptic effects (a spring and a sine): approximate, untested on hardware (0101
plays the game's own wheel forces instead once the game recognises the wheel). The
patch also turns on SDL's haptic subsystem in `thirdparty/CMakeLists.txt` (it was off) and the SDL gamepad
driver ignores wheels the wheel driver takes. Local builds need `libudev.h` (Homebrew `systemd`, added to
`tools/build_rexglue_sdk.sh`), otherwise SDL is built without udev and sees no joystick at all.
Test: a uinput virtual wheel; see [Playing](playing.md#racing-wheels-issue-4).

0099 adds the clutch pedal and a shifter to that driver (issue #125). FH1 already reads both from a 360 wheel:
the capabilities (`sub_82CBAFE0`) mark an H-pattern shifter when thumb RX is neither 0 nor 1 (its high byte is
the number of forward gears), the car's input frame (`sub_824B7360`) then takes the gear from the state's thumb
RX through control 7 (`sub_82CBC320`: -1 reverse, 0 neutral, 1 to 8), which the wheel layouts bind to it
(`sub_82598A38`), and the wheel layouts in `gamedb.slt` (`WheelLayouts`) put the clutch on the right stick Y. So
the clutch pedal goes to thumb RY (`wheel_clutch`, `wheel_invert_clutch`), and with `wheel_shifter = h` the
held gear button to thumb RX and the capabilities report the shifter; no gear button is neutral. The gear
buttons default to the Logitech Driving Force Shifter by model (`DefaultShifter`, by vendor:product): gears 1-6 on
12-17, reverse on 18 for the G29 and the G923 for PlayStation (046d:c24f, c266, c267) and on 11 for the G920 and
the G923 for Xbox (c262, c26e, c26d; #256: they number reverse before the gears, as PCGamingWiki's button tables
and a G923 for Xbox owner show), and are set with `wheel_shifter_buttons`; `wheel_shifter_clutch` / `wheel_clutch_threshold` engage a new gear only with the
clutch pressed; `wheel_shifter = sequential` adds up/down buttons as the bumpers. Clutch and gear are sent only
after the game itself read the capabilities (its flags are not 0; the SDK's own deadzone lookup passes 0), so a
game that took the wheel for a gamepad does not get them as right stick movement. The gear logic is in
`include/rex/input/wheel/wheel_shifter.h` (no SDL), tested by `forza-recomp/tests/wheel_shifter_test.cpp`.
Test: a uinput G29 with shifter and clutch in the game (gears, reverse, neutral, clutch, realistic and sequential
modes); see [Playing](playing.md#racing-wheels-issue-4).

0075 adds an atomic Vulkan graphics-settings transaction. A complete request
is consumed at a closed frame, after pending submissions finish. Scale-dependent
render targets, textures, reconstruction and pipelines are replaced together;
the previous configuration is restored if initialization fails. Active shader
microcode is transferred before releasing the old cache, and shader storage is
partitioned by output and scene scale. Native scene scaling is independent of
the HUD's output size. Scene-to-HUD blits use the actual raster sizes, including
fractional 1080p output within 2x backing storage. NVIDIA device extensions are
requested when the device is created so Native sessions can enable DLSS later.
The vblank worker observes refresh-rate changes without restarting. The native UI is added separately.

0076 initializes DLSS on a private GPU submission before rendering game frames,
orders NGX scratch/history accesses, enables buffer device addresses when available,
and ties presentation semaphores to swapchain images. See
[Rendering](rendering.md#vulkan-dlss-initialization-and-presentation-lifetime).

0077 adds SMAA 1x to the FH1 scene/native-HUD bridge. It fetches pinned, hash-checked
MIT sources and lookup tables from iryoku/smaa, packages the license, and compiles
three Vulkan compute passes. SMAA bypasses NGX and temporal jitter/history. The
live graphics transaction accepts the new scene mode and suppresses final-image
FXAA while scene AA is active. Optional diagnostics log guest MSAA sample counts
without changing the guest registers or render-target layout.

0078 lets passive ImGui dialogs opt out of continuous UI repaints. The performance
overlay draws with game frames; interactive dialogs continue to request redraws
for input and animations. This prevents the FPS display itself from driving the
presenter at a higher rate than the game.
The hint lives in a private runtime side table, set through the optional C export
`rexui_SetDialogContinuousRepaint` on the UI thread and forgotten when a dialog
is removed or its drawer is destroyed. Neither ImGuiDialog nor ImGuiDrawer gains
a public member or virtual function. The game compiles with the stock nightly
headers; older runtimes without the export safely retain continuous repainting.
Linux SDK CI compiles the actual HUD adapter against those stock headers, even
on an SDK cache hit, and tests passive/mixed dialog policy and hint cleanup.

0079 registers generated functions by iterating the existing null-terminated
`PPCFuncMappings` table rather than emitting one virtual call per guest function.
AppleClang's ARM64 SelectionDAG construction spent several minutes on the base
game's 77,344-call straight-line routine, including with optimization disabled.
The table has the same filtering, addresses, targets and order as the old calls
(all 77,344 entries compared equal in the base build). The generated DLL entry
points retain their exported names and use each module's own mapping table.

0080 finalizes the SPIR-V rectangle fallback's next-program-counter `OpPhi` after
emitting its next-guest-vertex path. Previously that path appended its `(0, block)`
operands after the instruction had already been built, so shaders with guest
control flow lacked a predecessor in their Phi. On devices without geometry
shaders, Forza's tone-mapping rectangle was lost: scene textures contained the
colored world, but the screen showed only the HUD. The M4 Pro opening drive renders
after the fix and responds to throttle and steering. The geometry-shader path is
unchanged. See [macOS validation](macos.md).

0081 adds an opt-in Vulkan sampler fallback, `vulkan_force_base_mip`. It clamps
2D texture sampling to mip 0 and disables anisotropy. It remains available for
diagnostics, but is no longer enabled by the macOS play script: patch 0082 removes
the tested crosshatch without sacrificing mip filtering.

0082 allows arithmetic contraction in Apple GPU color shaders, following the
contraction portion of [Pinyon Shift's macOS pixel-math tuning](https://github.com/arcanite24/shiftglue-sdk/commit/524c6fb61cf439ac54664a4bccf5eeaf8784ba9c).
It retains this SDK's existing SM3 multiplication rules. Vertex shaders, shaders
writing depth, memory exports and synthetic depth-only shaders stay precise.
`spirv_pixel_shader_contraction` defaults on for macOS and is gated to Apple GPUs;
it requires a restart. Its value is part of the shader modification key, and the
pipeline storage version is advanced. On M4 Pro, an on/off TU4 comparison changes
crosshatched Viper stripes and roughly 21–24 fps to solid stripes and the 30 fps cap,
with normal mipmaps and anisotropy. See [macOS validation](macos.md).

A trial backport of ShiftGlue's 4 KB shared-memory tracking is **not included**.
It passed isolated neighboring/direct/cross-page invalidation checks but caused
HUD corruption in both game builds with this older Xenos renderer. The source,
regression helper and captures are retained in the ignored validation directory;
see [the Pinyon comparison](macos.md#pinyon-shift-comparison).

0083 requests Cocoa fullscreen after showing the SDL window. The request made
while the window was hidden left the tested app windowed despite `fullscreen=true`.
It preserves the requested state before creating/showing the window, since
visibility notifications can publish the initial windowed state synchronously.
It raises the visible window before entering fullscreen when fullscreen was requested.
The local app bundle now enters a native fullscreen Space on launch; macOS's
GamePolicy log confirms that Game Mode becomes active. Other platforms retain
their existing window-creation order.

0084 adds experimental Apple MetalFX temporal and spatial scene reconstruction.
It exports Vulkan images as Metal textures, imports two Metal shared events as
Vulkan timeline semaphores, and splits the Vulkan command buffer around the
native Metal pass. A separate Metal queue avoids waiting behind MoltenVK work
that depends on the upscale. There is no CPU texture readback or per-frame CPU
GPU wait. The existing game-derived depth/motion decoder and projection jitter
feed temporal reconstruction; the HUD renders afterward at 1440p. Descriptor
sets rotate across frames, and dynamic graphics state is rebound after the
command-buffer split. Ordinary submissions keep their allocation-free path.
Mac command pools reserve the continuation buffer before MetalFX is active, so
the shared graphics transaction can enable either MetalFX mode during native
play. Unsupported output sizes are rejected before replacing render caches.

The native bridge is built only on macOS. Unsupported initialization reports an
error and retains native rendering. MetalFX output is restricted to the existing
integer HUD scales; a 1080p trial cropped the UI and is rejected. A register-level
MSAA reduction trial corrupted reflections/vegetation and is not included.
See [MetalFX usage and measurements](macos.md#apple-metalfx).

0085 copies packed scene/HUD resolve texels as whole words. Patch 0100 below
corrects its original storage-layout assumption. The common 1x-to-2x
path repeats 8/16-bit texels with masks and shifts and 32/64/128-bit texels with
word loads. Other integer scales use the same nearest-sample mapping; unaligned
sources retain the original byte path. The diagnostic flag
`fh1_fast_resolve_copy=false` selects the original algorithm. This changes no
MSAA, mip filtering, shader precision, or scene/HUD resolution.

Reference comparisons covered 270 format/scale/alignment combinations. A live
TU4 MetalFX Spatial session alternated the original and optimized algorithms:
the original averaged 40.4–41.8 fps and the optimized windows 49.4–54.8 fps.
The world, HUD and solid stripes rendered correctly. These were background
opening-scene samples with concurrent media analysis, not verified 1440p/60
foreground performance. See [1440p target](macos.md#1440p-at-60-fps-target).

0090 adds Xbox impulse trigger output to the SDL input driver. The optional C
export `ForzaInputTriggerRumble(user_mask, left, right, duration_ms)` routes only
to SDL Xbox One/Series devices that advertise trigger rumble, through the same
device assignments and SDL gamepad handles as normal game input/rumble. It uses
`SDL_RumbleGamepadTriggers`, leaving body motor state intact. Calls respect the
vibration setting, UI input blockers and focus, and clamp effects to 150 ms.
Two nonvirtual methods are added; class layouts and virtual tables are unchanged.
The game discovers the export dynamically, so an older runtime still loads and
the launcher explains that an update/rebuild is required. Patch number 0090 avoids the numbers reserved by the graphics, macOS and warm-up PRs. See [telemetry](telemetry.md#xbox-impulse-triggers).

Windows validation: both modified SDK sources compiled; an isolated runtime DLL
linked and exported the entry point. A probe with real SDL virtual controllers
verified trigger amplitudes, routing to guest users, unsupported-device rejection,
body rumble preservation, UI/focus/master-vibration gating and timeout. Physical
USB, Bluetooth and Xbox Wireless output and Linux builds remain untested.

### Photo directory failures (#211)

0097 keeps host directory-creation errors inside the existing guest filesystem
failure path. `HostPathEntry::CreateEntryInternal` previously used the throwing
`std::filesystem::create_directories` overload. A host filename rejected by Windows,
a file occupying the directory name, or access denial could therefore unwind out
of `NtCreateFile` and abort the process. The error-code overload logs the path and
host error, then returns null; `VirtualFileSystem::OpenFile` already translates
that failure to `X_STATUS_ACCESS_DENIED`. No exception catch or success fallback
hides a failed save. The patch also makes cached host-existence checks non-throwing.

Reproduced on Windows/D3D12 with a copied profile and an existing private TU4
development host: take a normal-size photo and save it as `invalid:Photo1`.
A private naming-dialog fixture supplied the text through the runtime's usual
asynchronous completion; the baseline filesystem code was unchanged. The process
aborted with `lr=831AD110` and all eleven guest return addresses matching the
reporter's log. The reporter's actual filename and host error are unknown, so
the colon is a demonstrated trigger rather than a claim about their input.

With the candidate, that same native D3D12 save reports **FILE OPEN FAILED** and
returns to the pause menu after dismissal, instead of aborting. A normally named
photo on Vulkan reports **SAVE SUCCESSFUL** and writes its image, thumbnail and
metadata to the copied profile. The source fixture's hashes are unchanged. These
are development-host functional checks, not a full rebuild of current main, a
photo-quality assessment, a large-photo upload test, or a reporter retest.

`tools/tests/sdk-filesystem` builds the real-runtime test from
`tools/test_vfs_host_errors.cpp` on Windows, in addition to #213's Linux coverage.
The Windows SDK job runs it before packaging and caching the overlay. Full CI
builds and reporter retesting remain necessary.

### Cross-scale scene/HUD resolve layout

0100 corrects the scene/HUD resolve mirror to resample the SDK's repeated
16-byte storage quads with the resolve's byte order. It supersedes the
individual-pixel mapping in 0046/0085, retaining whole-word loads where safe and
the `fh1_fast_resolve_copy` diagnostic switch. Both paths pass the standalone
Vulkan reference matrix, including transfers to/from 3x. See
[cross-scale minimap corruption](rendering.md#cross-scale-minimap-corruption-at-3x).

### XMA decoder crash (#228)

The reporter's game stopped during the opening drive with "Unhandled read fault at host 0x0"
in libc's `memcpy`, called from `BitStream::Copy` in `XmaContext::Decode` (Arch Linux; it
did not happen again on a second try). It was not a null input buffer. `Copy` reads from the
decoder's own two-packet copy of the input, and `TranslatePhysical` cannot return null: all
512 MB of guest physical memory is mapped. The reporter's libc (Arch's glibc 2.44 package of
27 September, whose signal return is at +0x3E6F0 as in their stack) faults at +0x17658C, on
`vmovdqu -0x20(%rsi,%rdx,1)` in `__memmove_avx_unaligned_erms`. That instruction loads the
last 32 bytes of the source, after the loads from its start have succeeded. With a length
near 2^64 the address is not canonical, and Linux reports that kind of fault (a general
protection fault) at address 0.

`Copy` makes such a length when it is asked for fewer bits than are left in the first byte:
`bits_left -= 8 - rel_offset_bits` wraps around, and `bits_left / 8` is 0x1FFFFFFFFFFFFFFF.
`Decode` passed the frame size from the bitstream through unchecked, so a frame size of 1 to
7 bits at an odd bit offset crashes. Real frames are far longer. In the reporter's log,
FFmpeg rejects a frame ("would have to skip") in the same millisecond, on the same thread, so
the read offset was no longer at a frame start.

What moved the read offset off the frames is not certain. The likeliest cause found is a race
in `Work`, which checked `is_enabled()` before taking the context's lock and not after. The
game locks a context before it rewrites it: the lock register calls `Disable`, then `Block`
waits until no `Work` holds the lock. A `Work` that was already waiting for the lock got past
`Block`, then decoded the context while the game rewrote it, and wrote its stale read offset
and current buffer over the game's. The race is frequent in this game, where three threads
lock contexts and at least two of them kick contexts too. A diagnostic build counted this
check catching more than 512 `Work` calls in the first four minutes of the opening drive. Of
the first 20, 12 were on the decoder's worker thread and 8 in a kick's own `Work` on a game
thread: in between, another thread had locked the context or the worker had done the kick's
work. Unpatched, each of them decoded the context anyway.

`tools/test_xma_invalid_input.cpp` runs `XmaContext::Work` on guest memory of its own. With a
thread polling `Work` as `XmaDecoder`'s worker does, the unpatched runtime wrote a context the
test had locked 7, 44 and 504 times in three runs of 20,000 lock cycles. Patched, it never
did. The test also feeds `Work` input it cannot read: a buffer marked valid with address 0,
with no packets, or with packets past the end of guest physical memory; a read offset past
the buffer's end; frame sizes of 1, 3, 6 and 14 bits; and a frame longer than two packets.
Unpatched, the 1-bit frame faults in `memcpy` with a length of 0x1FFFFFFFFFFFFFFF, the
reporter's crash. No packets, or a read offset past the end, make `Work` loop forever on the
kicking thread, logging "Invalid packet index" millions of times. Packets past physical
memory fault in `memcpy` too. Patched, each one is dropped or skipped with one warning, and
nothing is decoded from it.

Game runs on Linux (AMD, RADV), Title Update 4, offline, with scratch profiles and
`SDL_AUDIO_DRIVER=dummy`. The private runtime was the SDK build of main's patch set without
0101 (wheel force feedback), plus 0105 and #252's 0103. The SDK's SDL has no dummy driver,
and without 0103 the game stops at start when no audio output opens.
- The opening drive on a fresh profile: title screen, video calibration, main menu, the
  festival video, the Viper cinematic, then 2 minutes at full throttle. There was no fault,
  no decoder warning and no FFmpeg error. The warnings matched an unpatched run of the same
  script, apart from an ambient sound bank of the place where the car ended up. The game
  thread that kicks and locks the XMA contexts used 5.0% of a core during the video and the
  cinematic, patched and unpatched; 14.2% and 13.0% during the drive, which took different
  routes.
- Free roam on the user's copied save, 3 minutes of driving: no fault and no decoder
  warning.

The unpatched run did not crash either, and the crash was not reproduced in the game: it
needs a stale decode to leave the read offset on a frame size under 8 bits. Reporter retesting
remains necessary.

0112 frees cached Vulkan render targets when device memory runs low (#241). Each EDRAM layout
the game draws to gets its own render target image, kept for the session; an image covers the
EDRAM from its base for its pitch, about 10 MB at scale 1 and 90 MB at scale 3. The paint shop
draws each car side and each livery layer at its own size. Measured on a copied save (TU4,
Linux, RX 9060 XT 16 GB, RADV, 720p, scale 3, no trimming): 3.3 GB of device memory at the main
menu, 6.0 GB in free roam (38 render targets), 10.8 GB in the paint shop and 11.4 GB in the
decal editor (73), 13.2 GB after switching decal sides (86). The player's log (#241, Windows,
RX 9070 XT, 4096x2160, scale 3) shows the first failed render target 1 h 54 min into a session,
then failed scaled-resolve allocations; the car picture saved 4 minutes later held the upgrade
shop's part images that were left in its guest memory.

Runs with the patch on the same save (scratch profiles):
- `--vulkan_render_target_trim_percent=10` (a trim every 5 s): title, free roam, paint shop,
  decal editor, every decal side, a decal saved on the Corrado. 28-54 trims per run, about
  30 -> 8-12 render targets (3.2-3.7 GB -> 0.8-1.4 GB) each, device memory 4.3-6.3 GB. No
  error, the frames and the saved picture as without trimming.
- Default (90 %), 4096x2160 in headless gamescope, scale 3, 60 fps: 12.4 GB in the decal
  editor; loading the WRX STI from Car Select reached 15.0 of 15.4 GB and trimmed 100 -> 12
  render targets (12.7 GB -> 1.4 GB), 5.1 GB after. A decal saved on the WRX then wrote a
  correct picture over the broken one from the player's save.

0116 lets a controller be remapped (#279): `controller_remap` holds a profile per controller model
(`GUID:target=source,...`), written by the launcher's Controls page, Bindings. Rules and matching are in
`src/input/sdl/gamepad_remap.h` (no SDL; tested by `tools/test_gamepad_remap.cpp`: empty rules give the old
mapping over 20,000 random pad states, each supported pair, the thresholds, rejected pairs and the profile
matching). The launcher's parser (`launcher/forza_launcher/bindings.py`) accepts and drops the same rules:
20,000 random rule strings gave the same result in both. The profile is found when the pad connects (the log
says `SDL Controller remap (profile ...): ...`); from then on the pad's raw SDL state is kept and its XInput
state rebuilt from it on every axis or button event, so the keystroke path (menus) sees the remapped buttons
too. Pads without a profile take the old code path.

Game run (Linux, TU4, offline, scratch save, headless gamescope, the private runtime with 0116): a virtual
Xbox 360 pad (uinput, 045e:028e) bound in the launcher with A on Accelerate and Handbrake cleared
(`<guid>:a=none,rt=a`). The game's SDL 3 reported the same GUID as the launcher's SDL 2 and used the profile.
In free roam the pad's own RT held for 6 s left the car standing; A held for 8 s drove it off (75 on the
speedometer). Not tested with a real pad yet.
