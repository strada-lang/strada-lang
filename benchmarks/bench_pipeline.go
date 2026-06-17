// Go counterpart of bench_pipeline.strada (identical workload).
package main

import (
	"fmt"
	"sort"
	"strconv"
	"strings"
	"time"
)

func main() {
	t0 := time.Now()
	squares := make([]int, 2000000)
	for i := 0; i < 2000000; i++ {
		squares[i] = (i + 1) * (i + 1)
	}
	t1 := time.Now()
	fmt.Println("map-range:", squares[1999999], t1.Sub(t0).Seconds())

	mults := make([]int, 0)
	for i := 1; i <= 2000000; i++ {
		if i%7 == 0 {
			mults = append(mults, i)
		}
	}
	t2 := time.Now()
	fmt.Println("grep-range:", len(mults), t2.Sub(t1).Seconds())

	base := make([]int, 0, 300000)
	var seed int64 = 42
	for i := 0; i < 300000; i++ {
		seed = (seed*1103515245 + 12345) % 2147483648
		base = append(base, int(seed%1000000))
	}
	mapped := make([]int, len(base))
	for i, x := range base {
		mapped[i] = x*3 + 1
	}
	kept := make([]int, 0, len(mapped))
	for _, x := range mapped {
		if x%2 == 1 {
			kept = append(kept, x)
		}
	}
	sort.Slice(kept, func(a, b int) bool { return kept[a] > kept[b] })
	top := 0
	for i := 0; i < 100; i++ {
		top += kept[i]
	}
	t3 := time.Now()
	fmt.Println("chain:", top, t3.Sub(t2).Seconds())

	words := make([]string, 0, 500000)
	for i := 0; i < 500000; i++ {
		words = append(words, "w"+strconv.Itoa(i%1000))
	}
	jlen := 0
	for r := 0; r < 3; r++ {
		jlen += len(strings.Join(words, ","))
	}
	t4 := time.Now()
	fmt.Println("join:", jlen, t4.Sub(t3).Seconds())

	csv := strings.Join(words, ",")
	slen := 0
	for r := 0; r < 3; r++ {
		slen += len(strings.Split(csv, ","))
	}
	t5 := time.Now()
	fmt.Println("split-join:", slen, t5.Sub(t4).Seconds())
	fmt.Println("total:", t5.Sub(t0).Seconds())
}
