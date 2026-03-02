# SeaClaw Homebrew Formula

SeaClaw is an autonomous AI assistant runtime written in C11. This directory contains a Homebrew formula for local installation.

## Installation

Since there is no stable release tag yet, the formula is **head-only** and installs from the latest Git revision.

### From the local repository (development)

```bash
# Install directly from the local formula file
brew install --HEAD /Users/sethford/Documents/nullclaw/Formula/seaclaw.rb
```

Or, using the repo path relative to your clone:

```bash
cd /path/to/nullclaw
brew install --HEAD ./Formula/seaclaw.rb
```

### Optional: curl support

The formula builds with SQLite enabled by default. HTTP provider support via curl is optional:

```bash
brew install --HEAD --with-curl ./Formula/seaclaw.rb
```

## Requirements

- **cmake** (build dependency)
- **sqlite** (required for memory backend)
- **curl** (optional, for HTTP provider support)

## Verification

After installation, run:

```bash
seaclaw --help
seaclaw --version
```
