"""The Settings page: every forza.toml key plus the launcher's own options, in groups."""
from pathlib import Path

from PySide6.QtCore import QTimer, Qt, Signal
from PySide6.QtGui import QIntValidator
from PySide6.QtWidgets import (QButtonGroup, QCheckBox, QComboBox, QDoubleSpinBox, QFileDialog,
                               QFrame, QHBoxLayout, QLabel, QLineEdit, QListWidget, QPlainTextEdit,
                               QPushButton, QRadioButton, QScrollArea, QSpinBox, QStackedWidget,
                               QVBoxLayout, QWidget)

from .. import PUBLIC_SERVER, game, schema
from ..config import Config, render_toml, validate_extras
from . import theme
from .widgets import Switch, card, label

NO_WHEEL = Qt.StrongFocus


class _Wheelless:
    """Spin boxes and combos must not change when the page is scrolled over them."""

    def wheelEvent(self, e):
        if self.hasFocus():
            super().wheelEvent(e)
        else:
            e.ignore()


class Combo(_Wheelless, QComboBox):
    pass


class Spin(_Wheelless, QSpinBox):
    pass


class DSpin(_Wheelless, QDoubleSpinBox):
    pass


DEPENDS = {  # key -> the bool setting that must be on for it to matter
    "mnk_mouse": "mnk_mode", "mnk_sensitivity": "mnk_mode", "mnk_passthrough": "mnk_mode",
    "mnk_stick_ramp": "mnk_mode",
}


def make_control(s: schema.Setting, value, on_change):
    """A widget for the setting, calling on_change(value) when the player changes it."""
    if s.kind == "bool":
        w = Switch()
        w.setChecked(bool(value))
        w.toggled.connect(on_change)
        return w
    if s.kind == "int":
        w = Spin()
        w.setRange(int(s.lo if s.lo is not None else -10 ** 9), int(s.hi if s.hi is not None else 10 ** 9))
        w.setSingleStep(int(s.step))
        w.setValue(int(value))
        w.setMinimumWidth(130)
        w.valueChanged.connect(on_change)
        return w
    if s.kind == "float":
        w = DSpin()
        w.setRange(s.lo if s.lo is not None else -1e9, s.hi if s.hi is not None else 1e9)
        w.setDecimals(2)
        w.setSingleStep(s.step)
        w.setValue(float(value))
        w.setMinimumWidth(130)
        w.valueChanged.connect(on_change)
        return w
    if s.kind == "choice":
        w = Combo()
        for v, text in s.choices:
            w.addItem(str(text), v)
        i = w.findData(value)
        w.setCurrentIndex(max(i, 0))
        w.setMinimumWidth(190)
        w.currentIndexChanged.connect(lambda _i, w=w: on_change(w.currentData()))
        return w
    if s.kind == "combo":
        w = Combo()
        w.setEditable(True)
        for v, text in s.choices:
            w.addItem(str(text), v)
        i = w.findData(value)
        if i >= 0:
            w.setCurrentIndex(i)
        else:
            w.setEditText(str(value))
        w.setMinimumWidth(190)
        w.lineEdit().editingFinished.connect(lambda w=w: on_change(
            w.itemData(w.currentIndex()) if w.currentIndex() >= 0 and w.currentText() == w.itemText(w.currentIndex())
            else w.currentText().strip()))
        w.currentIndexChanged.connect(lambda i, w=w: on_change(w.itemData(i)) if i >= 0 else None)
        return w
    if s.kind == "fps":
        w = Combo()
        w.setEditable(True)
        for v in (30, 60, 90, 120, 144, 165, 240):
            w.addItem(f"{v} fps", v)
        w.lineEdit().setValidator(QIntValidator(1, 999))
        i = w.findData(int(value))
        if i >= 0:
            w.setCurrentIndex(i)
        else:
            w.setEditText(f"{value} fps")
        w.setMinimumWidth(130)

        def changed(_=None, w=w):
            digits = "".join(c for c in w.currentText() if c.isdigit())
            if digits:
                on_change(schema.coerce(s, int(digits)))
        w.currentIndexChanged.connect(changed)
        w.lineEdit().editingFinished.connect(changed)
        return w
    w = QLineEdit(str(value))  # str, keys
    w.setMinimumWidth(240)
    w.editingFinished.connect(lambda w=w: on_change(w.text().strip()))
    return w


class SettingRow(QWidget):
    def __init__(self, s: schema.Setting, cfg: Config, changed):
        super().__init__()
        self.s = s
        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 6, 0, 6)
        lay.setSpacing(24)
        col = QVBoxLayout()
        col.setSpacing(2)
        title = QLabel(s.label)
        title.setStyleSheet("font-weight: 600;")
        col.addWidget(title)
        if s.help:
            col.addWidget(label(s.help, "help"))
        lay.addLayout(col, 1)
        self.control = make_control(s, cfg.settings.get(s.key, s.default),
                                    lambda v, s=s: changed(s.key, v))
        tip = f"{s.key}" + (f" (default {s.default!r})" if s.default != "" else "")
        self.control.setToolTip(tip)
        title.setToolTip(tip)
        lay.addWidget(self.control, 0, Qt.AlignRight | Qt.AlignVCenter)


def heading(text):
    lb = QLabel(text.upper())
    lb.setObjectName("section")
    lb.setContentsMargins(0, 14, 0, 2)
    return lb


def hline():
    f = QFrame()
    f.setFixedHeight(1)
    f.setStyleSheet(f"background: {theme.BORDER};")
    return f


class SettingsPage(QWidget):
    changed = Signal()

    def __init__(self, cfg: Config, window):
        super().__init__()
        self.cfg = cfg
        self.window = window
        self.rows = {}
        self._save_timer = QTimer(self)
        self._save_timer.setSingleShot(True)
        self._save_timer.setInterval(350)
        self._save_timer.timeout.connect(self.flush)

        root = QHBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(16)
        self.groups = QListWidget()
        self.groups.setObjectName("groups")
        self.groups.setFixedWidth(150)
        self.stack = QStackedWidget()
        root.addWidget(self.groups)
        root.addWidget(self.stack, 1)
        for g in schema.GROUPS:
            self.groups.addItem(g)
            self.stack.addWidget(self._build_group(g))
        self.groups.currentRowChanged.connect(self.stack.setCurrentIndex)
        self.groups.setCurrentRow(0)
        self._apply_dependencies()

    # --- plumbing -------------------------------------------------------------------
    def set_value(self, key, value):
        self.cfg.settings[key] = schema.coerce(schema.BY_KEY[key], value)
        self._apply_dependencies()
        self.touched()

    def touched(self):
        self._save_timer.start()
        self.refresh_preview()

    def flush(self):
        """Save launcher.json and write forza.toml."""
        self._save_timer.stop()
        try:
            self.cfg.save()
        except OSError as e:
            self.window.status(f"Could not save launcher settings: {e}", True)
        self.window.write_toml()
        self.changed.emit()

    def _apply_dependencies(self):
        for key, dep in DEPENDS.items():
            row = self.rows.get(key)
            if row:
                row.control.setEnabled(bool(self.cfg.settings.get(dep)))
        for s in schema.S:
            if s.key.startswith("keybind_") and s.key in self.rows:
                self.rows[s.key].control.setEnabled(bool(self.cfg.settings.get("mnk_mode")))

    def _rows(self, lay, group, skip=()):
        section = None
        for s in schema.for_group(group):
            if s.key in skip:
                continue
            if s.section != section:
                section = s.section
                lay.addWidget(heading(section))
            row = SettingRow(s, self.cfg, self.set_value)
            self.rows[s.key] = row
            lay.addWidget(row)

    def _page(self, title, lead):
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(0, 0, 14, 20)
        lay.setSpacing(2)
        t = QLabel(title)
        t.setObjectName("pageTitle")
        t.setStyleSheet("font-size: 22px;")
        lay.addWidget(t)
        if lead:
            lay.addWidget(label(lead, "pageLead"))
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        scroll.setWidget(w)
        return scroll, lay

    # --- groups ---------------------------------------------------------------------
    def _build_group(self, g):
        leads = {
            "Display": "Window, frame rate and picture.",
            "Controls": "Controllers work out of the box; keyboard and mouse is available but untested.",
            "Audio": "",
            "Online": "Xbox LIVE is gone, so the game signs in to a Forza Live server instead.",
            "Profile": "Your saves live in the game's data folder. You can also continue a Xenia Canary save.",
            "Advanced": "For testing and troubleshooting. Everything here is also a forza.toml key or a command-line flag.",
        }
        scroll, lay = self._page(g, leads[g])
        if g == "Online":
            self._online_block(lay)
        if g == "Profile":
            self._profile_block(lay)
        self._rows(lay, g)
        if g == "Controls":
            lay.addWidget(label("Everything on this page is a forza.toml key; the keyboard bindings "
                                "use the SDK's key names.", "help"))
        if g == "Controls":
            self._pad_block(lay)
        if g == "Advanced":
            self._advanced_block(lay)
        lay.addStretch(1)
        return scroll

    def _pad_block(self, lay):
        lay.insertWidget(2, heading("Players"))
        row = QHBoxLayout()
        sw = Switch()
        sw.setChecked(bool(self.cfg.advanced.get("pad_per_user")))
        sw.toggled.connect(lambda v: self._adv("pad_per_user", v))
        col = QVBoxLayout()
        t = QLabel("Each controller is its own player")
        t.setStyleSheet("font-weight: 600;")
        col.addWidget(t)
        col.addWidget(label("Off (recommended): every controller drives player 1, so START on any pad "
                            "gets past the title screen. Sets FORZA_PAD_PER_USER=1 when on.", "help"))
        row.addLayout(col, 1)
        row.addWidget(sw)
        w = QWidget()
        w.setLayout(row)
        lay.insertWidget(3, w)

    def _adv(self, key, value):
        self.cfg.advanced[key] = value
        self.touched()

    # Online
    def _online_block(self, lay):
        lay.addWidget(heading("Server"))
        self.mode_group = QButtonGroup(self)
        modes = [
            ("public", f"Public server", f"{PUBLIC_SERVER}: the shared online world, leaderboards and "
                                          "rivals. You forward no ports."),
            ("local", "This PC", "Starts a Forza Live server just for this session (127.0.0.1:21200). "
                                  "Online features work, but only you are in it."),
            ("custom", "My own server", "A server you or a friend host (docs/hosting.md)."),
            ("offline", "Offline", "No Xbox LIVE and no server."),
        ]
        self.radios = {}
        for key, title, text in modes:
            rb = QRadioButton(title)
            rb.setStyleSheet("font-weight: 600;")
            self.mode_group.addButton(rb)
            self.radios[key] = rb
            lay.addWidget(rb)
            hint = label(text, "help")
            hint.setContentsMargins(26, 0, 0, 6)
            lay.addWidget(hint)
            rb.toggled.connect(lambda on, k=key: on and self._set_mode(k))
        self.custom = QLineEdit(self.cfg.online.get("custom_server", ""))
        self.custom.setPlaceholderText("host[:port], for example example.org:21200")
        self.custom.editingFinished.connect(self._custom_changed)
        lay.addWidget(self.custom)
        self.radios[self.cfg.online["mode"]].setChecked(True)
        self.custom.setEnabled(self.cfg.online["mode"] == "custom")

    def _set_mode(self, mode):
        self.cfg.online["mode"] = mode
        self.custom.setEnabled(mode == "custom")
        self.touched()

    def _custom_changed(self):
        self.cfg.online["custom_server"] = self.custom.text().strip()
        self.touched()

    # Profile
    def _profile_block(self, lay):
        lay.addWidget(heading("Save"))
        self.own = QRadioButton("My own profile")
        self.own.setStyleSheet("font-weight: 600;")
        lay.addWidget(self.own)
        h = label(f"Saves in {self.cfg.data_home() / game.LOCAL_PROFILE / game.TITLE_ID}", "help")
        h.setContentsMargins(26, 0, 0, 6)
        lay.addWidget(h)
        self.xenia = QRadioButton("Continue a Xenia Canary save")
        self.xenia.setStyleSheet("font-weight: 600;")
        lay.addWidget(self.xenia)
        h = label("Copies your Xenia profile's Forza Horizon save once and plays the copy, signed in "
                  "with that profile's XUID. The Xenia original is never modified.", "help")
        h.setContentsMargins(26, 0, 0, 6)
        lay.addWidget(h)
        form, f = card(QVBoxLayout, (14, 12, 14, 12), 8)
        row = QHBoxLayout()
        self.xcontent = QLineEdit(self.cfg.profile.get("xenia_content", ""))
        self.xcontent.setPlaceholderText(str(game.default_xenia_content()))
        b = QPushButton("Browse")
        b.clicked.connect(self._browse_xenia)
        row.addWidget(QLabel("Xenia content folder"))
        row.addWidget(self.xcontent, 1)
        row.addWidget(b)
        f.addLayout(row)
        row = QHBoxLayout()
        self.xuid = Combo()
        self.xuid.setEditable(True)
        self.xuid.setMinimumWidth(240)
        row.addWidget(QLabel("Profile (XUID)"))
        row.addWidget(self.xuid, 1)
        scan = QPushButton("Find profiles")
        scan.clicked.connect(self._scan_xenia)
        row.addWidget(scan)
        f.addLayout(row)
        self.recopy = QPushButton("Copy the save again on the next start")
        self.recopy.setCheckable(True)
        f.addWidget(self.recopy)
        self.xenia_note = label("", "help")
        f.addWidget(self.xenia_note)
        lay.addWidget(form)
        self.xform = form
        (self.xenia if self.cfg.profile["mode"] == "xenia" else self.own).setChecked(True)
        self.xform.setEnabled(self.cfg.profile["mode"] == "xenia")
        self.xenia.toggled.connect(self._profile_mode)
        self.xcontent.editingFinished.connect(self._xenia_fields)
        self.xuid.lineEdit().editingFinished.connect(self._xenia_fields)
        self.xuid.setEditText(self.cfg.profile.get("xenia_xuid", ""))
        self._scan_xenia(quiet=True)

    def _profile_mode(self, on):
        self.cfg.profile["mode"] = "xenia" if on else "own"
        self.xform.setEnabled(on)
        self.touched()

    def _browse_xenia(self):
        d = QFileDialog.getExistingDirectory(self, "Xenia content folder",
                                             self.xcontent.text() or str(game.default_xenia_content().parent))
        if d:
            self.xcontent.setText(d)
            self._xenia_fields()
            self._scan_xenia()

    def _xenia_fields(self):
        self.cfg.profile["xenia_content"] = self.xcontent.text().strip()
        self.cfg.profile["xenia_xuid"] = self.xuid.currentText().strip().upper()
        self.touched()

    def _scan_xenia(self, quiet=False):
        content = Path(self.xcontent.text().strip() or game.default_xenia_content())
        found = game.xenia_candidates(content)
        keep = self.xuid.currentText()
        self.xuid.clear()
        self.xuid.addItems(found)
        self.xuid.setEditText(keep or (found[0] if len(found) == 1 else ""))
        if found:
            self.xenia_note.setText(f"Found {len(found)} profile(s) with a Forza Horizon save in {content}.")
        else:
            self.xenia_note.setText(f"No Forza Horizon save found in {content}.")
        self._xenia_fields() if not quiet else None

    # Advanced
    def _advanced_block(self, lay):
        adv = self.cfg.advanced
        lay.insertWidget(2, heading("Launcher"))

        def line(title, help_, key, placeholder="", browse=False):
            w = QWidget()
            h = QHBoxLayout(w)
            h.setContentsMargins(0, 6, 0, 6)
            h.setSpacing(16)
            col = QVBoxLayout()
            col.setSpacing(2)
            t = QLabel(title)
            t.setStyleSheet("font-weight: 600;")
            col.addWidget(t)
            col.addWidget(label(help_, "help"))
            h.addLayout(col, 1)
            e = QLineEdit(adv.get(key, ""))
            e.setPlaceholderText(placeholder)
            e.setMinimumWidth(280)
            e.editingFinished.connect(lambda e=e: self._adv(key, e.text().strip()))
            h.addWidget(e)
            if browse:
                b = QPushButton("Browse")
                b.clicked.connect(lambda _=False, e=e, t=title: self._browse_into(e, t, key))
                h.addWidget(b)
            return w

        items = [
            line("Game program folder", "Where the game's executable is. Empty: the game the launcher built (the runtime folder in the game data folder).",
                 "game_dir", "automatic", True),
            line("Game data folder", "Saves, DLC, shader cache, extracted game files and the log. "
                 "Empty: " + str(game.paths.data_home()), "data_root", "automatic", True),
            line("Extra command-line flags", "Added to the game's command line, for example "
                 "--resolution_scale=2.", "extra_args", "--flag=value"),
        ]
        for i, w in enumerate(items):
            lay.insertWidget(3 + i, w)
        lay.addWidget(heading("Other settings"))
        lay.addWidget(label("Any other forza.toml key, one per line (key = value). They are written "
                            "to forza.toml as they are.", "help"))
        self.extras = QPlainTextEdit(adv.get("extras_toml", ""))
        self.extras.setPlaceholderText('vulkan_validation_enabled = true')
        self.extras.setFixedHeight(90)
        self.extras_err = label("", "bad")
        self.extras.textChanged.connect(self._extras_changed)
        lay.addWidget(self.extras)
        lay.addWidget(self.extras_err)
        lay.addWidget(heading("Environment variables"))
        lay.addWidget(label("NAME=value, one per line, set for the game (for example FORZA_NETTRACE=1).", "help"))
        self.envs = QPlainTextEdit(adv.get("extra_env", ""))
        self.envs.setFixedHeight(70)
        self.envs.textChanged.connect(lambda: self._adv("extra_env", self.envs.toPlainText()))
        lay.addWidget(self.envs)
        lay.addWidget(heading("What Play runs"))
        self.preview = QPlainTextEdit()
        self.preview.setReadOnly(True)
        self.preview.setFixedHeight(170)
        self.preview.setStyleSheet("font-family: monospace; font-size: 12px;")
        lay.addWidget(self.preview)
        reset = QPushButton("Reset all settings to defaults")
        reset.clicked.connect(self.window.reset_settings)
        lay.addWidget(reset, 0, Qt.AlignLeft)
        self.refresh_preview()

    def _browse_into(self, edit, title, key):
        d = QFileDialog.getExistingDirectory(self, title, edit.text() or str(Path.home()))
        if d:
            edit.setText(d)
            self._adv(key, d)

    def _extras_changed(self):
        text = self.extras.toPlainText()
        err = validate_extras(text)
        self.extras_err.setText(("Not valid TOML yet: " + err) if err else "")
        if not err:
            self._adv("extras_toml", text)

    def refresh_preview(self):
        if not hasattr(self, "preview"):
            return
        self.preview.setPlainText(self.window.preview_text())

    def rebuild(self):
        """After a reset: recreate the pages from the config."""
        self.rows.clear()
        cur = self.groups.currentRow()
        while self.stack.count():
            w = self.stack.widget(0)
            self.stack.removeWidget(w)
            w.deleteLater()
        for g in schema.GROUPS:
            self.stack.addWidget(self._build_group(g))
        self.groups.setCurrentRow(cur)
        self._apply_dependencies()
