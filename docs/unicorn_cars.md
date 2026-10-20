# Add unicorn cars to a save

Open **Saves → Edit profile → Unicorn cars → Add missing unicorns**.
The game must be closed. Apply any pending credits/XP/wristband edits or reload
before adding cars. The editor checks the selected garage, keeps a full
**Before adding unicorn cars** backup, and adds only models it does not already own.
A second run with all seven owned makes no save changes and creates no extra backup.

| Car ID | Car |
| --- | --- |
| 320 | 1991 Honda CR-X SiR |
| 326 | 1969 Ferrari Dino 246 GT |
| 330 | 1971 Lotus Elan Sprint |
| 1082 | 2009 Ford Fiesta Zetec S |
| 1197 | 2010 Saleen S5S Raptor |
| 1233 | 2010 Mazda MX-5 Superlight |
| 1488 | 2012 MINI John Cooper Works Coupé |

The action edits `ForzaProfile/PlayerDatabase` directly. It needs no server,
Message Center download, or game launch. Existing car rows, purchased parts,
tuning, other progress tables, the selected car, and companion files are preserved.
New stock parts and ratings come from the player's own `media/db/gamedb.slt`;
no car records or game key bytes are bundled. The seven model IDs are explicit:
`Data_Car.IsUnicorn` also includes nine barn finds, which this action does not add.

Duplicate detection uses `Career_Garage.CarId`, including cars acquired by any
other route. Existing duplicates are left alone. Each new garage row receives a
fresh unique ID above both the existing rows and the `LastUsedGarageId` counter,
which is advanced with the insertions. Stock tuning uses the native -1 sentinel;
stock part IDs are resolved through the upgrade tables, rather than borrowed
from an existing car. Existing liveries and thumbnails are not copied to new cars.

## PlayerDatabase format

The outer file is `cmss`, a big-endian used-size word, a 32-byte HMAC-SHA256,
and a secure payload. The payload contains a 20-byte seed followed by AES-CBC
ciphertext. XORing the five seed words gives the plaintext length; the last
16 seed bytes are the CBC IV. The MAC covers the seed plus the padded ciphertext.
Some older files overstate the outer used size, so the authenticated secure-stream
length is derived from the seed and bounded by both outer length and file capacity.

The plaintext begins with a big-endian uncompressed database size. Each subsequent
block has big-endian compressed and uncompressed sizes, followed by raw DEFLATE;
blocks contain at most 32 KiB of uncompressed data. Their concatenation is a SQLite
database. The codec bounds decompression, requires exact block lengths and stream
termination, and checks SQLite integrity before editing. Unknown garage schemas
are refused. The known schema includes `MirroredPerformanceIndex`.

The garage uses different key derivation from the profile prefix. Both take their
obfuscated table bytes from the player's own executable and use the configured save
owner XUID. Within the editor's 64-byte table window, the garage MAC uses bytes
16–47 with XUID index `(i-1) mod 8`; AES uses bytes 48–63 with index `(i-5) mod 8`.
Each byte is XORed with that owner byte and increased by `2*i` modulo 256. Known
executable table addresses are handled by the existing profile-editor loader;
only an authenticated match is accepted. Key material stays in memory.

The updated database is serialized, block-compressed, encrypted with a fresh IV,
re-signed, decoded again, and compared with the intended database before any
replacement. The file's allocated capacity is preserved. After a full backup,
the original garage bytes are checked again and the replacement is written through
a flushed temporary file and atomic rename. Cancellation, failed backup, a concurrent
save change, bad integrity, or failed replacement leaves the current file in place.
The full backup is available through the existing Saves restore action.

## Validation

`python -m unittest test_garage test_save_editor test_saves test_saves_ui` exercises
synthetic authenticated saves, multiple compression blocks, overstated stream size,
existing and duplicate model IDs, unique row IDs, backup/restore, unchanged companion
files, failed MACs, unknown schemas, cancellation, concurrent changes and failed
atomic replacement. Test fixtures contain no game-generated saves or game keys.

On 2026-10-20, offline validation on a disposable three-car free-roam save added
all seven, preserved every existing save file except PlayerDatabase, and confirmed
that repeating the edit was a byte-identical no-op. Restoring the safety backup
recovered the complete original save byte for byte. New stock parts and cached
stats were compared with the earlier seven native gift-created cars. The edited save then loaded natively with the existing instrumented TU4 runtime
(`rexglue 0.10.0.24-dev.gbd833a2`). All seven appeared with native car cards. After
a normal single-player save/quit and a full host-process restart, the MINI returned
as the active car and every other unicorn loaded successfully through Get In Car.
A final normal save retained exactly one of each unicorn and the original three
cars. Running the editor afterward was still a byte-identical no-op with no new
backup. The original fixture hashes remained unchanged, and both test processes
were closed normally. No server or gifting request was used.

This validates stock-car loading and save/reload on that TU4 build. Extended racing,
upgrading/tuning, resale/regifting and other game versions were not qualified.
