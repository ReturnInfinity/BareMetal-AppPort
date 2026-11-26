package main

import (
	"fmt"
	"runtime"
	"sync"
	"time"
)

func main() {
	fmt.Println("Hello from Go", runtime.Version(), "on", runtime.GOOS+"/"+runtime.GOARCH)

	var wg sync.WaitGroup
	var mu sync.Mutex
	total := 0
	for i := 0; i < 4; i++ {
		wg.Add(1)
		go func(n int) {
			defer wg.Done()
			for j := 0; j < 1000; j++ {
				mu.Lock()
				total++
				mu.Unlock()
			}
		}(i)
	}
	wg.Wait()
	fmt.Println("goroutines done, total =", total)

	start := time.Now()
	time.Sleep(250 * time.Millisecond)
	fmt.Println("slept", time.Since(start).Round(time.Millisecond))

	ch := make(chan int)
	go func() {
		for i := 0; i < 3; i++ {
			ch <- i
		}
		close(ch)
	}()
	for v := range ch {
		fmt.Println("recv", v)
	}
	fmt.Println("bye")
}
