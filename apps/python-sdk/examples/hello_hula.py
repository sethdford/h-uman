"""Hello-world example: validate + run a 1-node HuLa program.

Run from the repo root:

    cmake --build build --target human    # ensure ./build/human exists
    PYTHONPATH=apps/python-sdk \
      HUMAN_BIN=./build/human \
      python3 apps/python-sdk/examples/hello_hula.py
"""

from human import HuLa, HULA_SDK_VERSION_STRING


def main() -> int:
    print(f"human Python SDK v{HULA_SDK_VERSION_STRING}")

    # Minimal HuLa program: one EMIT node that produces a greeting slot.
    program = {
        "version": 1,
        "nodes": [
            {
                "id": "n1",
                "op": "emit",
                "emit_key": "greeting",
                "emit_value": "hello from the Python SDK",
            }
        ],
    }

    hula = HuLa()

    # Step 1: validate structure
    v = hula.validate(program)
    if not v.ok:
        print(f"validate failed (rc={v.returncode}):")
        print(v.stderr)
        return v.returncode

    print("validate: ok")

    # Step 2: execute with the CLI's built-in demo tools
    r = hula.run(program)
    if not r.ok:
        print(f"run failed (rc={r.returncode}):")
        print(r.stderr)
        return r.returncode

    print("run: ok")
    print("--- run stdout ---")
    print(r.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
