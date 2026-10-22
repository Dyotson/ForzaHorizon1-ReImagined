"""Press-to-bind controls (#279): pick a device, watch its inputs live, and bind each game
action by pressing the button or moving the axis for it."""
import time

from PySide6.QtCore import QRectF, QSize, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QKeySequence, QPainter, QPen, QShortcut
from PySide6.QtWidgets import (QComboBox, QGridLayout, QHBoxLayout, QLabel, QPushButton,
                               QSizePolicy, QVBoxLayout, QWidget)

from .. import bindings, paths, schema
from ..paths import IS_WINDOWS
from . import theme
from .widgets import label

BIND_TIMEOUT = 10.0  # seconds without an input before a Bind gives up
HATS = {1: "up", 2: "right", 4: "down", 8: "left", 3: "up right", 6: "down right", 12: "down left",
        9: "up left"}


class InputView(QWidget):
    """The selected device's axes as bars and its buttons as lamps that light while pressed."""
    ROW, LAMP, GAP = 20, 26, 6

    def __init__(self):
        super().__init__()
        self.snap = None
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.setAccessibleName("Live view of the selected device's axes and buttons")
        self.setMinimumHeight(40)

    def set_snapshot(self, snap):
        old = self.snap
        self.snap = snap
        if old is None or snap is None or (len(old.axes), len(old.buttons), len(old.hats)) != \
                (len(snap.axes), len(snap.buttons), len(snap.hats)):
            self.setFixedHeight(self.height_for(self.width()))
        self.update()

    def lamps_per_row(self, width):
        return max(1, (width + self.GAP) // (self.LAMP + self.GAP))

    def height_for(self, width):
        if not self.snap:
            return 40
        n = len(self.snap.buttons)
        rows = (n + self.lamps_per_row(width) - 1) // self.lamps_per_row(width) if n else 0
        return (len(self.snap.axes) * self.ROW + (12 if n else 0) + rows * (self.LAMP + self.GAP)
                + (self.ROW + 6 if self.snap.hats else 0) + 4)

    def resizeEvent(self, event):
        self.setFixedHeight(self.height_for(self.width()))
        super().resizeEvent(event)

    def sizeHint(self):
        return QSize(480, self.height_for(480))

    def paintEvent(self, _event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        if not self.snap:
            p.setPen(QColor(theme.MUTED))
            p.drawText(self.rect(), Qt.AlignLeft | Qt.AlignVCenter, "No device selected.")
            return
        y = 0
        w = self.width()
        for n, raw in enumerate(self.snap.axes):
            p.setPen(QColor(theme.MUTED))
            p.drawText(QRectF(0, y, 60, self.ROW), Qt.AlignLeft | Qt.AlignVCenter, f"Axis {n}")
            track = QRectF(64, y + 5, max(10, w - 128), self.ROW - 10)
            p.setPen(Qt.NoPen)
            p.setBrush(QColor(theme.BORDER))
            p.drawRoundedRect(track, 4, 4)
            frac = (bindings.norm(raw) + 1) / 2
            p.setBrush(QColor(theme.ORANGE))
            p.drawRoundedRect(QRectF(track.x(), track.y(), track.width() * frac, track.height()), 4, 4)
            p.setPen(QPen(QColor(theme.TEXT), 1))
            mid = track.x() + track.width() / 2
            p.drawLine(int(mid), int(track.y()) - 2, int(mid), int(track.bottom()) + 2)
            p.setPen(QColor(theme.MUTED))
            p.drawText(QRectF(w - 60, y, 60, self.ROW), Qt.AlignRight | Qt.AlignVCenter, str(raw))
            y += self.ROW
        if self.snap.buttons:
            y += 12
            per = self.lamps_per_row(w)
            for n, down in enumerate(self.snap.buttons):
                r, c = divmod(n, per)
                rect = QRectF(c * (self.LAMP + self.GAP), y + r * (self.LAMP + self.GAP), self.LAMP, self.LAMP)
                p.setPen(QPen(QColor(theme.ORANGE if down else theme.BORDER), 2))
                p.setBrush(QColor(theme.ORANGE) if down else Qt.NoBrush)
                p.drawEllipse(rect)
                p.setPen(QColor(theme.BG if down else theme.MUTED))
                p.drawText(rect, Qt.AlignCenter, str(n))
            y += ((len(self.snap.buttons) + per - 1) // per) * (self.LAMP + self.GAP)
        if self.snap.hats:
            y += 6
            text = "   ".join(f"Hat {n}: {HATS.get(v, 'centred')}" for n, v in enumerate(self.snap.hats))
            p.setPen(QColor(theme.ORANGE if any(self.snap.hats) else theme.MUTED))
            p.drawText(QRectF(0, y, w, self.ROW), Qt.AlignLeft | Qt.AlignVCenter, text)


class BindingRow(QWidget):
    def __init__(self, action, bind, clear, reset):
        super().__init__()
        self.action = action
        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 2, 0, 2)
        lay.setSpacing(10)
        self.title = QLabel(action.label)
        self.title.setMinimumWidth(150)
        self.title.setWordWrap(True)
        lay.addWidget(self.title, 3)
        self.value = QLabel("")
        self.value.setObjectName("muted")
        self.value.setMinimumWidth(120)
        self.value.setWordWrap(True)
        lay.addWidget(self.value, 3)
        self.bind = QPushButton("Bind")
        self.bind.setAccessibleName(f"Bind {action.label}")
        self.bind.clicked.connect(lambda: bind(self))
        self.clear = QPushButton("Clear")
        self.clear.setAccessibleName(f"Clear {action.label}")
        self.clear.clicked.connect(lambda: clear(self))
        self.reset = QPushButton("Reset to default")
        self.reset.setAccessibleName(f"Reset {action.label} to default")
        self.reset.clicked.connect(lambda: reset(self))
        self.bind.setMinimumWidth(90)
        for b in (self.bind, self.clear, self.reset):
            b.setCursor(Qt.PointingHandCursor)
            lay.addWidget(b)

    def show_value(self, text, conflict=False):
        self.value.setText(text)
        self.value.setStyleSheet(f"color: {theme.ORANGE};" if conflict else "")


class BindingsEditor(QWidget):
    """The Bindings card's content. `apply(changes)` stores settings ({key: value})."""
    settings_requested = Signal()
    calibrate_requested = Signal()

    def __init__(self, settings, apply):
        super().__init__()
        self.settings, self.apply = settings, apply
        self.backend = None
        self.devices = []
        self.device = None
        self.snap = None
        self.binding = None  # (row, Detector, deadline)
        self.capture = False  # a calibration holds the device too
        self.rows = []
        self.last_scan = 0.0
        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(12)
        head = QHBoxLayout()
        head.addWidget(label("Bindings", "cardTitle"), 1)
        self.picker = QComboBox()
        self.picker.setAccessibleName("Device to bind")
        self.picker.setMinimumWidth(260)
        self.picker.currentIndexChanged.connect(self.device_chosen)
        head.addWidget(self.picker)
        lay.addLayout(head)
        self.intro = label("", "help")
        lay.addWidget(self.intro)
        self.view = InputView()
        lay.addWidget(self.view)
        self.status = label("", "help")
        self.status.setStyleSheet(f"color: {theme.CREAM};")
        lay.addWidget(self.status)
        self.conflicts = label("", "")
        self.conflicts.setStyleSheet(f"color: {theme.ORANGE};")
        self.conflicts.hide()
        lay.addWidget(self.conflicts)
        self.table = QGridLayout()
        self.table.setContentsMargins(0, 0, 0, 0)
        self.table.setVerticalSpacing(4)
        lay.addLayout(self.table)
        tools = QHBoxLayout()
        self.calibrate = QPushButton("Calibrate wheel")
        self.calibrate.clicked.connect(self.calibrate_requested)
        self.calibrate.hide()
        tools.addWidget(self.calibrate)
        advanced = QPushButton("Text settings (advanced)")
        advanced.setObjectName("link")
        advanced.setCursor(Qt.PointingHandCursor)
        advanced.clicked.connect(self.settings_requested)
        tools.addWidget(advanced)
        tools.addStretch(1)
        lay.addLayout(tools)
        cancel = QShortcut(QKeySequence(Qt.Key_Escape), self)
        cancel.setContext(Qt.WidgetWithChildrenShortcut)
        cancel.activated.connect(lambda: self.cancel("Binding cancelled."))
        self.timer = QTimer(self)
        self.timer.setInterval(33)
        self.timer.timeout.connect(self.poll)
        self.show_device()

    # --- device -------------------------------------------------------------------------------
    def start(self, backend=None):
        if self.timer.isActive():
            return
        if backend is not None:
            self.backend = backend
        if self.backend is None:
            try:
                from ..gamepad import Joysticks
                self.backend = Joysticks()
            except (ImportError, OSError, RuntimeError) as exc:
                paths.log_error("open joysticks for bindings", exc)
                self.intro.setText("Controllers and wheels cannot be read here (SDL is unavailable).")
                return
        self.scan(force=True)
        self.timer.start()

    def stop(self):
        self.cancel("")
        self.timer.stop()
        if self.backend is not None:
            self.backend.close()
            self.backend = None
        self.device = None
        self.snap = None

    def captures(self, instance):
        """Whether launcher navigation must ignore this device (a Bind or calibration runs)."""
        return (self.binding is not None or self.capture) and self.device is not None \
            and self.device.instance == instance

    def kind_of(self, device):
        mode = self.settings().get("wheel_mode", "auto")
        if mode != "off" and (bindings.looks_like_wheel(device.name, device.vid, device.sdl_wheel)
                              or (mode == "on" and not device.gamepad)):
            return "wheel"
        return "controller" if device.gamepad else "unused"

    def scan(self, force=False):
        now = time.monotonic()
        if not force and now - self.last_scan < 1.0:
            return
        self.last_scan = now
        devices = self.backend.devices()
        if [d.instance for d in devices] == [d.instance for d in self.devices] and not force:
            return
        current = self.device.instance if self.device else None
        self.devices = devices
        self.picker.blockSignals(True)
        self.picker.clear()
        kinds = {"wheel": "wheel", "controller": "controller", "unused": "not used by the game"}
        for d in devices:
            self.picker.addItem(f"{d.name} ({kinds[self.kind_of(d)]})", d.instance)
        if not devices:
            self.picker.addItem("No controller or wheel connected", None)
        index = self.picker.findData(current) if current is not None else -1
        if index < 0 and devices:  # a wheel first: it is what most people bind
            index = next((i for i, d in enumerate(devices) if self.kind_of(d) == "wheel"), 0)
        self.picker.setCurrentIndex(max(0, index))
        self.picker.blockSignals(False)
        self.device_chosen()

    def device_chosen(self, *_):
        instance = self.picker.currentData()
        device = next((d for d in self.devices if d.instance == instance), None)
        if device is not None and self.device is not None and device.instance == self.device.instance:
            return
        self.cancel("")
        self.device = device
        self.snap = None
        if device is not None and self.backend is not None and not self.backend.open(device.instance):
            self.device = None
        self.show_device()

    def wheel(self):
        d = self.device
        axes, buttons, hats = self.backend.counts if self.backend is not None and d else (0, 0, 0)
        return bindings.WheelInfo(d.vid if d else 0, d.pid if d else 0, axes, buttons, hats, IS_WINDOWS)

    def known_wheel(self):
        w = self.wheel()
        return bool(bindings.known_wheel(w.vid, w.pid)) and w.num_axes > 3

    # --- rows ---------------------------------------------------------------------------------
    def actions(self):
        if self.device is not None and self.kind_of(self.device) == "wheel":
            return bindings.WHEEL_ACTIONS
        return []

    def show_device(self):
        while self.table.count():
            item = self.table.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        self.rows = []
        kind = self.kind_of(self.device) if self.device is not None else None
        self.calibrate.setVisible(kind == "wheel")
        if kind == "wheel":
            name = bindings.known_wheel(self.device.vid, self.device.pid)
            self.intro.setText(
                "Bind a row, then press the button or move the axis you want for it. "
                + (f"A {name} has its own defaults. " if name else
                   "Rows without a binding use what the game detects when it starts (keep the "
                   "pedals released then). ")
                + "Changes apply the next time the game starts.")
        elif kind == "controller":
            self.intro.setText("Controllers use the game's standard layout.")
        elif kind == "unused":
            self.intro.setText("The game does not use this device: it is neither a controller SDL "
                               "knows nor a wheel. To use it as a wheel, set Racing wheel to On in "
                               "Control settings.")
        else:
            self.intro.setText("Connect a controller or wheel to bind its inputs.")
        section = None
        r = 0
        for action in self.actions():
            if action.section != section:
                section = action.section
                head = label(self.section_title(section), "muted")
                head.setStyleSheet("font-weight: 600; padding-top: 8px;")
                self.table.addWidget(head, r, 0)
                r += 1
            row = BindingRow(action, self.begin, self.clear, self.reset)
            self.rows.append(row)
            self.table.addWidget(row, r, 0)
            r += 1
        self.refresh()

    def section_title(self, section):
        if section == "Shifter":
            mode = self.settings().get("wheel_shifter", "off")
            used = {"h": "gears used (H-pattern)", "sequential": "up and down used (Sequential)"}
            return f"Shifter · {used.get(mode, 'off: choose H-pattern or Sequential in Control settings')}"
        return section

    def binding_text(self, action):
        return bindings.wheel_binding_text(action, self.settings(), self.wheel())

    def conflict_list(self):
        return bindings.wheel_conflicts(self.settings(), self.wheel())

    def refresh(self):
        conflicts = self.conflict_list()
        involved = {label_ for _, labels in conflicts for label_ in labels}
        for row in self.rows:
            row.show_value(self.binding_text(row.action), row.action.label in involved)
            busy = self.binding is not None
            row.bind.setText("Cancel" if busy and self.binding[0] is row else "Bind")
            row.bind.setEnabled(not busy or self.binding[0] is row)
            row.clear.setEnabled(not busy)
            row.reset.setEnabled(not busy)
        if conflicts:
            self.conflicts.setText("Bound twice: " + "; ".join(
                f"{what}: {', '.join(labels)}" for what, labels in conflicts))
        self.conflicts.setVisible(bool(conflicts))

    # --- binding ------------------------------------------------------------------------------
    def prompt(self, action):
        if action.key == "steer":
            return "Turn the wheel to the right"
        if action.kind == "axis":
            return f"Press the {action.label.lower()} pedal"
        return f"Press the button for {action.label}"

    def begin(self, row):
        if self.binding is not None:
            if self.binding[0] is row:
                self.cancel("Binding cancelled.")
            return
        if self.snap is None:
            self.status.setText("Select a connected device first.")
            return
        kind = "steer" if row.action.key == "steer" else row.action.kind
        self.binding = (row, bindings.Detector(self.snap, kind), time.monotonic() + BIND_TIMEOUT)
        self.status.setText(f"{self.prompt(row.action)} now (press a button or move an axis). "
                            "Esc cancels.")
        self.refresh()

    def cancel(self, message):
        if self.binding is None:
            return
        self.binding = None
        self.status.setText(message)
        self.refresh()

    def finish(self, row, found):
        self.binding = None
        self.apply(self.changes_for(row.action, found))
        self.status.setText(f"{row.action.label}: {found.describe()}.")
        self.refresh()

    def changes_for(self, action, found):
        settings = self.settings()
        changes = bindings.set_wheel_binding(settings, action, found.number)
        if found.kind == "axis":
            if action.key == "steer":
                changes["wheel_invert_steering"] = found.direction < 0
            else:
                full = 32767 if found.direction > 0 else -32768
                invert, _ = bindings.pedal_calibration(found.rest, full, self.known_wheel())
                changes[f"wheel_invert_{action.key}"] = invert
        return changes

    def clear(self, row):
        self.apply(bindings.set_wheel_binding(self.settings(), row.action, None))
        self.status.setText(f"{row.action.label}: not bound.")
        self.refresh()

    def reset(self, row):
        self.apply(bindings.set_wheel_binding(self.settings(), row.action, "default"))
        self.status.setText(f"{row.action.label}: back to its default.")
        self.refresh()

    def poll(self):
        if self.backend is None:
            return
        self.scan()
        if self.device is None:
            self.view.set_snapshot(None)
            return
        snap = self.backend.read()
        if snap is None:  # unplugged
            self.cancel("The device was disconnected.")
            self.device = None
            self.scan(force=True)
            return
        self.snap = snap
        self.view.set_snapshot(snap)
        if self.binding is not None:
            row, detector, deadline = self.binding
            found = detector.feed(snap)
            if found is not None:
                self.finish(row, found)
            elif time.monotonic() > deadline:
                self.cancel("Nothing was pressed: binding cancelled.")
