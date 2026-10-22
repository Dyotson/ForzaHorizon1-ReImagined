"""The Gamertag setting: plain ASCII letters, digits and spaces, at most 15 characters, like
an Xbox 360 gamertag. Anything else (combining marks, foreign scripts, emoji, control
characters) would be shown to other players by the game, so it is cleaned here and the
server and the game clean it again."""

import unicodedata

MAX_LENGTH = 15
RULE = "Gamertags can only use letters, digits and spaces (at most 15)."


def clean_name(text) -> str:
    """The name with accents transliterated (Jose for Jose with an accent), everything but
    ASCII letters, digits and single spaces dropped, trimmed, at most 15 characters."""
    text = unicodedata.normalize("NFKD", text if isinstance(text, str) else "")
    kept = "".join(c if c.isalnum() else " " for c in text if c.isascii() and not unicodedata.combining(c))
    return " ".join(kept.split())[:MAX_LENGTH].strip()


def filter_typing(text) -> str:
    """What the Gamertag field keeps while the player types or pastes: accents transliterated,
    only ASCII letters, digits and spaces, at most 15. Spaces are not trimmed yet (clean_name
    does that when editing ends), so a space typed between two words stays."""
    text = unicodedata.normalize("NFKD", text if isinstance(text, str) else "")
    kept = "".join(c for c in text if c.isascii() and (c.isalnum() or c == " ") and not unicodedata.combining(c))
    return kept[:MAX_LENGTH]


def check_name(text) -> tuple[str, str]:
    """(the cleaned name, a message for the player, or "" when nothing had to change)."""
    text = text if isinstance(text, str) else ""
    cleaned = clean_name(text)
    if cleaned == text.strip():
        return cleaned, ""
    if not cleaned:
        return "", RULE + " Nothing valid was left, so the Gamertag is empty and your login name is used."
    return cleaned, f'{RULE} Changed to "{cleaned}".'
