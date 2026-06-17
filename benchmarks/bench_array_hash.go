// Go counterpart of bench_array_hash.strada (identical workload).
package main

import (
	"fmt"
	"strconv"
)

func main() {
	arr := make([]int, 0)
	for i := 0; i < 2000000; i++ {
		arr = append(arr, i)
	}
	fmt.Printf("array size: %d\n", len(arr))

	s := 0
	for j := 0; j < 2000000; j += 100 {
		s += arr[j]
	}
	fmt.Printf("array checksum: %d\n", s)

	h := make(map[string]int)
	for k := 0; k < 500000; k++ {
		h["key"+strconv.Itoa(k)] = k
	}
	fmt.Printf("hash size: %d\n", len(h))

	lookupSum := 0
	for m := 0; m < 500000; m++ {
		lookupSum += h["key"+strconv.Itoa(m)]
	}
	fmt.Printf("lookup sum: %d\n", lookupSum)

	for n := 0; n < 500000; n++ {
		delete(h, "key"+strconv.Itoa(n))
	}
	fmt.Printf("after delete: %d\n", len(h))
}
