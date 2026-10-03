# YAPF for C# / .NET

`Yapf.cs` is a single-file P/Invoke wrapper for .NET 6 or later (tested with
.NET 8).  It should also work in Unity with the native library in
`Assets/Plugins`, but that is not tested yet.  Add it to your project and put
the native library next to your program: `yapf.dll` (Windows),
`libyapf.so` (Linux) or `libyapf.dylib` (macOS) — from the
[Releases](https://github.com/Nexoniarz/YAPF/releases) or `make`.

```csharp
using Yapf;

var img = YapfImage.Load("texture.yapf");          // or YapfImage.Decode(bytes)
byte[] pixels = img.Pixels;                         // Width * Height * Channels
new YapfImage(256, 256, 4, rgba).Save("out.yapf");  // or .Encode() → byte[]
```

Example: `cd Example && dotnet run -- ../../../other/YAPF.YAPF`
(Linux: `LD_LIBRARY_PATH=../../../build dotnet run …` after `make`).
