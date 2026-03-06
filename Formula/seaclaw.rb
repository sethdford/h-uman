# typed: false
# frozen_string_literal: true

class Seaclaw < Formula
  desc "The smallest fully autonomous AI assistant infrastructure"
  homepage "https://seaclaw.ai"
  license "MIT"

  # Tagged releases: uncomment when publishing v0.2.0+ tarballs to GitHub.
  # url "https://github.com/seaclaw/seaclaw/archive/refs/tags/v0.2.0.tar.gz"
  # sha256 "..."  # Run: curl -sL <url> | sha256sum

  head "https://github.com/seaclaw/seaclaw.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "sqlite"
  depends_on "curl" => :optional

  def install
    args = %w[
      -DCMAKE_BUILD_TYPE=MinSizeRel
      -DSC_ENABLE_LTO=ON
      -DSC_ENABLE_SQLITE=ON
    ]
    args << "-DSC_ENABLE_CURL=#{build.with?("curl") ? "ON" : "OFF"}"

    system "cmake", "-S", ".", "-B", "build", *args, *std_cmake_args
    system "cmake", "--build", "build"
    bin.install "build/seaclaw"

    man1.install "docs/man/seaclaw.1"
    man1.install "docs/man/seaclaw-gateway.1"

    bash_completion.install "completions/seaclaw.bash" => "seaclaw"
    zsh_completion.install "completions/_seaclaw" => "_seaclaw"
  end

  test do
    assert_match "seaclaw", shell_output("#{bin}/seaclaw --version")
    system "#{bin}/seaclaw", "--version"
  end
end
