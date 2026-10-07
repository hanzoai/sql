package main

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"fmt"
	"strconv"
	"strings"
)

// A segment's name is its timeline and its number, as PostgreSQL writes it:
// TTTTTTTTLLLLLLLLSSSSSSSS, where the segment number is L * (2^32 / size) + S.
func parseSegment(name string, size uint64) (tli uint32, start uint64, ok bool) {
	if len(name) != 24 {
		return 0, 0, false
	}
	t, err1 := strconv.ParseUint(name[0:8], 16, 32)
	l, err2 := strconv.ParseUint(name[8:16], 16, 32)
	s, err3 := strconv.ParseUint(name[16:24], 16, 32)
	if err1 != nil || err2 != nil || err3 != nil || t == 0 {
		return 0, 0, false
	}
	per := (uint64(1) << 32) / size
	if s >= per {
		return 0, 0, false
	}
	return uint32(t), (l*per + s) * size, true
}

func segmentName(tli uint32, lsn, size uint64) string {
	per := (uint64(1) << 32) / size
	no := lsn / size
	return fmt.Sprintf("%08X%08X%08X", tli, no/per, no%per)
}

// A switch is where a timeline's parent ended: bytes before at belong to
// parent.
type switchpoint struct {
	parent uint32
	at     uint64
}

// parseHistory reads a timeline history file: one line per ancestor, its
// timeline, the LSN it ended at, and why.
func parseHistory(b []byte) ([]switchpoint, error) {
	var out []switchpoint
	sc := bufio.NewScanner(bytes.NewReader(b))
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		f := strings.Fields(line)
		if len(f) < 2 {
			return nil, fmt.Errorf("history line %q", line)
		}
		t, err := strconv.ParseUint(f[0], 10, 32)
		if err != nil {
			return nil, fmt.Errorf("history line %q", line)
		}
		at, err := parseLSN(f[1])
		if err != nil {
			return nil, fmt.Errorf("history line %q", line)
		}
		out = append(out, switchpoint{parent: uint32(t), at: at})
	}
	return out, sc.Err()
}

func parseLSN(s string) (uint64, error) {
	hi, lo, ok := strings.Cut(s, "/")
	if !ok {
		return 0, fmt.Errorf("lsn %q", s)
	}
	h, err1 := strconv.ParseUint(hi, 16, 32)
	l, err2 := strconv.ParseUint(lo, 16, 32)
	if err1 != nil || err2 != nil {
		return 0, fmt.Errorf("lsn %q", s)
	}
	return h<<32 | l, nil
}

func formatLSN(v uint64) string { return fmt.Sprintf("%X/%X", v>>32, uint32(v)) }

// owner is the timeline whose WAL holds the byte at lsn, read on timeline tli.
func owner(history []switchpoint, tli uint32, lsn uint64) uint32 {
	for _, h := range history {
		if lsn < h.at {
			return h.parent
		}
	}
	return tli
}

// history is timeline tli's ancestry, from its history file; timeline 1 has
// none.
func (s *store) history(ctx context.Context, tli uint32) ([]switchpoint, error) {
	if tli <= 1 {
		return nil, nil
	}
	b, err := s.get(ctx, s.key("history", fmt.Sprintf("%08X.history", tli)))
	if err != nil {
		return nil, err
	}
	return parseHistory(b)
}

// assemble reads timeline tli's bytes over [from, to) into out, which is
// to-from long, and returns how many leading bytes it filled: it stops at the
// first byte no stored object holds. Bytes before a switchpoint are read from
// the parent timeline that wrote them.
func (s *store) assemble(ctx context.Context, tli uint32, from, to, segment uint64, out []byte) (uint64, error) {
	hist, err := s.history(ctx, tli)
	if err != nil {
		return 0, err
	}
	cur := from
	for cur < to {
		t := owner(hist, tli, cur)
		end := to
		for _, h := range hist {
			if cur < h.at && h.at < end {
				end = h.at
				break
			}
		}
		got, err := s.fill(ctx, t, cur, end, segment, out[cur-from:end-from])
		if err != nil {
			return cur - from, err
		}
		cur += got
		if cur < end {
			break
		}
	}
	return cur - from, nil
}

// fill reads [from, to), all on timeline tli, from the finished segment when
// it was archived and from the sinks' chunks otherwise.
func (s *store) fill(ctx context.Context, tli uint32, from, to, segment uint64, out []byte) (uint64, error) {
	cur := from
	for cur < to {
		base := cur - cur%segment
		seg, err := s.get(ctx, s.key("seg", tliDir(tli), segmentName(tli, cur, segment)))
		switch {
		case err == nil && uint64(len(seg)) == segment:
			end := min(to, base+segment)
			copy(out[cur-from:end-from], seg[cur-base:end-base])
			cur = end
			continue
		case err != nil && !errors.Is(err, errMissing):
			return cur - from, err
		}
		end := min(to, base+segment)
		cs, err := s.chunks(ctx, tli, cur, end, segment)
		if err != nil {
			return cur - from, err
		}
		reached := cur
		for i := 0; i < len(cs) && reached < end; {
			// Of the chunks that hold the next byte, the one that reaches
			// furthest: one GET instead of every overlapping copy.
			best := -1
			for ; i < len(cs) && cs[i].start <= reached; i++ {
				if cs[i].end > reached && (best < 0 || cs[i].end > cs[best].end) {
					best = i
				}
			}
			if best < 0 {
				break
			}
			b, err := s.get(ctx, cs[best].key)
			if err != nil {
				return reached - from, err
			}
			if uint64(len(b)) != cs[best].end-cs[best].start {
				return reached - from, fmt.Errorf("%s holds %d bytes, its name says %d", cs[best].key, len(b), cs[best].end-cs[best].start)
			}
			stop := min(end, cs[best].end)
			copy(out[reached-from:stop-from], b[reached-cs[best].start:stop-cs[best].start])
			reached = stop
		}
		if reached < end {
			return reached - from, nil
		}
		cur = end
	}
	return cur - from, nil
}
