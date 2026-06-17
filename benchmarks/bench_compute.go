// Go counterpart of bench_compute.strada (identical workload).
package main

import "fmt"

func fib(n int) int {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

func main() {
	var s int64 = 0
	for i := int64(1); i <= 50000000; i++ {
		s += i
	}
	fmt.Printf("sum: %d\n", s)

	fibResult := 0
	for j := 0; j < 30; j++ {
		fibResult = fib(35)
	}
	fmt.Printf("fib(35): %d\n", fibResult)
}
