// java --enable-native-access=ALL-UNNAMED -Dyapf.library=../../build/libyapf.so Example.java ../../other/YAPF.YAPF
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

public class Example {
    public static void main(String[] args) throws Exception {
        Path path = Path.of(args.length > 0 ? args[0] : "../../other/YAPF.YAPF");
        byte[] file = Files.readAllBytes(path);

        Yapf.Image img = Yapf.decode(file);            // first call also loads the library
        double ms = Double.MAX_VALUE;                  // best of 10, as in a running program
        for (int i = 0; i < 10; i++) {
            long t = System.nanoTime();
            img = Yapf.decode(file);
            ms = Math.min(ms, (System.nanoTime() - t) / 1e6);
        }
        System.out.printf("%s: %dx%d, %d channels, decoded in %.2f ms%n", path, img.width(), img.height(), img.channels(), ms);
        System.out.printf("file is %d bytes, %.1f%% of the raw pixels%n", file.length, 100.0 * file.length / img.pixels().length);

        byte[] again = Yapf.encode(img);
        System.out.printf("re-encoded: %d bytes, identical: %b%n", again.length, Arrays.equals(again, file));

        byte[] rgb = new byte[64 * 64 * 3];
        for (int i = 0; i < rgb.length; i++) rgb[i] = (byte) (i * 3);
        Yapf.save(Path.of("java.yapf"), new Yapf.Image(64, 64, 3, rgb));
        System.out.println("wrote java.yapf");
    }
}
