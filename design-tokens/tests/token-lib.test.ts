import { test } from "node:test";
import assert from "node:assert/strict";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import {
  TOKEN_FILES,
  collectTokens,
  collectTypes,
  partitionByPlatform,
  platformOf,
  readTokenSources,
  resolveRefs,
  type TokenSource,
} from "../token-lib.js";

const DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

test("every real token file declares platform 'all'", () => {
  const sources = readTokenSources(DIR, TOKEN_FILES);
  assert.equal(sources.length, TOKEN_FILES.length);
  for (const s of sources) {
    assert.equal(platformOf(s), s.file === "quiet.tokens.json" ? "web" : "all", s.file);
  }
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

test("collectTypes maps each token path to its own $type", () => {
  assert.deepEqual(
    collectTypes({
      light: { bg: { $value: "#fff", $type: "color" }, gap: { $value: "2px", $type: "dimension" } },
    }),
    { "light.bg": "color", "light.gap": "dimension" },
  );
});

test("collectTypes inherits the nearest group $type (W3C DTCG)", () => {
  assert.deepEqual(
    collectTypes({
      light: {
        $type: "color",
        bg: { $value: "#fff" },
        size: { gap: { $value: "2px", $type: "dimension" } },
        nested: { $type: "dimension", pad: { $value: "4px" } },
      },
    }),
    { "light.bg": "color", "light.size.gap": "dimension", "light.nested.pad": "dimension" },
  );
});

test("resolveRefs follows chains of whole-value references", () => {
  assert.deepEqual(resolveRefs({ a: "{b}", b: "{c}", c: "#000000", d: 2 }), {
    a: "#000000",
    b: "#000000",
    c: "#000000",
    d: 2,
  });
});

test("resolveRefs throws on a reference to a missing token, naming both paths", () => {
  assert.throws(
    () => resolveRefs({ "light.zz": "{light.nope}" }),
    /light\.zz.*\{light\.nope\}/,
  );
});

test("resolveRefs throws on a reference cycle instead of looping", () => {
  assert.throws(() => resolveRefs({ a: "{b}", b: "{a}" }), /circular/);
});

test("resolveRefs leaves non-whole-value braces alone", () => {
  assert.deepEqual(resolveRefs({ g: "linear-gradient({x}, red)" }), {
    g: "linear-gradient({x}, red)",
  });
});
