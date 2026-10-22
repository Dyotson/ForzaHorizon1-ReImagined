# Startup shader warm-up

The game already waits for the SDK to rebuild previously stored shaders and
pipeline descriptions before resuming the guest. The launcher now shows this
stage as **Preparing cached shaders**, with a completed/total pipeline count on
Vulkan. Direct3D 12 reports the beginning and end of its existing preload stage.
The normal Stop button remains available. Older SDKs that do not report progress
keep the existing launcher behavior.

This prepares **previously encountered pipelines**. An empty cache has nothing to
warm, and a new race, car, effect or graphics configuration can still introduce
new shader combinations during gameplay. No game shaders or driver binaries are
bundled or downloaded by this feature.

## Local Vulkan driver cache

In addition to the existing shader/pipeline descriptions, Vulkan retains the
driver's pipeline data in `cache_root/shaders/local/*.vkpc`. At startup this data
seeds a `VkPipelineCache`, which is supplied to graphics pipeline creation.
The cache is saved after warm-up, and periodically when new pipelines have been
created. Shutdown also attempts to save pending data. Disk writes use a temporary
file and replace the previous file only after the write succeeds.

The **Keep compiled Vulkan pipelines** advanced setting is on by default and
requires **Keep a shader cache** and the Vulkan backend. Changing it requires a
restart. Turning it off leaves the original shader/pipeline description preload
in place. It does not delete existing cache files.

Files are local to the GPU vendor/device, driver version, pipeline cache UUID and
SDK pipeline/translator format versions. Incompatible, truncated, oversized or
checksum-invalid files are ignored. Driver rejection of initial data retries
with an empty cache; failure to create or write the cache falls back to normal
compilation. The data is opaque and should not be distributed between players.
The maximum accepted driver payload is 64 MiB.

Pipeline creation may run concurrently using the driver's synchronized cache.
The two cache-data queries take an exclusive lock to obtain a consistent
snapshot; file encoding and writes run outside that lock on the storage worker.
No extra GPU idle wait is introduced. Snapshot work can still contend with new
pipeline creation, so this is not a claim that runtime shader stalls disappear.

## Validation

- Launcher unit/Qt tests cover bounded progress parsing, old and rotated logs,
  partial lines, worker ordering, progress visibility and setting dependencies.
- `tools/test_driver_pipeline_cache.cpp` tests identity rejection, every single
  byte corruption, truncation, size bounds, failed writes and concurrent atomic
  replacement. CI runs it against the patched SDK on Windows and Linux.
- The Windows SDK builds with both Vulkan and Direct3D 12 enabled.
- A local RTX 5060 Ti test at native 2560x1440 loaded 410 stored Vulkan pipeline
  descriptions. The first start supplied no application cache and saved a
  15,980,775-byte driver payload. The next start supplied that payload and
  completed the preload again. Warm-up logged 801 ms and 296 ms respectively.
  Another game was running, and the driver already had its own cache: these
  timings are a smoke test, not an isolated speedup benchmark.
- A third start with a deliberately truncated local cache ignored it, rebuilt
  the known pipelines and replaced the damaged file successfully.
- Khronos synchronization validation reported no errors during warm-up. After
  warm-up it reported `VUID-vkQueueSubmit-pSignalSemaphores-00067` in presentation.
  The presenter is unchanged here; its semaphore ownership fix is carried
  separately in patch 0076 on the renderer experiment branch. This PR does not
  claim a validation-clean gameplay session or include that separate fix.

No first-encounter stutter, unseen-pipeline coverage, or all-driver performance
improvement is claimed by these checks.
