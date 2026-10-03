//! YAPF lossless images for Rust.
//!
//! ```no_run
//! let img = yapf::Image::load("texture.yapf")?;
//! println!("{}x{} with {} channels", img.width, img.height, img.channels);
//! let bytes = img.encode()?;
//! # Ok::<(), yapf::Error>(())
//! ```
use std::ffi::{c_int, c_void};
use std::fmt;
use std::path::Path;

#[repr(C)]
struct RawImage {
    width: u32,
    height: u32,
    channels: u8,
    gpu_format: u8,
    flags: u8,
    mip_levels: u8,
    pixels: *mut u8,
    mips: *mut *mut u8,
}

extern "C" {
    fn yapf_load_memory_mt(buffer: *const c_void, size: usize, threads: c_int) -> *mut RawImage;
    fn yapf_free(img: *mut RawImage);
    fn yapf_encode(img: *const RawImage, out_data: *mut *mut c_void, out_size: *mut usize) -> c_int;
    fn yapf_free_buffer(data: *mut c_void);
}

pub const FLAG_PREMULT_ALPHA: u8 = 1;
pub const FLAG_SRGB: u8 = 2;

#[derive(Debug)]
pub enum Error {
    /// Not a YAPF v1 file, or corrupt.
    Invalid,
    /// The encoder rejected the image (bad size or channel count) or ran out of memory.
    Encode(i32),
    Io(std::io::Error),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Invalid => write!(f, "not a valid YAPF file"),
            Error::Encode(c) => write!(f, "YAPF encoding failed (code {c})"),
            Error::Io(e) => write!(f, "{e}"),
        }
    }
}
impl std::error::Error for Error {}
impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self { Error::Io(e) }
}

/// A decoded image: `pixels` holds `width * height * channels` bytes, rows top to bottom.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Image {
    pub width: u32,
    pub height: u32,
    /// 1 gray, 2 gray + alpha, 3 RGB, 4 RGBA
    pub channels: u8,
    pub flags: u8,
    pub gpu_format: u8,
    pub pixels: Vec<u8>,
    /// Smaller mip levels (1, 2, …); empty when the file stores only the base image.
    pub mips: Vec<Vec<u8>>,
}

impl Image {
    pub fn new(width: u32, height: u32, channels: u8, pixels: Vec<u8>) -> Image {
        let gpu_format = match channels { 4 => 4, 3 => 5, 2 => 2, _ => 3 };
        Image { width, height, channels, flags: FLAG_SRGB, gpu_format, pixels, mips: Vec::new() }
    }

    /// Decode YAPF bytes; `threads` = 1 decodes on this thread, 0 uses every core.
    pub fn decode_with_threads(data: &[u8], threads: i32) -> Result<Image, Error> {
        unsafe {
            let p = yapf_load_memory_mt(data.as_ptr().cast(), data.len(), threads);
            if p.is_null() {
                return Err(Error::Invalid);
            }
            let r = &*p;
            let level = |m: usize| {
                let w = (r.width >> m).max(1) as usize;
                let h = (r.height >> m).max(1) as usize;
                std::slice::from_raw_parts(*r.mips.add(m), w * h * r.channels as usize).to_vec()
            };
            let img = Image {
                width: r.width, height: r.height, channels: r.channels, flags: r.flags,
                gpu_format: r.gpu_format, pixels: level(0),
                mips: (1..r.mip_levels as usize).map(level).collect(),
            };
            yapf_free(p);
            Ok(img)
        }
    }

    pub fn decode(data: &[u8]) -> Result<Image, Error> { Image::decode_with_threads(data, 1) }

    pub fn load<P: AsRef<Path>>(path: P) -> Result<Image, Error> {
        Image::decode(&std::fs::read(path)?)
    }

    /// Encode to the bytes of a `.yapf` file.
    pub fn encode(&self) -> Result<Vec<u8>, Error> {
        let mut levels: Vec<*mut u8> = std::iter::once(self.pixels.as_ptr() as *mut u8)
            .chain(self.mips.iter().map(|m| m.as_ptr() as *mut u8))
            .collect();
        let raw = RawImage {
            width: self.width, height: self.height, channels: self.channels,
            gpu_format: self.gpu_format, flags: self.flags, mip_levels: levels.len() as u8,
            pixels: levels[0], mips: levels.as_mut_ptr(),
        };
        if self.pixels.len() != self.width as usize * self.height as usize * self.channels as usize {
            return Err(Error::Encode(-1));
        }
        let mut data: *mut c_void = std::ptr::null_mut();
        let mut size = 0usize;
        let rc = unsafe { yapf_encode(&raw, &mut data, &mut size) };
        if rc != 0 {
            return Err(Error::Encode(rc));
        }
        let out = unsafe { std::slice::from_raw_parts(data as *const u8, size).to_vec() };
        unsafe { yapf_free_buffer(data) };
        Ok(out)
    }

    pub fn save<P: AsRef<Path>>(&self, path: P) -> Result<(), Error> {
        std::fs::write(path, self.encode()?)?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        for ch in 1..=4u8 {
            let (w, h) = (97u32, 61u32);
            let px: Vec<u8> = (0..w * h * ch as u32).map(|i| (i * 7 ^ i >> 5) as u8).collect();
            let img = Image::new(w, h, ch, px);
            let back = Image::decode(&img.encode().unwrap()).unwrap();
            assert_eq!(back, img);
        }
    }

    #[test]
    fn rejects_garbage() {
        assert!(Image::decode(b"not an image").is_err());
    }
}
