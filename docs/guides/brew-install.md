---
title: Installing human via Homebrew
created: 2026-05-24
status: operator-facing
---

# Installing human via Homebrew

The simplest way to install h-uman on macOS is via Homebrew.

## Quick Start

```bash
brew tap humanlabs/human https://github.com/sethdford/h-uman.git
brew install human
```

The formula will:
- Download and install the `human` binary to `$(brew --prefix)/bin/human`
- Create a launchd plist at `~/Library/LaunchAgents/com.human.daemon.plist`
- Auto-load the daemon (it will start automatically on next login)

## After Installation

### 1. Enable Full Disk Access
h-uman requires Full Disk Access to read messages from your messaging channels. This is a macOS security requirement for any app accessing iMessage, Slack, Discord, etc.

1. Open System Settings → Privacy & Security
2. Scroll down to "Full Disk Access"
3. Click the "+" button and add the human binary:
   ```bash
   $(brew --prefix)/bin/human
   ```
4. Restart the daemon:
   ```bash
   launchctl unload ~/Library/LaunchAgents/com.human.daemon.plist
   launchctl load ~/Library/LaunchAgents/com.human.daemon.plist
   ```

### 2. Run the Onboarding Wizard
```bash
human onboard
```

This interactive wizard will:
- Create your AI persona (name, communication style, interests)
- Configure messaging channels (iMessage, Slack, Discord, Telegram, etc.)
- Set up initial preferences

### 3. Verify the Daemon is Running
```bash
launchctl list | grep com.human.daemon
```

You should see output with a PID (process ID) > 0. If the PID is "-", the daemon isn't running — check Full Disk Access permission and try reloading.

## Checking Logs

Daemon logs are written to `~/.human/human.log`. To tail them in real-time:

```bash
tail -f ~/.human/human.log
```

## Uninstalling

To remove h-uman:

```bash
# Stop and unload the daemon
launchctl unload ~/Library/LaunchAgents/com.human.daemon.plist

# Remove the formula
brew uninstall human

# Remove the tap (optional)
brew untap humanlabs/human

# Clean up user data (optional)
rm -rf ~/.human
```

## Troubleshooting

### Daemon won't start
Run the diagnostic tool:
```bash
human doctor
```

This will check:
- Binary is executable
- Full Disk Access is enabled
- Config file is readable
- System has enough disk space

### Building from Source
If you want to build from the latest main branch instead of a release binary:

```bash
brew install human --HEAD
```

This will clone the repository and compile from source.

## For Developers

To test the formula locally:

```bash
# Verify formula syntax
brew audit --strict Formula/human.rb

# Install and test
brew install -s Formula/human.rb
```

## Related

- [Installation Guide](../guides/installation.md) — covers .pkg and source installs
- [Documentation Index](../../docs/CONCEPT_INDEX.md) — full h-uman documentation
