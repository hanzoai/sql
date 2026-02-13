.PHONY: build push run test clean

IMAGE := ghcr.io/hanzoai/sql
TAG := latest
PG_MAJOR := 16

build:
	docker build --build-arg PG_MAJOR=$(PG_MAJOR) -t $(IMAGE):$(TAG) .

push: build
	docker push $(IMAGE):$(TAG)

run:
	docker run -d --name hanzo-sql \
		-e POSTGRES_PASSWORD=hanzo \
		-e POSTGRES_DB=hanzo \
		-p 5432:5432 \
		$(IMAGE):$(TAG)

test: run
	@echo "Waiting for postgres to start..."
	@sleep 5
	@docker exec hanzo-sql psql -U postgres -d hanzo -c "CREATE EXTENSION IF NOT EXISTS vector; SELECT extversion FROM pg_extension WHERE extname='vector';"
	@docker stop hanzo-sql && docker rm hanzo-sql

clean:
	-docker stop hanzo-sql 2>/dev/null
	-docker rm hanzo-sql 2>/dev/null
