import mmap
import ctypes
import threading
from typing import Any, Dict

class SharedStateBus:
    """
    High-performance Shared State Bus using a memory-mapped file.
    Allows different components to communicate via a shared memory region.
    """
    def __init__(self, filename="/tmp/vesta_bus.bin", size=1024 * 1024):
        self.size = size
        self.filename = filename
        # Create a memory-mapped file for shared state
        self.mm = mmap.mmap(-1, self.size, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ | mmap.PROT_WRITE)
        self._lock = threading.Lock()

    def write_state(self, offset: int, data: bytes):
        with self._lock:
            self.mm.seek(offset)
            self.mm.write(data)

    def read_state(self, offset: int, length: int) -> bytes:
        with self._lock:
            self.mm.seek(offset)
            return self.mm.read(length)

    def close(self):
        self.mm.close()
