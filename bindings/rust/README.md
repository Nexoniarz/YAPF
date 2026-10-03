# YAPF for Rust

A crate that compiles the C library with the `cc` crate, so `cargo build`
works on Windows, macOS and Linux with no extra steps.

```toml
[dependencies]
yapf = { git = "https://github.com/Nexoniarz/YAPF" }
```

```rust
let img = yapf::Image::load("texture.yapf")?;           // or Image::decode(&bytes)
println!("{}x{}, {} channels", img.width, img.height, img.channels);
yapf::Image::new(256, 256, 4, rgba).save("out.yapf")?;   // or .encode() → Vec<u8>
```

Example: `cargo run --release --example demo -- ../../other/YAPF.YAPF` ·
tests: `cargo test`.
