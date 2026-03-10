# typed: false
# frozen_string_literal: true

class Human < Formula
  desc "The smallest fully autonomous AI assistant infrastructure"
  homepage "https://h-uman.ai"
  license "MIT"

  # Tagged releases: uncomment when publishing v0.2.0+ tarballs to GitHub.
  # url "https://github.com/sethdford/human/archive/refs/tags/v0.2.0.tar.gz"
  # sha256 "..."  # Run: curl -sL <url> | sha256sum

  head "https://github.com/sethdford/human.git", branch: "main"

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
    bin.install "build/human"

    man1.install "docs/man/human.1"
    man1.install "docs/man/human-gateway.1"

    bash_completion.install "completions/human.bash" => "human"
    zsh_completion.install "completions/_human" => "_human"
  end

  test do
    assert_match "human", shell_output("#{bin}/human --version")
    system "#{bin}/human", "--version"
  end
end
