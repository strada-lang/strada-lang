// Go counterpart of bench_data.strada (identical workload).
package main

import (
	"bufio"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"
)

func main() {
	path := "/tmp/go_bench_data.csv"
	rows := 500000
	t0 := time.Now()

	var seed int64 = 42
	var sb strings.Builder
	for i := 0; i < rows; i++ {
		seed = (seed*1103515245 + 12345) % 2147483648
		sb.WriteString(fmt.Sprintf("2026-06-12T01:00:00,user%d,action%d,%d,ok\n",
			seed%5000, (seed/7)%6, seed%50000))
	}
	os.WriteFile(path, []byte(sb.String()), 0644)
	t1 := time.Now()
	fmt.Println("generate:", rows, t1.Sub(t0).Seconds())

	byUser := make(map[string]int)
	byAction := make(map[string]int)
	bytesByUser := make(map[string]int)
	parsed := 0
	f, _ := os.Open(path)
	scanner := bufio.NewScanner(f)
	scanner.Buffer(make([]byte, 1024*1024), 1024*1024)
	for scanner.Scan() {
		line := scanner.Text()
		fields := strings.Split(line, ",")
		if len(fields) < 5 {
			continue
		}
		byUser[fields[1]]++
		byAction[fields[2]]++
		v, _ := strconv.Atoi(fields[3])
		bytesByUser[fields[1]] += v
		parsed++
	}
	f.Close()
	t2 := time.Now()
	fmt.Println("aggregate:", parsed, "users="+strconv.Itoa(len(byUser)), t2.Sub(t1).Seconds())

	ranked := make([]string, 0, len(bytesByUser))
	for u := range bytesByUser {
		ranked = append(ranked, u)
	}
	sort.Slice(ranked, func(a, b int) bool { return bytesByUser[ranked[a]] > bytesByUser[ranked[b]] })
	var report strings.Builder
	for i := 0; i < 20; i++ {
		u := ranked[i]
		report.WriteString(fmt.Sprintf("%s events=%d bytes=%d\n", u, byUser[u], bytesByUser[u]))
	}
	t3 := time.Now()
	fmt.Println("report:", report.Len(), t3.Sub(t2).Seconds())
	os.Remove(path)
	fmt.Println("total:", t3.Sub(t0).Seconds())
}
