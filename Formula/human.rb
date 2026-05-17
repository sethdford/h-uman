# typed: false
# frozen_string_literal: true

class Human < Formula
  desc "The smallest fully autonomous AI assistant infrastructure"
  homepage "https://h-uman.ai"
  license "MIT"
  version "0.5.0"

  on_macos do
    if Hardware::CPU.arm?
      url "https://github.com/sethdford/h-uman/releases/download/v0.5.0/human-macos-aarch64.bin"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
  end

  on_linux do
    if Hardware::CPU.arm?
      url "https://github.com/sethdford/h-uman/releases/download/v0.5.0/human-linux-aarch64.bin"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
    if Hardware::CPU.intel?
      url "https://github.com/sethdford/h-uman/releases/download/v0.5.0/human-linux-x86_64.bin"
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
  end

  def caveats
    on_macos do
      <<~EOS
        Apple Intelligence (on-device, free):
          If you have macOS 26+ and Apple Silicon, human uses Apple
          Intelligence by default. human-ondevice was built and installed
          alongside human — no third-party dependencies needed.

          Run: human onboard --apple
      EOS
    end
  end

  test do
    assert_match "human", shell_output("#{bin}/human --version")
    system "#{bin}/human", "--version"
  end
end
