// Go counterpart of bench_sort.strada (identical workload).
package main

import (
	"fmt"
	"sort"
	"strconv"
	"time"
)

var seed int64 = 42

func lcg() int64 {
	seed = (seed*1103515245 + 12345) % 2147483648
	return seed
}

func main() {
	ints := make([]int64, 0, 1000000)
	for i := 0; i < 1000000; i++ {
		ints = append(ints, lcg()%10000000)
	}
	strs := make([]string, 0, 500000)
	for i := 0; i < 500000; i++ {
		strs = append(strs, "key_"+strconv.FormatInt(lcg()%1000000, 10)+"_suffix")
	}

	t0 := time.Now()
	si := make([]int64, len(ints))
	copy(si, ints)
	sort.Slice(si, func(a, b int) bool { return si[a] < si[b] })
	t1 := time.Now()
	fmt.Println("int-sort:", si[0], t1.Sub(t0).Seconds())

	ss := make([]string, len(strs))
	copy(ss, strs)
	sort.Strings(ss)
	t2 := time.Now()
	fmt.Println("str-sort:", len(ss[0]), t2.Sub(t1).Seconds())

	sc := make([]int64, 500000)
	copy(sc, ints[:500000])
	sort.Slice(sc, func(a, b int) bool { return sc[a] > sc[b] })
	t3 := time.Now()
	fmt.Println("cmp-sort:", sc[0], t3.Sub(t2).Seconds())

	h := make(map[string]int)
	for i := 0; i < 200000; i++ {
		h["k"+strconv.Itoa(i)] = i
	}
	hsum := 0
	for r := 0; r < 5; r++ {
		hk := make([]string, 0, len(h))
		for k := range h {
			hk = append(hk, k)
		}
		sort.Strings(hk)
		hsum += len(hk[0])
	}
	t4 := time.Now()
	fmt.Println("hash-sort:", hsum, t4.Sub(t3).Seconds())
	fmt.Println("total:", t4.Sub(t0).Seconds())
}
