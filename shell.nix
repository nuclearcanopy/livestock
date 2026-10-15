{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  packages = with pkgs; [
    gcc
    python3
  ];

  shellHook = ''
    echo "livestock dev shell"
    echo "  build: g++ -O3 -march=native -fopenmp main.cpp -o clanstats"
    echo "  run:   python3 server.py   # http://127.0.0.1:8000"
  '';
}
