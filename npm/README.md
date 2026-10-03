# human

Autonomous AI assistant runtime in C11. <!-- fp:binary_mb -->~3 MB<!-- /fp --> binary. Zero dependencies. 50+ providers.

## Install

```bash
npm install -g human
```

This downloads a pre-built binary for your platform (macOS ARM/x86, Linux x86).

## Usage

```bash
human onboard --interactive  # first-time setup
human doctor                 # verify configuration
human agent -m "hello"       # send a message
```

## Build from source

```bash
git clone https://github.com/sethdford/h-uman.git
cd h-uman && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_LTO=ON
cmake --build .
```

## Links

- [Documentation](https://sethdford.github.io/human/)
- [GitHub](https://github.com/sethdford/h-uman)

## License

MIT
