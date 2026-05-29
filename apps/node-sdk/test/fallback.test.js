/**
 * Tests for HuLa binary resolution + auto-download fallback.
 *
 * Uses node:test + node:assert (stdlib). No real download: the pure
 * `resolveBinary` resolver takes `findOnPath` and `ensureBinary` as
 * injected callables, so every decision case is deterministic.
 * `findOnPath` itself is exercised against a real temp dir on PATH.
 */

import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtemp, writeFile, chmod, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";

import { HuLa, resolveBinary, findOnPath } from "../src/index.js";

// A spy that records calls and returns a fixed value.
function spy(returnValue) {
  const fn = async (...args) => {
    fn.calls.push(args);
    return returnValue;
  };
  fn.calls = [];
  return fn;
}

test("resolveBinary", async (t) => {
  await t.test("explicit wins, never consults PATH or downloads (AC#3)", async () => {
    const find = spy("/usr/local/bin/human");
    const ensure = spy("/cache/human-latest");
    const result = await resolveBinary({
      explicit: true,
      humanBin: "/custom/human",
      findOnPath: find,
      ensureBinary: ensure,
    });
    assert.equal(result, "/custom/human");
    assert.equal(find.calls.length, 0);
    assert.equal(ensure.calls.length, 0);
  });

  await t.test("explicit wins even when the path is missing (no download)", async () => {
    const find = spy(null);
    const ensure = spy("/cache/human-latest");
    const result = await resolveBinary({
      explicit: true,
      humanBin: "/no/such/human",
      findOnPath: find,
      ensureBinary: ensure,
    });
    assert.equal(result, "/no/such/human");
    assert.equal(ensure.calls.length, 0);
  });

  await t.test("PATH hit is used; ensureBinary never called (AC#1)", async () => {
    const find = spy("/usr/local/bin/human");
    const ensure = spy("/cache/human-latest");
    const result = await resolveBinary({
      explicit: false,
      humanBin: "human",
      findOnPath: find,
      ensureBinary: ensure,
    });
    assert.equal(result, "/usr/local/bin/human");
    assert.deepEqual(find.calls, [["human"]]);
    assert.equal(ensure.calls.length, 0);
  });

  await t.test("nothing explicit + nothing on PATH triggers download (AC#2)", async () => {
    const find = spy(null);
    const ensure = spy("/cache/human-latest");
    const result = await resolveBinary({
      explicit: false,
      humanBin: "human",
      findOnPath: find,
      ensureBinary: ensure,
    });
    assert.equal(result, "/cache/human-latest");
    assert.deepEqual(find.calls, [["human"]]);
    assert.equal(ensure.calls.length, 1);
  });
});

test("findOnPath", async (t) => {
  await t.test("finds an executable on PATH", async () => {
    const dir = await mkdtemp(path.join(tmpdir(), "humanpath-"));
    const exe = path.join(dir, "human");
    await writeFile(exe, "#!/bin/sh\necho hi\n", "utf8");
    await chmod(exe, 0o755);
    const savedPath = process.env.PATH;
    try {
      process.env.PATH = dir + path.delimiter + (savedPath || "");
      const found = await findOnPath("human");
      assert.equal(found, exe);
    } finally {
      process.env.PATH = savedPath;
      await rm(dir, { recursive: true, force: true });
    }
  });

  await t.test("returns null when not on PATH", async () => {
    const dir = await mkdtemp(path.join(tmpdir(), "humanpath-"));
    const savedPath = process.env.PATH;
    try {
      process.env.PATH = dir; // empty dir only — no `human`
      const found = await findOnPath("definitely-not-a-real-binary-xyz");
      assert.equal(found, null);
    } finally {
      process.env.PATH = savedPath;
      await rm(dir, { recursive: true, force: true });
    }
  });

  await t.test("a path-with-separator is probed directly", async () => {
    const dir = await mkdtemp(path.join(tmpdir(), "humanpath-"));
    const exe = path.join(dir, "human");
    await writeFile(exe, "#!/bin/sh\n", "utf8");
    await chmod(exe, 0o755);
    try {
      assert.equal(await findOnPath(exe), exe);
      assert.equal(await findOnPath(path.join(dir, "missing")), null);
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });
});

test("HuLa wiring", async (t) => {
  const savedEnv = process.env.HUMAN_BIN;
  const clearEnv = () => { delete process.env.HUMAN_BIN; };

  await t.test("explicit arg sets the explicit flag", () => {
    clearEnv();
    const hula = new HuLa("/custom/human");
    assert.equal(hula._explicit, true);
    assert.equal(hula.humanBin, "/custom/human");
    if (savedEnv !== undefined) process.env.HUMAN_BIN = savedEnv;
  });

  await t.test("HUMAN_BIN env sets the explicit flag", () => {
    process.env.HUMAN_BIN = "/env/human";
    const hula = new HuLa();
    assert.equal(hula._explicit, true);
    assert.equal(hula.humanBin, "/env/human");
    if (savedEnv !== undefined) process.env.HUMAN_BIN = savedEnv;
    else clearEnv();
  });

  await t.test("bare default is not explicit", () => {
    clearEnv();
    const hula = new HuLa();
    assert.equal(hula._explicit, false);
    assert.equal(hula.humanBin, "human");
    if (savedEnv !== undefined) process.env.HUMAN_BIN = savedEnv;
  });

  await t.test("explicit binary resolves without download", async () => {
    clearEnv();
    const hula = new HuLa("/custom/human");
    assert.equal(await hula._resolveBin(), "/custom/human");
    if (savedEnv !== undefined) process.env.HUMAN_BIN = savedEnv;
  });

  await t.test("_resolveBin caches the resolution (one promise)", async () => {
    clearEnv();
    const hula = new HuLa("/custom/human");
    const p1 = hula._resolveBin();
    const p2 = hula._resolveBin();
    assert.equal(p1, p2); // same cached promise, not a second resolution
    assert.equal(await p1, "/custom/human");
    if (savedEnv !== undefined) process.env.HUMAN_BIN = savedEnv;
  });
});
