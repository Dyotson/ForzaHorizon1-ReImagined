"""Controller badges and connection status for the Controls page."""
from PySide6.QtCore import QRectF, QSize, Qt
from PySide6.QtGui import QColor, QFont, QIcon, QPainter, QPen, QPixmap
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QVBoxLayout, QWidget

from . import theme
from .widgets import Disclosure


def button_icon(button):
    """Paint at 2x so the badges stay crisp on high-DPI displays."""
    width = 32 if len(button) > 1 else 24
    pixmap = QPixmap(width * 2, 48)
    pixmap.setDevicePixelRatio(2)
    pixmap.fill(Qt.transparent)
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.Antialiasing)
    color = {"A": "#75d58b", "B": "#ff7b82"}.get(button, theme.CREAM)
    painter.setPen(QPen(QColor(color), 1.5))
    painter.setBrush(QColor(theme.PANEL))
    rect = QRectF(1, 1, width - 2, 22)
    if button in ("A", "B"):
        painter.drawEllipse(rect)
    else:
        painter.drawRoundedRect(rect, 5, 5)
    font = QFont()
    font.setPixelSize(12)
    font.setBold(True)
    painter.setFont(font)
    painter.drawText(rect, Qt.AlignCenter, button)
    painter.end()
    return QIcon(pixmap)


class ControllerGuide(QFrame):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("card")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(24, 20, 24, 20)
        layout.setSpacing(8)
        title = QLabel("Launcher controller")
        title.setObjectName("cardTitle")
        header = QHBoxLayout()
        header.addWidget(title, 1)
        self.status = QLabel()
        self.status.setObjectName("help")
        self.status.setWordWrap(True)
        header.addWidget(self.status)
        layout.addLayout(header)
        self.controls = Disclosure("Launcher button hints")
        rows = self.controls.content
        rows.setSpacing(5)
        for buttons, text in ((("↑↓",), "Move focus"), (("←→",), "Change value"),
                              (("A",), "Select"), (("B",), "Back"),
                              (("LB", "RB"), "Pages")):
            row = QHBoxLayout()
            row.setSpacing(6)
            for button in buttons:
                badge = QLabel()
                badge.setAccessibleName(button)
                badge.setPixmap(button_icon(button).pixmap(QSize(32 if len(button) > 1 else 24, 24)))
                row.addWidget(badge)
            label = QLabel(text)
            label.setObjectName("help")
            row.addWidget(label, 1)
            rows.addLayout(row)
        layout.addWidget(self.controls)
        self.set_state()

    def set_state(self, connected=False, paused=False, error=""):
        self.controls.setVisible(connected)
        state = "No controller connected" if not connected else "Controls paused" if paused else "Connected"
        self.status.setText(error or state)
