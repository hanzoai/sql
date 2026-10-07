package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"sort"
	"strconv"
	"strings"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/credentials"
	"github.com/aws/aws-sdk-go-v2/service/s3"
	"github.com/aws/smithy-go"
)

// The store's layout, under WAL_S3_PREFIX in WAL_S3_BUCKET:
//
//	wal/TTTTTTTT/SSSSSSSSSSSSSSSS-EEEEEEEEEEEEEEEE.SINK  bytes [S, E) of timeline T, as a sink received them
//	seg/TTTTTTTT/NAME                                    a finished segment, or a .partial or .backup file
//	history/TTTTTTTT.history                             timeline T's history file
//
// Every object is written once (If-None-Match: *). The bytes at an LSN on a
// timeline never change, so two sinks writing the same range write the same
// bytes, and a write that landed before its answer was lost is found already
// there.
type store struct {
	c      *s3.Client
	bucket string
	prefix string
}

var errMissing = errors.New("not in the store")

func open(ctx context.Context) (*store, error) {
	endpoint, err := must("WAL_S3_ENDPOINT")
	if err != nil {
		return nil, err
	}
	bucket, err := must("WAL_S3_BUCKET")
	if err != nil {
		return nil, err
	}
	key, err := must("WAL_S3_ACCESS_KEY")
	if err != nil {
		return nil, err
	}
	secret, err := must("WAL_S3_SECRET_KEY")
	if err != nil {
		return nil, err
	}
	cfg := aws.Config{
		Region:      env("WAL_S3_REGION", "us-east-1"),
		Credentials: credentials.NewStaticCredentialsProvider(key, secret, ""),
		HTTPClient:  &http.Client{Transport: &http.Transport{MaxIdleConnsPerHost: 16}},
	}
	c := s3.NewFromConfig(cfg, func(o *s3.Options) {
		o.BaseEndpoint = aws.String(endpoint)
		o.UsePathStyle = true
	})
	return &store{c: c, bucket: bucket, prefix: strings.Trim(env("WAL_S3_PREFIX", "sql"), "/")}, nil
}

func (s *store) key(parts ...string) string {
	return s.prefix + "/" + strings.Join(parts, "/")
}

// create writes body at key once. An object already there is the same write
// landing twice, and counts as written.
func (s *store) create(ctx context.Context, key string, body []byte) error {
	for attempt := 0; ; attempt++ {
		_, err := s.c.PutObject(ctx, &s3.PutObjectInput{
			Bucket:        aws.String(s.bucket),
			Key:           aws.String(key),
			Body:          bytes.NewReader(body),
			ContentLength: aws.Int64(int64(len(body))),
			IfNoneMatch:   aws.String("*"),
		})
		if err == nil || status(err) == http.StatusPreconditionFailed {
			return nil
		}
		// 409: a concurrent conditional write to the same key; the next try
		// finds it there.
		if status(err) == http.StatusConflict && attempt < 5 {
			continue
		}
		return fmt.Errorf("put %s: %w", key, err)
	}
}

func (s *store) get(ctx context.Context, key string) ([]byte, error) {
	out, err := s.c.GetObject(ctx, &s3.GetObjectInput{Bucket: aws.String(s.bucket), Key: aws.String(key)})
	if err != nil {
		if status(err) == http.StatusNotFound {
			return nil, errMissing
		}
		return nil, fmt.Errorf("get %s: %w", key, err)
	}
	defer out.Body.Close()
	return io.ReadAll(out.Body)
}

// length is the size of the object at key.
func (s *store) length(ctx context.Context, key string) (uint64, error) {
	out, err := s.c.HeadObject(ctx, &s3.HeadObjectInput{Bucket: aws.String(s.bucket), Key: aws.String(key)})
	if err != nil {
		if status(err) == http.StatusNotFound {
			return 0, errMissing
		}
		return 0, fmt.Errorf("head %s: %w", key, err)
	}
	return uint64(aws.ToInt64(out.ContentLength)), nil
}

// list names every object under dir after the key after, in key order, stopping
// at the first name stop returns true for.
func (s *store) list(ctx context.Context, dir, after string, stop func(name string) bool) ([]string, error) {
	var names []string
	p := s3.NewListObjectsV2Paginator(s.c, &s3.ListObjectsV2Input{
		Bucket:     aws.String(s.bucket),
		Prefix:     aws.String(dir),
		StartAfter: aws.String(after),
	})
	for p.HasMorePages() {
		page, err := p.NextPage(ctx)
		if err != nil {
			return nil, fmt.Errorf("list %s: %w", dir, err)
		}
		for _, o := range page.Contents {
			name := strings.TrimPrefix(aws.ToString(o.Key), dir)
			if stop != nil && stop(name) {
				return names, nil
			}
			names = append(names, name)
		}
	}
	return names, nil
}

// dirs names what is directly under dir: its objects and, by their first
// path segment, its subdirectories.
func (s *store) dirs(ctx context.Context, dir string) ([]string, error) {
	var names []string
	p := s3.NewListObjectsV2Paginator(s.c, &s3.ListObjectsV2Input{
		Bucket:    aws.String(s.bucket),
		Prefix:    aws.String(dir),
		Delimiter: aws.String("/"),
	})
	for p.HasMorePages() {
		page, err := p.NextPage(ctx)
		if err != nil {
			return nil, fmt.Errorf("list %s: %w", dir, err)
		}
		for _, cp := range page.CommonPrefixes {
			names = append(names, strings.TrimSuffix(strings.TrimPrefix(aws.ToString(cp.Prefix), dir), "/"))
		}
		for _, o := range page.Contents {
			names = append(names, strings.TrimPrefix(aws.ToString(o.Key), dir))
		}
	}
	return names, nil
}

func status(err error) int {
	var re interface{ HTTPStatusCode() int }
	if errors.As(err, &re) {
		return re.HTTPStatusCode()
	}
	var ae smithy.APIError
	if errors.As(err, &ae) {
		switch ae.ErrorCode() {
		case "PreconditionFailed":
			return http.StatusPreconditionFailed
		case "ConditionalRequestConflict":
			return http.StatusConflict
		case "NoSuchKey", "NotFound":
			return http.StatusNotFound
		}
	}
	return 0
}

// A chunk is the bytes [start, end) of one timeline, as one sink stored them.
type chunk struct {
	start, end uint64
	key        string
}

func chunkName(start, end uint64, sink string) string {
	return fmt.Sprintf("%016X-%016X.%s", start, end, sink)
}

func parseChunk(name string) (chunk, bool) {
	r, _, ok := strings.Cut(name, ".")
	if !ok {
		return chunk{}, false
	}
	a, b, ok := strings.Cut(r, "-")
	if !ok || len(a) != 16 || len(b) != 16 {
		return chunk{}, false
	}
	start, err1 := strconv.ParseUint(a, 16, 64)
	end, err2 := strconv.ParseUint(b, 16, 64)
	if err1 != nil || err2 != nil || end <= start {
		return chunk{}, false
	}
	return chunk{start: start, end: end}, true
}

func tliDir(tli uint32) string { return fmt.Sprintf("%08X", tli) }

// chunks lists timeline tli's chunks that hold any byte of [from, to). A sink
// never stores a chunk longer than a segment, so none starting a segment
// before from can reach it.
func (s *store) chunks(ctx context.Context, tli uint32, from, to, segment uint64) ([]chunk, error) {
	dir := s.key("wal", tliDir(tli)) + "/"
	after := ""
	if from > segment {
		after = fmt.Sprintf("%016X", from-segment)
	}
	names, err := s.list(ctx, dir, after, func(name string) bool {
		c, ok := parseChunk(name)
		return ok && c.start >= to
	})
	if err != nil {
		return nil, err
	}
	var out []chunk
	for _, n := range names {
		c, ok := parseChunk(n)
		if !ok || c.end <= from || c.start >= to {
			continue
		}
		c.key = dir + n
		out = append(out, c)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].start != out[j].start {
			return out[i].start < out[j].start
		}
		return out[i].end > out[j].end
	})
	return out, nil
}
