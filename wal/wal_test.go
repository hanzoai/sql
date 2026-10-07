package main

import "testing"

const seg = 16 << 20

func TestSegmentNames(t *testing.T) {
	for _, c := range []struct {
		name string
		tli  uint32
		lsn  uint64
	}{
		{"000000010000000000000001", 1, 1 * seg},
		{"0000000200000001000000FF", 2, (1<<32 | 0xFF*seg)},
		{"00000003000000AB00000000", 3, 0xAB << 32},
	} {
		tli, start, ok := parseSegment(c.name, seg)
		if !ok || tli != c.tli || start != c.lsn {
			t.Fatalf("%s: got %d %X %v", c.name, tli, start, ok)
		}
		if got := segmentName(c.tli, c.lsn+123, seg); got != c.name {
			t.Fatalf("%X: named %s, want %s", c.lsn, got, c.name)
		}
	}
	for _, bad := range []string{"", "00000001000000000000000", "000000000000000000000001", "000000010000000000000100", "00000001000000000000000Z"} {
		if _, _, ok := parseSegment(bad, seg); ok {
			t.Fatalf("%q parsed", bad)
		}
	}
}

func TestChunkNames(t *testing.T) {
	n := chunkName(0x1_0000_0028, 0x1_0000_2000, "sink_a")
	c, ok := parseChunk(n)
	if !ok || c.start != 0x1_0000_0028 || c.end != 0x1_0000_2000 {
		t.Fatalf("%s: %+v %v", n, c, ok)
	}
	for _, bad := range []string{"x", "0000000000000010-0000000000000010.a", "0000000000000020-0000000000000010.a", "10-20.a"} {
		if _, ok := parseChunk(bad); ok {
			t.Fatalf("%q parsed", bad)
		}
	}
}

func TestHistoryOwner(t *testing.T) {
	h, err := parseHistory([]byte("1\t0/3000148\tno recovery target specified\n\n2\t0/5A00000\tno recovery target specified\n"))
	if err != nil || len(h) != 2 {
		t.Fatalf("%v %v", h, err)
	}
	for _, c := range []struct {
		lsn  uint64
		want uint32
	}{{0x3000147, 1}, {0x3000148, 2}, {0x59FFFFF, 2}, {0x5A00000, 3}} {
		if got := owner(h, 3, c.lsn); got != c.want {
			t.Fatalf("%s owned by %d, want %d", formatLSN(c.lsn), got, c.want)
		}
	}
	if _, err := parseHistory([]byte("1 nonsense\n")); err == nil {
		t.Fatal("a bad line parsed")
	}
}

func TestRecordEnd(t *testing.T) {
	// Within a page.
	if got := recordEnd(0x1000018, 100, seg); got != 0x1000018+100 {
		t.Fatalf("%X", got)
	}
	// Crossing one page boundary adds a short header.
	p := uint64(0x1000000 + page - 16)
	if got := recordEnd(p, 64, seg); got != p+64+shortPage {
		t.Fatalf("%X want %X", got, p+64+shortPage)
	}
	// Crossing into a new segment adds the long header.
	p = uint64(2*seg - 8)
	if got := recordEnd(p, 32, seg); got != p+32+longPage {
		t.Fatalf("%X want %X", got, p+32+longPage)
	}
	// Ending exactly at a page's end crosses nothing.
	p = uint64(0x1000000 + page - 32)
	if got := recordEnd(p, 32, seg); got != 0x1000000+page {
		t.Fatalf("%X", got)
	}
}
