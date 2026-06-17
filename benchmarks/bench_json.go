// Go counterpart of bench_json.strada (stdlib encoding/json — native, vs
// Strada's pure-Strada module; see BASELINE.md note).
package main

import (
	"encoding/json"
	"fmt"
	"strconv"
	"time"
)

type Profile struct {
	City  string   `json:"city"`
	Zip   int      `json:"zip"`
	Langs []string `json:"langs"`
}

type User struct {
	ID      int      `json:"id"`
	Name    string   `json:"name"`
	Email   string   `json:"email"`
	Active  int      `json:"active"`
	Score   float64  `json:"score"`
	Tags    []string `json:"tags"`
	Profile Profile  `json:"profile"`
}

type Doc struct {
	Count int    `json:"count"`
	Users []User `json:"users"`
}

func buildDoc(users int) Doc {
	list := make([]User, 0, users)
	for i := 0; i < users; i++ {
		active := 0
		if i%3 == 0 {
			active = 1
		}
		list = append(list, User{
			ID: i, Name: "user_" + strconv.Itoa(i),
			Email:  fmt.Sprintf("user%d@example.com", i),
			Active: active, Score: float64(i) * 1.5,
			Tags:    []string{"alpha", "beta", "tag" + strconv.Itoa(i%50)},
			Profile: Profile{City: "city" + strconv.Itoa(i%100), Zip: 10000 + (i % 90000), Langs: []string{"en", "de"}},
		})
	}
	return Doc{Count: users, Users: list}
}

type Small struct {
	Op   string `json:"op"`
	ID   int    `json:"id"`
	Args []int  `json:"args"`
}

func main() {
	doc := buildDoc(2000)
	t0 := time.Now()
	encLen := 0
	var jsonBytes []byte
	for r := 0; r < 20; r++ {
		jsonBytes, _ = json.Marshal(doc)
		encLen += len(jsonBytes)
	}
	t1 := time.Now()
	fmt.Println("encode:", encLen, t1.Sub(t0).Seconds())

	decUsers := 0
	for r := 0; r < 20; r++ {
		var d Doc
		json.Unmarshal(jsonBytes, &d)
		decUsers += d.Count
	}
	t2 := time.Now()
	fmt.Println("decode:", decUsers, t2.Sub(t1).Seconds())

	rt := 0
	for i := 0; i < 20000; i++ {
		small, _ := json.Marshal(Small{Op: "get", ID: i, Args: []int{1, 2, 3}})
		var s Small
		json.Unmarshal(small, &s)
		rt += s.ID % 2
	}
	t3 := time.Now()
	fmt.Println("roundtrip:", rt, t3.Sub(t2).Seconds())
	fmt.Println("total:", t3.Sub(t0).Seconds())
}
