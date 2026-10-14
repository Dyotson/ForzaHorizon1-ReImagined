"""The main window: a sidebar and the Play, Setup, Settings and About pages."""
import sys

from PySide6.QtCore import QTimer, Qt
from PySide6.QtGui import QIcon, QPixmap
from PySide6.QtWidgets import (QApplication, QButtonGroup, QHBoxLayout, QLabel, QMainWindow,
                               QMessageBox, QPushButton, QStackedWidget, QVBoxLayout, QWidget)

from .. import APP_NAME, __version__, builder, game, paths
from ..config import Config, import_toml, render_toml
from . import theme
from .pages import AboutPage, PlayPage, SetupPage
from .settings_page import SettingsPage


class MainWindow(QMainWindow):
    def __init__(self, cfg: Config):
        super().__init__()
        self.cfg = cfg
        self.session = None
        self._busy = False
        self.setWindowTitle(APP_NAME)
        icon = paths.asset("logo.png")
        if icon.is_file():
            self.setWindowIcon(QIcon(str(icon)))
        self.resize(1040, 760)
        self.setMinimumSize(900, 620)

        root = QWidget()
        root.setObjectName("root")
        self.setCentralWidget(root)
        h = QHBoxLayout(root)
        h.setContentsMargins(0, 0, 0, 0)
        h.setSpacing(0)

        side = QWidget()
        side.setObjectName("sidebar")
        side.setFixedWidth(210)
        sl = QVBoxLayout(side)
        sl.setContentsMargins(0, 22, 0, 16)
        sl.setSpacing(2)
        brand = QHBoxLayout()
        brand.setContentsMargins(18, 0, 12, 14)
        logo = QLabel()
        pm = QPixmap(str(paths.asset("logo.png")))
        if not pm.isNull():
            logo.setPixmap(pm.scaled(44, 44, Qt.KeepAspectRatio, Qt.SmoothTransformation))
        brand.addWidget(logo)
        col = QVBoxLayout()
        col.setSpacing(0)
        a = QLabel("Forza Horizon 1")
        a.setObjectName("brand")
        b = QLabel("ReImagined")
        b.setObjectName("brandSub")
        col.addWidget(a)
        col.addWidget(b)
        brand.addLayout(col)
        sl.addLayout(brand)

        self.stack = QStackedWidget()
        self.play = PlayPage(cfg, self)
        self.setup = SetupPage(cfg, self)
        self.settings = SettingsPage(cfg, self)
        self.about = AboutPage(cfg, self)
        self.navs = QButtonGroup(self)
        for i, (name, page) in enumerate((("Play", self.play), ("Setup", self.setup),
                                          ("Settings", self.settings), ("About", self.about))):
            btn = QPushButton(name)
            btn.setObjectName("nav")
            btn.setCheckable(True)
            btn.setCursor(Qt.PointingHandCursor)
            self.navs.addButton(btn, i)
            sl.addWidget(btn)
            self.stack.addWidget(page)
        self.navs.idClicked.connect(self.show_page)
        sl.addStretch(1)
        self.version = QLabel(f"Launcher {__version__}")
        self.version.setObjectName("version")
        self.version.setContentsMargins(20, 0, 0, 0)
        sl.addWidget(self.version)
        h.addWidget(side)
        body = QWidget()
        bl = QVBoxLayout(body)
        bl.setContentsMargins(28, 24, 20, 8)
        bl.addWidget(self.stack, 1)
        self.footer = QLabel("")
        self.footer.setObjectName("muted")
        bl.addWidget(self.footer)
        h.addWidget(body, 1)

        self.play.goto_setup.connect(lambda: self.show_page(1))
        self.play.play_requested.connect(self.start_game)
        self.setup.changed.connect(self.refresh_all)
        self.settings.changed.connect(self.play.refresh)
        self.poll = QTimer(self)
        self.poll.setInterval(500)
        self.poll.timeout.connect(self._poll)
        self._first_run()
        self.show_page(0)
        self.refresh_all()

    # ---- navigation -------------------------------------------------------------------
    def show_page(self, i):
        self.stack.setCurrentIndex(i)
        self.navs.button(i).setChecked(True)
        if i == 0:
            self.play.refresh()
        elif i == 1:
            self.setup.refresh()
        elif i == 3:
            self.about.refresh()
        self.footer.setText("")

    def refresh_all(self):
        self.play.refresh()
        self.about.refresh()
        install, _ = game.find_game(self.cfg)
        self.version.setText(f"Launcher {__version__}\nGame: {game.game_version(install)}")
        self.write_toml()

    def status(self, text, error=False):
        self.footer.setText(text)
        self.footer.setStyleSheet(f"color: {theme.ERR if error else theme.MUTED};")

    def set_busy(self, busy):
        self._busy = busy

    def closeEvent(self, e):
        if self.setup.busy():
            if QMessageBox.question(self, "Quit", "A task is still running. Stop it and quit? "
                                    "A build resumes where it stopped.") != QMessageBox.Yes:
                e.ignore()
                return
            self.setup.task.cancel()
            self.setup.task.wait(8000)
        self.settings.flush()
        self.stop_game()
        super().closeEvent(e)

    # ---- first run ---------------------------------------------------------------------
    def _first_run(self):
        if self.cfg.loaded:
            return
        install, _ = game.find_game(self.cfg)
        toml = install.dir / "forza.toml" if install else None
        if toml and toml.is_file():
            try:
                import_toml(self.cfg, toml.read_text(encoding="utf-8"))
            except OSError:
                pass
        try:
            self.cfg.save()
        except OSError:
            pass

    # ---- forza.toml ---------------------------------------------------------------------
    def write_toml(self):
        install, _ = game.find_game(self.cfg)
        if install is None:
            return
        try:
            from ..config import write_toml
            write_toml(self.cfg, install.dir / "forza.toml")
        except OSError as e:
            self.status(f"Could not write forza.toml in {install.dir}: {e}", True)

    def preview_text(self):
        install, looked = game.find_game(self.cfg)
        if install is None:
            return render_toml(self.cfg) + "\n# (the game is not built yet)"
        try:
            plan = game.build_plan(self.cfg, install, write=False)
        except Exception as e:
            return render_toml(self.cfg) + f"\n# {e}"
        lines = [f"# {plan.toml_path}", plan.toml_text.rstrip(), "", "# environment"]
        lines += [f"{k}={v}" for k, v in sorted(plan.env.items()) if k != "LD_LIBRARY_PATH"]
        lines += [f"unset {k}" for k in plan.unset]
        lines += ["", "# command", plan.command_line()]
        if plan.server_argv:
            lines += ["", "# local server", " ".join(plan.server_argv)]
        return "\n".join(lines)

    def reset_settings(self):
        if QMessageBox.question(self, "Reset settings", "Reset every setting on the Settings page to its default?") \
                != QMessageBox.Yes:
            return
        from .. import config, schema
        self.cfg.d["settings"] = {**schema.defaults(), **config.FIRST_RUN}
        self.cfg.d["online"] = {"mode": "public", "custom_server": ""}
        self.cfg.d["profile"] = {"mode": "own", "xenia_content": "", "xenia_xuid": ""}
        for k in ("extra_args", "extra_env", "extras_toml"):
            self.cfg.advanced[k] = ""
        self.settings.rebuild()
        self.settings.flush()

    # ---- playing -----------------------------------------------------------------------
    def running(self):
        return self.session is not None and self.session.poll() is None

    def start_game(self):
        if self.running():
            return
        install, looked = game.find_game(self.cfg)
        if install is None:
            self.play.show_message(game.missing_message(looked), True)
            return
        ok, msg = __import__("forza_launcher.installer", fromlist=["x"]).check_game_folder(self.cfg.game_files())
        if not ok:
            self.play.show_message(msg, True)
            return
        self.settings.flush()
        try:
            plan = game.build_plan(self.cfg, install, write=True, log=lambda t: self.status(t))
            session = game.Session(plan)
            session.start()
        except (RuntimeError, OSError) as e:
            self.play.show_message(f"Could not start the game: {e}", True)
            return
        self.session = session
        self.play.show_message("The game is running. " + " ".join(plan.notes))
        self.poll.start()
        self.play.refresh()

    def stop_game(self):
        if self.session is not None:
            self.session.stop()

    def _poll(self):
        if self.session is None:
            self.poll.stop()
            return
        code = self.session.poll()
        if code is None:
            return
        self.poll.stop()
        self.session.cleanup()
        self.session = None
        if code == 0:
            self.play.show_message("The game closed.")
        else:
            self.play.show_message(f"The game stopped (exit code {code}). The log folder has forza.log.", True)
        self.play.refresh()


def run(argv):
    from PySide6.QtCore import Qt as _Qt
    QApplication.setHighDpiScaleFactorRoundingPolicy(_Qt.HighDpiScaleFactorRoundingPolicy.PassThrough)
    app = QApplication([sys.argv[0]] + argv)
    app.setApplicationName("forza-reimagined-launcher")
    app.setDesktopFileName("forza-reimagined")
    app.setStyleSheet(theme.QSS)
    win = MainWindow(Config())
    win.show()
    return app.exec()
