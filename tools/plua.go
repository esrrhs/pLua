package main

import (
	"bytes"
	"compress/gzip"
	"encoding/binary"
	"flag"
	"fmt"
	"math"
	"os"
	"sort"
	"strings"
)

const (
	MAX_FUNC_NAME_SIZE = 127
	MAX_STACK_SIZE     = 64
	proMagic           = 0x41554C50 // 'PLUA' little-endian
)

type CallStack struct {
	count  int
	deps   int
	stacks []int
}

type FileData struct {
	version                              uint32
	kind, backend, intervalMs, hookCount uint32
	str2id                               map[string]int
	id2str                               map[int]string
	callstack                            []CallStack
}

func main() {
	input := flag.String("i", "", "input file (.pro)")
	proto := flag.String("proto", "", "gzip profile.proto CPU output (modern go tool pprof)")
	pprof := flag.String("pprof", "", "gen pprof symbolized-profiles")
	flame := flag.String("flame", "", "gen flamegraph collapsed stacks file")
	collapsed := flag.String("collapsed", "", "alias for -flame")

	flag.Parse()

	if len(*input) == 0 {
		flag.Usage()
		os.Exit(1)
	}

	filedata, ok := parse(*input)
	if !ok {
		os.Exit(1)
	}

	if *proto != "" {
		if err := showproto(filedata, *proto); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	}
	if *pprof != "" {
		showpprof(filedata, *pprof)
	}

	flameOut := *flame
	if flameOut == "" {
		flameOut = *collapsed
	}
	if flameOut != "" {
		showcollapsed(filedata, flameOut)
	}
}

func parse(filename string) (*FileData, bool) {
	data, err := os.ReadFile(filename)
	if err != nil {
		fmt.Printf("ReadFile fail %v\n", err)
		return nil, false
	}

	return parseData(data)
}

func parseData(data []byte) (*FileData, bool) {
	if len(data) < 4 {
		fmt.Printf("data error too small\n")
		return nil, false
	}

	headerSize := 0
	version := uint32(0)
	var kind, backend, intervalMs, hookCount uint32
	if len(data) >= 8 {
		magic := binary.LittleEndian.Uint32(data[0:4])
		if magic == proMagic {
			version = binary.LittleEndian.Uint32(data[4:8])
			if version < 1 || version > 2 {
				fmt.Printf("unsupported .pro version %v\n", version)
				return nil, false
			}
			headerSize = 8
			if version == 2 {
				headerSize = 24
				if len(data) < headerSize+4 {
					return nil, false
				}
				kind = binary.LittleEndian.Uint32(data[8:12])
				backend = binary.LittleEndian.Uint32(data[12:16])
				intervalMs = binary.LittleEndian.Uint32(data[16:20])
				hookCount = binary.LittleEndian.Uint32(data[20:24])
				if kind != 1 || (backend != 1 && backend != 2) || intervalMs == 0 || hookCount == 0 {
					return nil, false
				}
			}
		}
	}

	namemaplen := int(binary.LittleEndian.Uint32(data[len(data)-4 : len(data)]))
	if len(data) < headerSize+4 || namemaplen > (len(data)-headerSize-4)/9 {
		fmt.Printf("name map len fail %v\n", namemaplen)
		return nil, false
	}

	str2id := make(map[string]int)
	id2str := make(map[int]string)

	namenum := 0
	end := len(data) - 4
	for i := 0; i < len(data) && namenum < namemaplen; i++ {
		start := end - 4
		if start < headerSize || end < headerSize {
			fmt.Printf("get id fail %v %v\n", start, end)
			return nil, false
		}
		id := int(binary.LittleEndian.Uint32(data[start:end]))
		end -= 4

		start = end - 4
		if start < headerSize || end < headerSize {
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
		if start < headerSize {
			fmt.Printf("name out of range\n")
			return nil, false
		}
		str := string(data[start:end])
		end -= namelen

		if _, exists := id2str[id]; exists {
			return nil, false
		}
		str2id[str] = id
		id2str[id] = str

		namenum++
	}

	if (end-headerSize)%((MAX_STACK_SIZE+2)*4) != 0 {
		return nil, false
	}
	callstack := make([]CallStack, 0)
	for end > headerSize {
		stacks := make([]int, MAX_STACK_SIZE)
		for i := 0; i < MAX_STACK_SIZE; i++ {
			start := end - 4
			if start < headerSize {
				fmt.Printf("stack underflow\n")
				return nil, false
			}
			stack := int(binary.LittleEndian.Uint32(data[start:end]))
			end -= 4
			stacks[MAX_STACK_SIZE-i-1] = stack
		}

		start := end - 4
		if start < headerSize {
			fmt.Printf("deps underflow\n")
			return nil, false
		}
		deps := int(binary.LittleEndian.Uint32(data[start:end]))
		end -= 4

		if deps <= 0 || deps > MAX_STACK_SIZE {
			fmt.Printf("deps error %v\n", deps)
			return nil, false
		}

		start = end - 4
		if start < headerSize {
			fmt.Printf("count underflow\n")
			return nil, false
		}
		count := int(binary.LittleEndian.Uint32(data[start:end]))
		end -= 4

		if count <= 0 || (version == 2 && count > math.MaxInt32) {
			fmt.Printf("count error %v\n", count)
			return nil, false
		}

		for _, id := range stacks[:deps] {
			if _, ok := id2str[id]; !ok {
				return nil, false
			}
		}
		cs := CallStack{
			count:  count,
			deps:   deps,
			stacks: stacks[0:deps],
		}

		callstack = append(callstack, cs)
	}

	filedata := &FileData{
		version: version,
		kind:    kind, backend: backend, intervalMs: intervalMs, hookCount: hookCount,
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

func showcollapsed(filedata *FileData, filename string) {
	var b strings.Builder
	total := 0
	for _, cs := range filedata.callstack {
		var names []string
		for i := 0; i < len(cs.stacks); i++ {
			name, ok := filedata.id2str[cs.stacks[i]]
			if !ok {
				name = fmt.Sprintf("0x%x", cs.stacks[i])
			}
			name = strings.ReplaceAll(name, ";", ":")
			name = strings.ReplaceAll(name, "\r", "")
			name = strings.ReplaceAll(name, "\n", " ")
			name = strings.ToValidUTF8(name, "?")
			names = append(names, name)
		}
		b.WriteString(strings.Join(names, ";"))
		b.WriteString(fmt.Sprintf(" %d\n", cs.count))
		total += cs.count
	}

	err := os.WriteFile(filename, []byte(b.String()), 0644)
	if err != nil {
		fmt.Printf("write flame collapsed file fail: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("total collapsed sample %v\n", total)
}

// Minimal profile.proto wire encoder. Only varint and length-delimited fields
// are needed; no generated runtime or external dependencies. Field numbers are
// from github.com/google/pprof/proto/profile.proto.
func pv(dst []byte, field int, v uint64) []byte {
	dst = appendVarint(dst, uint64(field<<3))
	return appendVarint(dst, v)
}
func pb(dst []byte, field int, v []byte) []byte {
	dst = appendVarint(dst, uint64(field<<3|2))
	dst = appendVarint(dst, uint64(len(v)))
	return append(dst, v...)
}

func profileProto(f *FileData) ([]byte, error) {
	// v0/v1 do not identify CPU vs memory or carry a sampling period. Guessing
	// from filenames would silently turn bytes/instructions into CPU time.
	if f.version != 2 || f.kind != 1 {
		return nil, fmt.Errorf("-proto requires a v2 CPU .pro; legacy/v1 and memory profiles lack CPU metadata (use -flame or -pprof)")
	}
	stringsTable := []string{""}
	stringsIndex := map[string]uint64{"": 0}
	intern := func(s string) uint64 {
		s = strings.ToValidUTF8(s, "?")
		if i, ok := stringsIndex[s]; ok {
			return i
		}
		i := uint64(len(stringsTable))
		stringsIndex[s] = i
		stringsTable = append(stringsTable, s)
		return i
	}
	valueType := func(name, unit string) []byte { return pv(pv(nil, 1, intern(name)), 2, intern(unit)) }
	var out []byte
	out = pb(out, 1, valueType("samples", "count"))
	if f.backend == 1 {
		out = pb(out, 1, valueType("cpu", "nanoseconds"))
		out = pb(out, 11, valueType("cpu", "nanoseconds"))
		out = pv(out, 12, uint64(f.intervalMs)*1000000)
	} else if f.backend == 2 {
		out = pb(out, 11, valueType("instructions", "count"))
		out = pv(out, 12, uint64(f.hookCount))
	} else {
		return nil, fmt.Errorf("unknown CPU backend %d", f.backend)
	}
	ids := make([]int, 0, len(f.id2str))
	for id := range f.id2str {
		ids = append(ids, id)
	}
	sort.Ints(ids)
	locations := make(map[int]uint64)
	for i, id := range ids {
		n := uint64(i + 1)
		locations[id] = n
		function := pv(nil, 1, n)
		function = pv(function, 2, intern(f.id2str[id]))
		out = pb(out, 5, function)
		line := pv(nil, 1, n)
		location := pv(nil, 1, n)
		location = pb(location, 4, line)
		out = pb(out, 4, location)
	}
	for _, cs := range f.callstack {
		if cs.count <= 0 || len(cs.stacks) == 0 {
			return nil, fmt.Errorf("invalid sample")
		}
		var sample []byte
		// .pro is root-first; profile.proto requires leaf-first location IDs.
		for i := len(cs.stacks) - 1; i >= 0; i-- {
			id, ok := locations[cs.stacks[i]]
			if !ok {
				return nil, fmt.Errorf("unknown frame ID")
			}
			sample = pv(sample, 1, id)
		}
		sample = pv(sample, 2, uint64(cs.count))
		if f.backend == 1 {
			period := uint64(f.intervalMs) * 1000000
			if uint64(cs.count) > math.MaxInt64/period {
				return nil, fmt.Errorf("CPU weight overflow")
			}
			sample = pv(sample, 2, uint64(cs.count)*period)
		}
		out = pb(out, 2, sample)
	}
	comment := "pLua: delivered SIGPROF ticks; estimated CPU time; boundary snapshots, no native unwinding"
	if f.backend == 2 {
		comment = "pLua: VM instruction sampling only; C time is not measured; samples are not CPU nanoseconds"
	}
	out = pv(out, 13, intern(comment))
	for _, s := range stringsTable {
		out = pb(out, 6, []byte(s))
	}
	return out, nil
}

func showproto(f *FileData, filename string) error {
	data, err := profileProto(f)
	if err != nil {
		return err
	}
	var buf bytes.Buffer
	z := gzip.NewWriter(&buf)
	if _, err = z.Write(data); err != nil {
		return err
	}
	if err = z.Close(); err != nil {
		return err
	}
	return os.WriteFile(filename, buf.Bytes(), 0644)
}

func appendVarint(dst []byte, v uint64) []byte {
	var b [10]byte
	n := binary.PutUvarint(b[:], v)
	return append(dst, b[:n]...)
}
