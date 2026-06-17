// Go counterpart of bench_exceptions.strada. Go has no exceptions; the
// throw/catch machinery is mirrored with panic/recover (its nearest
// equivalent), and finally with defer.
package main

import (
	"errors"
	"fmt"
	"time"
)

type MiscErr struct{}

func (MiscErr) Error() string { return "misc" }

type WantedErr struct{}

func (WantedErr) Error() string { return "wanted" }

func tryNothrow(ok *int) {
	defer func() {
		if r := recover(); r != nil {
			*ok--
		}
	}()
	*ok++
}

func throwCatch(caught *int) {
	defer func() {
		if r := recover(); r != nil {
			*caught++
		}
	}()
	panic(errors.New("boom"))
}

func typedCatch(typed *int) {
	defer func() {
		if r := recover(); r != nil {
			switch r.(type) {
			case MiscErr:
				*typed--
			case WantedErr:
				*typed++
			default:
				*typed--
			}
		}
	}()
	panic(WantedErr{})
}

func deep(n int) int {
	if n <= 0 {
		panic(errors.New("bottom"))
	}
	return deep(n-1) + 1
}

func deepUnwind(c *int) {
	defer func() {
		if r := recover(); r != nil {
			*c++
		}
	}()
	deep(50)
}

func main() {
	t0 := time.Now()
	ok := 0
	for i := 0; i < 2000000; i++ {
		tryNothrow(&ok)
	}
	t1 := time.Now()
	fmt.Println("try-nothrow:", ok, t1.Sub(t0).Seconds())

	caught := 0
	for i := 0; i < 200000; i++ {
		throwCatch(&caught)
	}
	t2 := time.Now()
	fmt.Println("throw-catch:", caught, t2.Sub(t1).Seconds())

	typed := 0
	for i := 0; i < 200000; i++ {
		typedCatch(&typed)
	}
	t3 := time.Now()
	fmt.Println("typed:", typed, t3.Sub(t2).Seconds())

	deepCaught := 0
	for i := 0; i < 20000; i++ {
		deepUnwind(&deepCaught)
	}
	t4 := time.Now()
	fmt.Println("deep-unwind:", deepCaught, t4.Sub(t3).Seconds())

	fin := 0
	for i := 0; i < 1000000; i++ {
		func() {
			defer func() { fin++ }()
			fin++
		}()
	}
	t5 := time.Now()
	fmt.Println("finally:", fin, t5.Sub(t4).Seconds())
	fmt.Println("total:", t5.Sub(t0).Seconds())
}
