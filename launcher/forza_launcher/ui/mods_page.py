"""Mods (#149): add and remove mods, turn them on or off, and set their load order."""
from pathlib import Path

from PySide6.QtCore import QEvent, Qt, QTimer
from PySide6.QtWidgets import (QAbstractItemView, QFileDialog, QHBoxLayout, QHeaderView,
                               QMessageBox, QPushButton, QTableWidget, QTableWidgetItem,
                               QVBoxLayout, QWidget)

from .. import GITHUB_URL, mods
from .pages import open_folder, scroll_page
from .widgets import card, label
from . import theme

DOCS_URL = f"{GITHUB_URL}/blob/main/docs/modding.md"
GUIDE_LINK = f'<a href="{DOCS_URL}" style="color: {theme.ORANGE};">the mod guide</a>'


def _size(n: int) -> str:
    for unit in ("bytes", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n} {unit}" if unit == "bytes" else f"{n:.1f} {unit}"
        n /= 1024


def _link_label(text, name="muted"):
    lb = label(text, name)
    lb.setTextFormat(Qt.RichText)
    lb.setOpenExternalLinks(True)
    lb.setTextInteractionFlags(Qt.TextBrowserInteraction)
    return lb


class ModsPage(QWidget):
    COLUMNS = ["Mod", "Version", "Author", "Files"]

    def __init__(self, cfg, window):
        super().__init__()
        self.cfg, self.window = cfg, window
        self.mods = []
        self.clashes = {}
        self._filling = False
        self.setAcceptDrops(True)
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        sc, lay = scroll_page("Mods", "Change the game's files without editing them: each mod's "
                                      "files are laid over the game's archives when it starts.")
        outer.addWidget(sc)

        self.running_notice = label("The game is running: changes here apply the next time it starts.", "warn")
        self.running_notice.hide()
        lay.addWidget(self.running_notice)

        c, cl = card(spacing=14)
        cl.addWidget(label("Your mods", "cardTitle"))
        cl.addWidget(label("Ticked mods load from top to bottom. When two mods change the same file, "
                           "the one lower in the list wins. You can also drop a mod's .zip or folder "
                           "on this page.", "muted"))
        row = QHBoxLayout()
        row.setSpacing(12)
        self.add_button = QPushButton("Add mod…")
        self.add_button.clicked.connect(self.add_dialog)
        row.addWidget(self.add_button)
        self.remove_button = QPushButton("Remove")
        self.remove_button.clicked.connect(self.remove_selected)
        row.addWidget(self.remove_button)
        row.addStretch()
        self.folder_button = QPushButton("Open mods folder")
        self.folder_button.clicked.connect(lambda: open_folder(mods.mods_dir(self.cfg)))
        row.addWidget(self.folder_button)
        cl.addLayout(row)

        self.empty = _link_label("")
        cl.addWidget(self.empty)
        self.table = QTableWidget(0, len(self.COLUMNS))
        self.table.setHorizontalHeaderLabels(self.COLUMNS)
        self.table.setAccessibleName("Mods")
        self.table.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.table.setSelectionMode(QAbstractItemView.SingleSelection)
        self.table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.table.verticalHeader().hide()
        self.table.verticalHeader().setDefaultSectionSize(44)
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        for col in range(1, len(self.COLUMNS)):
            self.table.horizontalHeader().setSectionResizeMode(col, QHeaderView.ResizeToContents)
        self.table.setMinimumHeight(220)
        self.table.setStyleSheet(f'''
            QTableWidget {{ background: {theme.PANEL}; border: 1px solid {theme.BORDER};
                           gridline-color: {theme.BORDER}; selection-background-color: {theme.BORDER}; }}
            QTableWidget::item {{ padding: 8px; }}
            QTableWidget::item:selected {{ background: {theme.BORDER}; color: {theme.CREAM}; }}
            QHeaderView::section {{ padding: 8px; background: {theme.PANEL2}; border: none;
                                    border-bottom: 1px solid {theme.BORDER}; color: {theme.MUTED}; }}
        ''')
        self.table.itemChanged.connect(self._toggled)
        self.table.itemSelectionChanged.connect(self._selected)
        # A controller's A arrives as Return (ui/gamepad.py), which a check box in a table
        # does not take: Return or Enter turns the selected mod on or off.
        self.table.installEventFilter(self)
        cl.addWidget(self.table)
        row = QHBoxLayout()
        row.setSpacing(12)
        self.up_button = QPushButton("Move up")
        self.up_button.clicked.connect(lambda: self.move(-1))
        row.addWidget(self.up_button)
        self.down_button = QPushButton("Move down")
        self.down_button.clicked.connect(lambda: self.move(1))
        row.addWidget(self.down_button)
        row.addStretch()
        self.refresh_button = QPushButton("Refresh")
        self.refresh_button.clicked.connect(self.refresh)
        row.addWidget(self.refresh_button)
        cl.addLayout(row)
        self.details = label("", "muted")
        cl.addWidget(self.details)
        self.message = label("", "muted")
        cl.addWidget(self.message)
        lay.addWidget(c)

        c, cl = card(spacing=8)
        cl.addWidget(label("How mods work", "cardTitle"))
        cl.addWidget(label("A mod is a folder (or a .zip of one) with its files under media/, at the "
                           "game's own paths, and usually a mod.toml with its name. Changes apply the "
                           "next time you press Play. The game files stay as they are, so a rebuild or "
                           "an update keeps your mods.", "muted"))
        cl.addWidget(_link_label(f"Installing, load order, what to do when a mod will not load, and making "
                                 f"your own: {GUIDE_LINK}."))
        lay.addWidget(c)
        lay.addStretch()
        self.timer = QTimer(self)
        self.timer.setInterval(1000)
        self.timer.timeout.connect(self._running_changed)
        self.timer.start()
        self.refresh()

    # ---- state ------------------------------------------------------------------------------
    def _game_running(self) -> bool:
        running = getattr(self.window, "running", None)
        return bool(running and running())

    def _running_changed(self):
        on = self._game_running()
        if self.running_notice.isHidden() == on:
            self.running_notice.setVisible(on)
            self._selected()

    def _say(self, text, error=False):
        self.message.setText(text)
        self.message.setVisible(bool(text))
        self.message.setStyleSheet(f"color: {theme.ERR};" if error else "")

    # ---- the list ---------------------------------------------------------------------------
    def refresh(self):
        selected = self._current()
        self.mods = mods.ordered(self.cfg, mods.scan(mods.mods_dir(self.cfg), mods.game_media(self.cfg)))
        self.clashes = mods.conflicts(self.mods)
        self._fill(selected.folder if selected else None)
        folder = mods.mods_dir(self.cfg)
        self.empty.setText(f"No mods yet. Use <b>Add mod…</b> or put a mod folder in {folder}. "
                           f"How to install and make mods: {GUIDE_LINK}.")
        self.empty.setVisible(not self.mods)
        for w in (self.table, self.up_button, self.down_button, self.details):
            w.setVisible(bool(self.mods))
        if self.mods:
            on = sum(1 for m in self.mods if m.enabled and not m.problem)
            self._say(f"{on} of {len(self.mods)} mod(s) will load. Folder: {folder}")
        else:
            self._say("")
        self._running_changed()

    def _fill(self, select_folder=None):
        self._filling = True
        self.table.setRowCount(len(self.mods))
        for r, m in enumerate(self.mods):
            name = QTableWidgetItem(m.title + ("  (will not load)" if m.problem else ""))
            name.setFlags(Qt.ItemIsEnabled | Qt.ItemIsSelectable | Qt.ItemIsUserCheckable)
            name.setCheckState(Qt.Checked if m.enabled else Qt.Unchecked)
            name.setToolTip(m.problem or m.description or m.folder)
            self.table.setItem(r, 0, name)
            for col, text in ((1, m.version), (2, m.author), (3, f"{m.files} ({_size(m.size)})")):
                item = QTableWidgetItem(text)
                item.setFlags(Qt.ItemIsEnabled | Qt.ItemIsSelectable)
                self.table.setItem(r, col, item)
        self._filling = False
        rows = [i for i, m in enumerate(self.mods) if m.folder == select_folder]
        if rows:
            self.table.selectRow(rows[0])
        self._selected()

    def _current(self):
        r = self.table.currentRow()
        return self.mods[r] if 0 <= r < len(self.mods) else None

    def _toggled(self, item):
        if self._filling or item.column() != 0:
            return
        m = self.mods[item.row()]
        m.enabled = item.checkState() == Qt.Checked
        self._save()
        self.refresh()

    def eventFilter(self, obj, event):
        if (obj is self.table and event.type() == QEvent.KeyPress
                and event.key() in (Qt.Key_Return, Qt.Key_Enter)):
            item = self.table.item(self.table.currentRow(), 0)
            if item is not None:
                item.setCheckState(Qt.Unchecked if item.checkState() == Qt.Checked else Qt.Checked)
            return True
        return super().eventFilter(obj, event)

    def move(self, step):
        r = self.table.currentRow()
        to = r + step
        if not (0 <= r < len(self.mods) and 0 <= to < len(self.mods)):
            return
        self.mods[r], self.mods[to] = self.mods[to], self.mods[r]
        self._save()
        self.clashes = mods.conflicts(self.mods)
        self._fill(self.mods[to].folder)

    def _save(self):
        try:
            mods.remember(self.cfg, self.mods)
        except OSError as e:
            self._say(f"Could not save the mod list: {e}", error=True)

    def _selected(self):
        m = self._current()
        r = self.table.currentRow()
        self.up_button.setEnabled(m is not None and r > 0)
        self.down_button.setEnabled(m is not None and r < len(self.mods) - 1)
        self.remove_button.setEnabled(m is not None and not self._game_running())
        if m is None:
            self.details.setText("Select a mod to see its details." if self.mods else "")
            return
        lines = [m.summary()]
        if m.description:
            lines.append(m.description)
        lines.append(f"Folder: {m.folder}")
        if m.problem:
            lines.append(f"Will not load: {m.problem}")
        elif not m.enabled:
            lines.append("Turned off: tick it to load it.")
        if m.note:
            lines.append(m.note)
        clashes = self.clashes.get(m.folder, [])
        if clashes:
            titles = {x.folder: x.title for x in self.mods}
            by_other = {}
            for name, other, wins in clashes:
                by_other.setdefault((other, wins), []).append(name)
            for (other, wins), names in by_other.items():
                shown = ", ".join(f"media/{n}" for n in names[:3]) + (", …" if len(names) > 3 else "")
                who = f"this mod's copy is used (it is lower than {titles[other]})" if wins else \
                      f"{titles[other]}'s copy is used (it is lower in the list)"
                lines.append(f"Same file(s) as {titles[other]}: {shown}; {who}.")
        self.details.setText("\n".join(lines))

    # ---- adding and removing ----------------------------------------------------------------
    def _confirm(self, title, text) -> bool:
        return QMessageBox.question(self, title, text) == QMessageBox.Yes

    def add_dialog(self):
        box = QMessageBox(self)
        box.setWindowTitle("Add mod")
        box.setText("Add a mod from a .zip file or from a folder?")
        zip_button = box.addButton("Zip file…", QMessageBox.AcceptRole)
        folder_button = box.addButton("Folder…", QMessageBox.AcceptRole)
        box.addButton(QMessageBox.Cancel)
        box.exec()
        if box.clickedButton() is zip_button:
            path, _ = QFileDialog.getOpenFileName(self, "Mod to add", str(Path.home()), "Mod archives (*.zip)")
        elif box.clickedButton() is folder_button:
            path = QFileDialog.getExistingDirectory(self, "Mod folder to add", str(Path.home()))
        else:
            return
        if path:
            self.add_path(Path(path))

    def add_path(self, path: Path) -> bool:
        """Adds the mod in `path` (a .zip or a folder); says how it went on the page."""
        directory = mods.mods_dir(self.cfg)
        try:
            try:
                mod = mods.install(path, directory, media_of_game=mods.game_media(self.cfg))
            except mods.ModExists as clash:
                if self._game_running():
                    raise mods.InstallError(f'A mod named "{clash.folder}" is there already and the game '
                                            "is running: close the game to replace it.") from None
                if not self._confirm("Replace mod", f'A mod named "{clash.folder}" is there already. '
                                                    "Replace it with this one?"):
                    self._say(f'"{path.name}" was not added: "{clash.folder}" is kept.')
                    return False
                mod = mods.install(path, directory, replace=True, media_of_game=mods.game_media(self.cfg))
        except mods.InstallError as e:
            self.refresh()
            self._say(f"Not added: {e}", error=True)
            return False
        self.refresh()
        rows = [i for i, m in enumerate(self.mods) if m.folder == mod.folder]
        if rows:
            self.table.selectRow(rows[0])
        later = " It loads the next time the game starts." if self._game_running() else ""
        self._say(f"Added {mod.summary()} ({mod.files} file(s)).{later}")
        return True

    def remove_selected(self):
        m = self._current()
        if m is None:
            return
        if self._game_running():
            self._say("Close the game before removing a mod: its files are in use.", error=True)
            return
        if not self._confirm("Remove mod", f'Remove "{m.title}"? Its folder {m.folder} is deleted from the '
                                           "mods folder."):
            return
        try:
            mods.remove(self.cfg, m.folder)
        except (mods.InstallError, OSError) as e:
            self._say(str(e), error=True)
            return
        self.refresh()
        self._say(f"Removed {m.title}.")

    # ---- drag and drop ----------------------------------------------------------------------
    def dragEnterEvent(self, event):
        urls = event.mimeData().urls() if event.mimeData().hasUrls() else []
        if urls and all(u.isLocalFile() for u in urls):
            event.acceptProposedAction()

    def dropEvent(self, event):
        for url in event.mimeData().urls():
            if url.isLocalFile():
                self.add_path(Path(url.toLocalFile()))
        event.acceptProposedAction()
