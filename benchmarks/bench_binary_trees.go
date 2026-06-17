// Go counterpart of bench_binary_trees.strada (identical workload).
package main

import (
	"fmt"
	"time"
)

type Node struct {
	l, r *Node
}

func build(d int) *Node {
	if d == 0 {
		return &Node{}
	}
	return &Node{build(d - 1), build(d - 1)}
}

func check(n *Node) int {
	if n.l == nil {
		return 1
	}
	return 1 + check(n.l) + check(n.r)
}

func main() {
	maxDepth := 16
	t0 := time.Now()
	stretch := build(maxDepth + 1)
	sc := check(stretch)
	stretch = nil
	t1 := time.Now()
	fmt.Println("stretch:", sc, t1.Sub(t0).Seconds())

	longLived := build(maxDepth)
	t2 := time.Now()
	fmt.Println("long-lived: built", t2.Sub(t1).Seconds())

	for depth := 4; depth <= 14; depth += 2 {
		iters := 1 << (maxDepth - depth + 2)
		sum := 0
		for i := 0; i < iters; i++ {
			sum += check(build(depth))
		}
		fmt.Printf("depth %d: %d trees, check %d\n", depth, iters, sum)
	}
	t3 := time.Now()
	fmt.Println("iterate:", check(longLived), t3.Sub(t2).Seconds())
	fmt.Println("total:", t3.Sub(t0).Seconds())
}
