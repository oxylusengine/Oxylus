using Oxylus.Hosting;

namespace Oxylus;

// the app's frame clock, not a scene's: it keeps running while the editor pauses a scene, whose hooks then get a delta
// of 0 instead
public static unsafe class Time
{
    public static float DeltaTime => Runtime.Native.Time is null ? 0.0f : Runtime.Native.Time->DeltaTime;

    // seconds since the first frame
    public static double Elapsed => Runtime.Native.Time is null ? 0.0 : Runtime.Native.Time->Elapsed;

    public static ulong Frame => Runtime.Native.Time is null ? 0 : Runtime.Native.Time->Frame;
}
