#!/usr/bin/env node
/**
 * Figma Variables API Sync
 *
 * Pushes SeaClaw design tokens to Figma Variables, keeping the design file
 * in sync with code. Also exports Tokens Studio-compatible format.
 *
 * Usage:
 *   npx tsx design-tokens/figma-sync.ts --export   # Export Tokens Studio format
 *   npx tsx design-tokens/figma-sync.ts --push      # Push to Figma Variables API
 *     --file-key <FIGMA_FILE_KEY>
 *     --token <FIGMA_ACCESS_TOKEN>
 *
 * Environment variables:
 *   FIGMA_FILE_KEY    — Figma file key
 *   FIGMA_ACCESS_TOKEN — Figma personal access token
 */

import * as fs from "fs";
import * as path from "path";
import { fileURLToPath } from "url";

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const TOKENS_DIR = SCRIPT_DIR;
const OUT_DIR = path.resolve(SCRIPT_DIR, "..", "docs");

const TOKEN_FILES = [
  "base.tokens.json",
  "semantic.tokens.json",
  "typography.tokens.json",
  "motion.tokens.json",
  "glass.tokens.json",
  "components.tokens.json",
  "opacity.tokens.json",
  "elevation.tokens.json",
  "breakpoints.tokens.json",
];

interface FigmaVariable {
  name: string;
  resolvedType: "COLOR" | "FLOAT" | "STRING";
  valuesByMode: Record<
    string,
    string | number | { r: number; g: number; b: number; a: number }
  >;
}

function hexToFigmaColor(
  hex: string,
): { r: number; g: number; b: number; a: number } | null {
  const m = hex.match(/^#([0-9a-fA-F]{6})$/);
  if (!m) return null;
  return {
    r: parseInt(m[1].substring(0, 2), 16) / 255,
    g: parseInt(m[1].substring(2, 4), 16) / 255,
    b: parseInt(m[1].substring(4, 6), 16) / 255,
    a: 1,
  };
}

function collectFlatTokens(
  obj: Record<string, unknown>,
  prefix = "",
): Record<string, unknown> {
  const result: Record<string, unknown> = {};
  for (const [key, val] of Object.entries(obj)) {
    if (key.startsWith("$")) continue;
    const fullKey = prefix ? `${prefix}/${key}` : key;
    if (
      val &&
      typeof val === "object" &&
      "$value" in (val as Record<string, unknown>)
    ) {
      result[fullKey] = (val as Record<string, unknown>).$value;
    } else if (val && typeof val === "object") {
      Object.assign(
        result,
        collectFlatTokens(val as Record<string, unknown>, fullKey),
      );
    }
  }
  return result;
}

function exportTokensStudio(): void {
  const tokensStudioFormat: Record<string, Record<string, unknown>> = {};

  for (const file of TOKEN_FILES) {
    const p = path.join(TOKENS_DIR, file);
    if (!fs.existsSync(p)) continue;
    const data = JSON.parse(fs.readFileSync(p, "utf-8"));
    const setName = file.replace(".tokens.json", "");
    tokensStudioFormat[setName] = {};

    for (const [key, val] of Object.entries(data)) {
      if (key.startsWith("$")) continue;
      tokensStudioFormat[setName][key] = val;
    }
  }

  const outPath = path.join(OUT_DIR, "tokens-studio.json");
  if (!fs.existsSync(OUT_DIR)) fs.mkdirSync(OUT_DIR, { recursive: true });
  fs.writeFileSync(outPath, JSON.stringify(tokensStudioFormat, null, 2));
  console.log(`Wrote Tokens Studio format: ${outPath}`);
}

function buildFigmaVariables(): FigmaVariable[] {
  const variables: FigmaVariable[] = [];

  for (const file of TOKEN_FILES) {
    const p = path.join(TOKENS_DIR, file);
    if (!fs.existsSync(p)) continue;
    const data = JSON.parse(fs.readFileSync(p, "utf-8"));
    const flat = collectFlatTokens(data);

    for (const [key, val] of Object.entries(flat)) {
      const name = key.replace(/\//g, "/");

      if (typeof val === "string" && val.startsWith("#")) {
        const color = hexToFigmaColor(val);
        if (color) {
          variables.push({
            name,
            resolvedType: "COLOR",
            valuesByMode: { Default: color },
          });
        }
      } else if (typeof val === "number") {
        variables.push({
          name,
          resolvedType: "FLOAT",
          valuesByMode: { Default: val },
        });
      } else if (typeof val === "string") {
        variables.push({
          name,
          resolvedType: "STRING",
          valuesByMode: { Default: val },
        });
      }
    }
  }

  return variables;
}

async function pushToFigma(fileKey: string, token: string): Promise<void> {
  const variables = buildFigmaVariables();

  console.log(`Prepared ${variables.length} variables for Figma sync`);
  console.log(`Target file: ${fileKey}`);

  const payload = {
    variableCollections: [
      {
        action: "CREATE",
        id: "",
        name: "SeaClaw Design Tokens",
        initialModeId: "",
        modes: [{ action: "CREATE" as const, id: "", name: "Default" }],
      },
    ],
    variables: variables.map((v) => ({
      action: "CREATE" as const,
      id: "",
      name: v.name,
      variableCollectionId: "",
      resolvedType: v.resolvedType,
    })),
    variableModeValues: variables.map((v) => ({
      variableId: "",
      modeId: "",
      value: v.valuesByMode.Default,
    })),
  };

  const response = await fetch(
    `https://api.figma.com/v1/files/${fileKey}/variables`,
    {
      method: "POST",
      headers: {
        "X-Figma-Token": token,
        "Content-Type": "application/json",
      },
      body: JSON.stringify(payload),
    },
  );

  if (!response.ok) {
    const body = await response.text();
    console.error(`Figma API error (${response.status}): ${body}`);
    console.error("\nTo use this script, ensure:");
    console.error("  1. FIGMA_ACCESS_TOKEN has Variables write scope");
    console.error("  2. FIGMA_FILE_KEY points to a valid Figma file");
    process.exit(1);
  }

  const result = await response.json();
  console.log(`Pushed ${variables.length} variables to Figma`);
  console.log(JSON.stringify(result, null, 2));
}

async function main() {
  const args = process.argv.slice(2);

  if (args.includes("--export")) {
    exportTokensStudio();
    return;
  }

  if (args.includes("--push")) {
    const fileKeyIdx = args.indexOf("--file-key");
    const tokenIdx = args.indexOf("--token");

    const fileKey =
      (fileKeyIdx >= 0 ? args[fileKeyIdx + 1] : null) ||
      process.env.FIGMA_FILE_KEY;
    const token =
      (tokenIdx >= 0 ? args[tokenIdx + 1] : null) ||
      process.env.FIGMA_ACCESS_TOKEN;

    if (!fileKey || !token) {
      console.error(
        "Missing --file-key or --token (or FIGMA_FILE_KEY / FIGMA_ACCESS_TOKEN env vars)",
      );
      process.exit(1);
    }

    await pushToFigma(fileKey, token);
    return;
  }

  console.log("Usage:");
  console.log("  npx tsx design-tokens/figma-sync.ts --export");
  console.log(
    "  npx tsx design-tokens/figma-sync.ts --push --file-key <KEY> --token <TOKEN>",
  );
}

main();
