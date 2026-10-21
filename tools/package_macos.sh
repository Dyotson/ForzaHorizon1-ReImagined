#!/usr/bin/env bash
# Local Apple Silicon app bundle; includes recompiled game binaries, never distribute.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TU=0
SCALE=1
FSR=0
PRESET=responsive
while [ $# -gt 0 ]; do
    case "$1" in
        --tu4) TU=4 ;;
        --1440p) SCALE=2; FSR=0; PRESET=1440p ;;
        --fsr-1440p) SCALE=2; FSR=1; PRESET=fsr-1440p ;;
        --metalfx) SCALE=2; FSR=0; PRESET=metalfx ;;
        --metalfx-spatial) SCALE=2; FSR=0; PRESET=metalfx-spatial ;;
        --1440p-performance) SCALE=2; FSR=0; PRESET=metalfx-performance ;;
        --balanced) SCALE=1; FSR=0; PRESET=balanced ;;
        --max-fps) SCALE=1; FSR=0; PRESET=max-fps ;;
        *) echo "Usage: $0 [--tu4] [--balanced|--max-fps|--1440p|--1440p-performance|--fsr-1440p|--metalfx|--metalfx-spatial]" >&2; exit 2 ;;
    esac
    shift
done
[ "$(uname -s)" = Darwin ] || { echo "This helper requires macOS." >&2; exit 1; }
BUILD="$ROOT/forza-recomp/out/build/macos-$TU"
APP="${FORZA_MACOS_APP:-$ROOT/forza-recomp/out/app/Forza Horizon TU$TU $PRESET.app}"
DATA="${FORZA_DATA:-$ROOT/extracted/macos-test-game}"
[ -d "$DATA" ] || DATA="$ROOT/extracted/game"
PROFILE="${FORZA_MACOS_PROFILE:-$ROOT/forza-recomp/out/macos-app-profile-$TU-$PRESET}"
UPDATE="${FORZA_UPDATE:-$ROOT/extracted/tu4}"
[ -x "$BUILD/forza" ] || { echo "Build missing: $BUILD/forza" >&2; exit 1; }
[ -f "$DATA/default.xex" ] || { echo "Game data missing: $DATA" >&2; exit 1; }
[ ! -e "$APP" ] || { echo "App already exists; set FORZA_MACOS_APP to a new path: $APP" >&2; exit 1; }
mkdir -p "$(dirname "$APP")" "$PROFILE"
STAGING="${APP%.app}.tmp.$$.app"
trap 'rm -rf "$STAGING"' EXIT
mkdir -p "$STAGING/Contents/MacOS" "$STAGING/Contents/Resources"
for file in forza librexruntime.dylib librexgpu-xenos.dylib libTracyClient.dylib \
    forza_SpeechFacade_default forza_XMediaFacade_default \
    libforza_SpeechFacade_default.dylib libforza_XMediaFacade_default.dylib; do
    [ -f "$BUILD/$file" ] || { echo "Required app file missing: $BUILD/$file" >&2; exit 1; }
    cp -c "$BUILD/$file" "$STAGING/Contents/MacOS/$file"
done
for dir in vulkan upscalers fonts; do
    if [ -d "$BUILD/$dir" ]; then
        cp -cR "$BUILD/$dir" "$STAGING/Contents/Resources/$dir"
        ln -s "../Resources/$dir" "$STAGING/Contents/MacOS/$dir"
    fi
done
python3 - "$STAGING" "$TU" "$DATA" "$PROFILE" "$UPDATE" "$SCALE" "$FSR" "$PRESET" <<'PY'
import json, pathlib, plistlib, re, subprocess, sys
app, tu, data, profile, update, scale, fsr, preset = sys.argv[1:]
app = pathlib.Path(app)
build = subprocess.check_output(['xcrun', 'vtool', '-show-build', str(app/'Contents/MacOS/forza')], text=True)
minimum = re.search(r'\bminos\s+(\S+)', build).group(1)
info = {
    'CFBundleExecutable': 'forza',
    'CFBundleIdentifier': 'org.forzahorizon.reimagined.local.tu'+tu+'.'+preset,
    'CFBundleName': 'Forza Horizon Local',
    'CFBundleDisplayName': 'Forza Horizon Local',
    'CFBundlePackageType': 'APPL',
    'CFBundleShortVersionString': '0.0.1',
    'CFBundleVersion': '1',
    'LSMinimumSystemVersion': minimum,
    'LSApplicationCategoryType': 'public.app-category.games',
    'LSSupportsGameMode': True,
    'GCSupportsGameMode': True,
    'NSHighResolutionCapable': True,
    'LSEnvironment': {
        'XDG_DATA_HOME': str(pathlib.Path(profile)/'host-data'),
        'REX_GAME_DATA_ROOT': data, 'REX_USER_DATA_ROOT': profile,
        'REX_CACHE_ROOT': str(pathlib.Path(profile)/'cache'),
        'REX_LOG_FILE': str(pathlib.Path(profile)/'forza.log'),
        'FORZA_SERVER': '', 'FORZA_LIVE_SIGNIN': '0',
        'MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS': '0',
    },
}
lines = [
    'game_data_root = '+json.dumps(data),
    'user_data_root = '+json.dumps(profile),
    'cache_root = '+json.dumps(str(pathlib.Path(profile)/'cache')),
    'log_file = '+json.dumps(str(pathlib.Path(profile)/'forza.log')),
    'live_signin = false', 'fullscreen = true',
    'frame_rate = '+('240' if preset in ('max-fps', 'metalfx-performance') else '120' if preset.startswith('metalfx') else '60'),
    'resolution_scale = '+scale,
    'vulkan_max_frames_in_flight = '+('2' if preset.startswith('metalfx') or preset in ('balanced', 'max-fps') else '1'),
    'vulkan_allow_present_mode_immediate = true',
]
if preset in ('responsive', 'balanced', 'max-fps'):
    lines += ['present_effect = "fsr"']
if preset == 'max-fps':
    lines += ['present_fsr_max_upsampling_passes = 1',
              'video_mode_refresh_rate = 480', 'vsync = true',
              'forza_performance_overlay = "fps"']
if preset == 'metalfx-performance':
    lines += ['resolution = "1440p"', 'present_effect = "fsr"',
              'anisotropic_override = 3',
              'present_fsr_max_upsampling_passes = 1',
              'video_mode_refresh_rate = 480', 'vsync = false',
              'forza_performance_overlay = "fps"']
if tu == '4':
    if not (pathlib.Path(update)/'default.xex').is_file():
        raise SystemExit('TU4 data missing: '+update)
    lines += ['update_data_root = '+json.dumps(update)]
    info['LSEnvironment']['REX_UPDATE_DATA_ROOT'] = update
if fsr == '1':
    lines += ['fh1_upscaler = "fsr3_performance"', 'fh1_output_width = 2560',
              'fh1_output_height = 1440']
if preset.startswith('metalfx'):
    backend = 'metalfx_spatial' if preset in ('metalfx-spatial', 'metalfx-performance') else 'metalfx_temporal'
    lines += ['fh1_upscaler = '+json.dumps(backend), 'fh1_output_width = 2560',
              'fh1_output_height = 1440']
config = pathlib.Path(profile)/'forza.toml'
if not config.exists():
    config.write_text('\n'.join(lines)+'\n')
else:
    print('Keeping existing settings: '+str(config))
with (app/'Contents/Info.plist').open('wb') as f:
    plistlib.dump(info, f)
PY
codesign --force --deep --sign - --timestamp=none "$STAGING"
codesign --verify --deep --strict "$STAGING"
mv "$STAGING" "$APP"
printf 'Created local app: %s\n' "$APP"
