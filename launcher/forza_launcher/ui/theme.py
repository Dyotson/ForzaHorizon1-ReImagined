"""Colors and the Qt style sheet: the logo's sunset on a dark night."""
BG = "#140d2e"
PANEL = "#1c1340"
PANEL2 = "#241747"
BORDER = "#3a2770"
TEXT = "#f3ecff"
MUTED = "#a99bd1"
CREAM = "#ffe9c0"
ORANGE = "#ff9d3c"
PINK = "#ff5470"
OK = "#5fd38d"
ERR = "#ff7b7b"

from pathlib import Path

ARROW = (Path(__file__).resolve().parent / "down.svg").as_posix()

QSS = f"""
* {{ font-size: 14px; }}
QWidget {{ background: transparent; color: {TEXT}; }}
QMainWindow, QDialog, #root {{ background: {BG}; }}
#sidebar {{ background: #0f0925; border-right: 1px solid {BORDER}; }}
#brand {{ color: {CREAM}; font-size: 15px; font-weight: 700; }}
#brandSub {{ color: {PINK}; font-size: 12px; font-weight: 700; }}
QPushButton#nav {{ text-align: left; padding: 11px 18px; border: none; border-left: 3px solid transparent;
  color: {MUTED}; font-size: 15px; font-weight: 600; background: transparent; border-radius: 0; }}
QPushButton#nav:hover {{ color: {TEXT}; background: #1a1140; }}
QPushButton#nav:checked {{ color: {CREAM}; border-left: 3px solid {ORANGE}; background: #1c1340; }}
#version {{ color: {MUTED}; font-size: 12px; }}
#pageTitle {{ font-size: 26px; font-weight: 800; color: {CREAM}; }}
#pageLead {{ color: {MUTED}; font-size: 14px; }}
#card {{ background: {PANEL}; border: 1px solid {BORDER}; border-radius: 14px; }}
#cardTitle {{ font-size: 16px; font-weight: 700; color: {CREAM}; }}
#muted {{ color: {MUTED}; font-size: 13px; }}
#help {{ color: {MUTED}; font-size: 12px; }}
#section {{ color: {ORANGE}; font-size: 12px; font-weight: 800; letter-spacing: 2px; }}
#ok {{ color: {OK}; }} #bad {{ color: {ERR}; }}
#warn {{ background: #3a1530; border: 1px solid {PINK}; border-radius: 12px; padding: 12px 14px; color: #ffd6dc; }}
QPushButton {{ background: {PANEL2}; border: 1px solid {BORDER}; border-radius: 10px; padding: 9px 18px;
  color: {TEXT}; font-weight: 600; }}
QPushButton:hover {{ border-color: {ORANGE}; background: #2d1d57; }}
QPushButton:focus {{ border-color: {ORANGE}; }}
QPushButton#nav:focus {{ color: {CREAM}; background: #2d1d57; }}
QPushButton#play:focus {{ border: 2px solid {CREAM}; }}
QPushButton#link:focus {{ color: {CREAM}; text-decoration: underline; }}
QToolButton#disclosure {{ background: transparent; border: 1px solid transparent; border-radius: 6px;
  padding: 6px 8px; color: {MUTED}; font-weight: 600; }}
QToolButton#disclosure:hover {{ color: {CREAM}; background: {PANEL2}; }}
QToolButton#disclosure:focus {{ border-color: {ORANGE}; color: {CREAM}; }}
QPushButton:disabled {{ color: #6f6494; border-color: #2b2055; background: #19113a; }}
QPushButton#play {{ background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 {ORANGE}, stop:1 {PINK});
  color: #1a0f33; border: none; border-radius: 14px; padding: 16px 64px; font-size: 20px; font-weight: 800; }}
QPushButton#play:hover {{ background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #ffb060, stop:1 #ff7088); }}
QPushButton#play:disabled {{ background: #3a2a5e; color: #8c7fb3; }}
QPushButton#link {{ background: transparent; border: none; color: {ORANGE}; padding: 4px 0; text-align: left; }}
QPushButton#link:hover {{ color: {CREAM}; text-decoration: underline; }}
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit, QTextEdit {{
  background: #130c2c; border: 1px solid {BORDER}; border-radius: 8px; padding: 9px 14px;
  selection-background-color: {PINK}; selection-color: white; color: {TEXT}; }}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus {{ border-color: {ORANGE}; }}
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled {{ color: #6f6494; background: #17102f; }}
QSpinBox, QDoubleSpinBox {{ padding: 9px 0; }}
QAbstractSpinBox QLineEdit, QComboBox QLineEdit {{
  background: transparent; border: none; border-radius: 0; padding: 0; }}
QComboBox::drop-down {{ border: none; width: 26px; }}
QComboBox::down-arrow {{ image: url("{ARROW}"); width: 12px; height: 8px; margin-right: 10px; }}
QComboBox QAbstractItemView {{ background: {PANEL2}; border: 1px solid {BORDER}; selection-background-color: {PINK};
  outline: none; padding: 4px; }}
QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {{
  width: 0; border: none; }}
QProgressBar {{ background: #130c2c; border: 1px solid {BORDER}; border-radius: 8px; height: 16px; text-align: center;
  color: {TEXT}; font-size: 12px; }}
QProgressBar::chunk {{ border-radius: 7px;
  background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 {ORANGE}, stop:1 {PINK}); }}
QTabBar::tab {{ padding: 9px 14px; margin-right: 6px; color: {MUTED}; font-weight: 600;
  border: 1px solid transparent; border-radius: 9px; }}
QTabBar::tab:selected {{ color: {CREAM}; background: {PANEL2}; border-color: {BORDER}; }}
QTabBar::tab:hover {{ color: {TEXT}; background: {PANEL2}; }}
QTabBar::tab:focus {{ color: {CREAM}; border-color: {ORANGE}; }}
QScrollArea {{ border: none; background: transparent; }}
QScrollBar:vertical {{ background: transparent; width: 10px; margin: 2px; }}
QScrollBar::handle:vertical {{ background: {BORDER}; border-radius: 4px; min-height: 30px; }}
QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; }}
QRadioButton, QCheckBox {{ spacing: 10px; }}
QRadioButton::indicator {{ width: 16px; height: 16px; border-radius: 9px; border: 2px solid {BORDER}; background: #130c2c; }}
QRadioButton::indicator:checked {{ border-color: {ORANGE}; background: qradialgradient(cx:0.5,cy:0.5,radius:0.5,
  fx:0.5,fy:0.5, stop:0 {ORANGE}, stop:0.55 {ORANGE}, stop:0.6 #130c2c, stop:1 #130c2c); }}
QCheckBox::indicator {{ width: 16px; height: 16px; border-radius: 4px; border: 2px solid {BORDER}; background: #130c2c; }}
QCheckBox::indicator:checked {{ background: {ORANGE}; border-color: {ORANGE}; }}
QCheckBox:focus::indicator, QRadioButton:focus::indicator {{ border-color: {ORANGE}; }}
QToolTip {{ background: {PANEL2}; color: {TEXT}; border: 1px solid {ORANGE}; padding: 6px; }}
"""
