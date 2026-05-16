# ── Stage 1: build ───────────────────────────────────────────────────
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc make zlib1g-dev ca-certificates && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN make -j$(nproc) build/templar_api build/build_index

# ── Stage 2: build the index ──────────────────────────────────────────
FROM builder AS indexer

RUN mkdir -p /data && \
    ./build/build_index bench/references.json.gz bench/mcc_risk.json /data/index.bin && \
    cp bench/mcc_risk.json /data/mcc_risk.json

# ── Stage 3: minimal runtime ──────────────────────────────────────────
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends zlib1g \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/build/templar_api /app/templar_api
COPY --from=indexer /data /data

EXPOSE 8080
CMD ["/app/templar_api", "8080", "2", "/data/index.bin", "/data/mcc_risk.json"]