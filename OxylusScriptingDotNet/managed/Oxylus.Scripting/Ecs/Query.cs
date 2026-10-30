using System;
using System.Runtime.InteropServices;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

// owns a flecs query plus the storage to iterate it, so iterating allocates nothing. Only owned components match,
// every table hands out spans over its columns. Dispose it when done, or it stays registered in the world
public abstract unsafe class Query : IDisposable
{
    void* handle;
    readonly void* iter;
    readonly void** fields;
    readonly TableView* view;
    bool isIterating;

    private protected Query(World world, ReadOnlySpan<Term> terms)
    {
        fixed (Term* data = terms)
        {
            handle = Runtime.Native.CreateQuery(world.Handle, data, terms.Length);
        }

        if (handle is null)
        {
            throw new InvalidOperationException("Couldn't create the query, see the log");
        }

        iter = NativeMemory.AlignedAlloc(Runtime.Native.IterSize, 16);
        fields = (void**)NativeMemory.Alloc((nuint)terms.Length, (nuint)sizeof(void*));
        view = (TableView*)NativeMemory.Alloc((nuint)sizeof(TableView));
    }

    private protected TableView* View => view;

    private protected void Begin()
    {
        ObjectDisposedException.ThrowIf(handle is null, this);
        if (isIterating)
        {
            throw new InvalidOperationException("A query can't be iterated inside its own iteration");
        }

        Runtime.Native.QueryIter(handle, iter);
        isIterating = true;
    }

    // flecs finishes the iterator itself when it runs out
    private protected bool Next()
    {
        if (Runtime.Native.QueryNext(iter, view, fields))
        {
            return true;
        }

        isIterating = false;
        return false;
    }

    private protected void End()
    {
        if (isIterating)
        {
            Runtime.Native.IterFini(iter);
            isIterating = false;
        }
    }

    public void Dispose()
    {
        if (handle is null)
        {
            return;
        }

        End();
        Runtime.Native.DestroyQuery(handle);
        handle = null;
        NativeMemory.AlignedFree(iter);
        NativeMemory.Free(fields);
        NativeMemory.Free(view);
    }
}

public sealed unsafe class Query<T1> : Query
    where T1 : unmanaged
{
    internal Query(World world) : base(world, [new Term { Id = world.IdOf<T1>() }])
    {
    }

    public Enumerator GetEnumerator()
    {
        Begin();
        return new Enumerator(this);
    }

    public ref struct Enumerator(Query<T1> query)
    {
        public readonly Table<T1> Current => new(query.View);
        public readonly bool MoveNext() => query.Next();
        public readonly void Dispose() => query.End();
    }
}

public readonly unsafe ref struct Table<T1>
    where T1 : unmanaged
{
    readonly TableView* view;

    internal Table(TableView* view) => this.view = view;

    public int Count => view->Count;
    public ReadOnlySpan<ulong> EntityIds => new(view->Entities, view->Count);
    public Span<T1> Item1 => new(view->Fields[0], view->Count);

    public Entity Entity(int index) => new(view->World, view->Entities[index]);

    public void Deconstruct(out Span<T1> item1)
    {
        item1 = Item1;
    }
}

public sealed unsafe class Query<T1, T2> : Query
    where T1 : unmanaged
    where T2 : unmanaged
{
    internal Query(World world) : base(world, [new Term { Id = world.IdOf<T1>() }, new Term { Id = world.IdOf<T2>() }])
    {
    }

    public Enumerator GetEnumerator()
    {
        Begin();
        return new Enumerator(this);
    }

    public ref struct Enumerator(Query<T1, T2> query)
    {
        public readonly Table<T1, T2> Current => new(query.View);
        public readonly bool MoveNext() => query.Next();
        public readonly void Dispose() => query.End();
    }
}

public readonly unsafe ref struct Table<T1, T2>
    where T1 : unmanaged
    where T2 : unmanaged
{
    readonly TableView* view;

    internal Table(TableView* view) => this.view = view;

    public int Count => view->Count;
    public ReadOnlySpan<ulong> EntityIds => new(view->Entities, view->Count);
    public Span<T1> Item1 => new(view->Fields[0], view->Count);
    public Span<T2> Item2 => new(view->Fields[1], view->Count);

    public Entity Entity(int index) => new(view->World, view->Entities[index]);

    public void Deconstruct(out Span<T1> item1, out Span<T2> item2)
    {
        item1 = Item1;
        item2 = Item2;
    }
}

public sealed unsafe class Query<T1, T2, T3> : Query
    where T1 : unmanaged
    where T2 : unmanaged
    where T3 : unmanaged
{
    internal Query(World world) : base(world, [new Term { Id = world.IdOf<T1>() }, new Term { Id = world.IdOf<T2>() }, new Term { Id = world.IdOf<T3>() }])
    {
    }

    public Enumerator GetEnumerator()
    {
        Begin();
        return new Enumerator(this);
    }

    public ref struct Enumerator(Query<T1, T2, T3> query)
    {
        public readonly Table<T1, T2, T3> Current => new(query.View);
        public readonly bool MoveNext() => query.Next();
        public readonly void Dispose() => query.End();
    }
}

public readonly unsafe ref struct Table<T1, T2, T3>
    where T1 : unmanaged
    where T2 : unmanaged
    where T3 : unmanaged
{
    readonly TableView* view;

    internal Table(TableView* view) => this.view = view;

    public int Count => view->Count;
    public ReadOnlySpan<ulong> EntityIds => new(view->Entities, view->Count);
    public Span<T1> Item1 => new(view->Fields[0], view->Count);
    public Span<T2> Item2 => new(view->Fields[1], view->Count);
    public Span<T3> Item3 => new(view->Fields[2], view->Count);

    public Entity Entity(int index) => new(view->World, view->Entities[index]);

    public void Deconstruct(out Span<T1> item1, out Span<T2> item2, out Span<T3> item3)
    {
        item1 = Item1;
        item2 = Item2;
        item3 = Item3;
    }
}

public sealed unsafe class Query<T1, T2, T3, T4> : Query
    where T1 : unmanaged
    where T2 : unmanaged
    where T3 : unmanaged
    where T4 : unmanaged
{
    internal Query(World world) : base(world, [new Term { Id = world.IdOf<T1>() }, new Term { Id = world.IdOf<T2>() }, new Term { Id = world.IdOf<T3>() }, new Term { Id = world.IdOf<T4>() }])
    {
    }

    public Enumerator GetEnumerator()
    {
        Begin();
        return new Enumerator(this);
    }

    public ref struct Enumerator(Query<T1, T2, T3, T4> query)
    {
        public readonly Table<T1, T2, T3, T4> Current => new(query.View);
        public readonly bool MoveNext() => query.Next();
        public readonly void Dispose() => query.End();
    }
}

public readonly unsafe ref struct Table<T1, T2, T3, T4>
    where T1 : unmanaged
    where T2 : unmanaged
    where T3 : unmanaged
    where T4 : unmanaged
{
    readonly TableView* view;

    internal Table(TableView* view) => this.view = view;

    public int Count => view->Count;
    public ReadOnlySpan<ulong> EntityIds => new(view->Entities, view->Count);
    public Span<T1> Item1 => new(view->Fields[0], view->Count);
    public Span<T2> Item2 => new(view->Fields[1], view->Count);
    public Span<T3> Item3 => new(view->Fields[2], view->Count);
    public Span<T4> Item4 => new(view->Fields[3], view->Count);

    public Entity Entity(int index) => new(view->World, view->Entities[index]);

    public void Deconstruct(out Span<T1> item1, out Span<T2> item2, out Span<T3> item3, out Span<T4> item4)
    {
        item1 = Item1;
        item2 = Item2;
        item3 = Item3;
        item4 = Item4;
    }
}
