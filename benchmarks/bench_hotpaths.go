// Go counterpart of bench_hotpaths.strada — same workloads, same counts.
package main

import (
	"fmt"
	"strconv"
	"strings"
	"time"
)

type Bumper interface{ bump() int }

type Greeter struct{ n int }

func (g Greeter) bump() int { return g.n + 1 }

func secs(d time.Duration) float64 { return d.Seconds() }

func main() {
	t0 := time.Now()

	// 1. dynamic dispatch: receiver fetched from a slice through an interface.
	objs := []Bumper{Greeter{5}}
	sum := 0
	for i := 0; i < 5000000; i++ {
		p := objs[0]
		sum += p.bump()
	}
	t1 := time.Now()
	fmt.Printf("dispatch: %d %g\n", sum, secs(t1.Sub(t0)))

	// 2. hash counter loop with computed keys
	c := make(map[string]int)
	for j := 0; j < 10000000; j++ {
		k := "key" + strconv.Itoa(j%100)
		c[k] = c[k] + 1
	}
	t2 := time.Now()
	fmt.Printf("hash: %d %g\n", c["key0"], secs(t2.Sub(t1)))

	// 3. range loop
	rsum := 0
	for r := 0; r <= 20000000; r++ {
		rsum += r
	}
	t3 := time.Now()
	fmt.Printf("range: %d %g\n", rsum, secs(t3.Sub(t2)))

	// 4. large-string concat (fresh copy each iteration; mid-string byte read)
	big := strings.Repeat("y", 100000)
	tails := []string{strings.Repeat("z", 1000), strings.Repeat("w", 1000)}
	clen := 0
	for m := 0; m < 20000; m++ {
		t := big + tails[m%2]
		clen += len(t) + int(t[50000])
	}
	t4 := time.Now()
	fmt.Printf("concat: %d %g\n", clen, secs(t4.Sub(t3)))

	// 5. object construction + accessor
	obj := 0
	for o := 0; o < 2000000; o++ {
		p := Greeter{o % 100}
		obj += p.n
	}
	t5 := time.Now()
	fmt.Printf("objects: %d %g\n", obj, secs(t5.Sub(t4)))
}
