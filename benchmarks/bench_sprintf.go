// Go counterpart of bench_sprintf.strada. Go's fmt.Sprintf maps directly
// onto the formatting work (the JS version emulates sprintf with template
// literals + toFixed/toString/padStart, producing the same strings).
package main

import (
	"fmt"
	"strconv"
	"time"
)

func main() {
	t0 := time.Now()
	mlen := 0
	for i := 0; i < 500000; i++ {
		line := fmt.Sprintf("[INFO] user=u%d req=%d bytes=%d t=%.2fms",
			i%1000, i, (i*37)%100000, float64(i%500)/7.0)
		mlen += len(line)
	}
	t1 := time.Now()
	fmt.Println("mixed:", mlen, t1.Sub(t0).Seconds())

	nlen := 0
	for i := 0; i < 1000000; i++ {
		nlen += len(fmt.Sprintf("%d %x %o", i, i, i))
	}
	t2 := time.Now()
	fmt.Println("numeric:", nlen, t2.Sub(t1).Seconds())

	flen := 0
	for i := 0; i < 500000; i++ {
		v := float64(i) / 3.0
		flen += len(fmt.Sprintf("%.6f %.6e %v", v, v, v))
	}
	t3 := time.Now()
	fmt.Println("float:", flen, t3.Sub(t2).Seconds())

	wlen := 0
	for i := 0; i < 500000; i++ {
		f := strconv.FormatFloat(float64(i%1000)/10.0, 'f', 1, 64)
		row := fmt.Sprintf("%-20s|%10d|%08d|%6s%%", "row_"+strconv.Itoa(i%100), i, i%1000, f)
		wlen += len(row)
	}
	t4 := time.Now()
	fmt.Println("width:", wlen, t4.Sub(t3).Seconds())
	fmt.Println("total:", t4.Sub(t0).Seconds())
}
