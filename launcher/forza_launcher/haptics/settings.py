"""Launcher preferences; never written into the game's TOML."""
import math

DEFAULTS = dict(enabled=False, intensity=70, throttle_strength=3, brake_strength=7,
                xbox_enabled=False, xbox_intensity=50, xbox_throttle=35, xbox_brake=70)


def normalize(value):
    value = value if isinstance(value, dict) else {}
    out = dict(DEFAULTS)
    out['enabled'] = value.get('enabled') is True
    out['xbox_enabled'] = value.get('xbox_enabled') is True
    for key, maximum in [('intensity', 100), ('throttle_strength', 8), ('brake_strength', 8),
                         ('xbox_intensity', 100), ('xbox_throttle', 100), ('xbox_brake', 100)]:
        try:
            number = float(value.get(key, out[key]))
            if math.isfinite(number):
                out[key] = max(0, min(maximum, round(number)))
        except (TypeError, ValueError, OverflowError):
            pass
    return out
