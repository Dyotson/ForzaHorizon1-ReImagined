"""Unicorn model list and installed car-file preflight for garage editing."""

# Explicit list: Data_Car.IsUnicorn also includes the nine barn finds.
CARS = (
    (320, "Honda CR-X SiR", "HON_CRXsi_91"),
    (326, "Ferrari Dino 246 GT", "FER_Dino246_69"),
    (330, "Lotus Elan Sprint", "LOT_ElanSprint_72"),
    (1082, "Ford Fiesta Zetec S", "FOR_Fiesta_09"),
    (1197, "Saleen S5S Raptor", "SAL_S5SRaptor_10"),
    (1233, "Mazda MX-5 Superlight", "MAZ_MX5SL_10"),
    (1488, "MINI John Cooper Works Coupé", "MIN_CoupeJCW_12"),
)


def check_files(root):
    missing = [name for _, name, media in CARS
               if not (root / "media" / "cars" / (media + ".zip")).is_file()]
    if missing:
        raise RuntimeError("Missing unicorn car files: " + ", ".join(missing))
    if not (root / "media" / "db" / "gamedb.slt").is_file():
        raise RuntimeError("The game database is missing. Repair the game files in Setup.")
