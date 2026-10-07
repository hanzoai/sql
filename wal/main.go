// Command wal keeps a server's WAL in S3, so no member's disk holds anything the
// cluster cannot rebuild.
//
//	wal sink           stream the primary's WAL as a synchronous standby that
//	                   reports a position flushed only once its bytes are an
//	                   object in S3
//	wal push PATH NAME archive_command: a finished segment, as one object
//	wal restore NAME PATH
//	                   restore_command: a segment from its object, or assembled
//	                   from the sinks' objects when it was never finished
//	wal gate           pre_promote: refuse promotion until this standby has
//	                   replayed every complete record S3 holds
//
// A commit returns once a sink has its WAL in S3 (synchronous_standby_names
// names the sinks), and a standby is promoted only past every byte the sinks
// stored, so a commit the primary acknowledged survives the loss of every
// member's disk.
package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"syscall"
)

const usage = `usage:
  wal sink
  wal push PATH NAME
  wal restore NAME PATH
  wal gate`

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, usage)
		os.Exit(2)
	}
	var err error
	switch os.Args[1] {
	case "sink":
		err = sink(ctx)
	case "push":
		if len(os.Args) != 4 {
			err = fmt.Errorf("%s", usage)
			break
		}
		err = push(ctx, os.Args[2], os.Args[3])
	case "restore":
		if len(os.Args) != 4 {
			err = fmt.Errorf("%s", usage)
			break
		}
		os.Exit(restore(ctx, os.Args[2], os.Args[3]))
	case "gate":
		err = gate(ctx)
	default:
		err = fmt.Errorf("%s", usage)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "wal:", err)
		os.Exit(1)
	}
}

func env(name, fallback string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return fallback
}

func must(name string) (string, error) {
	v := os.Getenv(name)
	if v == "" {
		return "", fmt.Errorf("%s is not set", name)
	}
	return v, nil
}
