// Package yapf reads and writes YAPF lossless images (cgo bindings to the C library).
//
//	img, err := yapf.Load("texture.yapf")
//	data, err := yapf.Encode(&yapf.Image{Width: 256, Height: 256, Channels: 4, Pixels: rgba})
package yapf

/*
#cgo CFLAGS: -O2 -std=c99 -I${SRCDIR}/../..
#cgo !windows LDFLAGS: -lm -lpthread
#include <stdlib.h>
#include "yapf.h"
*/
import "C"

import (
	"errors"
	"os"
	"unsafe"
)

const (
	FlagPremultAlpha = 1
	FlagSRGB         = 2
)

var ErrInvalid = errors.New("yapf: not a valid YAPF file")

// Image holds Width*Height*Channels bytes in Pixels, rows top to bottom.
// Channels: 1 gray, 2 gray+alpha, 3 RGB, 4 RGBA.  Mips holds the smaller
// mip levels (1, 2, …), if any.
type Image struct {
	Width, Height int
	Channels      int
	Flags         uint8
	GPUFormat     uint8
	Pixels        []byte
	Mips          [][]byte
}

// Decode decodes YAPF bytes on the calling thread; DecodeThreads(data, 0) uses every core.
func Decode(data []byte) (*Image, error) { return DecodeThreads(data, 1) }

func DecodeThreads(data []byte, threads int) (*Image, error) {
	if len(data) == 0 {
		return nil, ErrInvalid
	}
	p := C.yapf_load_memory_mt(unsafe.Pointer(&data[0]), C.size_t(len(data)), C.int(threads))
	if p == nil {
		return nil, ErrInvalid
	}
	defer C.yapf_free(p)
	img := &Image{Width: int(p.width), Height: int(p.height), Channels: int(p.channels),
		Flags: uint8(p.flags), GPUFormat: uint8(p.gpu_format)}
	mips := unsafe.Slice(p.mips, int(p.mip_levels))
	for m := range mips {
		w, h := max(1, img.Width>>m), max(1, img.Height>>m)
		level := C.GoBytes(unsafe.Pointer(mips[m]), C.int(w*h*img.Channels))
		if m == 0 {
			img.Pixels = level
		} else {
			img.Mips = append(img.Mips, level)
		}
	}
	return img, nil
}

func Load(path string) (*Image, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return Decode(data)
}

// Encode returns the bytes of a .yapf file.
func Encode(img *Image) ([]byte, error) {
	if img.Channels < 1 || img.Channels > 4 || len(img.Pixels) != img.Width*img.Height*img.Channels {
		return nil, errors.New("yapf: pixels must hold Width*Height*Channels bytes")
	}
	// Copy the levels into C memory: cgo forbids passing Go pointers inside C structs.
	levels := append([][]byte{img.Pixels}, img.Mips...)
	table := (**C.uint8_t)(C.malloc(C.size_t(len(levels)) * C.size_t(unsafe.Sizeof(uintptr(0)))))
	defer C.free(unsafe.Pointer(table))
	ptrs := unsafe.Slice(table, len(levels))
	for i, l := range levels {
		ptrs[i] = (*C.uint8_t)(C.CBytes(l))
		defer C.free(unsafe.Pointer(ptrs[i]))
	}
	gpu := img.GPUFormat
	if gpu == 0 {
		gpu = map[int]uint8{4: 4, 3: 5, 2: 2, 1: 3}[img.Channels]
	}
	raw := C.yapf_image_t{width: C.uint32_t(img.Width), height: C.uint32_t(img.Height),
		channels: C.uint8_t(img.Channels), gpu_format: C.uint8_t(gpu), flags: C.uint8_t(img.Flags),
		mip_levels: C.uint8_t(len(levels)), pixels: ptrs[0], mips: table}
	var data unsafe.Pointer
	var size C.size_t
	if rc := C.yapf_encode(&raw, &data, &size); rc != 0 {
		return nil, errors.New("yapf: encoding failed")
	}
	defer C.yapf_free_buffer(data)
	return C.GoBytes(data, C.int(size)), nil
}

func Save(path string, img *Image) error {
	data, err := Encode(img)
	if err != nil {
		return err
	}
	return os.WriteFile(path, data, 0o644)
}
