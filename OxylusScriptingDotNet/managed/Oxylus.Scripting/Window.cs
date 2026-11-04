using System;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

// the app's window. Both sizes are zero while the app has none
public static unsafe class Window
{
    // the framebuffer's size in pixels
    public static (int Width, int Height) Size
    {
        get
        {
            WindowSizeData size;
            Runtime.Native.WindowSize(&size);
            return (size.PixelsX, size.PixelsY);
        }
    }

    // the size in screen coordinates, which differs from Size on a high DPI display
    public static (int Width, int Height) LogicalSize
    {
        get
        {
            WindowSizeData size;
            Runtime.Native.WindowSize(&size);
            return (size.LogicalX, size.LogicalY);
        }
    }

    // the new size in pixels, raised on the main thread
    public static event Action<int, int>? Resized;

    internal static void OnResized(uint width, uint height) => Resized?.Invoke((int)width, (int)height);

    internal static void Release() => Resized = null;
}
