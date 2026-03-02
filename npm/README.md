# seaclaw

Autonomous AI assistant runtime in C11. 239 KB binary. Zero dependencies. 50+ providers.

## Install

```bash
npm install -g seaclaw
```

This downloads a pre-built binary for your platform (macOS ARM/x86, Linux x86).

## Usage

```bash
seaclaw onboard --interactive  # first-time setup
seaclaw doctor                 # verify configuration
seaclaw agent -m "hello"       # send a message
```

## Build from source

```bash
git clone https://github.com/sethdford/seaclaw.git
cd seaclaw && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON
cmake --build .
```

## Links

- [Documentation](https://sethdford.github.io/seaclaw/)
- [GitHub](https://github.com/sethdford/seaclaw)

## License

MIT
