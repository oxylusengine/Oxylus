let
  pkgs = import <nixpkgs> { };
  xssWrapper = pkgs.writeTextDir "lib/pkgconfig/xss.pc" ''
    prefix=${pkgs.libXScrnSaver}
    exec_prefix=''${prefix}
    libdir=''${exec_prefix}/lib
    includedir=''${prefix}/include

    Name: Xss
    Description: X11 Screen Saver extension library
    Version: ${pkgs.libXScrnSaver.version}
    Requires: x11 xext
    Cflags: -I''${includedir}
    Libs: -L''${libdir} -lXss
  '';
in pkgs.mkShell.override {
  stdenv = pkgs.llvmPackages_latest.libcxxStdenv;
} {
  nativeBuildInputs = [
    pkgs.xmake
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config

    pkgs.mold

    pkgs.llvmPackages_latest.lldb
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

    pkgs.zenity

    # SDL3
    pkgs.util-macros
    pkgs.vulkan-loader
  ];

  buildInputs = [
    pkgs.util-macros
    pkgs.libX11
    pkgs.libxcb
    pkgs.libXScrnSaver
    pkgs.libXcursor
    pkgs.libXext
    pkgs.libXfixes
    pkgs.libXi
    pkgs.libXrandr
    pkgs.xorgproto
  ];

  PKG_CONFIG_PATH = "${xssWrapper}/lib/pkgconfig:$PKG_CONFIG_PATH";
  LIBCXX_PATH="${pkgs.llvmPackages_latest.libcxx.dev}";
  LD_LIBRARY_PATH="${pkgs.lib.makeLibraryPath [
    pkgs.llvmPackages_latest.libcxx
    pkgs.llvmPackages_latest.lldb
    pkgs.gcc14.cc.lib
    pkgs.vulkan-loader
    # SDL3
    pkgs.libX11
    pkgs.libxcb
    pkgs.libXScrnSaver
    pkgs.libXcursor
    pkgs.libXext
    pkgs.libXfixes
    pkgs.libXi
    pkgs.libXrandr
  ]}";
  NIX_ENFORCE_NO_NATIVE = "0";

  hardeningDisable = [ "all" ];
}
