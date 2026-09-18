{ pkgs, src, patches, deps, generators, version, buildCodec ? false }:
pkgs.llvmPackages.stdenv.mkDerivation {
  pname = if buildCodec then "floss-codec" else "floss-native";
  inherit version;
  inherit src patches;
  unpackPhase = ''
    runHook preUnpack
    mkdir -p source/bt
    cp -r $src/. source/bt/
    chmod -R u+w source/bt
    sourceRoot=source/bt
    runHook postUnpack
  '';
  nativeBuildInputs = with pkgs; [ gn ninja pkg-config python3 bison flex protobuf flatbuffers deps.cxxbridge generators.aconfig generators.sysprop_cpp ];
  buildInputs = with pkgs; [ deps.libchrome deps.modp dbus openssl tinyxml-2 fmt liblc3 flatbuffers protobuf zlib gtest abseil-cpp libevent glib double-conversion re2 nss ] ++ pkgs.lib.optional buildCodec pkgs.ffmpeg-headless;
  configurePhase = ''
    runHook preConfigure
    export FLOSS_PLATFORM_ROOT="$(realpath ..)"
    export CXX_OUTDIR="$FLOSS_PLATFORM_ROOT/out/Release"
    cp -r ${deps.commonMkFloss} ../common-mk
    mkdir -p ../external/proto_logging/stats/enums/bluetooth
    cp -r ${deps.archives.proto-logging}/. ../external/proto_logging/stats/enums/bluetooth/
    chmod -R u+w ../common-mk
    cat > ../.gn <<'GN'
    ${deps.gnRoot}
    GN
    gn gen "$CXX_OUTDIR" --root="$FLOSS_PLATFORM_ROOT" --args="platform_subdir=\"bt\" platform2_root=\"$FLOSS_PLATFORM_ROOT\" build_root=\"$FLOSS_PLATFORM_ROOT\" pkg_config=\"pkg-config\" cc=\"$CC\" cxx=\"$CXX\" ar=\"$AR\" clang_cc=true clang_cxx=true libbase_ver=\"1094370\" libdir=\"$out/lib\" enable_werror=false external_cxxflags=[\"-DNDEBUG\"] use={android=false bt_nonstandard_codecs=true clang=true asan=false msan=false ubsan=false coverage=false floss_rootcanal=false function_elimination_experiment=false fuzzer=false lto_experiment=false profiling=false proto_force_optimize_speed=false tcmalloc=false test=false cros_host=false cros_debug=false}"
    runHook postConfigure
  '';
  buildPhase = ''
    runHook preBuild
    ${pkgs.lib.optionalString (!buildCodec) ''ninja -C "$CXX_OUTDIR" -j$NIX_BUILD_CORES bt:tools''}
    ninja -C "$CXX_OUTDIR" -k 0 -j$NIX_BUILD_CORES ${if buildCodec then "bt/system/stack/mmc:mmc_service" else "bt/system/main:bluetooth-static"}
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    ${if buildCodec then ''
    install -Dm755 "$CXX_OUTDIR/mmc_service" "$out/bin/mmc_service"
    '' else ''
    mkdir -p $out/lib
    # GN emits a thin archive referencing build-directory objects. Materialize
    # its members before exporting it to the separate Rust derivation.
    printf 'CREATE %s\nADDLIB %s\nSAVE\nEND\n' \
      "$out/lib/libbluetooth-static.a" "$CXX_OUTDIR/libbluetooth-static.a" | $AR -M
    test "$(head -c 8 "$out/lib/libbluetooth-static.a")" = '!<arch>'
    ''}
    runHook postInstall
  '';
  doCheck = false;
}
