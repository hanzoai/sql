// s3util: the bench's bucket — make it, count a sink's objects, empty and drop it.
package main

import (
	"context"
	"fmt"
	"os"
	"strings"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/credentials"
	"github.com/aws/aws-sdk-go-v2/service/s3"
	"github.com/aws/aws-sdk-go-v2/service/s3/types"
)

func main() {
	ctx := context.Background()
	c := s3.NewFromConfig(aws.Config{Region: "us-east-1", Credentials: credentials.NewStaticCredentialsProvider(os.Getenv("WAL_S3_ACCESS_KEY"), os.Getenv("WAL_S3_SECRET_KEY"), "")}, func(o *s3.Options) {
		o.BaseEndpoint = aws.String(os.Getenv("WAL_S3_ENDPOINT")); o.UsePathStyle = true
	})
	b := aws.String(os.Getenv("WAL_S3_BUCKET"))
	switch os.Args[1] {
	case "make":
		_, err := c.CreateBucket(ctx, &s3.CreateBucketInput{Bucket: b})
		check(err)
	case "count": // count PREFIX: objects per suffix after the last '.'
		n := map[string]int{}
		p := s3.NewListObjectsV2Paginator(c, &s3.ListObjectsV2Input{Bucket: b, Prefix: aws.String(os.Args[2])})
		for p.HasMorePages() {
			pg, err := p.NextPage(ctx); check(err)
			for _, o := range pg.Contents {
				k := aws.ToString(o.Key); i := strings.LastIndex(k, "."); s := "-"
				if i >= 0 { s = k[i+1:] }
				n[s]++
			}
		}
		fmt.Println(n)
	case "drop":
		for {
			out, err := c.ListObjectVersions(ctx, &s3.ListObjectVersionsInput{Bucket: b}); check(err)
			var ids []types.ObjectIdentifier
			for _, v := range out.Versions { ids = append(ids, types.ObjectIdentifier{Key: v.Key, VersionId: v.VersionId}) }
			for _, m := range out.DeleteMarkers { ids = append(ids, types.ObjectIdentifier{Key: m.Key, VersionId: m.VersionId}) }
			if len(ids) == 0 { break }
			for i := 0; i < len(ids); i += 1000 {
				j := min(len(ids), i+1000)
				_, err := c.DeleteObjects(ctx, &s3.DeleteObjectsInput{Bucket: b, Delete: &types.Delete{Objects: ids[i:j], Quiet: aws.Bool(true)}}); check(err)
			}
		}
		_, err := c.DeleteBucket(ctx, &s3.DeleteBucketInput{Bucket: b}); check(err)
		fmt.Println("dropped", *b)
	}
}

func check(err error) { if err != nil { fmt.Fprintln(os.Stderr, err); os.Exit(1) } }
