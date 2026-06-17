// Go counterpart of bench_regex.strada (identical workload). Go's regexp
// is RE2; named captures use (?P<name>...) and tr/// is emulated by a
// rune scan, the closest stdlib equivalent.
package main

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"
	"time"
)

func main() {
	lines := make([]string, 0, 200000)
	for i := 0; i < 200000; i++ {
		code := 200 + ((i*7)%4)*100
		bytes := (i * 37) % 100000
		lines = append(lines, fmt.Sprintf("10.0.%d.%d - - [12/Jun/2026:01:0%d:00] \"GET /page/%d HTTP/1.1\" %d %d",
			(i/256)%256, i%256, i%10, i%1000, code, bytes))
	}

	t0 := time.Now()
	hits := 0
	reMatch := regexp.MustCompile(`" (4|5)\d\d `)
	for _, l := range lines {
		if reMatch.MatchString(l) {
			hits++
		}
	}
	t1 := time.Now()
	fmt.Println("match:", hits, t1.Sub(t0).Seconds())

	bytesTotal := 0
	reCap := regexp.MustCompile(`^(\S+) .* "(\w+) [^"]*" (\d+) (\d+)$`)
	for _, l := range lines {
		m := reCap.FindStringSubmatch(l)
		if m != nil {
			v, _ := strconv.Atoi(m[4])
			bytesTotal += v
		}
	}
	t2 := time.Now()
	fmt.Println("captures:", bytesTotal, t2.Sub(t1).Seconds())

	codeSum := 0
	reNamed := regexp.MustCompile(`" (?P<code>\d+) (?P<bytes>\d+)$`)
	ci := reNamed.SubexpIndex("code")
	for i := 0; i < 100000; i++ {
		m := reNamed.FindStringSubmatch(lines[i])
		if m != nil {
			v, _ := strconv.Atoi(m[ci])
			codeSum += v
		}
	}
	t3 := time.Now()
	fmt.Println("named:", codeSum, t3.Sub(t2).Seconds())

	subLen := 0
	reDigits := regexp.MustCompile(`\d+`)
	for i := 0; i < 100000; i++ {
		subLen += len(reDigits.ReplaceAllString(lines[i], "N"))
	}
	t4 := time.Now()
	fmt.Println("subst:", subLen, t4.Sub(t3).Seconds())

	subeLen := 0
	for i := 0; i < 50000; i++ {
		r := reDigits.ReplaceAllStringFunc(lines[i], func(m string) string {
			return strconv.Itoa(len(m))
		})
		subeLen += len(r)
	}
	t5 := time.Now()
	fmt.Println("subst-e:", subeLen, t5.Sub(t4).Seconds())

	trCount := 0
	for i := 0; i < 100000; i++ {
		c := 0
		for _, ch := range lines[i] {
			if ch >= 'a' && ch <= 'z' {
				c++
			}
		}
		_ = strings.ToUpper(lines[i])
		trCount += c
	}
	t6 := time.Now()
	fmt.Println("tr:", trCount, t6.Sub(t5).Seconds())
	fmt.Println("total:", t6.Sub(t0).Seconds())
}
