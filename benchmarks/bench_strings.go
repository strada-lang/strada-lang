// Go counterpart of bench_strings.strada. The accumulating concat uses
// strings.Builder (the idiomatic Go equivalent of JS cons-string concat);
// the regex passes use the stdlib regexp package, first-match only to
// mirror JS's non-global .replace().
package main

import (
	"fmt"
	"regexp"
	"strings"
)

func replaceFirst(re *regexp.Regexp, s, repl string) string {
	loc := re.FindStringIndex(s)
	if loc == nil {
		return s
	}
	return s[:loc[0]] + repl + s[loc[1]:]
}

func main() {
	var b strings.Builder
	for i := 0; i < 500000; i++ {
		b.WriteString("hello")
	}
	s := b.String()
	fmt.Printf("concat len: %d\n", len(s))

	csv := "alpha,bravo,charlie,delta,echo,foxtrot,golf,hotel"
	totalParts := 0
	for j := 0; j < 100000; j++ {
		parts := strings.Split(csv, ",")
		totalParts += len(parts)
	}
	fmt.Printf("split parts: %d\n", totalParts)

	template := "Hello NAME, welcome to PLACE on DATE"
	patName := regexp.MustCompile("NAME")
	patPlace := regexp.MustCompile("PLACE")
	patDate := regexp.MustCompile("DATE")
	result := ""
	for m := 0; m < 200000; m++ {
		result = template
		result = replaceFirst(patName, result, "World")
		result = replaceFirst(patPlace, result, "Strada")
		result = replaceFirst(patDate, result, "today")
	}
	fmt.Printf("regex result: %s\n", result)
}
