{
  description = "Cosmos: Embeddable Deterministic Simulation Testing (DST) library for C/C++";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [
          gcc
          clang
          cmake
          gnumake
          just
          clang-tools
          gdb
        ];
      };
    };
}
