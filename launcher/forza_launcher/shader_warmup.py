"""Read bounded startup progress from the game's existing local log."""
from dataclasses import dataclass
from pathlib import Path
import re

_PROGRESS = re.compile(r"SHADER_WARMUP: (Vulkan|D3D12) (begin|progress ([0-9]{1,10})/([0-9]{1,10})|complete in ([0-9]{1,12}) ms)$")
_LIMIT = 64 * 1024


@dataclass(frozen=True)
class Status:
    backend: str
    active: bool
    done: int = 0
    total: int = 0

    @property
    def message(self):
        if not self.active:
            return "Shader preparation finished."
        if self.total:
            return f"Preparing cached shaders — {self.done:,} of {self.total:,}"
        return "Preparing cached shaders…"


class Monitor:
    def __init__(self, path):
        self.path = Path(path) if path else None
        self.status = None
        self.offset = 0
        self.pending = b""
        self.identity = None
        self.initial = self._signature()
        self.started = False

    def _signature(self):
        try:
            info = self.path.stat() if self.path else None
            return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns) if info else None
        except OSError:
            return None

    def poll(self):
        signature = self._signature()
        if signature is None or (not self.started and signature == self.initial):
            return self.status
        identity, size = signature[:2], signature[2]
        skip_first = False
        if not self.started or identity != self.identity or size < self.offset:
            self.offset = max(0, size - _LIMIT)
            self.pending = b""
            self.status = None
            self.started = True
            skip_first = self.offset > 0
        if size - self.offset > _LIMIT:
            self.offset = size - _LIMIT
            self.pending = b""
            skip_first = True
        try:
            with self.path.open("rb") as source:
                source.seek(self.offset)
                data = source.read(_LIMIT)
                self.offset += len(data)
        except OSError:
            return self.status
        if skip_first and b"\n" in data:
            data = data.split(b"\n", 1)[1]
        lines = (self.pending + data).split(b"\n")
        self.pending = lines.pop()[-_LIMIT:]
        self.identity = identity
        for raw in lines:
            match = _PROGRESS.search(raw.decode("utf-8", errors="replace").strip())
            if not match:
                continue
            backend, event, done, total, elapsed = match.groups()
            if event == "begin":
                self.status = Status(backend, True)
            elif elapsed is not None:
                self.status = Status(backend, False)
            else:
                done, total = int(done), int(total)
                if not 0 < total <= 2147483647 or done > total:
                    continue
                if self.status and self.status.active and self.status.backend == backend and self.status.total == total:
                    done = max(done, self.status.done)  # Worker log lines can arrive out of order.
                self.status = Status(backend, True, done, total)
        return self.status
