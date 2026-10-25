using System;

namespace Oxylus;

// a dense array a world dispatches one hook over. Removing during a dispatch only leaves a hole, which is compacted
// once the outermost dispatch ends, so a behaviour that destroys another mid loop never skips or repeats one
internal sealed class BehaviourList(int kind)
{
    public const int All = 0;
    public const int Update = 1;
    public const int FixedUpdate = 2;
    public const int Kinds = 3;

    Behaviour?[] items = new Behaviour?[16];
    int count;
    int dispatching;
    bool hasHoles;

    public int Count => count;

    public Behaviour? this[int index] => items[index];

    public void Add(Behaviour behaviour)
    {
        if (count == items.Length)
        {
            Array.Resize(ref items, count * 2);
        }

        behaviour.Slots[kind] = count;
        items[count++] = behaviour;
    }

    public void Remove(Behaviour behaviour)
    {
        var index = behaviour.Slots[kind];
        if (index < 0)
        {
            return;
        }

        behaviour.Slots[kind] = -1;
        if (dispatching > 0)
        {
            items[index] = null;
            hasHoles = true;
            return;
        }

        var last = --count;
        var moved = items[last];
        items[index] = moved;
        items[last] = null;
        if (moved is not null && index != last)
        {
            moved.Slots[kind] = index;
        }
    }

    // what was added during the dispatch waits for the next one
    public int BeginDispatch()
    {
        dispatching++;
        return count;
    }

    public void EndDispatch()
    {
        if (--dispatching > 0 || !hasHoles)
        {
            return;
        }

        var write = 0;
        for (var read = 0; read < count; read++)
        {
            if (items[read] is not { } behaviour)
            {
                continue;
            }

            behaviour.Slots[kind] = write;
            items[write++] = behaviour;
        }

        Array.Clear(items, write, count - write);
        count = write;
        hasHoles = false;
    }

    public void Clear()
    {
        Array.Clear(items, 0, count);
        count = 0;
        hasHoles = false;
    }
}
