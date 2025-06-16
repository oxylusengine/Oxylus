let
  pkgs = import <nixpkgs> { };
  pkgs-unstable = import <nixpkgs-unstable> { };
in pkgs.mkShell.override {
  stdenv = pkgs-unstable.llvmPackages_20.libcxxStdenv;
} {
  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.gnumake
    pkgs.xmake

    pkgs-unstable.llvmPackages_20.bintools-unwrapped
    pkgs-unstable.llvmPackages_20.libcxx
    pkgs-unstable.llvmPackages_20.libcxx.dev
    pkgs-unstable.llvmPackages_20.compiler-rt
    (pkgs-unstable.llvmPackages_20.clang-tools.override {
      enableLibcxx = true;
    })
    pkgs-unstable.mold

    pkgs.pkg-config
    pkgs-unstable.python313
    pkgs-unstable.python313Packages.pip
    pkgs-unstable.python313Packages.setuptools
    pkgs-unstable.python313Packages.wheel

    pkgs.zlib.dev

    # for gltfpack
    pkgs-unstable.meshoptimizer

    # for SDL3
    (pkgs-unstable.sdl3.override {
      waylandSupport = false;
      openglSupport = false;
      testSupport = false;
    })
  ];

  shellHook = ''
    export LD_LIBRARY_PATH=${pkgs-unstable.llvmPackages_20.libcxx}/lib:$LD_LIBRARY_PATH
  '';

  hardeningDisable = [ "all" ];
}
