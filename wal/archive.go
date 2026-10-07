package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"
)

// segmentSize is WAL_SEGMENT_SIZE, PostgreSQL's wal_segment_size in bytes:
// 16 MiB unless the cluster was initialized with another.
func segmentSize() (uint64, error) {
	v, err := strconv.ParseUint(env("WAL_SEGMENT_SIZE", "16777216"), 10, 64)
	if err != nil || v == 0 || v&(v-1) != 0 {
		return 0, fmt.Errorf("WAL_SEGMENT_SIZE %q is not a power of two", os.Getenv("WAL_SEGMENT_SIZE"))
	}
	return v, nil
}

// fileDir is where an archived file lives: a segment, a .partial or a .backup
// under its timeline, a history file under history/.
func fileKey(s *store, name string) string {
	if strings.HasSuffix(name, ".history") {
		return s.key("history", name)
	}
	if len(name) >= 8 {
		return s.key("seg", name[:8], name)
	}
	return s.key("seg", "other", name)
}

// push is the archive_command: the finished file at path, stored once under
// its name.
func push(ctx context.Context, path, name string) error {
	s, err := open(ctx)
	if err != nil {
		return err
	}
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	return s.create(ctx, fileKey(s, name), b)
}

// restore is the restore_command. Its exit status is what PostgreSQL reads:
// 0 the file is at path, 1 the store has no such file, 255 the store could not
// be read. PostgreSQL ends recovery on an exit above 125 rather than taking an
// unreadable store for the end of WAL, so a standby is never promoted short of
// what the store holds.
func restore(ctx context.Context, name, path string) int {
	err := restoreOnce(ctx, name, path)
	for wait := time.Second; err != nil && !errors.Is(err, errMissing) && wait <= 16*time.Second; wait *= 2 {
		fmt.Fprintf(os.Stderr, "wal restore %s: %v; again in %s\n", name, err, wait)
		select {
		case <-ctx.Done():
			return 255
		case <-time.After(wait):
		}
		err = restoreOnce(ctx, name, path)
	}
	switch {
	case err == nil:
		return 0
	case errors.Is(err, errMissing):
		return 1
	default:
		fmt.Fprintf(os.Stderr, "wal restore %s: %v\n", name, err)
		return 255
	}
}

func restoreOnce(ctx context.Context, name, path string) error {
	s, err := open(ctx)
	if err != nil {
		return err
	}
	size, err := segmentSize()
	if err != nil {
		return err
	}
	b, err := s.get(ctx, fileKey(s, name))
	if err == nil {
		return write(path, b)
	}
	if !errors.Is(err, errMissing) {
		return err
	}
	tli, start, ok := parseSegment(name, size)
	if !ok {
		return errMissing
	}
	// Never finished: the segment the primary was writing when it stopped.
	// What the sinks hold of it, from its first byte, and zeros after, as
	// PostgreSQL leaves a segment it has not filled.
	seg := make([]byte, size)
	got, err := s.assemble(ctx, tli, start, start+size, size, seg)
	if err != nil {
		return err
	}
	if got == 0 {
		return errMissing
	}
	return write(path, seg)
}

// write puts b at path whole or not at all.
func write(path string, b []byte) error {
	tmp := path + ".wal"
	if err := os.WriteFile(tmp, b, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}
