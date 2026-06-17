// Go counterpart of bench_functions.strada (identical workload).
package main

import "fmt"

func add3(a, b, c int) int { return a + b + c }

func ackermann(m, n int) int {
	if m == 0 {
		return n + 1
	}
	if n == 0 {
		return ackermann(m-1, 1)
	}
	return ackermann(m-1, ackermann(m, n-1))
}

func main() {
	s := 0
	for i := 0; i < 5000000; i++ {
		s += add3(i, i+1, i+2)
	}
	fmt.Printf("call sum: %d\n", s)
	fmt.Printf("ackermann(3,8): %d\n", ackermann(3, 8))
}
