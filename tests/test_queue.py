import unittest
from queue_model import Clip, ClipQueue


def text(value):
    return Clip('text', value, len(value), value)


class QueueTests(unittest.TestCase):
    def test_mixed_fifo_and_repeat_copies(self):
        queue = ClipQueue()
        items = [text('first'), Clip('image', b'png', 3, 'image'), text('last'), text('last')]
        for item in items:
            queue.append(item)
        for item in items:
            self.assertIs(queue.peek(), item)
            queue.commit(item.id)
        self.assertIsNone(queue.peek())

    def test_restore_prepends_even_after_more_copies(self):
        queue = ClipQueue()
        first, second, third = text('a'), text('b'), text('c')
        queue.append(first)
        queue.append(second)
        queue.commit(first.id)
        queue.append(third)
        self.assertTrue(queue.undo())
        self.assertEqual(list(queue.items), [first,second,third])
        self.assertFalse(queue.undo())

    def test_limit_preserves_existing(self):
        queue = ClipQueue(max_bytes=3)
        queue.append(text('abc'))
        with self.assertRaises(ValueError):
            queue.append(text('d'))
        self.assertEqual(queue.peek().data,'abc')

    def test_changed_head_does_not_consume(self):
        queue = ClipQueue()
        queue.append(text('a'))
        with self.assertRaises(ValueError):
            queue.commit('wrong-id')
        self.assertEqual(len(queue.items),1)

    def test_clear_forgets_restore(self):
        queue = ClipQueue()
        item = text('a')
        queue.append(item)
        queue.commit(item.id)
        queue.clear()
        self.assertFalse(queue.undo())

    def test_remove_preserves_order(self):
        queue = ClipQueue()
        items = [text(str(i)) for i in range(3)]
        for item in items:
            queue.append(item)
        queue.remove(items[1].id)
        self.assertEqual(list(queue.items),[items[0],items[2]])


if __name__ == '__main__':
    unittest.main()
