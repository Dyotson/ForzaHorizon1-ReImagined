"""A focused profile editor launched from Saves. Tasks belong to the parent page."""
from copy import deepcopy

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QIcon, QPixmap
from PySide6.QtWidgets import (QComboBox, QDialog, QFormLayout, QHBoxLayout, QProgressBar,
                               QPushButton, QSpinBox, QVBoxLayout)

from .. import save_editor, garage
from .widgets import Disclosure, label


class ProfileEditor(QDialog):
    def __init__(self, page):
        super().__init__(page.window)
        self.page = page
        self.cfg, self.profile = deepcopy(page.cfg), page.profile
        self.document = None
        self.closing = False
        self.setWindowTitle('Edit FH1 profile')
        self.setWindowModality(Qt.WindowModal)
        self.resize(600, 520)
        self.setMinimumWidth(520)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(24, 24, 24, 24)
        layout.setSpacing(18)
        layout.addWidget(label('Edit profile', 'pageTitle'))
        layout.addWidget(label(self.profile.label, 'cardTitle'))
        self.summary = label('Loading profile…', 'muted')
        layout.addWidget(self.summary)
        form = QFormLayout()
        form.setHorizontalSpacing(24)
        form.setVerticalSpacing(16)
        self.controls = {}
        for path, title in (('Main/Credits', 'Credits'), ('Main/XP', 'Career XP')):
            control = QSpinBox()
            _kind, low, high = save_editor.EDITABLE[path]
            control.setRange(low, high)
            control.setGroupSeparatorShown(True)
            control.setAccessibleName(title)
            control.setMinimumHeight(42)
            control.lineEdit().setTextMargins(10, 2, 10, 2)
            self.controls[path] = control
            form.addRow(title, control)
        self.wristband = QComboBox()
        self.wristband.setAccessibleName('Career wristband')
        self.wristband.setMinimumHeight(42)
        for value, name in save_editor.WRISTBANDS:
            swatch = QPixmap(16, 16)
            swatch.fill(QColor(name.lower()))
            self.wristband.addItem(QIcon(swatch), name, value)
        self.controls['Main/WristbandLevel'] = self.wristband
        form.addRow('Wristband', self.wristband)
        layout.addLayout(form)
        layout.addWidget(label('A full backup is kept before applying changes. Wristband selects the festival tier; '
                               'XP is edited separately. This does not complete races, change level or award cars.', 'muted'))
        unicorns = Disclosure('Unicorn cars')
        unicorns.content.addWidget(label(
            'Add the seven rare cars to this selected save: Honda CR-X, Ferrari Dino, Lotus Elan, '
            'Ford Fiesta, Saleen S5S, Mazda MX-5 Superlight and MINI JCW Coupé.', 'muted'))
        unicorns.content.addWidget(label(
            'A full backup is kept before editing the garage. Only missing models are added; '
            'existing cars, upgrades and tuning are preserved.', 'muted'))
        self.unicorn_button = QPushButton('Add missing unicorns')
        self.unicorn_button.clicked.connect(self.add_unicorns)
        unicorns.content.addWidget(self.unicorn_button)
        layout.addWidget(unicorns)
        self.message = label('Reading and checking the encrypted profile…', 'muted')
        layout.addWidget(self.message)
        self.progress = QProgressBar()
        self.progress.setRange(0, 0)
        layout.addWidget(self.progress)
        row = QHBoxLayout()
        row.setSpacing(12)
        self.reload_button = QPushButton('Reload')
        self.reload_button.clicked.connect(self.reload)
        row.addWidget(self.reload_button)
        row.addStretch()
        self.close_button = QPushButton('Close')
        self.close_button.clicked.connect(self.reject)
        row.addWidget(self.close_button)
        self.apply_button = QPushButton('Back up and apply')
        self.apply_button.clicked.connect(self.apply)
        row.addWidget(self.apply_button)
        layout.addLayout(row)
        self._set_busy(True)

    def _set_busy(self, busy):
        self.progress.setVisible(busy)
        for control in self.controls.values():
            control.setEnabled(not busy and self.document is not None)
        self.apply_button.setEnabled(not busy and self.document is not None)
        self.unicorn_button.setEnabled(not busy and self.document is not None)
        self.reload_button.setEnabled(not busy)
        self.close_button.setText('Cancel' if busy else 'Close')

    def _loaded(self, document, message='Profile verified. Changes apply to the selected save.'):
        if self.closing:
            self.document = None
            super().reject()
            return
        self.document = document
        # Remove any previous unknown-value placeholder before reloading.
        while self.wristband.count() > len(save_editor.WRISTBANDS):
            self.wristband.removeItem(self.wristband.count()-1)
        for path, control in self.controls.items():
            value = document.values[path].value
            if control is self.wristband:
                index = control.findData(value)
                if index < 0:
                    control.addItem(f'Current value ({value}) — unchanged', value)
                    index = control.count()-1
                control.setCurrentIndex(index)
                continue
            # A signed XP value outside the supported edit range is displayed as
            # an error, never silently clamped and then written back.
            _kind, low, high = save_editor.EDITABLE[path]
            if not low <= value <= high:
                self._failed('This profile contains a value outside the supported editing range.')
                return
            control.setValue(value)
        level = document.values.get('Main/Level')
        self.summary.setText(f'Level: {level.value}' if level else 'Career profile')
        self.message.setText(message)
        self._set_busy(False)

    def _failed(self, message):
        self.document = None
        self.message.setText(message)
        self._set_busy(False)
        if self.closing:
            super().reject()

    def reload(self):
        if self.page.busy():
            return
        self.document = None
        self._set_busy(True)
        self.message.setText('Reading and checking the encrypted profile…')
        self.page._run('Opening profile editor…', lambda t: save_editor.load(self.cfg, self.profile, t.cancelled),
                       self._loaded, failed=self._failed)

    def _changes(self):
        changes = {}
        for path, control in self.controls.items():
            value = control.currentData() if control is self.wristband else control.value()
            if value != self.document.values[path].value:
                changes[path] = value
        return changes

    def add_unicorns(self):
        if self.document is None or self.page.busy():
            return
        if self._changes():
            self.message.setText('Apply your profile changes or reload before adding cars.')
            return
        document = self.document
        self._set_busy(True)
        self.message.setText('Checking the garage and backing up before adding missing cars…')

        def complete(result):
            message = (f'Added {len(result.added)} unicorn car(s). {len(result.already_owned)} already owned. '
                       'A “Before adding unicorn cars” backup was kept.' if result.added else
                       'All seven unicorn cars are already in this garage. Nothing was changed.')
            self._loaded(document, message)

        self.page._run('Adding unicorn cars…', lambda t: garage.add_missing(self.cfg, self.profile,
                       t.cancelled, self.page._copy_progress(t)), complete, failed=self._failed)

    def apply(self):
        if self.document is None or self.page.busy():
            return
        changes = self._changes()
        if not changes:
            self.message.setText('No values have changed.')
            return
        document = self.document
        self._set_busy(True)
        self.message.setText('Backing up the current save and applying changes…')

        def complete(result):
            self._loaded(result.document, 'Changes saved. A “Before profile edit” backup was kept.')

        self.page._run('Editing profile…', lambda t: save_editor.apply(document, changes, t.cancelled,
                       self.page._copy_progress(t)), complete, failed=self._failed)

    def reject(self):
        if self.page.busy():
            self.closing = True
            self.page.cancel()
            self.close_button.setEnabled(False)
        else:
            self.document = None
            super().reject()

    def closeEvent(self, event):
        if self.page.busy():
            self.reject()
            event.ignore()
        else:
            self.document = None
            super().closeEvent(event)
