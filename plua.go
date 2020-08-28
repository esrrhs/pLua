package main

import (
	"bytes"
	"encoding/binary"
	"flag"
	"fmt"
	"github.com/goccy/go-graphviz"
	"io/ioutil"
	"os"
	"sort"
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
	text := flag.Bool("text", false, "show text result")
	dot := flag.Bool("dot", false, "show dot result")
	png := flag.String("png", "", "gen png file")
	svg := flag.String("svg", "", "gen svg file")
	skip := flag.String("skip", "", "skip func")

	flag.Parse()

	if len(*input) == 0 {
		flag.Usage()
		os.Exit(1)
	}

	filedata, ok := parse(*input, *skip)
	if !ok {
		os.Exit(1)
	}

	if *text {
		showtext(filedata)
	}

	if *dot {
		showdot(filedata)
	}

	if *png != "" || *svg != "" {
		showpng(filedata, *png, *svg)
	}
}

func parse(filename string, skip string) (*FileData, bool) {

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

		if id2str[stacks[deps-1]] == skip {
			continue
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

func escape_dot_name(name string) string {
	return strings.Replace(name, "\"", "'", -1)
}

func gendot(filedata *FileData) string {

	ret := ""

	totalself := 0
	funcmapself := make(map[int]int)
	for _, cs := range filedata.callstack {
		funcmapself[cs.stacks[cs.deps-1]] += cs.count
		totalself += cs.count
	}

	total := 0
	funcmap := make(map[int]int)
	for _, cs := range filedata.callstack {
		for i := 0; i < cs.deps; i++ {
			funcmap[cs.stacks[i]] += cs.count
		}
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

	type pair struct {
		first  int
		second int
	}
	hassonset := make(map[int]int)
	funccallset := make(map[pair]int)
	for _, cs := range filedata.callstack {
		for i := 0; i < cs.deps-1; i++ {
			funccallset[pair{cs.stacks[i], cs.stacks[i+1]}]++
			hassonset[cs.stacks[i]]++
		}
	}

	ret += fmt.Sprintf("digraph G {\n")

	for _, ar := range funcarr {
		ret += fmt.Sprintf("\tnode%v [label=\"%v\\r%v (%v%%)\\r",
			ar.id, escape_dot_name(filedata.id2str[ar.id]), funcmapself[ar.id], funcmapself[ar.id]*100/totalself)

		_, ok := hassonset[ar.id]
		if ok {
			ret += fmt.Sprintf("%v (%v%%)\\r", ar.count, ar.count*100/total)
		}

		fontsize := funcmapself[ar.id] * 100 / totalself
		if fontsize < 10 {
			fontsize = 10
		}

		ret += fmt.Sprintf("\";")
		ret += fmt.Sprintf("fontsize=%v", fontsize)
		ret += fmt.Sprintf(";shape=box;")
		ret += fmt.Sprintf("];\n")
	}

	for k, _ := range funccallset {
		linewidth := float64(funcmap[k.second]) * 3.0 / float64(total)
		if linewidth < 0.5 {
			linewidth = 0.5
		}
		ret += fmt.Sprintf("\tnode%v->node%v [style=\"setlinewidth(%v)\" label=%v];\n", k.first, k.second, linewidth, funcmap[k.second])
	}

	ret += fmt.Sprintf("}\n")

	return ret
}

func showdot(filedata *FileData) {
	fmt.Print(gendot(filedata))
}

func showpng(filedata *FileData, png string, svg string) {
	dot := gendot(filedata)

	graph, err := graphviz.ParseBytes([]byte(dot))
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}

	if png != "" {
		g := graphviz.New()

		var buf bytes.Buffer
		if err := g.Render(graph, graphviz.PNG, &buf); err != nil {
			fmt.Println(err)
			os.Exit(1)
		}

		_, err = g.RenderImage(graph)
		if err != nil {
			fmt.Println(err)
			os.Exit(1)
		}

		if err := g.RenderFilename(graph, graphviz.PNG, png); err != nil {
			fmt.Println(err)
			os.Exit(1)
		}
	}

	if svg != "" {
		g := graphviz.New()

		var buf bytes.Buffer
		if err := g.Render(graph, graphviz.SVG, &buf); err != nil {
			fmt.Println(err)
			os.Exit(1)
		}

		_, err = g.RenderImage(graph)
		if err != nil {
			fmt.Println(err)
			os.Exit(1)
		}

		if err := g.RenderFilename(graph, graphviz.SVG, svg); err != nil {
			fmt.Println(err)
			os.Exit(1)
		}
	}
}
