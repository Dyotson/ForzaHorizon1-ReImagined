#!/usr/bin/env python3
"""Validate and summarize the native renderer's read-only capture inventory.

The input is metadata exported by native_renderer_capture. Nothing is loaded
from the game's assets or the capture itself, and no shader bytes are exported.
"""
import argparse
import collections
import csv
import json
from pathlib import Path


def summarize(folder):
    graph = json.loads((folder / "graph.json").read_text(encoding="utf-8"))
    if graph.get("schema") != 1 or graph.get("execution_ready") is not False:
        raise ValueError("Expected a version 1 diagnostic graph, not an executable renderer")
    operations = graph["operations"]
    images = {
        int(row["id"]): row
        for row in csv.DictReader((folder / "images.tsv").open(encoding="utf-8"), delimiter="\t")
    }
    kinds = collections.Counter()
    hazards = collections.Counter()
    targets = collections.Counter()
    candidates = []
    warnings = []
    previous_event = -1
    edges = 0
    for index, op in enumerate(operations):
        if op["id"] != index or op["event"] <= previous_event:
            raise ValueError("Operations must have sequential IDs and increasing event IDs")
        previous_event = op["event"]
        for dep in op["dependencies"]:
            if dep["before"] < 0 or dep["before"] >= index:
                raise ValueError("Dependency is not a preceding operation")
            hazards[dep["hazard"]] += 1
            edges += 1
        flags = op["flags"]
        kind = ("draw" if flags & 2 else "dispatch" if flags & 4 else
                "copy" if flags & 0x400 else "clear" if flags & 1 else "other")
        kinds[kind] += 1
        if kind == "draw":
            for resource in op.get("color_targets", []):
                targets[resource] += 1
        if op["undefined_reads"]:
            warnings.append({"event": op["event"], "resources": op["undefined_reads"]})
        if kind == "draw" and 0 < op["indices"] <= 6 and not op.get("depth_target"):
            candidates.append({"event": op["event"], "color_targets": op.get("color_targets", [])})
    if edges != graph["dependency_edges"]:
        raise ValueError("Dependency count does not match exported graph")
    return {
        "operations": len(operations), "resources": graph["resources"],
        "dependency_edges": edges, "operation_kinds": dict(kinds),
        "hazards": dict(hazards), "execution_ready": False,
        "potential_undefined_reads": warnings,
        "potential_undefined_reads_note": (
            "Storage descriptors and attachment loads are conservatively read/write. "
            "These are inspection points, not confirmed game or driver bugs."
        ),
        "single_quad_candidates": candidates,
        "candidate_note": "Small draws without depth are candidates for inspection; they may be HUD draws.",
        "most_used_color_targets": [
            {"id": rid, "draws": count, "description": images.get(rid)}
            for rid, count in targets.most_common(16)
        ],
        "next_required_work": [
            "Identify a world pass by shader and full state, not by draw size alone.",
            "Map guest resource/resolve ownership and image subresources.",
            "Implement explicit layouts, synchronization and CPU-readback fallback.",
            "Execute one replacement pass and compare its output before extending the path.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    args = parser.parse_args()
    report = summarize(args.folder)
    (args.folder / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{report['operations']} ordered operations; {report['resources']} resources; "
          f"{report['dependency_edges']} forward dependencies verified")
    print(json.dumps(report["operation_kinds"]))
    print(f"{len(report['single_quad_candidates'])} small-draw inspection candidates; "
          f"{len(report['potential_undefined_reads'])} conservative undefined-read warnings")


if __name__ == "__main__":
    main()
