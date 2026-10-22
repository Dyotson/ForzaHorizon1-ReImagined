# Controller artwork

Source: [e7d/gamepad-viewer](https://github.com/e7d/gamepad-viewer)
at commit `563552931253a2e558cf6fd5fc7fd254eefd9b2f`.

The files in `xbox-one/`, `ds4/`, and `dualsense/` are unmodified assets from
the matching `templates/` directories. The launcher composes the base artwork
and unpressed button/stick sprites using the positions in upstream's CSS.
DualSense's transparent outer margins are cropped for display.

MIT license, copyright (c) 2017–2020 Michaël “e7d” Ferrand; see `LICENSE`.
Upstream credits the original GamepadViewer skins by mrmcpowned as the basis
of its reworked artwork. Retain this notice and the license when distributing.
