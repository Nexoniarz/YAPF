// go run ./example ../../other/YAPF.YAPF
package main

import (
	"bytes"
	"fmt"
	"os"
	"time"

	yapf "github.com/Nexoniarz/YAPF/bindings/go"
)

func main() {
	path := "../../other/YAPF.YAPF"
	if len(os.Args) > 1 {
		path = os.Args[1]
	}
	file, err := os.ReadFile(path)
	if err != nil {
		panic(err)
	}
	img, err := yapf.Decode(file)
	if err != nil {
		panic(err)
	}
	best := time.Hour // best of 10, as in a running program
	for i := 0; i < 10; i++ {
		t := time.Now()
		img, _ = yapf.Decode(file)
		best = min(best, time.Since(t))
	}
	fmt.Printf("%s: %dx%d, %d channels, decoded in %.2f ms\n", path, img.Width, img.Height,
		img.Channels, float64(best.Microseconds())/1000)
	fmt.Printf("file is %d bytes, %.1f%% of the raw pixels\n", len(file), 100*float64(len(file))/float64(len(img.Pixels)))

	again, _ := yapf.Encode(img)
	fmt.Printf("re-encoded: %d bytes, identical: %v\n", len(again), bytes.Equal(again, file))

	px := make([]byte, 128*128)
	for i := range px {
		px[i] = byte(i % 128 * 2)
	}
	if err := yapf.Save("go.yapf", &yapf.Image{Width: 128, Height: 128, Channels: 1, Flags: yapf.FlagSRGB, Pixels: px}); err != nil {
		panic(err)
	}
	fmt.Println("wrote go.yapf")
}
