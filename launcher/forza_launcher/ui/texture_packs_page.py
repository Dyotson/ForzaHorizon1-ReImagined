"""Texture packs (#384): the packs in the textures folder (add, remove, turn on or off, order)."""
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QHBoxLayout, QVBoxLayout

from .. import GITHUB_URL, schema, texture_packs
from .mods_page import FolderListPage, _link_label, _size
from .widgets import Switch, card, label
from . import theme

DOCS_URL = f"{GITHUB_URL}/blob/main/docs/texture_packs.md"
GUIDE_LINK = f'<a href="{DOCS_URL}" style="color: {theme.ORANGE};">the texture pack guide</a>'


def _gb(n: int) -> str:
    return f"{n / 2**30:.1f} GB" if n >= 2**30 else f"{n / 2**20:.0f} MB"


class TexturePacksPage(FolderListPage):
    COLUMNS = ["Texture pack", "Author", "Textures", "Graphics memory"]
    NOUN = "texture pack"
    PLURAL = "texture packs"
    VERB = "be used"
    CONFLICT_UNIT = "texture(s)"
    ZIP_FILTER = "Texture packs (*.zip)"

    def __init__(self, cfg, window):
        super().__init__(cfg, window, "Texture packs",
                         "Higher-resolution textures, drawn by the graphics card in place of the "
                         "game's own. The game's files stay as they are.")
        lay = self.page_layout

        c, cl = card(spacing=14)
        self.switches = {}
        for key, title, text in (
                ("texture_replace", "Use texture packs",
                 "Draw the ticked packs' textures (Vulkan). Uses graphics memory, not the game's."),
                ("texture_dump", "Dump textures",
                 "Save each texture the game loads to textures/dump, named by its fingerprint: the "
                 "names pack files must have. Never share a dump.")):
            row = QHBoxLayout()
            row.setSpacing(20)
            col = QVBoxLayout()
            col.setSpacing(5)
            col.addWidget(label(title, "cardTitle"))
            col.addWidget(label(text, "muted"))
            row.addLayout(col, 1)
            sw = Switch()
            sw.setAccessibleName(title)
            sw.setFocusPolicy(Qt.StrongFocus)
            sw.toggled.connect(lambda on, key=key: self._set_option(key, on))
            row.addWidget(sw)
            self.switches[key] = sw
            cl.addLayout(row)
        self.vulkan_note = label("Texture packs need the Vulkan renderer (Settings > Display > Graphics "
                                 "API); Direct3D 12 only dumps.", "warn")
        cl.addWidget(self.vulkan_note)
        cl.addWidget(label("Both apply the next time the game starts.", "muted"))
        lay.addWidget(c)

        c, cl = card(spacing=14)
        cl.addWidget(label("Your texture packs", "cardTitle"))
        cl.addWidget(label("Ticked packs are used from top to bottom: when two packs replace the same "
                           "texture, the one lower in the list wins. You can also drop a pack's .zip "
                           "or folder on this page.", "muted"))
        self.build_list(cl, "Add pack…", "Open textures folder")
        self.loose = label("", "warn")
        cl.addWidget(self.loose)
        lay.addWidget(c)

        c, cl = card(spacing=8)
        cl.addWidget(label("How texture packs work", "cardTitle"))
        cl.addWidget(label("A pack is a folder (or a .zip of one) of .dds or .png files, each named by "
                           "the fingerprint of the game texture it replaces, as in your dump, and "
                           "usually a pack.toml with its name. Packs hold changed game art: share only "
                           "your own work, never the game's textures or a dump.", "muted"))
        cl.addWidget(_link_label(f"Making, sharing and installing packs: {GUIDE_LINK}."))
        lay.addWidget(c)
        lay.addStretch()
        self.refresh()

    # ---- the list (FolderListPage hooks) --------------------------------------------------------
    def folder(self):
        return texture_packs.textures_dir(self.cfg)

    def scan(self):
        return texture_packs.listed(self.cfg)

    def conflicts(self, items):
        return texture_packs.conflicts(items)

    def remember(self, items):
        texture_packs.remember(self.cfg, items)

    def install(self, path, replace):
        return texture_packs.install(path, self.folder(), replace=replace)

    def remove_folder(self, folder):
        texture_packs.remove(self.cfg, folder)

    def row_texts(self, p):
        return [p.author, f"{p.files} ({_size(p.size)})", _gb(p.gpu_bytes)]

    def empty_text(self):
        return (f"No texture packs yet. Use <b>Add pack…</b> or put a pack folder in {self.folder()}. "
                f"How packs work: {GUIDE_LINK}.")

    def detail_lines(self, p):
        lines = [p.description] if p.description else []
        lines.append(f"{p.files} texture(s), {_size(p.size)} on disk, about {_gb(p.gpu_bytes)} of "
                     "graphics memory when every one is in use.")
        return lines

    def added_text(self, p):
        return f"Added {p.summary()} ({p.files} texture(s)).{' ' + p.note if p.note else ''}"

    def remove_question(self, p):
        return f'Remove "{p.title}"? Its folder {p.folder} is deleted from the textures folder.'

    def refresh(self):
        super().refresh()
        for key, sw in self.switches.items():
            sw.blockSignals(True)
            sw.setChecked(bool(self.cfg.settings.get(key)))
            sw.blockSignals(False)
        vulkan = self.cfg.settings.get("gpu_backend", "vulkan") == "vulkan"
        self.vulkan_note.setVisible(not vulkan)
        self.switches["texture_replace"].setEnabled(vulkan)
        loose = texture_packs.loose_files(self.folder())
        self.loose.setText(f"{loose} texture file(s) are directly in the textures folder, where the game "
                           "does not use them: a pack is a folder. Put them in a folder of their own "
                           "(Open textures folder) and press Refresh.")
        self.loose.setVisible(bool(loose))
        if self.items and not self.cfg.settings.get("texture_replace"):
            self._say(self.message.text() + " Use texture packs is off: turn it on above to use them.")

    def _set_option(self, key, on):
        """Through the Settings page when there is one (it shows the change, saves and writes
        forza.toml), else straight into the config."""
        settings = getattr(self.window, "settings", None)
        if settings is not None and hasattr(settings, "apply_external"):
            settings.apply_external({key: on})
        else:
            self.cfg.settings[key] = schema.coerce(schema.BY_KEY[key], on)
            try:
                self.cfg.save()
            except OSError as e:
                self._say(f"Could not save the setting: {e}", error=True)
        self.refresh()

