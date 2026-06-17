// Go counterpart of bench_closures.strada (identical workload).
package main

import (
	"fmt"
	"strconv"
	"time"
)

func makeAdder(base int) func(int) int {
	bias := base * 2
	return func(x int) int { return x + bias }
}

func makeOuter(seed int) func(int) int {
	level0 := seed
	mid := func() func(int) int {
		return func(x int) int { return x + level0 }
	}
	return mid()
}

func main() {
	t0 := time.Now()
	made := 0
	for i := 0; i < 500000; i++ {
		_ = makeAdder(i)
		made++
	}
	t1 := time.Now()
	fmt.Println("create:", made, t1.Sub(t0).Seconds())

	add7 := makeAdder(7)
	sum := 0
	for i := 0; i < 5000000; i++ {
		sum += add7(i % 100)
	}
	t2 := time.Now()
	fmt.Println("invoke:", sum, t2.Sub(t1).Seconds())

	counter := 0
	bump := func() int { counter++; return counter }
	for i := 0; i < 2000000; i++ {
		bump()
	}
	t3 := time.Now()
	fmt.Println("capture-rw:", counter, t3.Sub(t2).Seconds())

	table := make(map[string]func(int) int)
	for i := 0; i < 16; i++ {
		table["op"+strconv.Itoa(i)] = makeAdder(i)
	}
	tsum := 0
	for i := 0; i < 200000; i++ {
		tsum += table["op"+strconv.Itoa(i%16)](i % 50)
	}
	t4 := time.Now()
	fmt.Println("table:", tsum, t4.Sub(t3).Seconds())

	deep := makeOuter(11)
	dsum := 0
	for i := 0; i < 200000; i++ {
		dsum += deep(i % 10)
	}
	t5 := time.Now()
	fmt.Println("transitive:", dsum, t5.Sub(t4).Seconds())
	fmt.Println("total:", t5.Sub(t0).Seconds())
}
