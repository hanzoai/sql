package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/jackc/pglogrepl"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgproto3"
)

// sink streams the primary's WAL over a physical replication slot named
// WAL_NAME and stores it in S3. It is a synchronous standby that writes to no
// disk: the flush position it reports is the end of the last object stored,
// so a commit waiting on it returns only once its WAL is in S3.
//
// Stores are back to back: while one is in flight the WAL that arrives
// gathers, and the next object holds all of it, so commits that arrive
// together share one PUT. WAL_EVERY is the least time between two stores, for
// a sink kept as the second one, where fewer PUTs are worth slower commits.
func sink(ctx context.Context) error {
	name, err := must("WAL_NAME")
	if err != nil {
		return err
	}
	source, err := must("WAL_SOURCE")
	if err != nil {
		return err
	}
	every, err := time.ParseDuration(env("WAL_EVERY", "0s"))
	if err != nil {
		return fmt.Errorf("WAL_EVERY: %w", err)
	}
	s, err := open(ctx)
	if err != nil {
		return err
	}
	for {
		err := stream(ctx, s, name, source, every)
		if ctx.Err() != nil {
			return nil
		}
		fmt.Fprintf(os.Stderr, "wal sink %s: %v; reconnecting\n", name, err)
		select {
		case <-ctx.Done():
			return nil
		case <-time.After(time.Second):
		}
	}
}

type stored struct {
	end uint64
	err error
}

func stream(ctx context.Context, s *store, name, source string, every time.Duration) error {
	conn, err := pgconn.Connect(ctx, source)
	if err != nil {
		return err
	}
	defer conn.Close(context.Background())
	if conn.ParameterStatus("application_name") != name {
		return fmt.Errorf("WAL_SOURCE must carry application_name=%s, so synchronous_standby_names can name this sink", name)
	}
	size, err := segmentSize()
	if err != nil {
		return err
	}
	sys, err := pglogrepl.IdentifySystem(ctx, conn)
	if err != nil {
		return err
	}
	start, tli, err := slot(ctx, conn, name, sys)
	if err != nil {
		return err
	}
	// A promoted member's copy of the slot can be ahead of what the sinks
	// stored, and reporting from it would skip the bytes between: start from
	// the store's own end when it is behind.
	restart := start
	top, err := s.high(ctx, tli, size)
	if err != nil {
		return err
	}
	if top > 0 && top < start {
		start = top
	}
	if err := histories(ctx, s, conn, sys.Timeline); err != nil {
		return err
	}
	first := true
	for {
		err := pglogrepl.StartReplication(ctx, conn, name, pglogrepl.LSN(start), pglogrepl.StartReplicationOptions{
			Timeline: int32(tli),
			Mode:     pglogrepl.PhysicalReplication,
		})
		var pe *pgconn.PgError
		if err != nil && first && start < restart && errors.As(err, &pe) && pe.Code == "58P01" {
			// The primary has recycled the WAL between the store's end and the
			// slot. Starting at the slot's segment is safe only when every
			// segment before it is archived whole.
			from := restart - restart%size
			if ok, cerr := s.archived(ctx, tli, start, from, size); cerr != nil {
				return cerr
			} else if !ok {
				return fmt.Errorf("the store ends at %s, the primary keeps WAL from %s, and the segments between are not archived: %w", formatLSN(start), formatLSN(restart), err)
			}
			start = from
			err = pglogrepl.StartReplication(ctx, conn, name, pglogrepl.LSN(start), pglogrepl.StartReplicationOptions{
				Timeline: int32(tli),
				Mode:     pglogrepl.PhysicalReplication,
			})
		}
		if err != nil {
			return err
		}
		first = false
		next, nextTLI, err := receive(ctx, s, conn, name, tli, start, size, every)
		if err != nil {
			return err
		}
		// The primary ended a historic timeline: go on with the next, from
		// where it began.
		start, tli = next, nextTLI
		if err := histories(ctx, s, conn, int32(tli)); err != nil {
			return err
		}
	}
}

// slot is where this sink resumes: its slot's restart position, which moves
// only to a position this sink reported flushed, so every byte before it is
// in the store.
//
// The sink never makes its slot. A slot made now would start at the primary's
// current position, and the first flush reported from it would release
// commits whose WAL, in an earlier segment, no sink ever stored. The slots are
// Patroni's permanent slots, kept on every member and advanced from the
// primary's, so the one a promoted member holds is at or behind what the
// sinks stored.
func slot(ctx context.Context, conn *pgconn.PgConn, name string, _ pglogrepl.IdentifySystemResult) (uint64, uint32, error) {
	res, err := conn.Exec(ctx, "READ_REPLICATION_SLOT "+name).ReadAll()
	if err != nil {
		return 0, 0, err
	}
	if len(res) != 1 || len(res[0].Rows) != 1 || res[0].Rows[0][1] == nil {
		return 0, 0, fmt.Errorf("the primary has no physical slot %s with a restart position: name it in Patroni's permanent slots", name)
	}
	lsn, err := parseLSN(string(res[0].Rows[0][1]))
	if err != nil {
		return 0, 0, err
	}
	var tli uint64
	if _, err := fmt.Sscan(string(res[0].Rows[0][2]), &tli); err != nil {
		return 0, 0, err
	}
	return lsn, uint32(tli), nil
}

// histories stores the history file of every timeline up to tli, once each.
func histories(ctx context.Context, s *store, conn *pgconn.PgConn, tli int32) error {
	for t := int32(2); t <= tli; t++ {
		h, err := pglogrepl.TimelineHistory(ctx, conn, t)
		if err != nil {
			return err
		}
		if err := s.create(ctx, s.key("history", h.FileName), h.Content); err != nil {
			return err
		}
	}
	return nil
}

// receive stores the stream from start until the primary ends the timeline,
// answering every keepalive with the end of what is stored.
func receive(ctx context.Context, s *store, conn *pgconn.PgConn, name string, tli uint32, start, size uint64, every time.Duration) (uint64, uint32, error) {
	var (
		buf      []byte
		bufStart = start
		flushed  = start
		inflight bool
		done     = make(chan stored, 1)
		last     time.Time
		status   = time.Now()
	)
	report := func() error {
		status = time.Now()
		return pglogrepl.SendStandbyStatusUpdate(ctx, conn, pglogrepl.StandbyStatusUpdate{
			WALWritePosition: pglogrepl.LSN(flushed),
			WALFlushPosition: pglogrepl.LSN(flushed),
			WALApplyPosition: pglogrepl.LSN(flushed),
		})
	}
	// store sends the gathered bytes as one object, never one that crosses a
	// segment's end, so a segment's objects are found from its own range.
	store := func() {
		if inflight || len(buf) == 0 || time.Since(last) < every {
			return
		}
		n := uint64(len(buf))
		if room := size - bufStart%size; n > room {
			n = room
		}
		body := append([]byte(nil), buf[:n]...)
		from := bufStart
		key := s.key("wal", tliDir(tli), chunkName(from, from+n, name))
		buf, bufStart = buf[n:], from+n
		inflight, last = true, time.Now()
		go func() {
			err := s.create(ctx, key, body)
			done <- stored{end: from + n, err: err}
		}()
	}
	for {
		store()
		wait := 10 * time.Second
		if !inflight && len(buf) > 0 {
			wait = min(wait, max(time.Millisecond, every-time.Since(last)))
		}
		rctx, cancel := context.WithTimeout(ctx, wait)
		msg, err := receiveOrStored(rctx, conn, done, inflight)
		cancel()
		if st, ok := msg.(stored); ok {
			inflight = false
			if st.err != nil {
				return 0, 0, st.err
			}
			flushed = st.end
			if err := report(); err != nil {
				return 0, 0, err
			}
			continue
		}
		if err != nil {
			if pgconn.Timeout(err) || errors.Is(err, context.DeadlineExceeded) {
				if time.Since(status) >= 10*time.Second {
					if err := report(); err != nil {
						return 0, 0, err
					}
				}
				continue
			}
			return 0, 0, err
		}
		switch m := msg.(type) {
		case *pgproto3.CopyData:
			switch m.Data[0] {
			case pglogrepl.PrimaryKeepaliveMessageByteID:
				ka, err := pglogrepl.ParsePrimaryKeepaliveMessage(m.Data[1:])
				if err != nil {
					return 0, 0, err
				}
				if ka.ReplyRequested {
					if err := report(); err != nil {
						return 0, 0, err
					}
				}
			case pglogrepl.XLogDataByteID:
				x, err := pglogrepl.ParseXLogData(m.Data[1:])
				if err != nil {
					return 0, 0, err
				}
				if uint64(x.WALStart) != bufStart+uint64(len(buf)) {
					return 0, 0, fmt.Errorf("stream jumped from %s to %s", formatLSN(bufStart+uint64(len(buf))), x.WALStart)
				}
				buf = append(buf, x.WALData...)
			}
		case *pgproto3.CopyDone:
			// Everything sent before the end of the timeline is stored first:
			// the next timeline's history names this one's end.
			for inflight || len(buf) > 0 {
				if !inflight {
					every = 0
					store()
				}
				st := <-done
				inflight = false
				if st.err != nil {
					return 0, 0, st.err
				}
				flushed = st.end
			}
			res, err := pglogrepl.SendStandbyCopyDone(ctx, conn)
			if err != nil {
				return 0, 0, err
			}
			if res == nil {
				return 0, 0, errors.New("the primary ended the stream without naming the next timeline")
			}
			return uint64(res.LSN), uint32(res.Timeline), nil
		case *pgproto3.ErrorResponse:
			return 0, 0, pgconn.ErrorResponseToPgError(m)
		}
	}
}

// receiveOrStored waits for the next message from the primary, or for the
// object in flight to land, whichever is first.
func receiveOrStored(ctx context.Context, conn *pgconn.PgConn, done chan stored, inflight bool) (any, error) {
	if inflight {
		select {
		case st := <-done:
			return st, nil
		default:
		}
	}
	type got struct {
		msg pgproto3.BackendMessage
		err error
	}
	if !inflight {
		return conn.ReceiveMessage(ctx)
	}
	// A message from the primary and a store that lands race; the wait for the
	// message is cut short when the store lands first.
	mctx, cancel := context.WithCancel(ctx)
	defer cancel()
	ch := make(chan got, 1)
	go func() {
		m, err := conn.ReceiveMessage(mctx)
		ch <- got{m, err}
	}()
	select {
	case st := <-done:
		cancel()
		g := <-ch
		if g.err == nil {
			// The message arrived anyway: hand it back after the store, by
			// pushing the store back and returning the message first.
			done <- st
			return g.msg, nil
		}
		if !pgconn.Timeout(g.err) && !errors.Is(g.err, context.Canceled) && !strings.Contains(g.err.Error(), "context canceled") {
			return nil, g.err
		}
		return st, nil
	case g := <-ch:
		return g.msg, g.err
	}
}
