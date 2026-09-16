{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  packages = with pkgs; [
    cmake ninja pkg-config
    bison flex swig
    tcl tclreadline
    eigen zlib cudd spdlog
    gtest
  ];
}
