# YAPF for Java (and Kotlin)

`Yapf.java` uses the Foreign Function & Memory API of **Java 22+** — no JNI,
no native glue to build; just the YAPF shared library (`yapf.dll`,
`libyapf.so`, `libyapf.dylib`) on the library path or passed with
`-Dyapf.library=/path/to/lib`.

```java
Yapf.Image img = Yapf.load(Path.of("texture.yapf"));    // or Yapf.decode(bytes)
byte[] file = Yapf.encode(new Yapf.Image(256, 256, 4, rgba));
```

Kotlin can call the same class directly.  Run with
`--enable-native-access=ALL-UNNAMED` to silence the native-access warning.

Example: `java --enable-native-access=ALL-UNNAMED -Dyapf.library=../../build/libyapf.so Example.java`
