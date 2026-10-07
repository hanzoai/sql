package main

import (
	"context"
	"encoding/binary"
	"fmt"
	"os"
	"time"

	"github.com/jackc/pgx/v5/pgconn"
)

const (
	page       = 8192 // XLOG_BLCKSZ
	shortPage  = 24   // MAXALIGN(sizeof(XLogPageHeaderData))
	longPage   = 40   // MAXALIGN(sizeof(XLogLongPageHeaderData)), a segment's first page
	recordHead = 24   // sizeof(XLogRecord)
)

// gate is the pre_promote command. A standby is promoted only once it has
// replayed every complete record the store holds: a commit was acknowledged
// only after a sink stored its record, so a standby short of the store would
// come up without commits clients were told are done. Patroni does not promote
// a member whose pre_promote fails, and gives up the leader lock for another.
//
// The store's last record can be incomplete: the primary's background writer
// flushes whole pages, so a sink can hold the head of a record whose tail
// never arrived. No commit that record carried was acknowledged, and replay
// stops before it, so the gate opens when the next record is not whole in the
// store.
func gate(ctx context.Context) error {
	timeout, err := time.ParseDuration(env("WAL_GATE_TIMEOUT", "5m"))
	if err != nil {
		return fmt.Errorf("WAL_GATE_TIMEOUT: %w", err)
	}
	s, err := open(ctx)
	if err != nil {
		return err
	}
	size, err := segmentSize()
	if err != nil {
		return err
	}
	conn, err := pgconn.Connect(ctx, env("WAL_LOCAL", "host=/var/run/postgresql dbname=postgres"))
	if err != nil {
		return err
	}
	defer conn.Close(context.Background())
	deadline := time.Now().Add(timeout)
	for {
		recovering, replay, err := replayed(ctx, conn)
		if err != nil {
			return err
		}
		if !recovering {
			return nil
		}
		tli, err := s.latest(ctx)
		if err != nil {
			return err
		}
		top, err := s.high(ctx, tli, size)
		if err != nil {
			return err
		}
		if replay >= top {
			fmt.Fprintf(os.Stderr, "wal gate: replayed %s, the store ends at %s on timeline %d\n", formatLSN(replay), formatLSN(top), tli)
			return nil
		}
		whole, err := s.recordWhole(ctx, tli, replay, top, size)
		if err != nil {
			return err
		}
		if !whole {
			fmt.Fprintf(os.Stderr, "wal gate: replayed %s; the record after it is not whole in the store, which ends at %s\n", formatLSN(replay), formatLSN(top))
			return nil
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("replayed %s after %s, short of the store's %s on timeline %d", formatLSN(replay), timeout, formatLSN(top), tli)
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(time.Second):
		}
	}
}

func replayed(ctx context.Context, conn *pgconn.PgConn) (bool, uint64, error) {
	res, err := conn.Exec(ctx, "select pg_is_in_recovery(), coalesce(pg_last_wal_replay_lsn(), '0/0')").ReadAll()
	if err != nil {
		return false, 0, err
	}
	if len(res) != 1 || len(res[0].Rows) != 1 {
		return false, 0, fmt.Errorf("replay position: no row")
	}
	row := res[0].Rows[0]
	lsn, err := parseLSN(string(row[1]))
	if err != nil {
		return false, 0, err
	}
	return string(row[0]) == "t", lsn, nil
}

// recordWhole reports whether the record after end — the end of the last one
// replayed — lies wholly before top, the store's end.
func (s *store) recordWhole(ctx context.Context, tli uint32, end, top, size uint64) (bool, error) {
	p := (end + 7) &^ 7
	if p%page == 0 {
		p += header(p, size)
	}
	if p+4 > top {
		return false, nil
	}
	b := make([]byte, 4)
	got, err := s.assemble(ctx, tli, p, p+4, size, b)
	if err != nil {
		return false, err
	}
	if got < 4 {
		return false, nil
	}
	total := uint64(binary.LittleEndian.Uint32(b))
	if total < recordHead {
		// Zeros: the end of the WAL written.
		return false, nil
	}
	return recordEnd(p, total, size) <= top, nil
}

func header(p, size uint64) uint64 {
	if p%size == 0 {
		return longPage
	}
	return shortPage
}

// recordEnd is where a record of total bytes that starts at p ends, past the
// page headers it crosses.
func recordEnd(p, total, size uint64) uint64 {
	for total > 0 {
		room := page - p%page
		take := min(total, room)
		total -= take
		p += take
		if total > 0 {
			p += header(p, size)
		}
	}
	return p
}
