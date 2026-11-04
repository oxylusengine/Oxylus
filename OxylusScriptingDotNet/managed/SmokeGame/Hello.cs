using Oxylus;

namespace SmokeGame;

public static class Hello
{
    [OnLoad]
    public static void SayHello() => Log.Info("hello from SmokeGame");
}
