// Yapf.java — YAPF for Java 22+ via the Foreign Function & Memory API (no JNI).
// Copyright 2026 Nexoniarz — Apache License 2.0.
//
// Needs the native library: yapf.dll / libyapf.so / libyapf.dylib on the
// library path, or its full path in the system property  -Dyapf.library=...
// Run with  --enable-native-access=ALL-UNNAMED  to avoid the warning.
//
//   Yapf.Image img = Yapf.load(Path.of("texture.yapf"));
//   byte[] file = Yapf.encode(new Yapf.Image(256, 256, 4, rgba));

import java.io.IOException;
import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;
import java.nio.file.Files;
import java.nio.file.Path;

import static java.lang.foreign.ValueLayout.*;

public final class Yapf {
    public static final int FLAG_PREMULT_ALPHA = 1, FLAG_SRGB = 2;

    /** pixels: width*height*channels bytes, rows top to bottom.  mips[0] == pixels. */
    public record Image(int width, int height, int channels, int flags, int gpuFormat, byte[][] mips) {
        public Image(int width, int height, int channels, byte[] pixels) {
            this(width, height, channels, FLAG_SRGB, switch (channels) { case 4 -> 4; case 3 -> 5; case 2 -> 2; default -> 3; },
                 new byte[][] { pixels });
        }
        public byte[] pixels() { return mips[0]; }
    }

    // yapf_image_t: u32 width, u32 height, u8 channels, gpu_format, flags, mip_levels, ptr pixels, ptr mips
    private static final StructLayout IMAGE = MemoryLayout.structLayout(
        JAVA_INT.withName("width"), JAVA_INT.withName("height"),
        JAVA_BYTE.withName("channels"), JAVA_BYTE.withName("gpu_format"),
        JAVA_BYTE.withName("flags"), JAVA_BYTE.withName("mip_levels"),
        MemoryLayout.paddingLayout(4), ADDRESS.withName("pixels"), ADDRESS.withName("mips"));

    private static final MethodHandle LOAD, FREE, ENCODE, FREE_BUFFER;
    static {
        Linker linker = Linker.nativeLinker();
        String custom = System.getProperty("yapf.library");
        SymbolLookup lib = custom != null
            ? SymbolLookup.libraryLookup(Path.of(custom), Arena.global())
            : SymbolLookup.libraryLookup(System.mapLibraryName("yapf"), Arena.global());
        LOAD = linker.downcallHandle(lib.find("yapf_load_memory_mt").orElseThrow(),
            FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_LONG, JAVA_INT));
        FREE = linker.downcallHandle(lib.find("yapf_free").orElseThrow(), FunctionDescriptor.ofVoid(ADDRESS));
        ENCODE = linker.downcallHandle(lib.find("yapf_encode").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, ADDRESS));
        FREE_BUFFER = linker.downcallHandle(lib.find("yapf_free_buffer").orElseThrow(), FunctionDescriptor.ofVoid(ADDRESS));
    }

    private Yapf() {}

    /** Decode YAPF bytes; threads = 1 decodes on this thread, 0 uses every core. */
    public static Image decode(byte[] data, int threads) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment in = arena.allocateFrom(JAVA_BYTE, data);
            MemorySegment p = (MemorySegment) LOAD.invokeExact(in, (long) data.length, threads);
            if (p.equals(MemorySegment.NULL)) throw new IllegalArgumentException("not a valid YAPF file");
            try {
                MemorySegment s = p.reinterpret(IMAGE.byteSize());
                int w = s.get(JAVA_INT, 0), h = s.get(JAVA_INT, 4);
                int ch = Byte.toUnsignedInt(s.get(JAVA_BYTE, 8)), levels = Byte.toUnsignedInt(s.get(JAVA_BYTE, 11));
                MemorySegment table = s.get(ADDRESS, 24).reinterpret(ADDRESS.byteSize() * levels);
                byte[][] mips = new byte[levels][];
                for (int m = 0; m < levels; m++) {
                    long n = (long) Math.max(1, w >> m) * Math.max(1, h >> m) * ch;
                    mips[m] = table.getAtIndex(ADDRESS, m).reinterpret(n).toArray(JAVA_BYTE);
                }
                return new Image(w, h, ch, Byte.toUnsignedInt(s.get(JAVA_BYTE, 10)),
                                 Byte.toUnsignedInt(s.get(JAVA_BYTE, 9)), mips);
            } finally {
                FREE.invokeExact(p);
            }
        } catch (RuntimeException e) {
            throw e;
        } catch (Throwable t) {
            throw new IllegalStateException(t);
        }
    }

    public static Image decode(byte[] data) { return decode(data, 1); }

    public static Image load(Path path) throws IOException { return decode(Files.readAllBytes(path)); }

    /** Encode to the bytes of a .yapf file. */
    public static byte[] encode(Image img) {
        if (img.pixels().length != img.width() * img.height() * img.channels())
            throw new IllegalArgumentException("pixels must hold width*height*channels bytes");
        try (Arena arena = Arena.ofConfined()) {
            byte[][] mips = img.mips();
            MemorySegment table = arena.allocate(ADDRESS, mips.length);
            for (int m = 0; m < mips.length; m++) table.setAtIndex(ADDRESS, m, arena.allocateFrom(JAVA_BYTE, mips[m]));
            MemorySegment s = arena.allocate(IMAGE);
            s.set(JAVA_INT, 0, img.width());
            s.set(JAVA_INT, 4, img.height());
            s.set(JAVA_BYTE, 8, (byte) img.channels());
            s.set(JAVA_BYTE, 9, (byte) img.gpuFormat());
            s.set(JAVA_BYTE, 10, (byte) img.flags());
            s.set(JAVA_BYTE, 11, (byte) mips.length);
            s.set(ADDRESS, 16, table.getAtIndex(ADDRESS, 0));
            s.set(ADDRESS, 24, table);
            MemorySegment outData = arena.allocate(ADDRESS), outSize = arena.allocate(JAVA_LONG);
            int rc = (int) ENCODE.invokeExact(s, outData, outSize);
            if (rc != 0) throw new IllegalStateException("yapf_encode failed (" + rc + ")");
            MemorySegment data = outData.get(ADDRESS, 0);
            try {
                return data.reinterpret(outSize.get(JAVA_LONG, 0)).toArray(JAVA_BYTE);
            } finally {
                FREE_BUFFER.invokeExact(data);
            }
        } catch (RuntimeException e) {
            throw e;
        } catch (Throwable t) {
            throw new IllegalStateException(t);
        }
    }

    public static void save(Path path, Image img) throws IOException { Files.write(path, encode(img)); }
}
