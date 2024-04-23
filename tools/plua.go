package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"io/ioutil"
	"math"
	"os"
	"strings"
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
	pprof := flag.String("pprof", "", "gen pprof symbolized-profiles")

	flag.Parse()

	if len(*input) == 0 {
		flag.Usage()
		os.Exit(1)
	}

	filedata, ok := parse(*input)
	if !ok {
		os.Exit(1)
	}

	if *pprof != "" {
		showpprof(filedata, *pprof)
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

func showpprof(filedata *FileData, filename string) {

	var output []byte

	output = append(output, []byte("--- symbol\n")...)
	output = append(output, []byte("binary=pLua\n")...)

	for id, str := range filedata.id2str {
		name := strings.Replace(str, "<", "'", -1)
		name = strings.Replace(name, ">", "'", -1)
		name = strings.Replace(name, "\"", "\\\"", -1)
		name = strings.ToValidUTF8(name, "?")
		tmp := fmt.Sprintf("0x%016x %s\n", id+0xFF000000, name)
		output = append(output, []byte(tmp)...)
	}

	output = append(output, []byte("---\n")...)
	output = append(output, []byte("--- profile\n")...)

	pack32 := func(v uint32) {
		var buff [4]byte
		binary.LittleEndian.PutUint32(buff[:], v)
		output = append(output, buff[:]...)
	}

	// print header (64-bit style)
	// (zero) (header-size) (version) (sample-period) (zero)
	header := []byte{0, 0, 3, 0, 0, 0, 1, 0, 0, 0}
	for _, h := range header {
		pack32(uint32(h))
	}

	total := 0

	for _, cs := range filedata.callstack {
		pack32(uint32(cs.count))
		pack32(uint32(cs.count / int(math.Pow(2, 32))))
		total += cs.count
		pack32(uint32(cs.deps))
		pack32(uint32(cs.deps / int(math.Pow(2, 32))))
		for i := len(cs.stacks) - 1; i >= 0; i-- {
			csp := cs.stacks[i]
			pack32(uint32(csp + 0xFF000000))
			pack32(uint32(0))
		}
	}

	f, err := os.Create(filename)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
	defer f.Close()
	f.Write(output)

	fmt.Printf("total sample %v\n", total)
}
