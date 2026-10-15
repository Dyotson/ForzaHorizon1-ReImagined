"""Play, Setup and About pages."""
import os
import sys
import time
from pathlib import Path

from PySide6.QtCore import QTimer, Qt, QUrl, Signal
from PySide6.QtGui import QDesktopServices, QPixmap
from PySide6.QtWidgets import (QCheckBox, QFileDialog, QHBoxLayout, QLabel, QLineEdit, QMessageBox,
                               QPlainTextEdit, QProgressBar, QPushButton, QScrollArea, QSpinBox,
                               QVBoxLayout, QWidget)

from .. import GITHUB_URL, PUBLIC_SERVER, __version__, build_env, builder, datafolder, game, installer, paths, pins
from ..config import Config
from . import theme, workers
from .widgets import Hero, StatusRow, card, label


def scroll_page(title, lead):
    """A page scaffold: returns (outer scroll widget, inner layout)."""
    inner = QWidget()
    lay = QVBoxLayout(inner)
    lay.setContentsMargins(0, 0, 12, 24)
    lay.setSpacing(14)
    t = QLabel(title)
    t.setObjectName("pageTitle")
    lay.addWidget(t)
    if lead:
        lay.addWidget(label(lead, "pageLead"))
    sc = QScrollArea()
    sc.setWidgetResizable(True)
    sc.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
    sc.setWidget(inner)
    return sc, lay


def open_folder(path: Path):
    path.mkdir(parents=True, exist_ok=True)
    QDesktopServices.openUrl(QUrl.fromLocalFile(str(path)))


def human_time(seconds):
    m = int(seconds // 60)
    return f"{m // 60} h {m % 60} min" if m >= 60 else f"{m} min"


# ================================================================== Play ==================

class PlayPage(QWidget):
    goto_setup = Signal()
    play_requested = Signal()

    def __init__(self, cfg: Config, window):
        super().__init__()
        self.cfg, self.window = cfg, window
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        sc, lay = scroll_page("Play", "")
        outer.addWidget(sc)
        # the title is part of the banner; hide the text title on this page
        lay.itemAt(0).widget().hide()
        lay.addWidget(Hero())
        self.warn = label("", "warn")
        self.warn.hide()
        lay.addWidget(self.warn)

        c, cl = card()
        self.r_files = StatusRow("Game files")
        self.r_build = StatusRow("Game")
        self.r_dlc = StatusRow("DLC")
        self.r_online = StatusRow("Online")
        for r in (self.r_files, self.r_build, self.r_dlc, self.r_online):
            cl.addWidget(r)
        lay.addWidget(c)

        row = QHBoxLayout()
        self.play = QPushButton("Play")
        self.play.setObjectName("play")
        self.play.setCursor(Qt.PointingHandCursor)
        self.play.clicked.connect(self._clicked)
        row.addWidget(self.play)
        self.stop = QPushButton("Stop")
        self.stop.clicked.connect(window.stop_game)
        self.stop.hide()
        row.addWidget(self.stop)
        row.addStretch(1)
        logs = QPushButton("Open log folder")
        logs.clicked.connect(lambda: open_folder(cfg.data_home()))
        row.addWidget(logs)
        lay.addLayout(row)
        self.msg = label("", "muted")
        lay.addWidget(self.msg)
        lay.addStretch(1)
        self.mode = "setup"
        self._keep_warn = False

    def _clicked(self):
        if self.mode == "setup":
            self.goto_setup.emit()
        else:
            self.play_requested.emit()

    def refresh(self):
        cfg = self.cfg
        root = cfg.game_files()
        if (root / "default.xex").is_file():
            files_ok, files_msg = installer.check_game_folder(root)
            if files_ok:
                files_msg = str(root)
        else:
            files_ok, files_msg = False, "Not set up yet: open Setup."
        self.r_files.set(files_ok, files_msg)
        state, detail = builder.build_status(cfg.data_home(), cfg["tu_package"])
        install, looked = game.find_game(cfg)
        if install is not None:
            self.r_build.set(True if state != "stale" else None,
                             f"{game.game_version(install)}  -  {install.dir}" +
                             ("  (a rebuild is available: open Setup)" if state == "stale" else ""))
        else:
            self.r_build.set(False if state != "partial" else None, detail + " Open Setup and press Build.")
        n = game.installed_dlc(cfg)
        self.r_dlc.set(True if n else None, f"{n} DLC package(s) installed" if n else
                       "None installed (optional): add them in Setup.")
        mode = cfg.online["mode"]
        self.r_online.set(True, {
            "public": f"Public server, {PUBLIC_SERVER}",
            "local": "Local server on this PC (only you)",
            "custom": "Your server: " + (cfg.online.get("custom_server") or "(no address set)"),
            "offline": "Offline",
        }[mode] + "   (Settings > Online)")
        ready = files_ok and install is not None
        if self.window.running():
            self.play.setText("Playing...")
            self.play.setEnabled(False)
            self.stop.show()
        else:
            self.stop.hide()
            self.play.setEnabled(True)
            if ready:
                self.mode = "play"
                self.play.setText("Play")
            else:
                self.mode = "setup"
                self.play.setText("Set up the game")
        if not self._keep_warn:
            self.warn.setVisible(False)

    def clear_message(self):
        self._keep_warn = False
        self.warn.hide()
        self.msg.setText("")

    def show_message(self, text, warn=False, keep=False):
        """A warning stays until the next start: refresh() runs right after it and used to hide
        it, so a game that failed to start left no trace but a stale "The game is running"."""
        if warn:
            self._keep_warn = keep
            self.msg.setText("")
            self.warn.setText(text)
            self.warn.show()
        else:
            self.msg.setText(text)


# ================================================================== Setup =================

class StepCard:
    """A numbered card with a title, status line and a body layout."""

    def __init__(self, number, title, lead):
        self.frame, self.lay = card(spacing=10)
        head = QHBoxLayout()
        n = QLabel(str(number))
        n.setFixedSize(30, 30)
        n.setAlignment(Qt.AlignCenter)
        n.setStyleSheet(f"background: qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 {theme.ORANGE}, stop:1 {theme.PINK});"
                        f"color: #1a0f33; font-weight: 800; border-radius: 15px;")
        head.addWidget(n)
        t = QLabel(title)
        t.setObjectName("cardTitle")
        head.addWidget(t)
        head.addStretch(1)
        self.badge = QLabel("")
        head.addWidget(self.badge)
        self.lay.addLayout(head)
        if lead:
            self.lay.addWidget(label(lead, "muted"))

    def set_badge(self, text, ok):
        color = {True: theme.OK, False: theme.ERR, None: theme.ORANGE}[ok]
        self.badge.setText(text)
        self.badge.setStyleSheet(f"color: {color}; font-weight: 700;")


class WrappedCheck(QWidget):
    """A check box whose text wraps: a QCheckBox keeps its whole text on one line, and a long
    one (the Microsoft license line on Windows, in Segoe UI) forced the Setup page wider than
    the window (#26). Exposes isChecked, setChecked and toggled like a QCheckBox."""

    def __init__(self, text):
        super().__init__()
        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(0)
        self.box = QCheckBox()
        lay.addWidget(self.box, 0, Qt.AlignTop)
        self.text = label(text, "")
        self.text.setMinimumWidth(0)
        self.text.setTextInteractionFlags(Qt.NoTextInteraction)
        lay.addWidget(self.text, 1)
        self.toggled = self.box.toggled
        self.isChecked = self.box.isChecked
        self.setChecked = self.box.setChecked

    def mouseReleaseEvent(self, e):
        if e.button() == Qt.LeftButton:
            self.box.toggle()
        super().mouseReleaseEvent(e)


def path_row(placeholder, on_file=None, on_dir=None, file_title="Choose a file"):
    row = QHBoxLayout()
    edit = QLineEdit()
    edit.setPlaceholderText(placeholder)
    row.addWidget(edit, 1)
    btns = []
    if on_file:
        b = QPushButton("Choose file")
        b.clicked.connect(on_file)
        row.addWidget(b)
        btns.append(b)
    if on_dir:
        b = QPushButton("Choose folder")
        b.clicked.connect(on_dir)
        row.addWidget(b)
        btns.append(b)
    return row, edit, btns


class SetupPage(QWidget):
    changed = Signal()   # something finished: pages should refresh

    def __init__(self, cfg: Config, window):
        super().__init__()
        self.cfg, self.window = cfg, window
        self.task = None
        self.fail = {}   # last failure shown per message label, kept until the next run (refresh() must not wipe it)
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        sc, lay = scroll_page("Setup", "The game is built on this computer from your own copy of Forza Horizon. "
                              "Nothing from the game is downloaded or shared: no game code or data is included "
                              "with this launcher.")
        outer.addWidget(sc)

        # 0. where --------------------------------------------------------------------
        f0, l0 = card()
        t0 = QLabel("Where the game is built")
        t0.setStyleSheet("font-weight: 700; font-size: 15px;")
        l0.addWidget(t0)
        l0.addWidget(label("The extracted game, the compiler downloads, the build and your saves all live in this folder "
                           "(about 15 GB or more). Pick a folder on another drive to keep them off C:. Avoid Documents, "
                           "Desktop and OneDrive folders: OneDrive or Windows security can block the build there.", "help"))
        frow = QHBoxLayout()
        self.folder = QLineEdit()
        self.folder.setReadOnly(True)
        frow.addWidget(self.folder, 1)
        self.change_btn = QPushButton("Change folder")
        self.change_btn.clicked.connect(lambda: self.change_folder())
        frow.addWidget(self.change_btn)
        ob = QPushButton("Open")
        ob.clicked.connect(lambda: open_folder(self.cfg.data_home()))
        frow.addWidget(ob)
        l0.addLayout(frow)
        self.bar0 = QProgressBar()
        self.bar0.hide()
        l0.addWidget(self.bar0)
        self.msg0 = label("", "muted")
        l0.addWidget(self.msg0)
        lay.addWidget(f0)

        # 1. game files ---------------------------------------------------------------
        self.c1 = StepCard(1, "Your game files",
                           "Choose your Games on Demand package (the file in 4D5309C9/00007000, next to its "
                           ".data folder), your disc 1 ISO, a default.xex, or a folder you already extracted. The base version of "
                           "the game is needed, without title updates.")
        row, self.pkg, _ = path_row("Package, default.xex or folder", self._pick_file, self._pick_dir)
        self.pkg.setText(cfg["game_package"])
        self.pkg.editingFinished.connect(self._pkg_changed)
        self.c1.lay.addLayout(row)
        self.go1 = QPushButton("Extract game files")
        self.go1.clicked.connect(self._use_game_files)
        brow = QHBoxLayout()
        brow.addWidget(self.go1)
        self.cancel1 = QPushButton("Cancel")
        self.cancel1.hide()
        self.cancel1.clicked.connect(self._cancel)
        brow.addWidget(self.cancel1)
        brow.addStretch(1)
        self.c1.lay.addLayout(brow)
        self.bar1 = QProgressBar()
        self.bar1.hide()
        self.c1.lay.addWidget(self.bar1)
        self.msg1 = label("", "muted")
        self.c1.lay.addWidget(self.msg1)
        lay.addWidget(self.c1.frame)

        # 2. title update -----------------------------------------------------------------
        self.ctu = StepCard(2, "Title Update 4 (recommended)",
                            "Recommended: needed for Horizon Rally and the 1000 Club (the Rally Expansion and the "
                            "1000 Club Expansion). Your Title Update 4 package (the file in 4D5309C9/000B0000, "
                            "update 4000D145), or a zip holding it. It is applied to your default.xex and the game "
                            "is built from that version. Optional: without it you get the base game, and the "
                            "expansions' cars but not their modes.")
        row, self.tu, _ = path_row("Title update package", self._pick_tu, None)
        self.tu.setText(cfg["tu_package"])
        self.tu.editingFinished.connect(self._tu_changed)
        self.ctu.lay.addLayout(row)
        self.msgtu = label("", "muted")
        self.ctu.lay.addWidget(self.msgtu)
        lay.addWidget(self.ctu.frame)

        # 3. DLC ------------------------------------------------------------------------
        self.c2 = StepCard(3, "DLC (optional)",
                           "The folder with your DLC: packages (4D5309C9/00000002), or a Xenia content folder (or its "
                           "00000002 folder). They are installed for the game to find; you can add more later. "
                           "Without DLC the extra cars and events are missing.")
        row, self.dlc, _ = path_row("Folder of DLC", None, self._pick_dlc)
        self.dlc.setText(cfg["dlc_dir"])
        self.dlc.editingFinished.connect(self._dlc_changed)
        self.c2.lay.addLayout(row)
        self.dlclist = label("", "help")
        self.c2.lay.addWidget(self.dlclist)
        self.go2 = QPushButton("Install DLC")
        self.go2.clicked.connect(self._install_dlc)
        brow = QHBoxLayout()
        brow.addWidget(self.go2)
        brow.addStretch(1)
        self.c2.lay.addLayout(brow)
        self.bar2 = QProgressBar()
        self.bar2.hide()
        self.c2.lay.addWidget(self.bar2)
        self.msg2 = label("", "muted")
        self.c2.lay.addWidget(self.msg2)
        lay.addWidget(self.c2.frame)

        # 3. build ----------------------------------------------------------------------
        self.c3 = StepCard(4, "Build the game",
                           "The launcher compiles the game on this computer: it generates C++ from your "
                           "default.xex and builds it with a C++ compiler. It takes a while (see below), uses a lot "
                           "of memory, and is only repeated when the project's version changes.")
        self.checks = QVBoxLayout()
        self.checks.setSpacing(2)
        self.c3.lay.addLayout(self.checks)
        self.tools_msg = label("", "muted")
        self.c3.lay.addWidget(self.tools_msg)
        jrow = QHBoxLayout()
        jrow.addWidget(QLabel("Parallel jobs"))
        self.jobs = QSpinBox()
        self.jobs.setRange(1, 64)
        self.jobs.setValue(cfg.advanced.get("build_jobs") or build_env.default_jobs())
        self.jobs.valueChanged.connect(lambda v: (cfg.advanced.__setitem__("build_jobs", v), cfg.save()))
        jrow.addWidget(self.jobs)
        jrow.addWidget(label("Each job uses about 1.5 GB of memory. More jobs are faster; too many run out of "
                             "memory.", "help"), 1)
        self.c3.lay.addLayout(jrow)
        self.lowprio = WrappedCheck("Run at low priority (the computer stays usable)")
        self.lowprio.setChecked(True)
        self.c3.lay.addWidget(self.lowprio)
        self.ms = None
        if sys.platform == "win32":
            self.ms = WrappedCheck("I accept Microsoft's license terms for the C++ runtime and Windows SDK "
                                "(downloaded to this PC only if no Visual Studio 2022 with the C++ tools is installed)")
            self.ms.setChecked(bool(cfg.advanced.get("accept_ms_license")))
            self.ms.toggled.connect(lambda v: (cfg.advanced.__setitem__("accept_ms_license", v), cfg.save()))
            self.c3.lay.addWidget(self.ms)
            lnk = QPushButton("Read the license")
            lnk.setObjectName("link")
            lnk.clicked.connect(lambda: QDesktopServices.openUrl(QUrl(pins.MS_LICENSE_URL)))
            self.c3.lay.addWidget(lnk, 0, Qt.AlignLeft)
        brow = QHBoxLayout()
        self.go3 = QPushButton("Build")
        self.go3.setObjectName("play")
        self.go3.setStyleSheet("font-size: 16px; padding: 12px 40px;")
        self.go3.clicked.connect(self._build)
        brow.addWidget(self.go3)
        self.cancel3 = QPushButton("Cancel")
        self.cancel3.hide()
        self.cancel3.clicked.connect(self._cancel)
        brow.addWidget(self.cancel3)
        brow.addStretch(1)
        self.clean = QPushButton("Free disk space")
        self.clean.setToolTip("Delete the build files (about 1 GB). The game keeps working; a later rebuild starts from scratch.")
        self.clean.clicked.connect(self._clean)
        brow.addWidget(self.clean)
        self.c3.lay.addLayout(brow)
        self.bar3 = QProgressBar()
        self.bar3.setRange(0, 1000)
        self.c3.lay.addWidget(self.bar3)
        self.status3 = label("", "muted")
        self.c3.lay.addWidget(self.status3)
        self.logview = QPlainTextEdit()
        self.logview.setReadOnly(True)
        self.logview.setMaximumBlockCount(3000)
        self.logview.setFixedHeight(190)
        self.logview.setStyleSheet("font-family: monospace; font-size: 11px;")
        self.c3.lay.addWidget(self.logview)
        lay.addWidget(self.c3.frame)
        lay.addStretch(1)

        self.timer = QTimer(self)
        self.timer.setInterval(1000)
        self.timer.timeout.connect(self._tick)
        self.t0 = 0
        self._last = ("", 0.0)
        self.refresh()

    # ---- state ------------------------------------------------------------------------
    def busy(self):
        return self.task is not None and self.task.isRunning()

    def refresh_folder(self):
        cfg = self.cfg
        home = cfg.data_home()
        self.folder.setText(str(home))
        self.change_btn.setEnabled(not self.busy())
        notes = []
        if cfg.advanced.get("blocked_root") == str(home):
            notes.append((True, "The last build could not use this folder (access denied). "
                                + datafolder.blocked_message(home)))
        for lvl, text in datafolder.check_folder(home, cfg.game_files_ready(), writable=False):
            if lvl != "ok":
                notes.append((lvl == "error", text))
        if not self.fail.get("msg0"):
            self.msg0.setText("\n".join(t for _e, t in notes))
            self.msg0.setStyleSheet(f"color: {theme.ERR if any(e for e, _t in notes) else theme.ORANGE};")

    def refresh(self):
        cfg = self.cfg
        self.refresh_folder()
        root = cfg.game_files()
        have = (root / "default.xex").is_file()
        if have:
            ok, msg = installer.check_game_folder(root)
            self.c1.set_badge("Ready" if ok else "Problem", ok)
            self.msg1.setText(("Using " + str(root) + ("  -  " + msg if "different printing" in msg else "")) if ok else msg)
            self.msg1.setObjectName("muted" if ok else "bad")
        else:
            self.c1.set_badge("Needed", None)
            self.msg1.setText(self.fail.get("msg1", ""))
        if have:
            self.fail.pop("msg1", None)
        self.msg1.setStyleSheet(f"color: {theme.ERR if self.fail.get('msg1') or (have and not ok) else theme.MUTED};")
        tu = cfg["tu_package"].strip()
        if tu:
            ok_tu, msg_tu = installer.inspect_title_update(tu)
            self.ctu.set_badge("Ready" if ok_tu else "Problem", ok_tu)
            self.msgtu.setText(msg_tu + ("  This launcher has no title update tool: reinstall it."
                                         if ok_tu and builder.title_update_script() is None else
                                         "  The game is built with Title Update 4." if ok_tu else ""))
            self.msgtu.setStyleSheet(f"color: {theme.MUTED if ok_tu else theme.ERR};")
        else:
            self.ctu.set_badge("Recommended", None)
            self.msgtu.setText("No title update: the base game is built (no Horizon Rally, no 1000 Club).")
        d = self.dlc.text().strip()
        if d:
            try:
                found, skipped = installer.describe_dlc_folder(d)
                txt = (f"{len(found)} package(s) found: " + ", ".join(n for _f, n in found[:12]) +
                       (" ..." if len(found) > 12 else "")) if found else (
                    "No DLC found in this folder. Pick the folder of your DLC packages, or a Xenia content folder.")
                if skipped:
                    txt += (f"  Skipped {len(skipped)} folder(s) that do not look like DLC: " +
                            ", ".join(f"{n} ({why})" for n, why in skipped[:4]) + (" ..." if len(skipped) > 4 else ""))
                self.dlclist.setText(txt)
            except installer.InstallError as e:
                self.dlclist.setText(str(e))
        else:
            self.dlclist.setText("")
        n = game.installed_dlc(cfg)
        self.c2.set_badge(f"{n} installed" if n else "Optional", True if n else None)
        state, detail = builder.build_status(cfg.data_home(), cfg["tu_package"])
        label_for = {"built": ("Built", True), "stale": ("Rebuild needed", None), "partial": ("Unfinished", None),
                     "none": ("Not built", None), "nosource": ("Missing sources", False)}[state]
        self.c3.set_badge(*label_for)
        self.go3.setText({"stale": "Rebuild", "partial": "Resume build", "built": "Rebuild"}.get(state, "Build"))
        self.status3.setText(self.fail.get("status3") or detail)
        st = builder.read_state(cfg.data_home())
        if state == "built" and st.get("seconds") and not self.fail.get("status3"):
            self.status3.setText(f"{detail} (took {human_time(st['seconds'])}, up to {st.get('peak_rss_gb', '?')} GB of memory)")
        self.clean.setVisible(state == "built" and (builder.build_dir(cfg.data_home()) / "forza-recomp" / "out").exists())
        self._refresh_checks()
        busy = self.busy()
        self.go1.setEnabled(not busy and bool(self.pkg.text().strip()))
        self.go2.setEnabled(not busy and bool(self.dlc.text().strip()))
        self.go3.setEnabled(not busy and have)
        self._pkg_kind()

    def _refresh_checks(self):
        while self.checks.count():
            w = self.checks.takeAt(0).widget()
            if w:
                w.deleteLater()
        root = self.cfg.data_home()
        tools = build_env.detect_tools(root)
        for lvl, text in build_env.preflight(root, (self.cfg.game_files() / "default.xex").is_file(),
                                              bool(tools.missing)):
            r = StatusRow("")
            r.title.hide()
            r.set({"ok": True, "warn": None, "error": False}[lvl], text)
            self.checks.addWidget(r)
        if tools.problems:
            r = StatusRow("")
            r.title.hide()
            r.set(False, tools.problems[0])
            self.checks.addWidget(r)
        for h in tools.hints:
            r = StatusRow("")
            r.title.hide()
            r.set(None, h)
            self.checks.addWidget(r)
        if tools.missing:
            mb = sum(pins.SIZES_MB.get(m, 0) for m in tools.missing)
            self.tools_msg.setText(f"Missing tools ({', '.join(tools.missing)}) will be downloaded: about {mb} MB, "
                                   "checked against pinned checksums, kept in the launcher's data folder.")
        else:
            self.tools_msg.setText("Toolchain: " + "; ".join(f"{k} {v}" for k, v in tools.source.items()))

    def _pkg_kind(self):
        p = self.pkg.text().strip()
        if not p:
            self.go1.setText("Extract game files")
            return
        path = Path(p).expanduser()
        if path.is_dir() and not path.name.endswith(".data") and (path / "default.xex").is_file():
            self.go1.setText("Use this folder")
        elif path.is_file() and path.suffix.lower() == ".xex":
            self.go1.setText("Use these files")
        else:
            self.go1.setText("Extract game files")

    # ---- where the game is built ---------------------------------------------------------
    def change_folder(self, preset=None):
        """Choose the folder the game is built in (Discussion #31). Checks it (writable, space, OneDrive),
        offers to copy the saves and files of the current folder, then switches. Nothing is deleted."""
        if self.busy():
            return
        cfg = self.cfg
        cur = cfg.data_home()
        if preset is None:
            d = QFileDialog.getExistingDirectory(self, "Folder to build the game in", str(cur.parent))
            if not d:
                return
            new = datafolder.normalize_choice(d)
        else:
            new = Path(preset)
        if os.path.normcase(os.path.abspath(new)) == os.path.normcase(os.path.abspath(cur)):
            return
        game_dir = cur / "game"
        own_game = (game_dir / "default.xex").is_file() and not cfg.d.get("game_root", "").strip()
        user_bytes = datafolder.estimate_move(cur, True, False)
        game_bytes = datafolder.estimate_move(cur, False, True) if own_game else 0
        copy_user = copy_game = False
        if user_bytes > 1 << 20:
            box = QMessageBox(self)
            box.setWindowTitle("Move to the new folder")
            box.setText(f"New folder: {new}")
            box.setInformativeText(f"Copy your saves, DLC and the built game ({user_bytes / 2**30:.1f} GB) from {cur} "
                                   "to the new folder? The old folder is not changed or deleted.")
            copy_b = box.addButton("Copy them", QMessageBox.AcceptRole)
            box.addButton("Start empty", QMessageBox.DestructiveRole)
            cancel_b = box.addButton(QMessageBox.Cancel)
            box.exec()
            if box.clickedButton() in (cancel_b, None):
                return
            copy_user = box.clickedButton() is copy_b
        if own_game:
            box2 = QMessageBox(self)
            box2.setWindowTitle("Game files")
            box2.setText(f"The extracted game files ({game_bytes / 2**30:.1f} GB) are in {game_dir}.")
            box2.setInformativeText("Copy them to the new folder too, or leave them there and use them from there?")
            cg = box2.addButton("Copy them", QMessageBox.AcceptRole)
            box2.addButton("Leave them there", QMessageBox.RejectRole)
            cancel2 = box2.addButton(QMessageBox.Cancel)
            box2.exec()
            if box2.clickedButton() in (cancel2, None):
                return
            copy_game = box2.clickedButton() is cg
        extra = (user_bytes if copy_user else 0) + (game_bytes if copy_game else 0)
        checks = datafolder.check_folder(new, game_files_ready=cfg.game_files_ready(), extra_gb=extra / 2**30)
        errors = [t for lvl, t in checks if lvl == "error"]
        warns = [t for lvl, t in checks if lvl == "warn"]
        if errors:
            QMessageBox.warning(self, "This folder will not do", "\n\n".join(errors) + "\n\nChoose another folder.")
            return
        if warns and QMessageBox.question(self, "Use this folder anyway?", "\n\n".join(warns)) != QMessageBox.Yes:
            return

        def switch():
            cfg.advanced["data_root"] = str(new)
            if own_game and not copy_game:
                cfg["game_root"] = str(game_dir)
            cfg.advanced["blocked_root"] = ""
            cfg.advanced["data_root_asked"] = True
            cfg.save()

        if copy_user or copy_game:
            t = workers.move_task(cur, new, copy_user, copy_game)

            def done(ok, _msg):
                if ok:
                    switch()
            t.done.connect(done)
            self._start(t, self.bar0, self.cancel1, self.msg0)
            return
        err = datafolder.probe_writable(new)
        if err:
            QMessageBox.warning(self, "This folder will not do", datafolder.blocked_message(new, err))
            return
        switch()
        self.msg0.setText(f"The game will be built in {new}.")
        self.refresh()
        self.changed.emit()

    def offer_move(self):
        """Once, at start (Windows): the v0.1.x folder Documents\\forza is still in use and looks risky
        (OneDrive, not writable, or the last build was denied access): offer the new default."""
        cfg = self.cfg
        cur = cfg.data_home()
        if self.busy() or cfg.advanced.get("data_root") or cfg.advanced.get("data_root_asked") \
                or not datafolder.is_legacy_default(cur):
            return
        why = datafolder.in_onedrive(cur)
        if not why and cfg.advanced.get("blocked_root") == str(cur):
            why = "the last build could not use it (access denied)"
        if not why:
            err = datafolder.probe_writable(cur)
            why = f"Windows does not let the launcher write there ({err})" if err else None
        if not why:
            return
        new = datafolder.local_data_home()
        box = QMessageBox(self)
        box.setWindowTitle("Move the game's folder?")
        box.setText(f"The game's data folder is {cur}, and that is a risky place: {why}.")
        box.setInformativeText(
            f"OneDrive, antivirus software or Windows Controlled folder access can block a build there (\"access "
            f"denied\"). The new default is {new}. Your saves and files can be copied; nothing in the old folder "
            "is changed or deleted.")
        move = box.addButton("Choose what to move", QMessageBox.AcceptRole)
        keep = box.addButton("Keep this folder", QMessageBox.RejectRole)
        box.exec()
        if box.clickedButton() is move:
            self.change_folder(new)
        elif box.clickedButton() is keep:
            cfg.advanced["data_root_asked"] = True
            cfg.save()

    # ---- step 1 ------------------------------------------------------------------------
    def _pick_file(self):
        f, _ = QFileDialog.getOpenFileName(self, "Games on Demand package, disc ISO or default.xex",
                                           self.pkg.text() or str(Path.home()))
        if f:
            self.pkg.setText(f)
            self._pkg_changed()

    def _pick_dir(self):
        d = QFileDialog.getExistingDirectory(self, "Extracted game folder or package folder",
                                             self.pkg.text() or str(Path.home()))
        if d:
            self.pkg.setText(d)
            self._pkg_changed()

    def _pkg_changed(self):
        self.cfg["game_package"] = self.pkg.text().strip()
        self.cfg.save()
        self.refresh()

    def _use_game_files(self):
        p = Path(self.pkg.text().strip()).expanduser()
        self.cfg["game_package"] = str(p)
        if p.is_file() and p.suffix.lower() == ".xex" or (p.is_dir() and (p / "default.xex").is_file()
                                                         and not p.name.endswith(".data")):
            folder = p.parent if p.is_file() else p

            def use(ok, msg, folder=folder):   # runs before _finished, which refreshes
                if ok:
                    self.cfg["game_root"] = str(folder)
                    self.cfg.save()

            # the executable of another printing of the game is compared with the supported one: seconds
            t = workers.verify_task(str(folder))
            t.done.connect(use)
            self._start(t, self.bar1, self.cancel1, self.msg1)
            return
        try:
            installer.resolve_package(str(p))
        except installer.InstallError as e:
            self._show_fail("msg1", self.msg1, str(e))
            return
        dest = self.cfg.data_home() / "game"
        if (dest / "default.xex").is_file():
            if QMessageBox.question(self, "Replace game files", f"{dest} already holds game files. Replace them?") \
                    != QMessageBox.Yes:
                return
        self.cfg["game_root"] = ""
        self.cfg.save()
        self._start(workers.extract_task(str(p), dest), self.bar1, self.cancel1, self.msg1)

    # ---- title update ----------------------------------------------------------------------
    def _pick_tu(self):
        f, _ = QFileDialog.getOpenFileName(self, "Title update package", self.tu.text() or str(Path.home()))
        if f:
            self.tu.setText(f)
            self._tu_changed()

    def _tu_changed(self):
        self.cfg["tu_package"] = self.tu.text().strip()
        self.cfg.save()
        self.refresh()

    # ---- step 2 ------------------------------------------------------------------------
    def _pick_dlc(self):
        d = QFileDialog.getExistingDirectory(self, "Folder of DLC (packages or a Xenia content folder)", self.dlc.text() or str(Path.home()))
        if d:
            self.dlc.setText(d)
            self._dlc_changed()

    def _dlc_changed(self):
        self.cfg["dlc_dir"] = self.dlc.text().strip()
        self.cfg.save()
        self.refresh()

    def _install_dlc(self):
        self._dlc_changed()
        self._start(workers.dlc_task(self.dlc.text().strip(), self.cfg.data_home()), self.bar2, None, self.msg2)

    # ---- step 3 ------------------------------------------------------------------------
    def _build(self):
        cfg = self.cfg
        root = cfg.game_files()
        ok, msg = installer.check_game_folder(root)
        if not ok:
            QMessageBox.warning(self, "Game files", msg)
            return
        tu = cfg["tu_package"].strip()
        if tu:
            ok_tu, msg_tu = installer.inspect_title_update(tu)
            if not ok_tu:
                QMessageBox.warning(self, "Title update",
                                    msg_tu + "\n\nChoose a valid Title Update 4 package, or clear the field to build "
                                    "the base game.")
                return
        if self.ms is not None and not self.ms.isChecked():
            if not build_env.usable_vs_installed() and not (cfg.data_home() / "xwin" / "crt" / "include").is_dir():
                QMessageBox.information(self, "Microsoft license",
                                        "Accept Microsoft's license for the C++ runtime and Windows SDK to continue "
                                        "(or install Visual Studio 2022 Build Tools).")
                return
        tools = build_env.detect_tools(cfg.data_home())
        if tools.problems:
            QMessageBox.warning(self, "Toolchain", tools.problems[0])
            return
        errors = [t for lvl, t in build_env.preflight(cfg.data_home(), True, bool(tools.missing)) if lvl == "error"]
        if errors and QMessageBox.question(self, "This computer may not manage the build",
                                           "\n".join(errors) + "\n\nTry anyway?") != QMessageBox.Yes:
            return
        opts = builder.Options(cfg.data_home(), self.jobs.value(), self.lowprio.isChecked(),
                               bool(self.ms and self.ms.isChecked()), title_update=cfg["tu_package"].strip())
        self.logview.clear()
        self.bar3.setRange(0, 1000)
        t = workers.build_task(opts, str(root))
        t.progress.connect(lambda f, d: self.bar3.setValue(int(f * 1000)))
        t.log.connect(self.logview.appendPlainText)
        self.t0 = time.time()
        self.timer.start()
        self._start(t, self.bar3, self.cancel3, self.status3, status_to=self.status3)

    def _clean(self):
        if QMessageBox.question(self, "Free disk space",
                                "Delete the build files? The game keeps working. If the project updates, "
                                "the next build starts from scratch.") == QMessageBox.Yes:
            freed = builder.clean_build_tree(self.cfg.data_home())
            self.status3.setText(f"Freed up to {freed / 2**30:.1f} GB.")
            self.refresh()

    def _tick(self):
        if self.busy() and self.t0:
            text, frac = self._last
            el = time.time() - self.t0
            eta = ""
            if 0.26 < frac < 0.95:
                rem = el * (0.95 - frac) / max(frac - 0.24, 0.01)
                eta = f", about {human_time(rem)} left"
            self.status3.setText(f"{text}  ({human_time(el)} elapsed{eta})")

    # ---- running a task -----------------------------------------------------------------
    def _start(self, task, bar, cancel_btn, msg, status_to=None):
        if self.busy():
            return
        self.task = task
        self.fail.clear()
        self._bar, self._cancel_btn, self._msg = bar, cancel_btn, msg
        bar.show()
        bar.setValue(0)
        if cancel_btn:
            cancel_btn.show()
        msg.setStyleSheet(f"color: {theme.MUTED};")
        if bar is not self.bar3:
            task.progress.connect(lambda f, d, bar=bar, msg=msg: (bar.setRange(0, 1000), bar.setValue(int(f * 1000)),
                                                                   msg.setText(d[-90:])))
        task.status.connect(lambda s: self._set_status(s))
        task.done.connect(self._finished)
        for b in (self.go1, self.go2, self.go3):
            b.setEnabled(False)
        task.start()
        self.window.set_busy(True)

    def _set_status(self, s):
        self._last = (s, self._last[1])
        self._msg.setText(s)
        if self._bar is self.bar3:
            self._tick()

    def _show_fail(self, key, label_widget, message):
        """Show a failure in red and keep it there: refresh() runs right after a task ends and
        used to wipe the message, so a failed extraction just looked like the bar vanishing."""
        self.fail[key] = message
        label_widget.setText(message)
        label_widget.setStyleSheet(f"color: {theme.ERR};")

    def _finished(self, ok, message):
        self.timer.stop()
        self._bar.setVisible(self._bar is self.bar3)
        if self._cancel_btn:
            self._cancel_btn.hide()
        key = {id(self.msg0): "msg0", id(self.msg1): "msg1", id(self.msg2): "msg2", id(self.status3): "status3"}.get(id(self._msg), "msg")
        failed = not ok and message != "Cancelled."
        blocked = bool(getattr(self.task, "blocked", False))
        if blocked:
            self.cfg.advanced["blocked_root"] = str(self.cfg.data_home())
            self.cfg.save()
        elif ok and self._bar is self.bar3 and self.cfg.advanced.get("blocked_root"):
            self.cfg.advanced["blocked_root"] = ""
            self.cfg.save()
        if not ok and self._bar is self.bar3 and not blocked:
            message += "  Press Resume build to continue."
        if failed:
            self._show_fail(key, self._msg, message)
        else:
            self._msg.setText(message)
            self._msg.setStyleSheet(f"color: {theme.OK if ok else theme.MUTED};")
        # keep the QThread object alive until its run() has really returned
        self._zombies = [z for z in getattr(self, "_zombies", []) if z.isRunning()] + [self.task]
        self.task = None
        self.window.set_busy(False)
        self.refresh()
        self.changed.emit()
        if failed and blocked:
            if QMessageBox.question(self, "The folder is blocked", message + "\n\nChoose another folder now?") \
                    == QMessageBox.Yes:
                self.change_folder()
        elif failed:
            QMessageBox.warning(self, "Something went wrong", message)

    def _cancel(self):
        if self.task:
            self.task.cancel()
            self._msg.setText("Cancelling...")


# ================================================================== About =================

class AboutPage(QWidget):
    def __init__(self, cfg: Config, window):
        super().__init__()
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        sc, lay = scroll_page("About", "")
        outer.addWidget(sc)
        head = QHBoxLayout()
        logo = QLabel()
        pm = QPixmap(str(paths.asset("logo.png")))
        if not pm.isNull():
            logo.setPixmap(pm.scaled(96, 96, Qt.KeepAspectRatio, Qt.SmoothTransformation))
        head.addWidget(logo)
        col = QVBoxLayout()
        t = QLabel("Forza Horizon 1: ReImagined")
        t.setStyleSheet(f"font-size: 20px; font-weight: 800; color: {theme.CREAM};")
        col.addWidget(t)
        self.ver = label("", "muted")
        col.addWidget(self.ver)
        head.addLayout(col, 1)
        lay.addLayout(head)
        c, cl = card()
        cl.addWidget(label("This launcher contains no game code and no game data. It builds the game on your "
                           "computer from your own copy of Forza Horizon (Xbox 360), which you must own.", "muted"))
        cl.addWidget(label("Forza Horizon is (c) Microsoft. This project is not affiliated with, endorsed by or "
                           "sponsored by Microsoft, Playground Games or Turn 10 Studios. It contains no game code "
                           "or assets; you must own the game.", "muted"))
        lay.addWidget(c)
        from .updates import UpdateCard
        self.updates = UpdateCard(cfg, window)
        lay.addWidget(self.updates)
        for text, url in (("Project page and source", GITHUB_URL),
                          ("Releases", GITHUB_URL + "/releases/latest"),
                          ("Report a problem", GITHUB_URL + "/issues")):
            b = QPushButton(text)
            b.setObjectName("link")
            b.clicked.connect(lambda _=False, u=url: QDesktopServices.openUrl(QUrl(u)))
            lay.addWidget(b, 0, Qt.AlignLeft)
        lay.addStretch(1)
        self.cfg = cfg

    def refresh(self):
        install, _ = game.find_game(self.cfg)
        self.ver.setText(f"Launcher {__version__}   -   Game: {game.game_version(install)}")
