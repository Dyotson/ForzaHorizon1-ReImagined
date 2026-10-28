# Custom radio

The launcher's Radio page edits the three original station slots. Choose a name,
add local music files or a folder, select **Add** or **Replace**, and optionally
choose a PNG logo. **Build stations** creates and enables the Custom radio mod.
Close the game before building and restart it afterwards. Changes to a playlist
do not take effect until its next successful build.

FFmpeg with libmp3lame is required. Choose its executable with **Locate FFmpeg**
or make it available on PATH. The launcher does not download an encoder or music.
Songs are converted locally to 48 kHz stereo MP3 at 160 kb/s. Each song is limited
to 30 minutes; each station accepts at most 200 songs, subject to the game's 2 GB
bank limit. Logos fit within 210 × 100 pixels without cropping.

Settings stay in launcher.json and generated assets stay in
`<data>/mods/reimagined-radio/`, outside the versioned installation. Updates do
not require adding songs again. The original game bank and selected music files
are read-only. A failed or cancelled build leaves the previously installed mod
in place. The original playlists, bank indexes and compressed audio are retained;
Replace changes the selected station's playlist, rather than deleting stock
samples used elsewhere in the game. DJ dialogue remains the original dialogue.

**Restore this station on next build** removes its customization. When no custom
stations remain, Build disables the generated mod. It can also be disabled on the
Mods page. Other mods that replace the same files may conflict; the normal mod
load order applies. Generated banks and string tables contain game assets and
must stay local.

## Implementation provenance and validation

This feature was independently authored for this repository. No Pinyon Shift
implementation was copied or adapted. Its user-facing feature description was
consulted before implementation; it was not used as source code or a patch.

Format inputs:

- Private inspection of the player's original `Radio_Music.fsb`,
  `RadioSystem.xml`, `RadioSoundbankInfo_Music.xml` and LSB2 string tables.
  The bank contains 127 full FSB4 sample headers and three named SYNC points per
  inspected header. Original sample indexes are preserved by appending samples.
- Public FSB field/flag descriptions in the
  [vgmstream format reader](https://github.com/vgmstream/vgmstream/blob/master/src/meta/fsb.c).
  Format facts informed the new writer; its implementation was not copied.
- This repository's existing `extract_map_points.py` archive reader and
  `xdc_decode.py` Xbox tile helper. FFmpeg runs as an external encoder.

Focused tests cover preservation of original bank bytes, empty-build round trips,
malformed bank rejection, cancellation, MP3 framing, Unicode string rebuilding,
configuration restart, restore behavior and logo tile round trips.

Runtime qualification is pending: actual music playback, station/HUD names and
logos, station switching, DJ timing, base/TU4 behavior and restore/restart. Source
and file-format tests do not establish those behaviors. Keep this change draft
until those checks pass.
