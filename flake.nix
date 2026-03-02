{
  description = "seaclaw — autonomous AI assistant runtime";

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
          pname = "seaclaw";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = [ pkgs.cmake ];
          buildInputs = [
            pkgs.sqlite
          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
            pkgs.curl
          ];

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=MinSizeRel"
            "-DSC_ENABLE_LTO=ON"
          ];

          meta = with pkgs.lib; {
            description = "Smallest fully autonomous AI assistant infrastructure";
            homepage = "https://github.com/seaclaw/seaclaw";
            license = licenses.mit;
            platforms = platforms.unix;
          };
        };

        devShells.default = pkgs.mkShell {
          name = "seaclaw";
          packages = with pkgs; [
            cmake
            sqlite
            clang-tools
          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
            curl
          ];

          shellHook = ''
            echo "SeaClaw dev shell — cmake, sqlite3, clang-tools available"
          '';
        };
      }
    );
}
