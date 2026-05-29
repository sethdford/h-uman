---
title: Installation Guide for Human
created: 2026-05-24
status: operator-facing
---

# Installation Guide for Human

**Last updated:** May 2026 (human 0.1.0+)

This guide walks you through installing human on macOS (Intel and Apple Silicon), verifying the installation, and uninstalling if needed. Choose the path that matches your setup:

- **macOS .pkg installer** — easiest, recommended for most users
- **Homebrew** — if you prefer package management
- **From source** — for developers

---

## Quick Start: Install from macOS .pkg

### Step 1: Download

Visit [human's latest release](https://github.com/sethdford/h-uman/releases/latest) and download the .pkg installer for your architecture:

- **Apple Silicon (M1, M2, M3, etc.):** `human-macos-arm64.pkg`
- **Intel Mac:** `human-macos-x86_64.pkg`

Alternatively, use the stable download link:

```bash
# Apple Silicon
curl -L -o ~/Downloads/human-latest.pkg \
  https://github.com/sethdford/h-uman/releases/latest/download/human-macos-arm64.pkg

# Intel Mac
curl -L -o ~/Downloads/human-latest.pkg \
  https://github.com/sethdford/h-uman/releases/latest/download/human-macos-x86_64.pkg
```

### Step 2: Install

1. Open **Finder** and navigate to **Downloads**.
2. Double-click `human-macos-arm64.pkg` (or the x86_64 version).
3. Follow the installer prompts and click **Install**.
4. Enter your macOS password when prompted.
5. The app installs to `/Applications/Human.app`.

### Step 3: Address Gatekeeper Warnings

You may see a Gatekeeper warning: **"human cannot be opened because it is from an unidentified developer."**

This is expected and safe. The human installer is notarized by Apple. To proceed:

1. Click **Cancel** in the warning dialog (don't click "Move to Trash").
2. Open **System Settings** → **General** → **Security & Privacy**.
3. Scroll down to **Human** and click **Open**.
4. Confirm the action in the dialog that appears.

Alternatively, right-click the app in Finder and select **Open** → **Open** (this bypasses the warning without a System Settings trip).

**Why this happens:** Apple requires Developer ID signing to suppress these warnings. human is signed and notarized; the warning is a side effect of how macOS verifies first-party apps. See [Apple's Gatekeeper documentation](https://support.apple.com/en-us/102445) for details.

### Step 4: Grant System Permissions

Human needs two permissions to function fully on macOS:

#### Full Disk Access

This allows human to index your chat database and build search results. **Optional but recommended** for search functionality.

1. Open **System Settings** → **Privacy & Security** → **Full Disk Access**.
2. Click **+** to add an app.
3. Navigate to `/Applications/Human.app` and select it.
4. Restart the human daemon (see **Verify Installation** below).

Without this permission, human can still chat and execute tools; search will be limited.

#### Accessibility

This allows human to send tapbacks and emoji reactions in iMessage. **Required** if you use iMessage on macOS.

1. Open **System Settings** → **Privacy & Security** → **Accessibility**.
2. Click **+** to add an app.
3. Navigate to `/Applications/Human.app` and select it.
4. Restart the human daemon.

### Step 5: Verify Installation

Run the human CLI to confirm everything works:

```bash
/Applications/Human.app/Contents/MacOS/human --version
```

You should see output like:

```
human 0.1.0 (macos-arm64, built 2026-05-24)
```

To start the daemon and check status:

```bash
/Applications/Human.app/Contents/MacOS/human_daemon &
/Applications/Human.app/Contents/MacOS/human doctor
```

The `human doctor` command (coming in Sprint C3) will perform a comprehensive health check: daemon connectivity, permissions, configuration, and system requirements. For now, a successful `--version` output confirms the binary is installed and working.

---

## Homebrew Installation

If you prefer package management via Homebrew:

```bash
brew install human
```

This installs:

- `/usr/local/bin/human` — the CLI binary
- `/usr/local/libexec/human_daemon` — the daemon
- `~/Library/LaunchAgents/com.human.daemon.plist` — automatic startup on login

The daemon starts automatically on next login. To start it now:

```bash
launchctl load ~/Library/LaunchAgents/com.human.daemon.plist
human --version
```

For permissions (Full Disk Access, Accessibility), follow the same steps in the .pkg section above.

---

## Building from Source

If you're a developer or want to build the latest unreleased code:

1. Clone the repository:
   ```bash
   git clone https://github.com/sethdford/h-uman.git
   cd h-uman
   ```

2. Build using CMake (see `CMakePresets.json` and the root `CLAUDE.md` Build & Test section for full details):
   ```bash
   cmake --preset dev
   cmake --build --preset dev
   ```

3. Run the CLI or daemon:
   ```bash
   ./build/human --version
   ./build/human_daemon
   ```

4. To install to `/Applications/Human.app`:
   ```bash
   cmake --preset release
   cmake --build --preset release
   # Follow the installer steps from the .pkg section above, or copy manually:
   cp -r build/Release/Human.app /Applications/
   ```

See `CMakePresets.json` and the root `CLAUDE.md` Build & Test section for advanced build options (TUI, minimal binary, no-SQLite, profiling).

---

## Troubleshooting

### Problem: "human cannot be opened" (Gatekeeper warning)

**Solution:** See the **Gatekeeper Warnings** section above. The app is safe; you need to manually approve it in System Settings or via right-click-Open.

### Problem: "Permission denied" when using iMessage tapbacks or reactions

**Symptoms:** Error message like `hu_err_permission_denied` in logs when trying to send a reaction.

**Solution:** Grant Accessibility permission (see **Accessibility** section above). Restart the daemon:

```bash
launchctl unload ~/Library/LaunchAgents/com.human.daemon.plist
launchctl load ~/Library/LaunchAgents/com.human.daemon.plist
```

### Problem: Daemon won't start

**Symptoms:** `human --version` works, but `human_daemon` exits immediately or hangs.

**Troubleshooting:**

1. Check the daemon log:
   ```bash
   log stream --predicate 'process == "human_daemon"' --level debug
   ```

2. Verify the config file exists and is valid JSON:
   ```bash
   cat ~/.human/config.json | jq .
   ```

3. Check if port 9999 (default) is in use:
   ```bash
   lsof -i :9999
   ```

**Solution:** See `human doctor` (Sprint C3) for comprehensive diagnostics.

### Problem: "human doctor" command not found

**Status:** `human doctor` is a forthcoming diagnostic tool landing in Sprint C3 of the distribution roadmap. For now, check logs and config manually using the troubleshooting steps above.

### Problem: Chat database not indexed / search is slow

**Symptoms:** Searching past conversations returns no results.

**Solution:** Grant Full Disk Access (see **Full Disk Access** section above). Restart the daemon. The indexer will rebuild the search index on next startup.

---

## Uninstalling Human

### Via .pkg or Homebrew

1. **Remove the app bundle:**
   ```bash
   rm -rf /Applications/Human.app
   ```

2. **Remove daemon and config (optional):**
   ```bash
   rm -rf ~/.human
   rm ~/Library/LaunchAgents/com.human.daemon.plist
   ```

3. **If installed via Homebrew:**
   ```bash
   brew uninstall human
   ```

A forthcoming `human uninstall` command (Sprint TBD) will automate this process. For now, the manual steps above fully remove the app.

### Revoking System Permissions

To revoke Full Disk Access or Accessibility permissions:

1. Open **System Settings** → **Privacy & Security** → **Full Disk Access** (or **Accessibility**).
2. Find **Human** in the list and click **-** to remove it.

---

## Image Placeholders

The following screenshots document the installer experience. They will be captured and added in a follow-up update (Sprint C5).

### Gatekeeper Prompt

**Placeholder:** `docs/guides/img/install-gatekeeper-prompt.png`

*Captures the Gatekeeper warning dialog ("human cannot be opened because it is from an unidentified developer") and the path to System Settings > Privacy & Security to approve the app.*

### Full Disk Access Permission

**Placeholder:** `docs/guides/img/install-full-disk-access.png`

*Captures System Settings > Privacy & Security > Full Disk Access, showing Human.app added to the allowlist.*

### Successful Installation

**Placeholder:** `docs/guides/img/install-success.png`

*Captures the terminal output of `human --version` and `human doctor` (once available) confirming the daemon is running.*

---

## Next Steps

After installation:

1. **Configure your profile:** Edit `~/.human/config.json` to add your AI provider (OpenAI, Gemini, or another OpenAI-compatible endpoint) and personalization settings.
2. **Start the daemon:** `launchctl load ~/Library/LaunchAgents/com.human.daemon.plist` (automatic if installed via Homebrew or .pkg with launchd enabled).
3. **Chat:** Use the CLI or your preferred messaging client (Slack, Discord, Telegram, iMessage, etc.) to interact with human.
4. **Run `human doctor`:** Once Sprint C3 lands, run `human doctor` for a comprehensive health check and personalized recommendations.

See [README.md](../../README.md) for feature overview and [h-uman.ai](https://h-uman.ai) for full documentation.
