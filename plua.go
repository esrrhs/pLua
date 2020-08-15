package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"io/ioutil"
	"os"
	"sort"
)

const (
	MAX_FUNC_NAME_SIZE = 127
	MAX_STACK_SIZE     = 64
)

type CallStack struct {
	count  int
	deps   int
	stacks []int
}

type FileData struct {
	str2id    map[string]int
	id2str    map[int]string
	callstack []CallStack
}

func main() {

	input := flag.String("i", "", "input file")
	text := flag.Bool("text", false, "show text result")

	flag.Parse()

	if len(*input) == 0 {
		flag.Usage()
		os.Exit(1)
	}

	filedata, ok := parse(*input)
	if !ok {
		os.Exit(1)
	}

	if *text {
		showtext(filedata)
	}

}

func parse(filename string) (*FileData, bool) {

	data, err := ioutil.ReadFile(filename)
	if err != nil {
		fmt.Printf("ReadFile fail %v\n", err)
		return nil, false
	}

	if len(data) < 4 {
		fmt.Printf("data error too small\n")
		return nil, false
	}

	namemaplen := int(binary.LittleEndian.Uint32(data[len(data)-4 : len(data)]))
	if namemaplen < 0 {
		fmt.Printf("name map len fail %v\n", namemaplen)
		return nil, false
	}

	str2id := make(map[string]int)
	id2str := make(map[int]string)

	namenum := 0
	end := len(data) - 4
	for i := 0; i < len(data) && namenum < namemaplen; i++ {
		start := end - 4
		if start < 0 || end < 0 {
			fmt.Printf("get id fail %v %v\n", start, end)
			return nil, false
		}
		id := int(binary.LittleEndian.Uint32(data[start:end]))
		end -= 4

		start = end - 4
		if start < 0 || end < 0 {
			fmt.Printf("get name len fail %v %v\n", start, end)
			return nil, false
		}
		namelen := int(binary.LittleEndian.Uint32(data[start:end]))
		end -= 4

		if namelen <= 0 || namelen > MAX_FUNC_NAME_SIZE {
			fmt.Printf("name len error %v\n", namelen)
			return nil, false
		}

		start = end - namelen
		str := string(data[start:end])
		end -= namelen

		str2id[str] = id
		id2str[id] = str
		namenum++
	}

	callstack := make([]CallStack, 0)
	for end > 0 {
		stacks := make([]int, MAX_STACK_SIZE)
		for i := 0; i < MAX_STACK_SIZE; i++ {
			start := end - 4
			stack := int(binary.LittleEndian.Uint32(data[start:end]))
			end -= 4
			stacks[MAX_STACK_SIZE-i-1] = stack
		}

		start := end - 4
		deps := int(binary.LittleEndian.Uint32(data[start:end]))
		end -= 4

		if deps <= 0 || deps > MAX_STACK_SIZE {
			fmt.Printf("deps error %v\n", deps)
			return nil, false
		}

		start = end - 4
		count := int(binary.LittleEndian.Uint32(data[start:end]))
		end -= 4

		if count <= 0 {
			fmt.Printf("count error %v\n", count)
			return nil, false
		}

		cs := CallStack{
			count:  count,
			deps:   deps,
			stacks: stacks[0:deps],
		}

		callstack = append(callstack, cs)
	}

	filedata := &FileData{
		str2id:    str2id,
		id2str:    id2str,
		callstack: callstack,
	}

	return filedata, true
}

func showtext(filedata *FileData) {

	total := 0
	funcmap := make(map[int]int)
	for _, cs := range filedata.callstack {
		funcmap[cs.stacks[cs.deps-1]] += cs.count
		total += cs.count
	}

	type arr struct {
		id    int
		count int
	}
	funcarr := make([]arr, 0)
	for k, v := range funcmap {
		funcarr = append(funcarr, arr{k, v})
	}

	sort.Slice(funcarr, func(i, j int) bool {
		return funcarr[j].count < funcarr[i].count
	})

	for _, ar := range funcarr {
		fmt.Printf("%v%%\t%v\n", ar.count*100/total, filedata.id2str[ar.id])
	}
}
