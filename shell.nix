let
  pkgs = import <nixpkgs> { };
in pkgs.mkShell.override {
  stdenv = pkgs.llvmPackages_21.libcxxStdenv;
} {
  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.gnumake
    pkgs.xmake

    pkgs.llvmPackages_21.bintools-unwrapped
    pkgs.llvmPackages_21.libcxx
    pkgs.llvmPackages_21.libcxx.dev
    pkgs.llvmPackages_21.compiler-rt
    (pkgs.llvmPackages_21.clang-tools.override {
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
    export LD_LIBRARY_PATH=${pkgs.llvmPackages_21.libcxx}/lib:$LD_LIBRARY_PATH
    # slang needs libstdc++
    export LD_LIBRARY_PATH=${pkgs.gcc14.cc.lib}/lib:$LD_LIBRARY_PATH
    export LD_LIBRARY_PATH=${pkgs.lua5_3_compat}/lib:$LD_LIBRARY_PATH
  '';

  hardeningDisable = [ "all" ];
}
