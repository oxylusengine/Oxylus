let
  pkgs = import <nixpkgs> { };
in pkgs.mkShell.override {
  stdenv = pkgs.llvmPackages_20.libcxxStdenv;
} {
  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.gnumake
    pkgs.xmake

    pkgs.llvmPackages_20.bintools-unwrapped
    pkgs.llvmPackages_20.libcxx
    pkgs.llvmPackages_20.libcxx.dev
    pkgs.llvmPackages_20.compiler-rt
    (pkgs.llvmPackages_20.clang-tools.override {
      enableLibcxx = true;
    })
    pkgs.mold

    pkgs.pkg-config
    pkgs.python313
    pkgs.python313Packages.pip
    pkgs.python313Packages.setuptools
    pkgs.python313Packages.wheel

    pkgs.zlib.dev

    # for gltfpack
    pkgs.meshoptimizer

    # for SDL3
    pkgs.sdl3
  ];

  shellHook = ''
    export LD_LIBRARY_PATH=${pkgs.llvmPackages_20.libcxx}/lib:$LD_LIBRARY_PATH
  '';

  hardeningDisable = [ "all" ];
}
