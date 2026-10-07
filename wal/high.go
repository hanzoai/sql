package main

import (
	"context"
	"errors"
	"strconv"
	"strings"
)

// high is how far the store reaches on timeline tli: the end of the last
// finished segment or of the last chunk, whichever is later. Zero when the
// store holds nothing of the timeline.
func (s *store) high(ctx context.Context, tli uint32, segment uint64) (uint64, error) {
	var top uint64
	segs, err := s.list(ctx, s.key("seg", tliDir(tli))+"/", "", nil)
	if err != nil {
		return 0, err
	}
	for _, n := range segs {
		if t, start, ok := parseSegment(n, segment); ok && t == tli && start+segment > top {
			top = start + segment
		}
	}
	after := ""
	if top > segment {
		after = strings.ToUpper(strconv.FormatUint(top-segment, 16))
		after = strings.Repeat("0", 16-len(after)) + after
	}
	names, err := s.list(ctx, s.key("wal", tliDir(tli))+"/", after, nil)
	if err != nil {
		return 0, err
	}
	for _, n := range names {
		if c, ok := parseChunk(n); ok && c.end > top {
			top = c.end
		}
	}
	return top, nil
}

// archived reports whether every segment of timeline tli from the one holding
// from up to to (a segment start) is in the store whole.
func (s *store) archived(ctx context.Context, tli uint32, from, to, segment uint64) (bool, error) {
	for at := from - from%segment; at < to; at += segment {
		n, err := s.length(ctx, s.key("seg", tliDir(tli), segmentName(tli, at, segment)))
		if err != nil {
			if errors.Is(err, errMissing) {
				return false, nil
			}
			return false, err
		}
		if n != segment {
			return false, nil
		}
	}
	return true, nil
}

// latest is the highest timeline the store has history or WAL for.
func (s *store) latest(ctx context.Context) (uint32, error) {
	var top uint32 = 1
	for _, dir := range []string{"history", "wal", "seg"} {
		names, err := s.dirs(ctx, s.key(dir)+"/")
		if err != nil {
			return 0, err
		}
		for _, n := range names {
			if len(n) < 8 {
				continue
			}
			if t, err := strconv.ParseUint(n[:8], 16, 32); err == nil && uint32(t) > top {
				top = uint32(t)
			}
		}
	}
	return top, nil
}
