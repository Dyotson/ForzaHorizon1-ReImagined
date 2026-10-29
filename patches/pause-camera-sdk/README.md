# Recorded pause-world SDK overlay

This narrow overlay pairs with the host producer in PR #487. It applies to
ReIGlue `317216e8d66d766bd13abdb672975458f30ca576` (`fh1-0.10.0.24-r2`). It is
separate from the retired `patches/rexglue-sdk` series. A future ReIGlue pin must
receive a fresh source audit before this overlay can apply.

`tools/build_rexglue_sdk.sh` constructs the audited desired tree in a temporary
Git index before its existing local-change check. It accepts an exact base or
its own exact patched files, preserves matching file timestamps on repeat runs,
and refuses unrelated SDK edits. HEAD remains pinned; the build-owned SDK index
describes the overlay. `REIGLUE_DEV=1` continues to build the developer's checkout
as supplied and does not install this overlay implicitly.

The patch contains the recorded marker parser, typed world/car ownership,
primary-target and side-effect guards, Vulkan raster interception, and the r19
lease. A single host certificate associates a sealed UID with its captured
identity and lifecycle revision. New publications may be pending while the
current UID executes; real cancellation, identity change, VR, mode disable, or
the existing 250 ms freshness limit revoke it. Each native world replay still
requires its own markers and main-target witness. World FrameEnd removes draw
permission, and actual XE_SWAP retires the presented-frame lease.

All feature flags remain off by default in the paired host. Only the Vulkan
backend changes raster emission; other backends retain their ordinary draw
paths. TU4 native ownership is supplied by the paired host, whose TU0 and VR
fallback gates must remain in place. The SDK does not discover ownership from
shader hashes, dimensions, or an unmarked paused screen. This overlay adds no
fade compositor, pan qualification, or additional supported platform claim.

The source is independently authored instrumentation and policy. Existing SDK
copyright and license notices remain in their original files. No extracted game
code, generated recompilation units, assets, saves, or binaries are included.
`manifest.json` records the exact patch and each before/after Git-blob SHA-256;
text uses Git's LF representation. The private r19 source differs only in normal
checkout line endings from the twelve delivered production files.

Source and standalone checks:

```sh
python3 tools/pause_sdk_patch_tree.py --sdk /path/to/ReIGlue --base 317216e8d66d766bd13abdb672975458f30ca576
python3 -m unittest discover -s tools -p test_pause_sdk_patch_tree.py
# After the normal source-only SDK preparation has applied the overlay:
cmake -S /path/to/ReIGlue/tests/fh1_pause -B /tmp/fh1-pause-tests -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/fh1-pause-tests --parallel 1
ctest --test-dir /tmp/fh1-pause-tests --output-on-failure
```

The first command validates a temporary index and prints its tree; it leaves the
checkout and real index alone. The C++ fixture extracts the actual SDK refresh,
marker dispatch, draw-decision and swap code, then runs it with the real wire
parser, consumer, permit and target witness. It requires no game or renderer
build. On a Windows xwin environment without debug CRT libraries, configure
with `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL` as well.

These source checks do not qualify the paired runtime or visuals. Public
readiness depends on the coordinator's separately recorded r19 test results.
