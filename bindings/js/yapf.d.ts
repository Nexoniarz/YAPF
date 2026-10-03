// TypeScript declarations for yapf.js
export interface YapfImage {
  width: number;
  height: number;
  /** 1 gray, 2 gray + alpha, 3 RGB, 4 RGBA */
  channels: 1 | 2 | 3 | 4;
  /** width * height * channels bytes, rows top to bottom */
  pixels: Uint8Array;
  gpuFormat?: number;
  flags?: number;
  mipLevels?: number;
  /** mips[0] is the full-size image */
  mips?: Uint8Array[];
}

/** Decode a .yapf file; throws on corrupt or unsupported data. */
export function decode(bytes: Uint8Array | ArrayBuffer): Required<YapfImage>;
/** Encode an image to the bytes of a .yapf file (identical to the C encoder). */
export function encode(image: YapfImage): Uint8Array;
/** Any channel count → RGBA, ready for `new ImageData(rgba, w, h)`. */
export function toRGBA(image: YapfImage, level?: number): Uint8ClampedArray;

export const GPU: { RGBA8: 0; RGB8: 1; RG8: 2; R8: 3; SRGB8_A8: 4; SRGB8: 5 };
export const FLAG: { PREMULT_ALPHA: 1; SRGB: 2 };
export const VERSION: 1;
