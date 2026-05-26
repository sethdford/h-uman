/**
 * Hello-world example: validate + run a 1-node HuLa program.
 *
 * Run from the repo root:
 *
 *   cmake --build build --target human   # ensure ./build/human exists
 *   HUMAN_BIN=./build/human node apps/node-sdk/examples/hello_hula.js
 */

import { HuLa, HULA_SDK_VERSION_STRING } from "../src/index.js";

async function main() {
  console.log(`@human/hula-sdk v${HULA_SDK_VERSION_STRING}`);

  // Minimal HuLa program: one EMIT node that produces a greeting slot.
  const program = {
    name: "hello",
    version: 1,
    root: {
      id: "n1",
      op: "emit",
      emit_key: "greeting",
      emit_value: "hello from the Node SDK",
    },
  };

  const hula = new HuLa();

  // Step 1: validate structure
  const v = await hula.validate(program);
  if (!v.ok) {
    console.error(`validate failed (rc=${v.returncode}):`);
    console.error(v.stderr);
    return v.returncode;
  }
  console.log("validate: ok");

  // Step 2: execute with the CLI's built-in demo tools
  const r = await hula.run(program);
  if (!r.ok) {
    console.error(`run failed (rc=${r.returncode}):`);
    console.error(r.stderr);
    return r.returncode;
  }
  console.log("run: ok");
  console.log("--- run stdout ---");
  console.log(r.stdout);
  return 0;
}

process.exit(await main());
