let
  pkgs = import <nixpkgs> { };
in pkgs.mkShell.override {
  stdenv = pkgs.llvmPackages_latest.libcxxStdenv;
} {
  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.gnumake
    pkgs.xmake

    pkgs.llvmPackages_latest.bintools-unwrapped
    pkgs.llvmPackages_latest.libcxx
    pkgs.llvmPackages_latest.libcxx.dev
    pkgs.llvmPackages_latest.compiler-rt
    (pkgs.llvmPackages_latest.clang-tools.override {
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
    (pkgs.sdl3.override {
      waylandSupport = false;
    })
  ];

  NIX_ENFORCE_NO_NATIVE = "0";
  shellHook = ''
    export LD_LIBRARY_PATH=${pkgs.llvmPackages_latest.libcxx}/lib:$LD_LIBRARY_PATH
    # slang needs libstdc++
    export LD_LIBRARY_PATH=${pkgs.gcc14.cc.lib}/lib:$LD_LIBRARY_PATH
    export LIBCXX_PATH=${pkgs.llvmPackages_latest.libcxx.dev}
  '';

  hardeningDisable = [ "all" ];
}
