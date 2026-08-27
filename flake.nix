{
  description = "Shadix plotting and benchmark environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        python = pkgs.python3.withPackages (ps: with ps; [
          polars
          seaborn
          matplotlib
          pytest
        ]);
        fontsConf = pkgs.makeFontsConf {
          fontDirectories = [ pkgs.libertine ];
        };
      in
      {
        devShells.default = pkgs.mkShell {
          buildInputs = [ python pkgs.libertine ];
          FONTCONFIG_FILE = fontsConf;
          shellHook = ''
            # Force matplotlib to rebuild its font cache in a shell-local dir
            # so it picks up Linux Libertine O from the Nix-provided fontconfig.
            export MPLCONFIGDIR="$(pwd)/.mplcache"
            mkdir -p "$MPLCONFIGDIR"
          '';
        };

        # Build/test toolchain for the C++ harnesses (valkey/, nats/, pulsar/).
        devShells.benchmarks = pkgs.mkShell {
          buildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            git
            just
            openssl
            protobuf
            boost
            curl.dev
            zlib
            zstd
            snappy
            nats-server
          ];
        };

        packages.default = python;
      }
    );
}
