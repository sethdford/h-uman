# typed: false
# frozen_string_literal: true

class Seaclaw < Formula
  desc "Autonomous AI assistant runtime — minimal C11 binary"
  homepage "https://sethdford.github.io/seaclaw/"
  license "MIT"
  head "https://github.com/sethdford/seaclaw.git", branch: "main"

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
    bash_completion.install "completions/seaclaw.bash" => "seaclaw"
    zsh_completion.install "completions/seaclaw.zsh" => "_seaclaw"
  end

  test do
    assert_match "seaclaw", shell_output("#{bin}/seaclaw --version")
    system "#{bin}/seaclaw", "--help"
  end
end
