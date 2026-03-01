# syntax=docker/dockerfile:1

# ── Stage 1: Build ────────────────────────────────────────────
FROM alpine:3.23 AS builder

RUN apk add --no-cache build-base cmake sqlite-dev curl-dev

WORKDIR /app
COPY CMakeLists.txt ./
COPY src/ src/
COPY include/ include/
COPY asm/ asm/
COPY vendor/ vendor/
COPY cmake/ cmake/

RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON -DSC_ENABLE_CURL=ON && \
    make -j$(nproc)

# ── Stage 2: Config Prep ─────────────────────────────────────
FROM busybox:1.37 AS config

RUN mkdir -p /seaclaw-data/.seaclaw /seaclaw-data/workspace

RUN cat > /seaclaw-data/.seaclaw/config.json << 'EOF'
{
  "api_key": "",
  "default_provider": "openrouter",
  "default_model": "anthropic/claude-sonnet-4",
  "default_temperature": 0.7,
  "gateway": {
    "port": 3000,
    "host": "::",
    "allow_public_bind": true
  }
}
EOF

RUN chown -R 65534:65534 /seaclaw-data

# ── Stage 3: Runtime Base (shared) ────────────────────────────
FROM alpine:3.23 AS release-base

LABEL org.opencontainers.image.source=https://github.com/seaclaw/seaclaw

RUN apk add --no-cache ca-certificates curl tzdata libsqlite3

COPY --from=builder /app/build/seaclaw /usr/local/bin/seaclaw
COPY --from=config /seaclaw-data /seaclaw-data

ENV SEACLAW_WORKSPACE=/seaclaw-data/workspace
ENV HOME=/seaclaw-data
ENV SEACLAW_GATEWAY_PORT=3000

WORKDIR /seaclaw-data
EXPOSE 3000
ENTRYPOINT ["seaclaw"]
CMD ["gateway", "--port", "3000", "--host", "::"]

# Optional autonomous mode (explicit opt-in):
#   docker build --target release-root -t seaclaw:root .
FROM release-base AS release-root
USER 0:0

# Safe default image (used when no --target is provided)
FROM release-base AS release
USER 65534:65534
