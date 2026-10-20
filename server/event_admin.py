#!/usr/bin/env python3
"""Manage organizer-approved Forza Live community events (standard library only)."""
import argparse
import json
import os
from pathlib import Path
import sys
import urllib.error
import urllib.parse
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None  # never forward an organizer token to another endpoint


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="http://127.0.0.1:21201")
    parser.add_argument("--token-file", type=Path, help="otherwise use FORZA_ADMIN_TOKEN; never a URL parameter")
    commands = parser.add_subparsers(dest="action", required=True)
    commands.add_parser("list")
    create = commands.add_parser("create"); create.add_argument("file", type=Path, help="event JSON")
    enroll = commands.add_parser("enroll"); enroll.add_argument("xuid", help="signed-in player's hex XUID")
    for action in ("start", "close", "attendance"):
        commands.add_parser(action).add_argument("event")
    for action in ("award", "revoke"):
        cmd = commands.add_parser(action); cmd.add_argument("event"); cmd.add_argument("xuid")
        if action == "award":
            cmd.add_argument("--owned-models", required=True,
                             help="confirmed already owned reward IDs, comma separated; 'none' if all are missing")
    args = parser.parse_args()
    url = urllib.parse.urlsplit(args.server)
    if url.scheme not in ("http", "https") or not url.netloc or url.query or url.fragment or url.username:
        parser.error("--server must be an HTTP(S) server URL without credentials/query/fragment")
    token = args.token_file.read_text().strip() if args.token_file else os.environ.get("FORZA_ADMIN_TOKEN", "")
    endpoint, payload = "/admin/events", None
    if args.action == "create": payload = json.loads(args.file.read_text(encoding="utf-8-sig"))
    elif args.action == "enroll": endpoint += "/enroll"; payload = {"xuid": args.xuid}
    elif args.action != "list":
        endpoint += "/" + urllib.parse.quote(args.event, safe="") + "/" + args.action
        if args.action not in ("attendance",): payload = {}
        if args.action in ("award", "revoke"): payload["xuid"] = args.xuid
        if args.action == "award":
            payload["owned_models"] = [] if args.owned_models == "none" else [int(m) for m in args.owned_models.split(",")]
    headers = {"Content-Type": "application/json"}
    if token: headers["X-Admin-Token"] = token
    request = urllib.request.Request(args.server.rstrip("/") + endpoint,
                                     None if payload is None else json.dumps(payload).encode(), headers)
    try:
        with urllib.request.build_opener(NoRedirect()).open(request, timeout=10) as result:
            print(json.dumps(json.load(result), indent=2))
    except urllib.error.HTTPError as error:
        print("Event request failed (%d): %s" % (error.code, error.read(4096).decode(errors="replace")), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__": sys.exit(main())
