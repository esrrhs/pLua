package main

import (
	"bytes"
	"compress/gzip"
	"encoding/binary"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func fixture(version, backend uint32) []byte {
	var b bytes.Buffer
	put := func(v uint32) {
		if err := binary.Write(&b, binary.LittleEndian, v); err != nil {
			panic(err)
		}
	}
	if version > 0 {
		put(proMagic)
		put(version)
	}
	if version == 2 {
		put(1)
		put(backend)
		put(10)
		put(1000)
	}
	put(7)
	put(2)
	for i := 0; i < 64; i++ {
		if i == 0 {
			put(10)
		} else if i == 1 {
			put(20)
		} else {
			put(0)
		}
	}
	for _, e := range []struct {
		id   uint32
		name string
	}{{10, "root"}, {20, "hot_leaf"}} {
		b.WriteString(e.name)
		put(uint32(len(e.name)))
		put(e.id)
	}
	put(2)
	return b.Bytes()
}

// Independent tiny wire reader: tests field identities, reference consistency,
// stack orientation and units instead of comparing an encoder to itself.
type field struct {
	num int
	v   uint64
	b   []byte
}

func fields(t *testing.T, data []byte) []field {
	t.Helper()
	var out []field
	for len(data) > 0 {
		key, n := binary.Uvarint(data)
		if n <= 0 {
			t.Fatal("bad key")
		}
		data = data[n:]
		f := field{num: int(key >> 3)}
		v, n := binary.Uvarint(data)
		if n <= 0 {
			t.Fatal("bad value/length")
		}
		data = data[n:]
		switch key & 7 {
		case 0:
			f.v = v
		case 2:
			if v > uint64(len(data)) {
				t.Fatal("bad length")
			}
			f.b = data[:int(v)]
			data = data[int(v):]
		default:
			t.Fatalf("unexpected wire %d", key&7)
		}
		out = append(out, f)
	}
	return out
}
func values(fs []field, num int) []uint64 {
	var vs []uint64
	for _, f := range fs {
		if f.num == num {
			vs = append(vs, f.v)
		}
	}
	return vs
}
func TestProtoSemantics(t *testing.T) {
	for _, backend := range []uint32{1, 2} {
		t.Run(map[uint32]string{1: "cpu", 2: "count"}[backend], func(t *testing.T) {
			f, ok := parseData(fixture(2, backend))
			if !ok {
				t.Fatal("parse")
			}
			data, err := profileProto(f)
			if err != nil {
				t.Fatal(err)
			}
			fs := fields(t, data)
			var st []string
			var samples, types [][]field
			functions := map[uint64]uint64{}
			locations := map[uint64]uint64{}
			var period []field
			for _, v := range fs {
				switch v.num {
				case 6:
					st = append(st, string(v.b))
				case 1:
					types = append(types, fields(t, v.b))
				case 2:
					samples = append(samples, fields(t, v.b))
				case 4:
					ls := fields(t, v.b)
					var id uint64
					for _, l := range ls {
						if l.num == 1 {
							id = l.v
						}
					}
					for _, l := range ls {
						if l.num == 4 {
							locations[id] = values(fields(t, l.b), 1)[0]
						}
					}
				case 5:
					fn := fields(t, v.b)
					functions[values(fn, 1)[0]] = values(fn, 2)[0]
				case 11:
					period = fields(t, v.b)
				}
			}
			if len(st) == 0 || st[0] != "" {
				t.Fatal("string index zero")
			}
			if len(samples) != 1 {
				t.Fatal("samples")
			}
			ids := values(samples[0], 1)
			weights := values(samples[0], 2)
			if len(ids) != 2 || ids[0] == 0 || ids[1] == 0 || st[functions[locations[ids[0]]]] != "hot_leaf" || st[functions[locations[ids[1]]]] != "root" {
				t.Fatal("leaf-first/ID references")
			}
			if weights[0] != 7 {
				t.Fatal(weights)
			}
			if st[values(types[0], 1)[0]] != "samples" || st[values(types[0], 2)[0]] != "count" {
				t.Fatal("sample type")
			}
			if backend == 1 {
				if len(weights) != 2 || weights[1] != 70000000 || len(types) != 2 || values(fs, 12)[0] != 10000000 {
					t.Fatal("CPU weights/period", weights)
				}
				if st[values(types[1], 1)[0]] != "cpu" || st[values(types[1], 2)[0]] != "nanoseconds" {
					t.Fatal("CPU type")
				}
			} else {
				if len(weights) != 1 || len(types) != 1 || values(fs, 12)[0] != 1000 || st[values(period, 1)[0]] != "instructions" || st[values(period, 2)[0]] != "count" {
					t.Fatal("count must not be CPU ns")
				}
			}
		})
	}
}
func TestParsingAndErrors(t *testing.T) {
	for _, v := range []uint32{0, 1, 2} {
		if _, ok := parseData(fixture(v, 1)); !ok {
			t.Fatalf("v%d", v)
		}
	}
	valid := fixture(2, 1)
	for i := 0; i < len(valid); i++ {
		if _, ok := parseData(valid[:i]); ok {
			t.Fatalf("accepted truncated offset %d", i)
		}
	}
	cases := map[string][]byte{}
	mutation := func(name string, offset int, v uint32) {
		d := append([]byte(nil), valid...)
		binary.LittleEndian.PutUint32(d[offset:], v)
		cases[name] = d
	}
	mutation("version", 4, 99)
	mutation("kind", 8, 2)
	mutation("backend", 12, 3)
	mutation("zero interval", 16, 0)
	mutation("zero step", 20, 0)
	mutation("depth", 28, 65)
	mutation("unknown frame", 32, 999)
	mutation("zero count", 24, 0)
	mutation("negative count", 24, 0xffffffff)
	mutation("name count", len(valid)-4, 0xffffffff)
	mutation("name length", len(valid)-12, 128)
	mutation("duplicate ID", len(valid)-8, 10)
	for name, d := range cases {
		t.Run(name, func(t *testing.T) {
			if _, ok := parseData(d); ok {
				t.Fatal("accepted malformed input")
			}
		})
	}
	for _, v := range []uint32{0, 1} {
		f, _ := parseData(fixture(v, 1))
		if _, err := profileProto(f); err == nil {
			t.Fatal("legacy/memory must require metadata")
		}
	}
	f, _ := parseData(valid)
	f.intervalMs = 0xffffffff
	f.callstack[0].count = 0x7fffffff
	if _, err := profileProto(f); err == nil {
		t.Fatal("overflow")
	}
}
func TestModernPprof(t *testing.T) {
	goexe, err := exec.LookPath("go")
	if err != nil {
		t.Skip("go tool unavailable")
	}
	for _, backend := range []uint32{1, 2} {
		f, _ := parseData(fixture(2, backend))
		path := filepath.Join(t.TempDir(), "cpu.pb.gz")
		if err := showproto(f, path); err != nil {
			t.Fatal(err)
		}
		file, err := os.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		z, err := gzip.NewReader(file)
		if err != nil {
			t.Fatal(err)
		}
		data, err := io.ReadAll(z)
		if err != nil {
			t.Fatal(err)
		}
		z.Close()
		file.Close()
		if len(fields(t, data)) == 0 {
			t.Fatal("empty")
		}
		output, err := exec.Command(goexe, "tool", "pprof", "-top", path).CombinedOutput()
		if err != nil {
			t.Fatalf("go tool pprof: %s %v", output, err)
		}
		if !strings.Contains(string(output), "hot_leaf") || !strings.Contains(string(output), "100%") {
			t.Fatalf("bad top: %s", output)
		}
		if backend == 1 && !strings.Contains(string(output), "70ms") {
			t.Fatalf("CPU units: %s", output)
		}
		if backend == 2 && !strings.Contains(string(output), "Type: samples") {
			t.Fatalf("count units: %s", output)
		}
		t.Logf("backend %d: %s", backend, output)
	}
}
func TestLegacyAndFlameStillWork(t *testing.T) {
	for _, v := range []uint32{0, 1, 2} {
		f, ok := parseData(fixture(v, 1))
		if !ok {
			t.Fatal("parse")
		}
		dir := t.TempDir()
		showcollapsed(f, filepath.Join(dir, "stacks.fl"))
		showpprof(f, filepath.Join(dir, "legacy.prof"))
		d, _ := os.ReadFile(filepath.Join(dir, "stacks.fl"))
		if string(d) != "root;hot_leaf 7\n" {
			t.Fatalf("flame: %q", d)
		}
		d, _ = os.ReadFile(filepath.Join(dir, "legacy.prof"))
		if !bytes.HasPrefix(d, []byte("--- symbol\n")) {
			t.Fatal("legacy output")
		}
	}
}
