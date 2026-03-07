# sdk/ — Plugin SDK

Templates and scaffolding for building seaclaw plugins.

## Structure

- `sdk/templates/provider/` — Provider plugin template
- `sdk/templates/channel/` — Channel plugin template
- `sdk/templates/tool/` — Tool plugin template

## Rules

- All plugins implement the vtable pattern (`void *ctx` + function pointers)
- Templates must compile standalone against `include/seaclaw/` headers
- Use `SC_IS_TEST` guards for side effects in test harnesses
- Registration keys: stable, lowercase, user-facing (e.g., `"my_provider"`)
- Follow naming: `sc_<name>_t` for types, `sc_<module>_<action>` for functions

## Adding a New Template

1. Copy the closest existing template directory
2. Replace placeholder names and TODO comments
3. Ensure it compiles: `cmake -B build && cmake --build build`
4. Update `sdk/README.md` with the new template
