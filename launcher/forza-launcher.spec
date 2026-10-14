# PyInstaller spec: one directory with the launcher executable.
#   pyinstaller --noconfirm forza-launcher.spec          (from the launcher/ folder)
# packaging/make_package.py then adds the project sources, the Forza Live server and the SDK
# files beside it. The Forza Live server runs inside this executable (--live-server), so
# the stdlib modules it imports are collected as hidden imports.
import ast
import sys
from pathlib import Path

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
    (str(images / "logo.svg"), "assets"),
    (str(images / "logo.png"), "assets"),
    (str(images / "social-preview.png"), "assets"),
    (str(root / "forza_launcher" / "ui" / "down.svg"), "forza_launcher/ui"),
    (str(repo / "tools" / "xcontent_extract.py"), "tools"),
    (str(repo / "tools" / "xiso_extract.py"), "tools"),
    (str(repo / "tools" / "install_dlc.py"), "tools"),
]
# The title update step (a Python tool the frozen app runs itself with --run-script).
for _tool in ("apply_title_update.py", "lzx_delta.py"):
    if (repo / "tools" / _tool).is_file():
        datas.append((str(repo / "tools" / _tool), "tools"))

a = Analysis(
    [str(root / "main.py")],
    pathex=[str(root), str(repo / "tools")],
    datas=datas,
    hiddenimports=hidden + ["install_dlc", "xcontent_extract", "xiso_extract", "tomllib"],
    excludes=["tkinter", "unittest", "PySide6.QtWebEngineCore", "PySide6.QtWebEngineWidgets",
              "PySide6.QtQml", "PySide6.QtQuick", "PySide6.Qt3DCore", "PySide6.QtMultimedia",
              "PySide6.QtNetwork", "PySide6.QtPdf", "PySide6.QtSql", "PySide6.QtTest"],
    noarchive=False,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz, a.scripts, [],
    exclude_binaries=True,
    name="forza-launcher",
    console=False,
    icon=str(images / "logo.png") if sys.platform == "win32" else None,
)
coll = COLLECT(exe, a.binaries, a.datas, strip=False, upx=False, name="forza-launcher")
