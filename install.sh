#!/bin/sh
# seaclaw install script — run with: curl -fsSL https://seaclaw.ai/install.sh | sh
# POSIX-compatible, no bash required

# ANSI colors (work without tput where unavailable)
# shellcheck disable=SC2039
green() { [ -t 1 ] && printf '\033[32m%s\033[0m\n' "$1" || printf '%s\n' "$1"; }
# shellcheck disable=SC2039
red()   { [ -t 1 ] && printf '\033[31m%s\033[0m\n' "$1" || printf '%s\n' "$1"; }
# shellcheck disable=SC2039
bold()  { [ -t 1 ] && printf '\033[1m%s\033[0m' "$1" || printf '%s' "$1"; }

REPO="sethdford/seaclaw"
INSTALL_URL="https://seaclaw.ai/install.sh"

print_help() {
    bold "seaclaw "
    printf "— install script\n\n"
    printf "Usage: curl -fsSL %s | sh\n" "$INSTALL_URL"
    printf "       curl -fsSL %s | sh -s -- --help\n\n" "$INSTALL_URL"
    printf "Options:\n"
    printf "  --help    Show this help and exit\n\n"
    printf "Detects OS (Linux/macOS) and arch (x86_64/aarch64), downloads the\n"
    printf "appropriate binary from GitHub releases, and installs to\n"
    printf "/usr/local/bin (or ~/.local/bin if not writable).\n"
}

detect_arch() {
    a=$(uname -m 2>/dev/null)
    case "$a" in
        x86_64|amd64)  echo "x86_64" ;;
        aarch64|arm64)  echo "aarch64" ;;
        *) red "Unsupported architecture: $a" ; exit 1 ;;
    esac
}

detect_os() {
    os=$(uname -s 2>/dev/null)
    case "$os" in
        Linux)  echo "linux" ;;
        Darwin) echo "macos" ;;
        *)      red "Unsupported OS: $os" ; exit 1 ;;
    esac
}

download() {
    url="$1"
    out="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "$out" "$url"
        return $?
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$out" "$url"
        return $?
    else
        red "Neither curl nor wget found. Please install one of them."
        exit 1
    fi
}

main() {
    for arg in "$@"; do
        case "$arg" in
            --help|-h) print_help; exit 0 ;;
        esac
    done

    arch=$(detect_arch)
    os=$(detect_os)

    case "${os}-${arch}" in
        linux-x86_64)   bin_name="seaclaw-linux-x86_64.bin" ;;
        linux-aarch64)  bin_name="seaclaw-linux-aarch64.bin" ;;
        macos-aarch64)  bin_name="seaclaw-macos-aarch64.bin" ;;
        *)
            red "Unsupported platform: $os $arch"
            exit 1
            ;;
    esac

    # Resolve version from latest GitHub release
    api_url="https://api.github.com/repos/$REPO/releases/latest"
    version=""
    if command -v curl >/dev/null 2>&1; then
        version=$(curl -fsSL "$api_url" 2>/dev/null | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1)
    elif command -v wget >/dev/null 2>&1; then
        version=$(wget -q -O - "$api_url" 2>/dev/null | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1)
    fi

    if [ -z "$version" ]; then
        red "Could not fetch latest release version. Check network or try again later."
        exit 1
    fi

    url="https://github.com/$REPO/releases/download/$version/$bin_name"
    tmpdir="${TMPDIR:-/tmp}"
    tmp="$tmpdir/seaclaw-$$.bin"

    printf "Installing seaclaw %s (%s-%s)...\n" "$version" "$os" "$arch"
    if ! download "$url" "$tmp"; then
        red "Download failed: $url"
        rm -f "$tmp" 2>/dev/null
        exit 1
    fi

    # Choose install dir
    install_dir="/usr/local/bin"
    if [ ! -w "/usr/local/bin" ] 2>/dev/null; then
        install_dir="$HOME/.local/bin"
        mkdir -p "$install_dir" 2>/dev/null
        if [ ! -d "$install_dir" ] || [ ! -w "$install_dir" ]; then
            red "Cannot write to /usr/local/bin or $install_dir. Run with sudo or fix permissions."
            rm -f "$tmp" 2>/dev/null
            exit 1
        fi
    fi

    install_path="$install_dir/seaclaw"
    if ! mv "$tmp" "$install_path" 2>/dev/null; then
        red "Failed to move binary to $install_path"
        rm -f "$tmp" 2>/dev/null
        exit 1
    fi

    chmod +x "$install_path"

    green "seaclaw v$version installed to $install_path"

    if [ "$install_dir" = "$HOME/.local/bin" ]; then
        printf "\nEnsure %s is in your PATH:\n" "$install_dir"
        printf "  export PATH=\"\$PATH:%s\"\n" "$install_dir"
    fi

    if "$install_path" --version 2>/dev/null; then
        green "Run 'seaclaw help' to get started."
    fi
}

main "$@"
