// dotnet run -- ../../../other/YAPF.YAPF   (with yapf.dll / libyapf.so on the library path)
using System.Diagnostics;
using Yapf;

string path = args.Length > 0 ? args[0] : "../../../other/YAPF.YAPF";
byte[] file = File.ReadAllBytes(path);

var img = YapfImage.Decode(file);                 // first call also loads the library / JIT
double ms = double.MaxValue;                      // best of 10, as in a running program
for (int i = 0; i < 10; i++)
{
    var sw = Stopwatch.StartNew();
    img = YapfImage.Decode(file);
    ms = Math.Min(ms, sw.Elapsed.TotalMilliseconds);
}
Console.WriteLine($"{path}: {img.Width}x{img.Height}, {img.Channels} channels, decoded in {ms:F2} ms");
Console.WriteLine($"file is {file.Length} bytes, {100.0 * file.Length / img.Pixels.Length:F1}% of the raw pixels");

byte[] again = img.Encode();
Console.WriteLine($"re-encoded: {again.Length} bytes, identical: {again.AsSpan().SequenceEqual(file)}");

var rgba = new byte[64 * 64 * 4];
for (int i = 0; i < rgba.Length; i += 4) { rgba[i] = (byte)(i / 4 % 64 * 4); rgba[i + 1] = 100; rgba[i + 2] = 200; rgba[i + 3] = 255; }
new YapfImage(64, 64, 4, rgba).Save("csharp.yapf");
Console.WriteLine("wrote csharp.yapf");
