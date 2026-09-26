import { test } from "node:test";
import assert from "node:assert/strict";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import {
  TOKEN_FILES,
  collectTokens,
  partitionByPlatform,
  platformOf,
  readTokenSources,
  type TokenSource,
} from "../token-lib.js";

const DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

test("every real token file declares platform 'all'", () => {
  const sources = readTokenSources(DIR, TOKEN_FILES);
  assert.equal(sources.length, TOKEN_FILES.length);
  for (const s of sources) assert.equal(platformOf(s), "all", s.file);
});

test("platformOf throws when the key is missing, naming the file", () => {
  const src: TokenSource = { file: "x.tokens.json", data: { $extensions: {} } };
  assert.throws(() => platformOf(src), /x\.tokens\.json.*com\.human\.platform/);
});

test("platformOf throws on an unknown platform value", () => {
  const src: TokenSource = {
    file: "y.tokens.json",
    data: { $extensions: { "com.human.platform": "ios" } },
  };
  assert.throws(() => platformOf(src), /y\.tokens\.json.*"ios"/);
});

test("partitionByPlatform separates web from shared", () => {
  const a: TokenSource = { file: "a", data: { $extensions: { "com.human.platform": "all" } } };
  const w: TokenSource = { file: "w", data: { $extensions: { "com.human.platform": "web" } } };
  const { shared, web } = partitionByPlatform([a, w]);
  assert.deepEqual(shared.map((s) => s.file), ["a"]);
  assert.deepEqual(web.map((s) => s.file), ["w"]);
});

test("collectTokens skips $-prefixed keys and flattens $value leaves", () => {
  const map = collectTokens({
    $description: "ignored",
    light: { bg: { $value: "#fff", $type: "color" } },
  });
  assert.deepEqual(map, { "light.bg": "#fff" });
});
