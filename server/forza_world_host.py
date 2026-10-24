"""Who leads a Horizon World session after its host left (issue #12, docs/online_vision.md).

The game picks a new host among a world's remaining players by itself, without telling
Xbox LIVE. Every player in a session someone else hosts publishes a standby record of it
(MATCH_PUBLISH op 2), and the one the game made its host says so (op 4); searchers are
placed with the record of the session's leader (ForzaLive.standby_sessions).

A host whose game was closed or lost its connection stays registered until it times out
(PEER_TIMEOUT, 30 s), and its records with it. Measured with four games: the game made
one of the other three its host 16 s after the host's game was closed, and for the next
14 s searchers were still placed with the gone host's record: the join failed and the
searcher opened a world of its own. So:

- a session that a member reports to host (op 4) is no longer the session of the player
  that published it as its host (op 1): that record is out of date;
- among the members that report to host the session, the latest to become it leads (the
  earlier one is the host that left), then the other members, the oldest first.
"""

import time


def note_host(peer, host):
    """Before a standby (op 2) or host standby (op 4) of peer is stored: remember when the
    game made it the host."""
    if host and not getattr(peer, "standby_host", False):
        peer.host_since = time.monotonic()


def taken_over(peers, key):
    """The session ids of playlist key that one of their members reports to host."""
    return {p.standby[1][:8] for p in peers
            if getattr(p, "standby_host", False) and p.standby and p.standby[0] == key}


def lead_order(peer):
    """Sort key of a session's members, its leader first."""
    if getattr(peer, "standby_host", False):
        return 0, -getattr(peer, "host_since", 0.0), peer.standby[2]
    return 1, 0.0, peer.standby[2]
