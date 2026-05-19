JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
BUILD ?= build

.PHONY: all configure build test clean release asan check fmt format-check fuzz bench setup install hooks lint tidy coverage validate ci prove demo-loop demo-loop-build demo-loop-full m3-status m3-dpo m3-train-mlx m3-drift m3-routes m3-promote m3-loop-now m3-extract m3-counterfactuals m3-probe m3-collect m3-h-demo

all: build test

configure:
	@mkdir -p $(BUILD)
	cmake -B $(BUILD) -DCMAKE_BUILD_TYPE=Debug -DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_CURL=ON

build: $(BUILD)/CMakeCache.txt
	cmake --build $(BUILD) -j$(JOBS)

$(BUILD)/CMakeCache.txt:
	@$(MAKE) configure

test: build
	$(BUILD)/human_tests

asan:
	cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
		-DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_CURL=ON
	cmake --build build-asan -j$(JOBS)
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 build-asan/human_tests

release:
	cmake -B $(BUILD) -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_LTO=ON \
		-DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_CURL=ON
	cmake --build $(BUILD) -j$(JOBS)
	@SIZE=$$(stat -c%s $(BUILD)/human 2>/dev/null || stat -f%z $(BUILD)/human); \
	echo "Binary: $$((SIZE / 1024)) KB"

check: build
	@echo "Running tests (use 'make asan' for AddressSanitizer)"
	$(BUILD)/human_tests

lint:
	cmake -B build-tidy -DCMAKE_BUILD_TYPE=Debug -DHU_ENABLE_ALL_CHANNELS=ON \
		-DHU_ENABLE_CURL=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	@find src -name '*.c' | head -50 | xargs clang-tidy -p build-tidy 2>&1 | tail -30

tidy: lint

coverage:
	cmake -B build-cov -DCMAKE_BUILD_TYPE=Debug -DHU_ENABLE_ALL_CHANNELS=ON \
		-DHU_ENABLE_CURL=ON -DCMAKE_C_FLAGS="--coverage -fprofile-arcs -ftest-coverage"
	cmake --build build-cov -j$(JOBS)
	build-cov/human_tests
	@echo "Generating coverage report..."
	@lcov --capture --directory build-cov --output-file build-cov/coverage.info --ignore-errors mismatch 2>/dev/null || true
	@lcov --remove build-cov/coverage.info '/usr/*' --output-file build-cov/coverage.info 2>/dev/null || true
	@if command -v genhtml >/dev/null 2>&1; then \
		genhtml build-cov/coverage.info --output-directory build-cov/html; \
		echo "Coverage report: build-cov/html/index.html"; \
	else echo "Install lcov for HTML report"; fi

clean:
	rm -rf $(BUILD) build-asan build-check build-fuzz build-cov build-tidy

fmt:
	@find src include tests -name '*.c' -o -name '*.h' | xargs clang-format -i

format-check:
	@find src include tests -name '*.c' -o -name '*.h' | xargs clang-format --dry-run -Werror

fuzz:
	cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DHU_ENABLE_FUZZ=ON \
		-DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_CURL=ON
	cmake --build build-fuzz -j$(JOBS)
	@echo "Fuzz targets built. Run: ./build-fuzz/fuzz_<name> -max_total_time=30"

bench: release
	@if [ -x scripts/benchmark.sh ]; then scripts/benchmark.sh $(BUILD)/human; \
	else echo "Binary: $$(stat -c%s $(BUILD)/human 2>/dev/null || stat -f%z $(BUILD)/human) bytes"; fi

install: release
	cmake --install $(BUILD) --prefix $(or $(PREFIX),$(HOME)/.local)
	@echo "Installed to $(or $(PREFIX),$(HOME)/.local)/bin/human"

setup:
	@echo "==> Installing dependencies"
	@if [ "$$(uname)" = "Darwin" ]; then echo "  brew install cmake sqlite curl"; \
	else echo "  sudo apt install build-essential cmake libsqlite3-dev libcurl4-openssl-dev"; fi
	@echo ""
	@$(MAKE) configure
	@$(MAKE) hooks
	@echo "==> Ready. Run: make test"

hooks:
	git config core.hooksPath .githooks
	@echo "Git hooks activated (.githooks/)"

prove:
	@bash scripts/prove-intelligence.sh

# M3 closed-loop live-fire demo. Needs the release-preset binary so the
# daemon's chat path runs without ASan. demo-loop-build is the build
# step (idempotent — cmake re-uses the build cache); demo-loop runs the
# orchestration. Together they're the e2e proof for the dormant LoRA
# loop: stub-MLX + service-loop --with-gateway + driver → swap.
#
#   make demo-loop-build   # one-time: cmake --preset release + build human
#   make demo-loop         # idempotent: cleanup + chat + driver + swap
demo-loop-build:
	cmake --preset release >/dev/null
	cmake --build --preset release --target human -j$(JOBS)
	@echo "==> Release binary: build-release/human"

demo-loop: demo-loop-build
	@bash scripts/live_fire_m3_loop.sh

# Phase C full loop — produces outcomes, runs REAL lora-persona training,
# A/B-evaluates the candidate vs an empty-tensors baseline. Different from
# `demo-loop` which uses --simulate-train.
demo-loop-full: demo-loop-build
	@bash scripts/live_fire_m3_full_loop.sh

# D1 (2026-05-18) — single-screen status of the M3 personalization loop.
# Pure file-inspection; safe to run while the daemon is live.
m3-status:
	@python3 scripts/m3_status.py

# D7 (2026-05-18) — summarize REWRITE preference pairs captured by the
# guard chain; optionally export Alpaca-DPO format for downstream training.
m3-dpo:
	@python3 scripts/m3_dpo_from_rewrites.py

# E2 (2026-05-18) — MLX-lm.lora bridge. Trains a real LoRA against the
# served model when mlx_lm is installed; otherwise produces a stub
# safetensors with a clear "install mlx-lm" hint.
m3-train-mlx:
	@python3 scripts/m3_mlx_lora_bridge.py --check-only \
		--pairs $${PAIRS:-~/.human/training-data/m3-rewrite-pairs.jsonl} \
		--adapter-out $${OUT:-/tmp/m3-mlx-test.safetensors}

# E4 (2026-05-18) — drift detection over outcome windows. Compares the
# most-recent adapter's metrics against the prior one; flags
# DEGRADING or NEEDS_ROLLBACK.
m3-drift:
	@python3 scripts/m3_drift_detector.py

# E5 (2026-05-18) — per-contact adapter routing CLI.
m3-routes:
	@python3 scripts/m3_contact_routing.py list

# G2 (2026-05-18) — promote/rollback CLI for the live MLX server.
# Pass ADAPTER=<path> to promote that adapter; otherwise shows current.
m3-promote:
	@if [ -n "$$ADAPTER" ]; then \
		python3 scripts/m3_promote.py promote --adapter "$$ADAPTER" --yes ; \
	else \
		python3 scripts/m3_promote.py current ; \
	fi

# G4 (2026-05-18) — manually trigger the autonomous loop cycle once.
# The launchd plist (scripts/ai.human.m3-loop.plist) runs this weekly;
# `make m3-loop-now` lets you exercise it ad hoc.
m3-loop-now:
	@bash scripts/m3_loop_cycle.sh

# H1 (2026-05-18) — multi-channel corpus extractor.
# Pulls Seth-authored turns from iMessage chat.db + memory.db (gmail/
# slack stubbed). PII redaction is mandatory by default. Writes the
# unified JSONL to ~/.human/training-data/m3-corpus.jsonl.
m3-extract:
	@python3 scripts/m3_extract_corpus.py \
		--out $${OUT:-$$HOME/.human/training-data/m3-corpus.jsonl} \
		--sources $${SRC:-imessage,memory_db} \
		--max-per-source $${MAX:-10000}

# H2 (2026-05-18) — counterfactual preference generator.
# Reads H1 corpus, pairs each Seth turn with its preceding user
# context (same-contact), generates style-violation variants, emits
# Alpaca-DPO pairs. LLM-as-judge when OPENAI_API_KEY set; synthetic
# deterministic fallback otherwise.
m3-counterfactuals:
	@python3 scripts/m3_generate_counterfactuals.py \
		--corpus $${IN:-$$HOME/.human/training-data/m3-corpus.jsonl} \
		--out $${OUT:-$$HOME/.human/training-data/m3-counterfactuals.jsonl} \
		--max-records $${MAX:-200} \
		$${NO_LLM:+--no-llm}

# H3 (2026-05-18) — active-learning probe.
# Picks an unanswered incoming message from H1 corpus, generates K
# candidate responses via the gateway (or synthetic fallback), and
# queues an A/B/C question for Seth. Set SIM=1 for local simulate
# mode (prints to stdout, no real iMessage send).
m3-probe:
	@python3 scripts/m3_active_probe.py \
		--corpus $${IN:-$$HOME/.human/training-data/m3-corpus.jsonl} \
		--pairs-out $${OUT:-$$HOME/.human/training-data/m3-active-probe-pairs.jsonl} \
		--queue $${QUEUE:-$$HOME/.human/training-data/m3-active-probe-queue.jsonl} \
		$${SIM:+--simulate-delivery} \
		$${RESP:+--simulate-response=$$RESP}

# H3b (2026-05-19) — probe queue collector.
# Drains the active-probe queue into Alpaca-DPO preference pairs.
# Default mode is simulate-tick (single-pass, optional --simulate-response).
# Set MODE=dispatch / MODE=poll for production wires (currently stubs).
# Example: RESP=A make m3-collect
m3-collect:
	@python3 scripts/m3_probe_collector.py \
		--queue $${QUEUE:-$$HOME/.human/training-data/m3-active-probe-queue.jsonl} \
		--pairs-out $${OUT:-$$HOME/.human/training-data/m3-active-probe-pairs.jsonl} \
		--mode $${MODE:-simulate-tick} \
		$${RESP:+--simulate-response=$$RESP}

# H demo (2026-05-19) — full data-acquisition tier end-to-end.
# Runs H1 → H2 → H3 → H3b in isolated paths and produces a combined
# Alpaca-DPO training file. Set FIX=1 to use fixture DBs (CI mode).
m3-h-demo:
	@bash scripts/m3_h_tier_demo.sh $${FIX:+--fixture}

validate: format-check build test
	@echo "Validation passed."

ci: format-check build test
	@scripts/check-untested.sh
	@echo "CI checks passed."
