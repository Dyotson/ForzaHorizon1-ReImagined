#!/usr/bin/env python3
"""Inspect/export a local FH1/FM3/FM4 LivCatalog or LivGroups save without modifying it."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"launcher"))
from forza_launcher import ugc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="saved-content package or extracted catalog directory")
    parser.add_argument("--export", type=Path, help="new directory for payloads, attribution and previews")
    args = parser.parse_args()
    try:
        catalog = ugc.read_catalog(args.source)
        if args.export:
            print(ugc.export_catalog(catalog, args.export))
        else:
            print(json.dumps(ugc.description(catalog), indent=2, ensure_ascii=False))
    except (ugc.CatalogError, OSError) as exc:
        parser.exit(1, f"{exc}\n")


if __name__ == "__main__":
    main()
