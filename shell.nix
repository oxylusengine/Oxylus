let
  pkgs = import <nixpkgs> { };
in pkgs.mkShell.override {
  stdenv = pkgs.llvmPackages_latest.libcxxStdenv;
} {
  nativeBuildInputs = [
    pkgs.xmake
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config

    pkgs.mold

    pkgs.llvmPackages_latest.libcxx
    pkgs.llvmPackages_latest.libcxx.dev
    pkgs.llvmPackages_latest.compiler-rt
    pkgs.llvmPackages_latest.bintools-unwrapped
    (pkgs.llvmPackages_latest.clang-tools.override {
      enableLibcxx = true;
     })

    pkgs.python313
    pkgs.python313Packages.pip
    pkgs.python313Packages.setuptools
    pkgs.python313Packages.wheel

    pkgs.meshoptimizer

    # SDL3
    pkgs.xorg.libX11
    pkgs.xorg.libxcb
    pkgs.xorg.libXScrnSaver
    pkgs.xorg.libXcursor
    pkgs.xorg.libXext
    pkgs.xorg.libXfixes
    pkgs.xorg.libXi
    pkgs.xorg.libXrandr

    pkgs.vulkan-loader
  ];

  LIBCXX_PATH="${pkgs.llvmPackages_latest.libcxx.dev}";
  LD_LIBRARY_PATH="${pkgs.lib.makeLibraryPath [
    pkgs.llvmPackages_latest.libcxx
    pkgs.gcc14.cc.lib
    pkgs.vulkan-loader
    # SDL3
    pkgs.xorg.libX11
    pkgs.xorg.libxcb
    pkgs.xorg.libXScrnSaver
    pkgs.xorg.libXcursor
    pkgs.xorg.libXext
    pkgs.xorg.libXfixes
    pkgs.xorg.libXi
    pkgs.xorg.libXrandr
  ]}";
  NIX_ENFORCE_NO_NATIVE = "0";

  hardeningDisable = [ "all" ];
}
