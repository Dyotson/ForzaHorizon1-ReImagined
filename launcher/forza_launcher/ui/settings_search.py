"""Search existing settings controls without duplicating their state."""
from dataclasses import dataclass

from PySide6.QtCore import QEvent, QSize, Qt, Signal
from PySide6.QtGui import QKeySequence, QShortcut
from PySide6.QtWidgets import (QHBoxLayout, QLabel, QLineEdit, QListWidget,
                               QListWidgetItem, QSizePolicy, QVBoxLayout, QWidget)


@dataclass
class SearchTarget:
    label: str
    group: str
    section: str
    widget: QWidget
    focus: QWidget
    keywords: str

    def rank(self, query):
        title = self.label.casefold()
        text = " ".join((title, self.group, self.section, self.keywords)).casefold()
        if not all(word in text for word in query.split()):
            return None
        return 0 if query == title else 1 if title.startswith(query) else 2 if query in title else 3


class SettingsSearch(QWidget):
    chosen = Signal(object)

    def __init__(self, parent):
        super().__init__(parent)
        self.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Maximum)
        self.targets = []
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)
        bar = QHBoxLayout()
        self.edit = QLineEdit()
        self.edit.setPlaceholderText("Search settings… (Ctrl+F)")
        self.edit.setAccessibleName("Search settings")
        self.edit.setClearButtonEnabled(True)
        self.count = QLabel()
        bar.addWidget(self.edit, 1)
        bar.addWidget(self.count)
        layout.addLayout(bar)
        self.results = QListWidget()
        self.results.setObjectName("settingsSearchResults")
        self.results.setAccessibleName("Settings search results")
        self.results.setWordWrap(True)
        layout.addWidget(self.results)
        self.results.hide()
        self.count.hide()
        self.edit.textChanged.connect(self.refresh)
        self.edit.returnPressed.connect(self._choose_current)
        self.results.itemClicked.connect(self._choose)
        self.edit.installEventFilter(self)
        self.results.installEventFilter(self)
        self.shortcut = QShortcut(QKeySequence.Find, parent)
        self.shortcut.setContext(Qt.WidgetWithChildrenShortcut)
        self.shortcut.activated.connect(self.focus_search)

    def add(self, label, group, section, widget, focus=None, keywords=""):
        focus = focus if focus is not None else widget
        if not focus.accessibleName():
            focus.setAccessibleName(label)
        self.targets.append(SearchTarget(label, group, section, widget, focus, keywords))

    def add_setting(self, setting, row):
        choices = " ".join(str(value) + " " + str(label) for value, label in setting.choices)
        self.add(setting.label, setting.group, setting.section, row, row.control,
                 " ".join((setting.key, setting.help, choices)))

    def clear_targets(self):
        self.results.clear()
        self.targets.clear()
        self.results.hide()

    def refresh(self):
        query = self.edit.text().strip().casefold()
        self.results.clear()
        if not query:
            self.count.hide()
            self.results.hide()
            return
        matches = []
        for index, target in enumerate(self.targets):
            rank = target.rank(query)
            if rank is not None and not target.widget.isHidden():
                matches.append((rank, index, target))
        for _, _, target in sorted(matches):
            item = QListWidgetItem(f"{target.label}\n{target.group} · {target.section}")
            item.setData(Qt.UserRole, target)
            item.setSizeHint(QSize(0, 54))
            item.setToolTip(target.label + "\n" + target.keywords)
            self.results.addItem(item)
        total = len(matches)
        self.count.setText(f"{total} setting{'s' if total != 1 else ''} found" if total else "No settings found")
        self.count.show()
        self.results.setFixedHeight(min(total, 4) * 54 + 8)
        self.results.setVisible(bool(total))
        if total:
            self.results.setCurrentRow(0)

    def dismiss(self):
        self.results.hide()

    def focus_search(self):
        self.edit.setFocus(Qt.ShortcutFocusReason)
        self.edit.selectAll()
        self.refresh()

    def _choose_current(self):
        if not self.results.isHidden() and self.results.currentItem():
            self._choose(self.results.currentItem())

    def _choose(self, item):
        target = item.data(Qt.UserRole)
        self.dismiss()
        self.chosen.emit(target)

    def eventFilter(self, watched, event):
        if event.type() == QEvent.KeyPress:
            if event.key() == Qt.Key_Escape:
                self.edit.clear()
                self.edit.setFocus()
                return True
            if watched is self.edit and event.key() == Qt.Key_Down and self.results.count():
                self.results.show()
                self.results.setFocus()
                return True
            if watched is self.results and event.key() in (Qt.Key_Return, Qt.Key_Enter):
                self._choose_current()
                return True
        return super().eventFilter(watched, event)
