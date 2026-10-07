"""In-memory FIFO; clipboard data never goes to disk."""
from dataclasses import dataclass, field
from collections import deque
import time
import uuid


@dataclass
class Clip:
    kind: str
    data: object
    size: int
    label: str
    id: str = field(default_factory=lambda: uuid.uuid4().hex)
    created: float = field(default_factory=time.time)


class ClipQueue:
    def __init__(self, max_items=200, max_bytes=256 * 1024 * 1024):
        self.items = deque()
        self.max_items, self.max_bytes = max_items, max_bytes
        self.last_pasted = None

    @property
    def size(self):
        return sum(item.size for item in self.items)

    def append(self, clip):
        if len(self.items) >= self.max_items or self.size + clip.size > self.max_bytes:
            raise ValueError('Queue is full. Paste or remove some items, then resume collecting.')
        self.items.append(clip)

    def peek(self):
        return self.items[0] if self.items else None

    def commit(self, clip_id):
        if not self.items or self.items[0].id != clip_id:
            raise ValueError('The queue changed before the paste finished. Try again.')
        self.last_pasted = self.items.popleft()

    def undo(self):
        if self.last_pasted is None:
            return False
        clip = self.last_pasted
        if len(self.items) >= self.max_items or self.size + clip.size > self.max_bytes:
            raise ValueError('Make room in the queue before restoring the last item.')
        self.items.appendleft(clip)
        self.last_pasted = None
        return True

    def remove(self, clip_id):
        self.items = deque(item for item in self.items if item.id != clip_id)

    def clear(self):
        self.items.clear()
        self.last_pasted = None
