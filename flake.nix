{
  description = "human — autonomous AI assistant runtime";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "human";
          version = "0.3.0";
          src = ./.;

          nativeBuildInputs = [ pkgs.cmake ];
          buildInputs = [
            pkgs.sqlite
            pkgs.curl
          ];

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=MinSizeRel"
            "-DSC_ENABLE_LTO=ON"
            "-DSC_ENABLE_CURL=ON"
            "-DSC_ENABLE_SQLITE=ON"
            "-DSC_ENABLE_ALL_CHANNELS=ON"
          ];

          meta = with pkgs.lib; {
            description = "Smallest fully autonomous AI assistant infrastructure";
            homepage = "https://github.com/sethdford/human";
            license = licenses.mit;
            platforms = platforms.unix;
          };
        };

        devShells.default = pkgs.mkShell {
          name = "human";
          packages = with pkgs; [
            cmake
            sqlite
            clang-tools
          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
            curl
          ];

          shellHook = ''
            echo "Human dev shell — cmake, sqlite3, clang-tools available"
          '';
        };
      }
    );
}
