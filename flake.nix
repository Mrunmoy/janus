{
  description = "libcfuture: Zero-Heap Lock-Free Future/Promise Framework for Embedded C";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
      in
      {
        devShells.default = pkgs.mkShell {
          name = "libcfuture-dev-shell";

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            llvmPackages.clang
            llvmPackages.bintools
            cppcheck
            clang-tools
            lcov
            valgrind
            gtest
          ];

          shellHook = ''
            export CC=clang
            export CXX=clang++
            echo "libcfuture development shell initialized with Clang."
          '';
        };
      });
}
