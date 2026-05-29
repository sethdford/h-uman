/**
 * @human/hula-sdk — Node.js SDK for HuLa (Human Language) programs.
 *
 * M5 Phase 1 (2026-05-26): subprocess-based wrapper around the
 * `human hula` CLI. Apps using this SDK do NOT need to link the C
 * library or build native bindings — they just need the `human`
 * binary on PATH (or pointed at via the HUMAN_BIN env var, or
 * passed explicitly to the HuLa constructor).
 *
 * Mirrors the Python SDK API surface 1:1 so cross-language
 * documentation stays consistent.
 *
 * Quick start:
 *
 *   import { HuLa } from "@human/hula-sdk";
 *
 *   const hula = new HuLa();
 *   const program = {
 *     name: "hello",
 *     version: 1,
 *     root: { id: "n1", op: "emit",
 *             emit_key: "greeting", emit_value: "hello from node" }
 *   };
 *   const v = await hula.validate(program);
 *   if (v.ok) {
 *     const r = await hula.run(program);
 *     console.log(r.stdout);
 *   }
 *
 * Status: Phase 1 ships the subprocess boundary. Phase 2 will add a
 * native N-API binding once the project builds a shared libhuman
 * dylib. Both phases expose the same JS API so existing SDK
 * consumers won't need to change anything when Phase 2 lands.
 */

import { spawn } from "node:child_process";
import { writeFile, unlink, mkdtemp, access } from "node:fs/promises";
import { constants as fsConstants } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";

// Mirrored from include/human/hula_sdk.h. Bumped in lock-step with the
// C macros and the Python SDK so cross-language consumers can detect
// API breaks.
export const HULA_SDK_VERSION_MAJOR = 0;
export const HULA_SDK_VERSION_MINOR = 1;
export const HULA_SDK_VERSION_PATCH = 0;
export const HULA_SDK_VERSION_STRING =
  `${HULA_SDK_VERSION_MAJOR}.${HULA_SDK_VERSION_MINOR}.${HULA_SDK_VERSION_PATCH}`;

/**
 * Result of a `human hula <verb>` subprocess invocation.
 *
 * @typedef {object} HulaResult
 * @property {boolean} ok          True if the subprocess exited 0.
 * @property {number}  returncode  Raw exit status.
 * @property {string}  stdout      Captured stdout.
 * @property {string}  stderr      Captured stderr (failure reason).
 */

/**
 * Spawn a subprocess and capture its output. Resolves with a
 * HulaResult; never rejects on non-zero exit (the caller checks
 * `.ok` and `.stderr`). Rejects only on spawn-level failure
 * (binary not found, etc.).
 */
function spawnCapture(bin, argv, timeoutMs = 30000) {
  return new Promise((resolve, reject) => {
    const child = spawn(bin, argv, { stdio: ["ignore", "pipe", "pipe"] });
    let stdout = "";
    let stderr = "";
    let timedOut = false;
    const timer = setTimeout(() => {
      timedOut = true;
      child.kill("SIGKILL");
    }, timeoutMs);

    child.stdout.on("data", (b) => { stdout += b.toString("utf8"); });
    child.stderr.on("data", (b) => { stderr += b.toString("utf8"); });
    child.on("error", (err) => {
      clearTimeout(timer);
      reject(err);
    });
    child.on("close", (code) => {
      clearTimeout(timer);
      if (timedOut) {
        resolve({
          ok: false,
          returncode: -1,
          stdout,
          stderr: stderr + "\n[hula-sdk] subprocess timed out",
        });
        return;
      }
      resolve({
        ok: code === 0,
        returncode: code ?? -1,
        stdout,
        stderr,
      });
    });
  });
}

/**
 * Look up an executable on PATH (a minimal `which`).
 *
 * If `name` already contains a path separator it is treated as a
 * direct path and probed in place. Otherwise each PATH entry is
 * checked for an executable `name`. Returns the resolved absolute-ish
 * path, or null if nothing executable is found.
 *
 * @param {string} name
 * @returns {Promise<string|null>}
 */
export async function findOnPath(name) {
  if (name.includes(path.sep)) {
    try {
      await access(name, fsConstants.X_OK);
      return name;
    } catch {
      return null;
    }
  }
  const dirs = (process.env.PATH || "").split(path.delimiter).filter(Boolean);
  for (const dir of dirs) {
    const candidate = path.join(dir, name);
    try {
      await access(candidate, fsConstants.X_OK);
      return candidate;
    } catch {
      // keep looking
    }
  }
  return null;
}

/**
 * Pure resolver for which `human` binary to invoke.
 *
 * Decision order (I/O is injected via `findOnPath` and `ensureBinary`
 * so this is unit-testable without a real PATH or a real download):
 *
 *   1. An explicitly-chosen binary (constructor arg or HUMAN_BIN) is
 *      returned as-is — NEVER triggers a download, even if missing.
 *   2. Otherwise, if `findOnPath(humanBin)` resolves, use it.
 *   3. Otherwise (nothing explicit, nothing on PATH) fall back to
 *      `ensureBinary()` — the platform-matched auto-download.
 *
 * @param {object} deps
 * @param {boolean} deps.explicit      Caller passed humanBin or set HUMAN_BIN.
 * @param {string}  deps.humanBin      Configured binary name/path.
 * @param {(name:string)=>Promise<string|null>} deps.findOnPath PATH lookup.
 * @param {()=>Promise<string>} deps.ensureBinary Last-resort downloader.
 * @returns {Promise<string>} the resolved binary path.
 */
export async function resolveBinary({ explicit, humanBin, findOnPath: find, ensureBinary }) {
  if (explicit) return humanBin;
  const onPath = await find(humanBin);
  if (onPath) return onPath;
  return await ensureBinary();
}

/**
 * Subprocess-based binding to the `human hula` CLI.
 *
 * Construct once and reuse. Each call to `validate` / `run` /
 * `compile` / `expand` / `replay` spawns a fresh `human` process.
 * Concurrency-safe (no shared state across calls).
 *
 * Binary resolution is LAZY: construction never touches the network.
 * The binary is resolved on first use via `resolveBinary` and cached.
 */
export class HuLa {
  /**
   * @param {string} [humanBin]
   *   Path to the `human` binary. Defaults to the HUMAN_BIN env var
   *   if set, otherwise the first `human` on PATH. If neither is set
   *   and nothing is on PATH, a platform-matched binary is
   *   auto-downloaded on first use (see ./binary.js ensureBinary). An
   *   explicitly-passed `humanBin` or HUMAN_BIN always wins and never
   *   downloads.
   */
  constructor(humanBin) {
    // Whether the caller explicitly chose a binary. Captured at
    // construction so a later process.env mutation can't retroactively
    // turn an implicit default into an "explicit" choice mid-session.
    this._explicit = humanBin != null || process.env.HUMAN_BIN != null;
    this.humanBin = humanBin || process.env.HUMAN_BIN || "human";
    // Lazily resolved on first use; cached thereafter (a Promise so
    // concurrent first calls share one resolution).
    this._resolvedBin = null;
  }

  /**
   * Resolve (and cache) the binary path, downloading as a last resort.
   * Cheap on the common path (explicit or on PATH); only the
   * genuinely-missing case performs network I/O, and only once.
   *
   * @returns {Promise<string>}
   */
  _resolveBin() {
    if (this._resolvedBin === null) {
      this._resolvedBin = resolveBinary({
        explicit: this._explicit,
        humanBin: this.humanBin,
        findOnPath,
        // Import lazily so merely constructing a HuLa never loads the
        // download machinery and offline users who supply a binary
        // never pay for it.
        ensureBinary: async () => {
          const mod = await import("./binary.js");
          return mod.ensureBinary();
        },
      });
    }
    return this._resolvedBin;
  }

  /**
   * Write `program` to a temp file and run `human hula <verb>` on
   * it. Always cleans up the temp file.
   */
  async _runWithProgram(verb, program) {
    const dir = await mkdtemp(path.join(tmpdir(), "hula-"));
    const tmpPath = path.join(dir, `${verb}.hula.json`);
    try {
      await writeFile(tmpPath, JSON.stringify(program), "utf8");
      const bin = await this._resolveBin();
      return await spawnCapture(bin, ["hula", verb, tmpPath]);
    } finally {
      try { await unlink(tmpPath); } catch { /* ignore */ }
    }
  }

  /**
   * Validate a HuLa program (structure, refs, depth, cycles).
   * Returns a HulaResult; check `.ok`. `.stderr` has the reason on
   * failure.
   *
   * @param {object} program
   * @returns {Promise<HulaResult>}
   */
  validate(program) {
    return this._runWithProgram("validate", program);
  }

  /**
   * Execute a HuLa program with the CLI's built-in demo tools
   * (echo, search, write, analyze). For production execution with
   * real tools, embed the C SDK directly (see
   * include/human/hula_sdk.h).
   *
   * @param {object} program
   * @returns {Promise<HulaResult>}
   */
  run(program) {
    return this._runWithProgram("run", program);
  }

  /**
   * Return the canonical HuLa JSON Schema. `result.stdout` contains
   * the schema's path on the first line followed by the schema body.
   *
   * @returns {Promise<HulaResult>}
   */
  async schema() {
    const bin = await this._resolveBin();
    return spawnCapture(bin, ["hula", "schema"]);
  }

  /**
   * Expand `{{key}}` placeholders in `template` using `vars`.
   * Wraps `human hula expand <tmpl> <vars.json>`.
   *
   * @param {string} template
   * @param {object} vars
   * @returns {Promise<HulaResult>}
   */
  async expand(template, vars) {
    const dir = await mkdtemp(path.join(tmpdir(), "hula-"));
    const tmplPath = path.join(dir, "tmpl.txt");
    const varsPath = path.join(dir, "vars.json");
    try {
      await writeFile(tmplPath, template, "utf8");
      await writeFile(varsPath, JSON.stringify(vars), "utf8");
      const bin = await this._resolveBin();
      return await spawnCapture(bin, ["hula", "expand", tmplPath, varsPath]);
    } finally {
      try { await unlink(tmplPath); } catch { /* ignore */ }
      try { await unlink(varsPath); } catch { /* ignore */ }
    }
  }

  /**
   * Compile a HuLa source to canonical JSON. With `lite: true`,
   * `source` is treated as lite-syntax. With `lite: false` (default),
   * it's expected to be canonical HuLa JSON which the CLI normalizes.
   *
   * Note: this is a syntactic transform, NOT LLM-driven synthesis.
   * LLM-driven program synthesis is on the M5 Phase 3 roadmap.
   *
   * @param {string} source
   * @param {{ lite?: boolean }} [opts]
   * @returns {Promise<HulaResult>}
   */
  async compile(source, opts = {}) {
    const lite = !!opts.lite;
    const dir = await mkdtemp(path.join(tmpdir(), "hula-"));
    const suffix = lite ? ".hula" : ".hula.json";
    const tmpPath = path.join(dir, `compile${suffix}`);
    try {
      await writeFile(tmpPath, source, "utf8");
      const argv = ["hula", "compile"];
      if (lite) argv.push("--lite");
      argv.push(tmpPath);
      const bin = await this._resolveBin();
      return await spawnCapture(bin, argv);
    } finally {
      try { await unlink(tmpPath); } catch { /* ignore */ }
    }
  }

  /**
   * Re-run an embedded HuLa program from a captured trace.
   *
   * @param {object} trace        Trace JSON (typically from a prior
   *                              `run` with HU_HULA_TRACE_DIR set)
   * @param {{ configPath?: string }} [opts]
   * @returns {Promise<HulaResult>}
   */
  async replay(trace, opts = {}) {
    const dir = await mkdtemp(path.join(tmpdir(), "hula-"));
    const tmpPath = path.join(dir, "trace.json");
    try {
      await writeFile(tmpPath, JSON.stringify(trace), "utf8");
      const argv = ["hula", "replay"];
      if (opts.configPath) argv.push("--config", opts.configPath);
      argv.push(tmpPath);
      const bin = await this._resolveBin();
      return await spawnCapture(bin, argv);
    } finally {
      try { await unlink(tmpPath); } catch { /* ignore */ }
    }
  }
}
