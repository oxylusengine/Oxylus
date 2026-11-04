using Oxylus.Hosting;

namespace Oxylus;

public static unsafe class App
{
    // stops the app after the current frame
    public static void Quit() => Runtime.Native.AppQuit();
}
