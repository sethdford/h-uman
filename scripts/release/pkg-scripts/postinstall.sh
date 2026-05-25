#!/bin/bash
# Post-install script for human .pkg installer (US-C1.2)
# Runs as root after successful package installation.
# Current directory: /private/tmp/InstallationSandbox.XXXXX/InstallationPayload

set -euo pipefail

# Verify the bundle was installed to /Applications
if [[ ! -d "/Applications/Human.app" ]]; then
    echo "ERROR: Human.app not found at /Applications/Human.app" >&2
    exit 1
fi

# Verify executable exists and is executable
if [[ ! -x "/Applications/Human.app/Contents/MacOS/human" ]]; then
    echo "ERROR: human executable not found or not executable at /Applications/Human.app/Contents/MacOS/human" >&2
    exit 1
fi

# Installation successful
echo "Human installed to /Applications/Human.app"
exit 0
