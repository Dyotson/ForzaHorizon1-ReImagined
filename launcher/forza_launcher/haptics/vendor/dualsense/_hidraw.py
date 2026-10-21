"""Linux /dev/hidraw direct I/O.

The hidapi PyPI wheel is built against libusb, which cannot claim HID
interfaces already owned by the hid-playstation kernel driver. This module
bypasses hidapi on Linux and talks to /dev/hidraw nodes directly via the
kernel's hidraw character device interface instead.
"""

import glob
import os


def enumerate(vendor_id=0, product_id=0):
    """Return a list of HID device dicts matching the given vendor/product IDs."""
    devices = []
    for node in sorted(glob.glob("/dev/hidraw*")):
        uevent = f"/sys/class/hidraw/{os.path.basename(node)}/device/uevent"
        try:
            with open(uevent) as fh:
                hid_id = next(ln for ln in fh if ln.startswith("HID_ID=")).strip()[7:]
            bus, vid, pid = (int(x, 16) for x in hid_id.split(":"))
        except (OSError, StopIteration, ValueError):
            continue
        if (vendor_id and vid != vendor_id) or (product_id and pid != product_id):
            continue
        devices.append({"path": node.encode(), "product_id": pid, "bus_type": bus})
    return devices


class device:
    """Minimal hidraw device handle."""

    def __init__(self):
        self._fd = -1

    def open_path(self, path):
        if isinstance(path, bytes):
            path = path.decode()
        self._fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)

    def set_nonblocking(self, _):
        pass

    def write(self, data):
        return os.write(self._fd, bytes(data))

    def read(self, size, timeout_ms=0):
        try:
            return os.read(self._fd, size)
        except BlockingIOError:
            return b""

    def close(self):
        if self._fd >= 0:
            os.close(self._fd)
            self._fd = -1
