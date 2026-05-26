/**
 * Smoke tests for the Node.js SDK. Uses node:test (stdlib) so no
 * npm install is needed for the test runner itself.
 *
 * Run from the repo root after building the `human` binary:
 *
 *   cmake --build build --target human
 *   HUMAN_BIN=./build/human node --test apps/node-sdk/test/
 */

import { test } from "node:test";
import assert from "node:assert/strict";
import { access, constants } from "node:fs/promises";
import path from "node:path";

import {
  HuLa,
  HULA_SDK_VERSION_STRING,
} from "../src/index.js";

// Skip the binary-dependent tests if the `human` binary isn't on disk.
// Matches the pattern used by the Python SDK smoke tests.
async function humanBinAvailable() {
  const binPath = process.env.HUMAN_BIN || "human";
  // If it's a path with a directory component, check the file directly.
  if (path.isAbsolute(binPath) || binPath.includes(path.sep) || binPath.includes("/")) {
    try {
      await access(binPath, constants.X_OK);
      return true;
    } catch {
      return false;
    }
  }
  // Bare basename — walk PATH.
  const PATH = process.env.PATH || "";
  for (const dir of PATH.split(path.delimiter)) {
    try {
      await access(path.join(dir, binPath), constants.X_OK);
      return true;
    } catch { /* try next */ }
  }
  return false;
}

const HUMAN_AVAILABLE = await humanBinAvailable();

const skipUnlessHuman = HUMAN_AVAILABLE
  ? {}
  : { skip: "human binary not on PATH or HUMAN_BIN; build with " +
            "`cmake --build build --target human` and set HUMAN_BIN" };

test("HULA_SDK_VERSION_STRING is a MAJOR.MINOR.PATCH semver", () => {
  assert.equal(typeof HULA_SDK_VERSION_STRING, "string");
  const parts = HULA_SDK_VERSION_STRING.split(".");
  assert.equal(parts.length, 3,
               `expected MAJOR.MINOR.PATCH, got ${HULA_SDK_VERSION_STRING}`);
  for (const p of parts) {
    assert.match(p, /^\d+$/, `non-numeric version part: ${p}`);
  }
});

test("validate accepts a 1-node EMIT program", skipUnlessHuman, async () => {
  const program = {
    name: "smoke",
    version: 1,
    root: {
      id: "n1",
      op: "emit",
      emit_key: "k",
      emit_value: "v",
    },
  };
  const hula = new HuLa();
  const r = await hula.validate(program);
  assert.equal(r.ok, true,
               `validate failed: rc=${r.returncode}, stderr=${r.stderr}`);
});

test("validate rejects program missing root", skipUnlessHuman, async () => {
  const hula = new HuLa();
  const r = await hula.validate({ name: "broken", version: 1 });
  assert.equal(r.ok, false,
               `expected validate to fail on malformed program; got rc=${r.returncode}`);
});

test("schema returns canonical hula-program schema", skipUnlessHuman, async () => {
  const hula = new HuLa();
  const r = await hula.schema();
  assert.equal(r.ok, true,
               `schema failed: rc=${r.returncode}, stderr=${r.stderr}`);
  assert.match(r.stdout, /\$schema/);
  assert.match(r.stdout, /hula-program/);
});

test("expand substitutes {{key}} from vars dict", skipUnlessHuman, async () => {
  const hula = new HuLa();
  const r = await hula.expand("Hi {{name}} from {{place}}.",
                              { name: "Seth", place: "h-uman" });
  assert.equal(r.ok, true,
               `expand failed: rc=${r.returncode}, stderr=${r.stderr}`);
  assert.match(r.stdout, /Hi Seth from h-uman\./);
});

test("compile normalizes canonical-JSON HuLa program", skipUnlessHuman, async () => {
  const program =
    '{"name":"compile_smoke","version":1,' +
    '"root":{"id":"n1","op":"emit","emit_key":"k","emit_value":"v"}}';
  const hula = new HuLa();
  const r = await hula.compile(program);
  assert.equal(r.ok, true,
               `compile failed: rc=${r.returncode}, stderr=${r.stderr}`);
  assert.match(r.stdout, /emit/);
});
