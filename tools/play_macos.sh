#!/usr/bin/env bash
# Isolated offline smoke test; never imports the player's existing profile.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TU=0
SKIP_INTRO=0
LOW_LATENCY=0
METALFX=0
BALANCED=0
MAX_FPS=0
PERFORMANCE_1440P=0
# --test-drive uses a clone with single-frame startup movies, then navigates
# the scratch profile to Single Player.
while [ $# -gt 0 ]; do
    case "$1" in
        --tu4) TU=4; shift ;;
        --skip-intro) SKIP_INTRO=1; shift ;;
        --low-latency) LOW_LATENCY=1; shift ;;
        --metalfx) METALFX=1; BALANCED=0; MAX_FPS=0; PERFORMANCE_1440P=0; LOW_LATENCY=1; shift ;;
        --balanced) BALANCED=1; METALFX=0; MAX_FPS=0; PERFORMANCE_1440P=0; LOW_LATENCY=1; shift ;;
        --max-fps) MAX_FPS=1; METALFX=0; BALANCED=0; PERFORMANCE_1440P=0; LOW_LATENCY=1; shift ;;
        --1440p-performance) PERFORMANCE_1440P=1; METALFX=1; MAX_FPS=0; BALANCED=0; LOW_LATENCY=1; shift ;;
        --test-drive)
            SKIP_INTRO=1
            export FORZA_AUTOPRESS="3:A,7:START,11:A,14:A,18:A,22:A"
            shift ;;
        *) break ;;
    esac
done
BUILD="$ROOT/forza-recomp/out/build/macos-$TU"
DATA="${FORZA_DATA:-$ROOT/extracted/game}"
if [ "$SKIP_INTRO" = 1 ]; then DATA="$("$ROOT/tools/prepare_macos_test_game.sh")"; fi
PROFILE="${FORZA_MACOS_PROFILE:-$ROOT/forza-recomp/out/macos-profile-$TU}"
if [ ! -x "$BUILD/forza" ] || [ ! -f "$DATA/default.xex" ]; then
    echo "Missing macOS build or game files; see docs/macos.md." >&2
    exit 1
fi
mkdir -p "$PROFILE"
# The game's identity/relaunch hooks use GetUserFolder independently of user_data_root.
export XDG_DATA_HOME="$PROFILE/host-data"
if [ "$TU" = 4 ]; then
    UPDATE="${FORZA_UPDATE:-$ROOT/extracted/tu4}"
    [ -f "$UPDATE/default.xex" ] || { echo "Missing TU4 files at $UPDATE" >&2; exit 1; }
    set -- --update_data_root="$UPDATE" "$@"
fi
cd "$BUILD"
export FORZA_SERVER=
export FORZA_LIVE_SIGNIN=0
# Pinyon's macOS tuning uses Metal's direct bindings. Respect an explicit override.
export MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS="${MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS:-0}"
DEFAULTS=(--live_signin=false)
HAS_FRAME_RATE=0
HAS_RESOLUTION_SCALE=0
HAS_QUEUE_DEPTH=0
HAS_IMMEDIATE_PRESENT=0
HAS_UPSCALER=0
HAS_OUTPUT_WIDTH=0
HAS_OUTPUT_HEIGHT=0
HAS_PRESENT_EFFECT=0
HAS_PRESENT_PASSES=0
HAS_REFRESH_RATE=0
HAS_OVERLAY=0
HAS_VSYNC=0
HAS_RESOLUTION=0
HAS_FILTERING=0
for arg in "$@"; do
    case "$arg" in
        --frame_rate|--frame_rate=*) HAS_FRAME_RATE=1 ;;
        --resolution_scale|--resolution_scale=*) HAS_RESOLUTION_SCALE=1 ;;
        --vulkan_max_frames_in_flight|--vulkan_max_frames_in_flight=*) HAS_QUEUE_DEPTH=1 ;;
        --vulkan_allow_present_mode_immediate|--vulkan_allow_present_mode_immediate=*|--no-vulkan_allow_present_mode_immediate) HAS_IMMEDIATE_PRESENT=1 ;;
        --fh1_upscaler|--fh1_upscaler=*) HAS_UPSCALER=1 ;;
        --fh1_output_width|--fh1_output_width=*) HAS_OUTPUT_WIDTH=1 ;;
        --fh1_output_height|--fh1_output_height=*) HAS_OUTPUT_HEIGHT=1 ;;
        --present_effect|--present_effect=*) HAS_PRESENT_EFFECT=1 ;;
        --present_fsr_max_upsampling_passes|--present_fsr_max_upsampling_passes=*) HAS_PRESENT_PASSES=1 ;;
        --video_mode_refresh_rate|--video_mode_refresh_rate=*) HAS_REFRESH_RATE=1 ;;
        --forza_performance_overlay|--forza_performance_overlay=*) HAS_OVERLAY=1 ;;
        --vsync|--vsync=*|--no-vsync) HAS_VSYNC=1 ;;
        --resolution|--resolution=*) HAS_RESOLUTION=1 ;;
        --anisotropic_override|--anisotropic_override=*) HAS_FILTERING=1 ;;
    esac
done
# CLI11 rejects duplicate options that are already registered at startup.
# Apply defaults only when the caller has not supplied the setting.
if [ "$HAS_FRAME_RATE" = 0 ]; then
    # A faster guest vblank clock gives the slower reconstruction path finer pacing.
    if [ "$MAX_FPS" = 1 ] || [ "$PERFORMANCE_1440P" = 1 ]; then DEFAULTS+=(--frame_rate=240)
    elif [ "$METALFX" = 1 ]; then DEFAULTS+=(--frame_rate=120)
    elif [ "$LOW_LATENCY" = 1 ]; then DEFAULTS+=(--frame_rate=60)
    else DEFAULTS+=(--frame_rate=30); fi
fi
if [ "$HAS_RESOLUTION_SCALE" = 0 ]; then
    if [ "$METALFX" = 1 ]; then DEFAULTS+=(--resolution_scale=2)
    else DEFAULTS+=(--resolution_scale=1); fi
fi
if [ "$METALFX" = 1 ]; then
    if [ "$HAS_UPSCALER" = 0 ]; then
        if [ "$PERFORMANCE_1440P" = 1 ]; then DEFAULTS+=(--fh1_upscaler=metalfx_spatial)
        else DEFAULTS+=(--fh1_upscaler=metalfx_temporal); fi
    fi
    if [ "$HAS_OUTPUT_WIDTH" = 0 ]; then DEFAULTS+=(--fh1_output_width=2560); fi
    if [ "$HAS_OUTPUT_HEIGHT" = 0 ]; then DEFAULTS+=(--fh1_output_height=1440); fi
fi
if { [ "$BALANCED" = 1 ] || [ "$MAX_FPS" = 1 ] || [ "$PERFORMANCE_1440P" = 1 ]; } && [ "$HAS_PRESENT_EFFECT" = 0 ]; then
    DEFAULTS+=(--present_effect=fsr)
fi
if [ "$MAX_FPS" = 1 ] || [ "$PERFORMANCE_1440P" = 1 ]; then
    # Keep one EASU pass, then let the final stretch fit the Retina drawable.
    # This avoids chaining another full-resolution EASU pass for a small stretch.
    if [ "$HAS_PRESENT_PASSES" = 0 ]; then DEFAULTS+=(--present_fsr_max_upsampling_passes=1); fi
    if [ "$HAS_REFRESH_RATE" = 0 ] && [ "$HAS_FRAME_RATE" = 0 ]; then DEFAULTS+=(--video_mode_refresh_rate=480); fi
    if [ "$HAS_OVERLAY" = 0 ]; then DEFAULTS+=(--forza_performance_overlay=fps); fi
fi
if [ "$PERFORMANCE_1440P" = 1 ]; then
    # 720p world, reconstructed 1440p output with a native 1440p HUD.
    # VSync off also releases the SDK's guest vblank pacing; this is uncapped.
    if [ "$HAS_VSYNC" = 0 ]; then DEFAULTS+=(--vsync=false); fi
    if [ "$HAS_RESOLUTION" = 0 ]; then DEFAULTS+=(--resolution=1440p); fi
    if [ "$HAS_FILTERING" = 0 ]; then DEFAULTS+=(--anisotropic_override=3); fi
fi
if [ "$LOW_LATENCY" = 1 ]; then
    if [ "$HAS_QUEUE_DEPTH" = 0 ]; then
        if [ "$METALFX" = 1 ] || [ "$BALANCED" = 1 ] || [ "$MAX_FPS" = 1 ]; then DEFAULTS+=(--vulkan_max_frames_in_flight=2)
        else DEFAULTS+=(--vulkan_max_frames_in_flight=1); fi
    fi
    if [ "$HAS_IMMEDIATE_PRESENT" = 0 ]; then DEFAULTS+=(--vulkan_allow_present_mode_immediate=true); fi
fi
exec ./forza --game_data_root="$DATA" --user_data_root="$PROFILE" \
    --cache_root="$PROFILE/cache" --log_file="$PROFILE/forza.log" \
    "${DEFAULTS[@]}" \
    "$@"
