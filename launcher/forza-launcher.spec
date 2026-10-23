# PyInstaller spec: one directory with the launcher executable.
#   pyinstaller --noconfirm forza-launcher.spec          (from the launcher/ folder)
# packaging/make_package.py then adds the project sources, the Forza Live server and the SDK
# files beside it. The Forza Live server runs inside this executable (--live-server), so
# the stdlib modules it imports are collected as hidden imports.
import ast
import sys
from pathlib import Path

from PyInstaller.utils.hooks import collect_data_files, collect_dynamic_libs, collect_submodules

root = Path(SPECPATH).resolve()
repo = root.parent
images = repo / "docs" / "images"

stdlib = set(sys.stdlib_module_names)
hidden = set()
for f in (repo / "server").glob("*.py"):
    if f.name.startswith("test_"):
        continue
    for node in ast.walk(ast.parse(f.read_text(encoding="utf-8"))):
        if isinstance(node, ast.Import):
            hidden.update(a.name.split(".")[0] for a in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module and node.level == 0:
            hidden.add(node.module.split(".")[0])
hidden = sorted(m for m in hidden if m in stdlib)
hidden += ["zipfile", "tempfile", "shutil", "subprocess", "io", "sqlite3", "xml.etree.ElementTree", "http.server", "ssl", "hashlib", "hmac", "zlib"]

datas = [
    (str(root / "forza_launcher" / "update_helper.py"), "forza_launcher"),
    (str(images / "logo.svg"), "assets"),
    (str(images / "logo.png"), "assets"),
    (str(repo / "forza-recomp" / "resources" / "forza.ico"), "assets"),
    (str(images / "social-preview.png"), "assets"),
    (str(root / "forza_launcher" / "ui" / "down.svg"), "forza_launcher/ui"),
    (str(root / "forza_launcher" / "ui" / "check.svg"), "forza_launcher/ui"),
    (str(root / "forza_launcher" / "ui" / "controller_art"), "forza_launcher/ui/controller_art"),
    (str(repo / "tools" / "xcontent_extract.py"), "tools"),
    (str(repo / "tools" / "xiso_extract.py"), "tools"),
    (str(repo / "tools" / "install_dlc.py"), "tools"),
]
# certifi's cacert.pem (the fallback CA bundle of fetch.py) and truststore (the OS certificate store on
# Windows/macOS), both imported lazily and so invisible to the import scan (#85).
datas += collect_data_files("certifi")
datas += [(str(root / 'forza_launcher' / 'haptics' / 'vendor' / name), 'forza_launcher/haptics/vendor')
          for name in ('LICENSE', 'README.md')]
if sys.platform != 'linux':
    hidden += ['hid']
hidden += ["certifi"] + collect_submodules("truststore")
# PySDL2 loads SDL through ctypes; retain the wheel's DLL directory and licenses.
datas += collect_data_files("sdl2dll")
hidden += ["sdl2dll"]

# The title update step (a Python tool the frozen app runs itself with --run-script), and the shader
# prebuild (#226), which imports pipeline_list, fxobj_shaders, xxh3, lzx_delta and apply_title_update.
for _tool in ("apply_title_update.py", "lzx_delta.py", "xdc_decode.py", "livery_codec.py", "pipeline_list.py", "fxobj_shaders.py", "xxh3.py", "native_menu_scene.py", "offline_cars_flow.py"):
    if (repo / "tools" / _tool).is_file():
        datas.append((str(repo / "tools" / _tool), "tools"))
datas.append((str(repo / "tools" / "native_menu" / "fh1_donors.json"), "tools/native_menu"))
for name in ("change_car", "tune_car", "garage", "autoshow"):
    datas.append((str(repo / "tools" / "native_menu" / "icons" / (name + ".xds")), "tools/native_menu/icons"))
# The shipped pipeline list (hashes and render state only).
datas.append((str(root / "forza_launcher" / "data" / "pipelines-4D5309C9.json"), "forza_launcher/data"))

a = Analysis(
    [str(root / "main.py")],
    pathex=[str(root), str(repo / "tools")],
    datas=datas,
    binaries=collect_dynamic_libs("sdl2dll"),
    hiddenimports=hidden + ["install_dlc", "xcontent_extract", "xiso_extract", "xdc_decode", "livery_codec", "tomllib", "pipeline_list",
                            "fxobj_shaders", "xxh3", "lzx_delta", "apply_title_update", "offline_cars_flow"],
    excludes=["tkinter", "unittest", "PySide6.QtWebEngineCore", "PySide6.QtWebEngineWidgets",
              "PySide6.QtQml", "PySide6.QtQuick", "PySide6.Qt3DCore", "PySide6.QtMultimedia",
              "PySide6.QtNetwork", "PySide6.QtPdf", "PySide6.QtSql", "PySide6.QtTest"],
    noarchive=False,
)
# Recent Qt releases use Windows' ICU API. A tool on the build machine's PATH
# (for example Poppler) can supply a different icuuc.dll with versioned exports.
# Bundling it shadows the OS DLL and prevents QtCore from loading at startup.
# Keep version-suffixed ICU libraries used by older Qt builds; these three names
# belong to the Windows API and must be resolved on the player's system.
if sys.platform == "win32":
    a.binaries = [entry for entry in a.binaries
                  if Path(entry[0]).name.lower() not in {"icu.dll", "icuuc.dll", "icuin.dll"}]
pyz = PYZ(a.pure)
exe = EXE(
    pyz, a.scripts, [],
    exclude_binaries=True,
    name="forza-launcher",
    console=False,
    icon=str(repo / "forza-recomp" / "resources" / "forza.ico") if sys.platform == "win32" else None,
)
coll = COLLECT(exe, a.binaries, a.datas, strip=False, upx=False, name="forza-launcher")
