# syntax=docker/dockerfile:1

# ── Stage 1: Build ────────────────────────────────────────────
FROM alpine:3.23 AS builder

RUN apk add --no-cache build-base cmake sqlite-dev curl-dev linux-headers

WORKDIR /app
COPY CMakeLists.txt ./
COPY src/ src/
COPY include/ include/
COPY asm/ asm/
COPY vendor/ vendor/
COPY cmake/ cmake/

RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON -DSC_ENABLE_CURL=ON -DSC_BUILD_TESTS=OFF && \
    make -j$(nproc)

# ── Stage 2: Config Prep ─────────────────────────────────────
FROM busybox:1.37 AS config

RUN mkdir -p /human-data/.human /human-data/workspace

RUN cat > /human-data/.human/config.json << 'EOF'
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

RUN chown -R 65534:65534 /human-data

# ── Stage 3: Runtime Base (shared) ────────────────────────────
FROM alpine:3.23 AS release-base

LABEL org.opencontainers.image.source=https://github.com/sethdford/human

RUN apk add --no-cache ca-certificates curl tzdata sqlite-libs

COPY --from=builder /app/build/human /usr/local/bin/human
COPY --from=config /human-data /human-data

ENV HUMAN_WORKSPACE=/human-data/workspace
ENV HOME=/human-data
ENV HUMAN_GATEWAY_PORT=3000

WORKDIR /human-data
EXPOSE 3000
ENTRYPOINT ["human"]
CMD ["gateway", "--port", "3000", "--host", "::"]

# Optional autonomous mode (explicit opt-in):
#   docker build --target release-root -t human:root .
FROM release-base AS release-root
USER 0:0

# Safe default image (used when no --target is provided)
FROM release-base AS release
USER 65534:65534
