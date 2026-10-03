// Yapf.cs — C# bindings for YAPF (P/Invoke).  Copyright 2026 Nexoniarz — Apache 2.0.
//
// Needs the native library next to your program or on the library path:
// yapf.dll (Windows), libyapf.so (Linux), libyapf.dylib (macOS).
//
//   var img = YapfImage.Load("texture.yapf");
//   byte[] pixels = img.Pixels;                 // Width * Height * Channels, rows top to bottom
//   byte[] file = new YapfImage(256, 256, 4, myRgba).Encode();
using System;
using System.IO;
using System.Runtime.InteropServices;

namespace Yapf
{
    public sealed class YapfException : Exception
    {
        public YapfException(string message) : base(message) { }
    }

    public sealed class YapfImage
    {
        public const byte FlagPremultipliedAlpha = 1, FlagSrgb = 2;

        public int Width { get; }
        public int Height { get; }
        public int Channels { get; }            // 1 gray, 2 gray+alpha, 3 RGB, 4 RGBA
        public byte Flags { get; }
        public byte GpuFormat { get; }
        public byte[] Pixels => Mips[0];
        public byte[][] Mips { get; }           // Mips[0] is the full-size image

        public YapfImage(int width, int height, int channels, byte[] pixels,
                         byte flags = FlagSrgb, byte[][]? mips = null)
        {
            if (pixels.Length != width * height * channels)
                throw new ArgumentException("pixels must hold width * height * channels bytes");
            Width = width; Height = height; Channels = channels; Flags = flags;
            GpuFormat = channels switch { 4 => (byte)4, 3 => (byte)5, 2 => (byte)2, _ => (byte)3 };
            Mips = mips ?? new[] { pixels };
            Mips[0] = pixels;
        }

        private YapfImage(Native n)
        {
            Width = (int)n.width; Height = (int)n.height; Channels = n.channels;
            Flags = n.flags; GpuFormat = n.gpu_format;
            Mips = new byte[n.mip_levels][];
            for (int m = 0; m < n.mip_levels; m++)
            {
                int w = Math.Max(1, Width >> m), h = Math.Max(1, Height >> m);
                Mips[m] = new byte[w * h * Channels];
                Marshal.Copy(Marshal.ReadIntPtr(n.mips, m * IntPtr.Size), Mips[m], 0, Mips[m].Length);
            }
        }

        /// <summary>Decode YAPF bytes.  threads: 1 = calling thread, 0 = all cores.</summary>
        public static YapfImage Decode(byte[] data, int threads = 1)
        {
            IntPtr p = NativeMethods.yapf_load_memory_mt(data, (UIntPtr)data.Length, threads);
            if (p == IntPtr.Zero) throw new YapfException("not a valid YAPF file");
            try { return new YapfImage(Marshal.PtrToStructure<Native>(p)); }
            finally { NativeMethods.yapf_free(p); }
        }

        public static YapfImage Load(string path, int threads = 1) => Decode(File.ReadAllBytes(path), threads);

        /// <summary>Encode to the bytes of a .yapf file.</summary>
        public byte[] Encode()
        {
            var handles = new GCHandle[Mips.Length];
            IntPtr table = Marshal.AllocHGlobal(IntPtr.Size * Mips.Length);
            try
            {
                for (int m = 0; m < Mips.Length; m++)
                {
                    handles[m] = GCHandle.Alloc(Mips[m], GCHandleType.Pinned);
                    Marshal.WriteIntPtr(table, m * IntPtr.Size, handles[m].AddrOfPinnedObject());
                }
                var n = new Native
                {
                    width = (uint)Width, height = (uint)Height, channels = (byte)Channels,
                    gpu_format = GpuFormat, flags = Flags, mip_levels = (byte)Mips.Length,
                    pixels = handles[0].AddrOfPinnedObject(), mips = table,
                };
                int rc = NativeMethods.yapf_encode(ref n, out IntPtr data, out UIntPtr size);
                if (rc != 0) throw new YapfException($"yapf_encode failed ({rc})");
                try
                {
                    var bytes = new byte[(int)size];
                    Marshal.Copy(data, bytes, 0, bytes.Length);
                    return bytes;
                }
                finally { NativeMethods.yapf_free_buffer(data); }
            }
            finally
            {
                foreach (var h in handles) if (h.IsAllocated) h.Free();
                Marshal.FreeHGlobal(table);
            }
        }

        public void Save(string path) => File.WriteAllBytes(path, Encode());

        [StructLayout(LayoutKind.Sequential)]
        private struct Native
        {
            public uint width, height;
            public byte channels, gpu_format, flags, mip_levels;
            public IntPtr pixels, mips;
        }

        private static class NativeMethods
        {
            private const string Lib = "yapf";
            [DllImport(Lib)] public static extern IntPtr yapf_load_memory_mt(byte[] data, UIntPtr size, int threads);
            [DllImport(Lib)] public static extern void yapf_free(IntPtr img);
            [DllImport(Lib)] public static extern int yapf_encode(ref Native img, out IntPtr data, out UIntPtr size);
            [DllImport(Lib)] public static extern void yapf_free_buffer(IntPtr data);
        }
    }
}
