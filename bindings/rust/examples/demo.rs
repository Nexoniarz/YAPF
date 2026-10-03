// cargo run --release --example demo -- ../../other/YAPF.YAPF
use std::time::Instant;

fn main() -> Result<(), yapf::Error> {
    let path = std::env::args().nth(1).unwrap_or_else(|| "../../other/YAPF.YAPF".into());
    let file = std::fs::read(&path)?;

    let mut img = yapf::Image::decode(&file)?;
    let mut best = f64::MAX; // best of 10, as in a running program
    for _ in 0..10 {
        let t = Instant::now();
        img = yapf::Image::decode(&file)?;
        best = best.min(t.elapsed().as_secs_f64() * 1e3);
    }
    println!("{path}: {}x{}, {} channels, decoded in {best:.2} ms", img.width, img.height, img.channels);
    println!("file is {} bytes, {:.1}% of the raw pixels",
             file.len(), 100.0 * file.len() as f64 / img.pixels.len() as f64);

    let again = img.encode()?;
    println!("re-encoded: {} bytes, identical: {}", again.len(), again == file);

    let px: Vec<u8> = (0..128 * 128).flat_map(|i| [(i % 128 * 2) as u8, (i / 128 * 2) as u8, 128]).collect();
    yapf::Image::new(128, 128, 3, px).save("rust.yapf")?;
    println!("wrote rust.yapf");
    Ok(())
}
