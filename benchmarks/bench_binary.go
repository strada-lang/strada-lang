// Go counterpart of bench_binary.strada (encoding/binary + base64;
// pack/unpack via explicit big-endian writes, the idiomatic Go form).
package main

import (
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"time"
)

func main() {
	t0 := time.Now()
	packed := make([][]byte, 0, 200000)
	for i := 0; i < 200000; i++ {
		b := make([]byte, 7+8)
		binary.BigEndian.PutUint32(b[0:], uint32(i*13))
		binary.BigEndian.PutUint16(b[4:], uint16(i%65536))
		b[6] = byte(i % 256)
		copy(b[7:], "payload!")
		packed = append(packed, b)
	}
	t1 := time.Now()
	fmt.Println("pack:", len(packed), t1.Sub(t0).Seconds())

	usum := 0
	for _, b := range packed {
		usum += int(binary.BigEndian.Uint16(b[4:]))
	}
	t2 := time.Now()
	fmt.Println("unpack:", usum, t2.Sub(t1).Seconds())

	blob := make([]byte, 262144)
	for i := 0; i < 65536; i++ {
		blob[i*4] = byte(i % 256)
		blob[i*4+1] = byte((i * 7) % 256)
		blob[i*4+2] = byte((i * 13) % 256)
		blob[i*4+3] = byte((i * 31) % 256)
	}
	b64Len := 0
	for r := 0; r < 20; r++ {
		enc := base64.StdEncoding.EncodeToString(blob)
		dec, _ := base64.StdEncoding.DecodeString(enc)
		b64Len += len(dec)
	}
	t3 := time.Now()
	fmt.Println("base64:", b64Len, t3.Sub(t2).Seconds())

	big := make([]byte, 0, len(blob)*8)
	for r := 0; r < 8; r++ {
		big = append(big, blob...)
	}
	cksum := 0
	for i := 0; i < len(big); i += 16 {
		cksum = (cksum + int(big[i])) % 65536
	}
	t4 := time.Now()
	fmt.Println("bytes:", cksum, t4.Sub(t3).Seconds())

	frame := make([]byte, 1000000)
	for i := 0; i < 1000000; i++ {
		frame[i] = byte(i % 256)
	}
	t5 := time.Now()
	fmt.Println("build:", len(frame), t5.Sub(t4).Seconds())
	fmt.Println("total:", t5.Sub(t0).Seconds())
}
