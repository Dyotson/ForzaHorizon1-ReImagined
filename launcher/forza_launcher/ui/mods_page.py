"""Mods (#149): the mods in the mods folder, on or off, and their load order."""
from PySide6.QtCore import Qt
from PySide6.QtWidgets import (QAbstractItemView, QHBoxLayout, QHeaderView, QPushButton,
                               QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget)

from .. import GITHUB_URL, mods
from .pages import open_folder, scroll_page
from .widgets import card, label
from . import theme

DOCS_URL = f"{GITHUB_URL}/blob/main/docs/modding.md"


def _size(n: int) -> str:
    for unit in ("bytes", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n} {unit}" if unit == "bytes" else f"{n:.1f} {unit}"
        n /= 1024


class ModsPage(QWidget):
    COLUMNS = ["Mod", "Version", "Author", "Files"]

    def __init__(self, cfg, window):
        super().__init__()
        self.cfg, self.window = cfg, window
        self.mods = []
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        sc, lay = scroll_page("Mods", "Change the game's files without editing them: each mod's "
                                      "files are laid over the game's archives when it starts.")
        outer.addWidget(sc)

        c, cl = card(spacing=14)
        cl.addWidget(label("Your mods", "cardTitle"))
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
        cl.addWidget(self.table)
        row = QHBoxLayout()
        row.setSpacing(12)
        self.up_button = QPushButton("Move up")
        self.up_button.clicked.connect(lambda: self.move(-1))
        row.addWidget(self.up_button)
        self.down_button = QPushButton("Move down")
        self.down_button.clicked.connect(lambda: self.move(1))
        row.addWidget(self.down_button)
        self.refresh_button = QPushButton("Refresh")
        self.refresh_button.clicked.connect(self.refresh)
        row.addWidget(self.refresh_button)
        row.addStretch()
        self.folder_button = QPushButton("Open mods folder")
        self.folder_button.clicked.connect(lambda: open_folder(mods.mods_dir(self.cfg)))
        row.addWidget(self.folder_button)
        cl.addLayout(row)
        self.details = label("", "muted")
        cl.addWidget(self.details)
        self.message = label("", "muted")
        cl.addWidget(self.message)
        lay.addWidget(c)

        c, cl = card(spacing=8)
        cl.addWidget(label("How mods load", "cardTitle"))
        cl.addWidget(label("Ticked mods load from top to bottom: when two change the same file, the "
                           "lower one wins. Changes apply the next time you press Play. The game "
                           "files stay as they are, so a rebuild or an update keeps your mods.", "muted"))
        how = label(f'Each mod is a folder with its files under <b>media/</b>, at the game\'s own paths. '
                    f'<a href="{DOCS_URL}" style="color: {theme.ORANGE};">Making a mod</a> explains '
                    f'the layout and how to find a file\'s path.', "muted")
        how.setTextFormat(Qt.RichText)
        how.setOpenExternalLinks(True)
        how.setTextInteractionFlags(Qt.TextBrowserInteraction)
        cl.addWidget(how)
        lay.addWidget(c)
        lay.addStretch()
        self._filling = False
        self.refresh()

    # ---- the list -------------------------------------------------------------------------
    def refresh(self):
        selected = self._current()
        self.mods = mods.ordered(self.cfg, mods.scan(mods.mods_dir(self.cfg)))
        self._fill(selected.folder if selected else None)
        if not self.mods:
            self.message.setText(f"No mods yet. Put each mod in its own folder in {mods.mods_dir(self.cfg)}, "
                                 "then press Refresh.")
        else:
            on = sum(1 for m in self.mods if m.enabled and not m.problem)
            self.message.setText(f"{on} of {len(self.mods)} mod(s) will load. Folder: {mods.mods_dir(self.cfg)}")

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

    def move(self, step):
        r = self.table.currentRow()
        to = r + step
        if not (0 <= r < len(self.mods) and 0 <= to < len(self.mods)):
            return
        self.mods[r], self.mods[to] = self.mods[to], self.mods[r]
        self._save()
        self._fill(self.mods[to].folder)

    def _save(self):
        try:
            mods.remember(self.cfg, self.mods)
        except OSError as e:
            self.message.setText(f"Could not save the mod list: {e}")

    def _selected(self):
        m = self._current()
        r = self.table.currentRow()
        self.up_button.setEnabled(m is not None and r > 0)
        self.down_button.setEnabled(m is not None and r < len(self.mods) - 1)
        if m is None:
            self.details.setText("Select a mod to see its details.")
            return
        lines = [m.summary()]
        if m.description:
            lines.append(m.description)
        lines.append(f"Folder: {m.folder}")
        if m.problem:
            lines.append(f"Will not load: {m.problem}")
        if m.note:
            lines.append(m.note)
        self.details.setText("\n".join(lines))
