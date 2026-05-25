# typed: false
# frozen_string_literal: true

# tap: humanlabs/human
# The line above is the canonical tap path. It is parsed verbatim by
# website/scripts/check-install-matches-formula.mjs to enforce that the
# install one-liner rendered on h-uman.ai never drifts from this formula.
# Do not change without updating website/src/data/install.json in the
# same PR — the drift detector test will fail by design otherwise.

class Human < Formula
  desc "The smallest fully autonomous AI assistant infrastructure"
  homepage "https://h-uman.ai"
  license "MIT"
  version "0.5.0"

  on_macos do
    if Hardware::CPU.arm?
      url "https://github.com/sethdford/h-uman/releases/download/v0.5.0/human-macos-aarch64.bin"
      # TODO: replace with actual release sha256 before shipping
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
    if Hardware::CPU.intel?
      url "https://github.com/sethdford/h-uman/releases/download/v0.5.0/human-macos-x86_64.bin"
      # TODO: replace with actual release sha256 before shipping
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
  end

  on_linux do
    if Hardware::CPU.arm?
      url "https://github.com/sethdford/h-uman/releases/download/v0.5.0/human-linux-aarch64.bin"
      # TODO: replace with actual release sha256 before shipping
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
    if Hardware::CPU.intel?
      url "https://github.com/sethdford/h-uman/releases/download/v0.5.0/human-linux-x86_64.bin"
      # TODO: replace with actual release sha256 before shipping
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
  end

  # TODO(US-8.4): once codesigning + notarization ships, add a `bottle do` block
  # so users get a pre-compiled, ABI-relocatable tarball instead of the raw
  # pre-built binary downloaded via `url`. Bottling an *unsigned* binary makes
  # tamper attribution worse (the bottle SHA covers the tarball, not the inner
  # binary), so this stays deferred until signing lands. See US-9.1 design doc
  # (sprints/sprint-9/designs/US-9.1.md, "Bottle vs build-from-source").

  # Build from source (HEAD or when pre-built binary unavailable)
  head "https://github.com/sethdford/h-uman.git", branch: "main"

  depends_on "cmake" => :build if build.head?
  depends_on "sqlite" if build.head?
  depends_on "curl" => :optional

  on_macos do
    depends_on xcode: ["16.0", :build]
  end

  def install
    if build.head?
      args = %w[
        -DCMAKE_BUILD_TYPE=MinSizeRel
        -DHU_ENABLE_LTO=ON
        -DHU_ENABLE_SQLITE=ON
        -DHU_ENABLE_ALL_CHANNELS=ON
      ]
      args << "-DHU_ENABLE_CURL=#{build.with?("curl") ? "ON" : "OFF"}"

      system "cmake", "-S", ".", "-B", "build", *args, *std_cmake_args
      system "cmake", "--build", "build", "--target", "human", "-j", ENV.make_jobs.to_s
      bin.install "build/human"
    else
      # Pre-built binary: just install it
      bin.install Dir["human-*"].first => "human"
    end

    # Build and install on-device server on macOS
    if OS.mac? && File.exist?("apps/tools/human-ondevice/Package.swift")
      Dir.chdir("apps/tools/human-ondevice") do
        system "swift", "build", "-c", "release"
        bin.install ".build/release/human-ondevice"
      end
    end

    man1.install "docs/man/human.1" if File.exist?("docs/man/human.1")
    man1.install "docs/man/human-gateway.1" if File.exist?("docs/man/human-gateway.1")

    if File.exist?("completions/human.bash")
      bash_completion.install "completions/human.bash" => "human"
    end
    zsh_completion.install "completions/_human" => "_human" if File.exist?("completions/_human")
    fish_completion.install "completions/human.fish" if File.exist?("completions/human.fish")

    # Install launchd plist template on macOS
    if OS.mac?
      plist_template = "scripts/install/human-daemon.plist.template"
      if File.exist?(plist_template)
        # Create launchd plist with substituted paths
        launchd_path = File.expand_path("~/Library/LaunchAgents/com.human.daemon.plist")
        template_content = File.read(plist_template)
        brew_prefix = HOMEBREW_PREFIX
        rendered = template_content
          .gsub("{{BREW_PREFIX}}", brew_prefix)
          .gsub("{{HOME}}", File.expand_path("~"))

        # Create LaunchAgents directory if it doesn't exist
        FileUtils.mkdir_p(File.dirname(launchd_path))
        File.write(launchd_path, rendered)
        # Ensure plist is readable
        File.chmod(0644, launchd_path)
      end
    end
  end

  def post_install
    # On macOS, load the daemon into launchd
    if OS.mac?
      launchd_path = File.expand_path("~/Library/LaunchAgents/com.human.daemon.plist")
      if File.exist?(launchd_path)
        # Unload if already loaded to allow fresh load
        system "launchctl", "unload", launchd_path, out: :null, err: :null
        # Load the daemon
        system "launchctl", "load", launchd_path
        # Verify daemon starts
        sleep 1
        system "#{bin}/human", "--version"
      end
    end
  end

  def caveats
    on_macos do
      <<~EOS
        Installation complete! The human daemon has been installed and configured.

        NEXT STEPS:
          1. Run: human onboard
             This interactive wizard configures your first AI persona and
             messaging channels.

          2. Enable Full Disk Access (required):
             System Settings → Privacy & Security → Full Disk Access
             Add the human binary at: #{HOMEBREW_PREFIX}/bin/human

          3. Verify daemon is running:
             launchctl list | grep com.human.daemon
             (should show PID > 0)

        APPLE INTELLIGENCE (on-device, free):
          If you have macOS 26+ and Apple Silicon, human uses Apple
          Intelligence by default. human-ondevice was built and installed
          alongside human — no third-party dependencies needed.

          Run: human onboard --apple

        LOGS & TROUBLESHOOTING:
          Daemon logs: ~/.human/human.log
          Run: human doctor
             for a full system diagnostic
      EOS
    end
  end

  test do
    # Smoke test: verify daemon binary is installed and callable
    output = shell_output("#{bin}/human --version")
    assert_match version.to_s, output

    # Verify launchd plist is installed on macOS
    if OS.mac?
      launchd_path = File.expand_path("~/Library/LaunchAgents/com.human.daemon.plist")
      assert File.exist?(launchd_path), "launchd plist not installed"
      # Verify plist is valid XML (rough check)
      plist_content = File.read(launchd_path)
      assert plist_content.include?("<?xml"), "plist missing XML declaration"
      assert plist_content.include?("com.human.daemon"), "plist missing correct Label"
    end
  end
end
