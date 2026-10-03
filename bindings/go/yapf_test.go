package yapf

import (
	"bytes"
	"os"
	"testing"
)

func TestRoundtrip(t *testing.T) {
	for ch := 1; ch <= 4; ch++ {
		w, h := 97, 61
		px := make([]byte, w*h*ch)
		for i := range px {
			px[i] = byte(i*7 ^ i>>5)
		}
		data, err := Encode(&Image{Width: w, Height: h, Channels: ch, Flags: FlagSRGB, Pixels: px})
		if err != nil {
			t.Fatal(err)
		}
		back, err := Decode(data)
		if err != nil || !bytes.Equal(back.Pixels, px) {
			t.Fatalf("roundtrip failed for %d channels", ch)
		}
	}
}

func TestSampleReencodesIdentically(t *testing.T) {
	file, err := os.ReadFile("../../other/YAPF.YAPF")
	if err != nil {
		t.Skip("sample not found")
	}
	img, err := Decode(file)
	if err != nil {
		t.Fatal(err)
	}
	again, _ := Encode(img)
	if !bytes.Equal(again, file) {
		t.Fatal("re-encoded sample differs from the C encoder's output")
	}
}

func TestRejectsGarbage(t *testing.T) {
	if _, err := Decode([]byte("not an image")); err == nil {
		t.Fatal("garbage was accepted")
	}
}
