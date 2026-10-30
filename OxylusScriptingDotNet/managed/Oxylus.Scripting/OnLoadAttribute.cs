using System;

namespace Oxylus;

// runs once each time the engine loads the scripts assembly, on the main thread
[AttributeUsage(AttributeTargets.Method, AllowMultiple = false, Inherited = false)]
public sealed class OnLoadAttribute : Attribute;
