// Go counterpart of bench_utf8.strada. Go strings are UTF-8 bytes, so
// byte lengths are len(s) directly. Case folding is Unicode-aware
// (strings.ToUpper/ToLower); validation uses utf8.ValidString. Go's
// stdlib has no Unicode normalization (that lives in golang.org/x/text),
// so the NFC step is approximated by a rune-rebuild of comparable cost
// to keep the workload dependency-free.
package main

import (
	"fmt"
	"strings"
	"time"
	"unicode/utf8"
)

func main() {
	corpus := make([]string, 0, 200000)
	for i := 0; i < 200000; i++ {
		corpus = append(corpus, fmt.Sprintf("Müller-Straße %d — café №%d übergröße", i, i%100))
	}

	t0 := time.Now()
	caseLen := 0
	for _, s := range corpus {
		u := strings.ToUpper(s)
		l := strings.ToLower(u)
		caseLen += len(l)
	}
	t1 := time.Now()
	fmt.Println("case:", caseLen, t1.Sub(t0).Seconds())

	valid := 0
	for _, s := range corpus {
		if utf8.ValidString(s) {
			valid++
		}
	}
	t2 := time.Now()
	fmt.Println("valid:", valid, t2.Sub(t1).Seconds())

	built := 0
	for i := 0; i < 50000; i++ {
		var sb strings.Builder
		for j := 0; j < 20; j++ {
			sb.WriteRune(rune(0xE9 + (j % 16)))
			sb.WriteRune(rune(0x4E00 + (j % 64)))
		}
		built += sb.Len()
	}
	t3 := time.Now()
	fmt.Println("chr-build:", built, t3.Sub(t2).Seconds())

	var acc strings.Builder
	for i := 0; i < 30000; i++ {
		acc.WriteString("ü")
		acc.WriteString(fmt.Sprintf("%d", i))
		acc.WriteString("ß—")
	}
	t4 := time.Now()
	fmt.Println("concat:", acc.Len(), t4.Sub(t3).Seconds())

	decomposed := "Café resumé naivë"
	nfcLen := 0
	for i := 0; i < 50000; i++ {
		nfcLen += len(string([]rune(decomposed)))
	}
	t5 := time.Now()
	fmt.Println("nfc:", nfcLen, t5.Sub(t4).Seconds())
	fmt.Println("total:", t5.Sub(t0).Seconds())
}
