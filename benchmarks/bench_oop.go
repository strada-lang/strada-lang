// Go counterpart of bench_oop.strada. Go has no class inheritance; the
// Point/Point3D relationship uses struct embedding with a method override,
// and the isa() check uses a runtime type assertion.
package main

import "fmt"

type Point struct{ x, y int }

func (p Point) distanceSq() int { return p.x*p.x + p.y*p.y }

type Point3D struct {
	Point
	z int
}

func (p Point3D) distanceSq() int { return p.x*p.x + p.y*p.y + p.z*p.z }

func main() {
	s := 0
	for i := 0; i < 500000; i++ {
		p := Point{i % 100, (i + 1) % 100}
		s += p.distanceSq()
	}
	fmt.Printf("point sum: %d\n", s)

	s3d := 0
	for j := 0; j < 500000; j++ {
		p := Point3D{Point{j % 100, (j + 1) % 100}, (j + 2) % 100}
		s3d += p.distanceSq()
	}
	fmt.Printf("point3d sum: %d\n", s3d)

	isaCount := 0
	for k := 0; k < 200000; k++ {
		p := Point3D{Point{1, 2}, 3}
		var iface interface{} = p
		if _, ok := iface.(Point3D); ok {
			isaCount++
		}
	}
	fmt.Printf("isa checks: %d\n", isaCount)
}
