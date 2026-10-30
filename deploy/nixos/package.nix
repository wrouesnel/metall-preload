# Builds libmetall_preload.so with nixpkgs' toolchain. Used by module.nix;
# build it on its own with
#   nix-build -E 'with import <nixpkgs> {}; callPackage ./deploy/nixos/package.nix {}'
{
  lib,
  stdenv,
  cmake,
  boost,
  fetchFromGitHub,
}:
let
  # Must match the revision pinned in CMakeLists.txt.
  metall = fetchFromGitHub {
    owner = "LLNL";
    repo = "metall";
    rev = "3aa4b9a5e2f3ac7836beba755b818726f9665440";
    hash = "sha256-WSYCRiJfLVHXG85xeQHCBJVifytOaw1RS5gnlau3DwM=";
  };
in
stdenv.mkDerivation {
  pname = "metall-preload";
  version = "0-unstable";

  src = lib.fileset.toSource {
    root = ../..;
    fileset = lib.fileset.unions [
      ../../CMakeLists.txt
      ../../exports.map
      ../../metall-preload.cpp
    ];
  };

  nativeBuildInputs = [ cmake ];
  buildInputs = [ boost ];

  # FetchContent cannot download inside the sandbox; hand it the source.
  cmakeFlags = [ "-DFETCHCONTENT_SOURCE_DIR_METALL=${metall}" ];

  # Only the library: Metall's own install rules would add its headers.
  installPhase = ''
    runHook preInstall
    install -Dm755 libmetall_preload.so $out/lib/libmetall_preload.so
    runHook postInstall
  '';

  meta = {
    description = "LD_PRELOAD library that serves nix's heap from an LLNL Metall datastore";
    platforms = lib.platforms.linux;
  };
}
