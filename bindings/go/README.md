# YAPF for Go

A cgo package that builds the C library itself (needs a C compiler, as any
cgo package).

```sh
go get github.com/Nexoniarz/YAPF/bindings/go
```

```go
import yapf "github.com/Nexoniarz/YAPF/bindings/go"

img, err := yapf.Load("texture.yapf")           // or yapf.Decode(bytes)
data, err := yapf.Encode(&yapf.Image{Width: 256, Height: 256, Channels: 4, Pixels: rgba})
```

Example: `go run ./example ../../other/YAPF.YAPF` · tests: `go test`.
