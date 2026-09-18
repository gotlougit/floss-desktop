{ pkgs }:
let
  inherit (pkgs) lib;
  pins = builtins.fromJSON (builtins.readFile ./floss-deps.json);
  archives = lib.mapAttrs (name: pin: pkgs.fetchzip (pin // { name = "floss-${name}"; extension = "tar.gz"; stripRoot = false; })) pins;
  commonMk = pkgs.runCommand "floss-common-mk" {} ''mkdir -p $out; cp -r ${archives.common-mk}/. $out/'';
  commonMkFloss = pkgs.runCommand "floss-common-mk-generators" {
    nativeBuildInputs = [ pkgs.patch ];
  } ''
    mkdir -p $out
    cp -r ${commonMk}/. $out/
    chmod -R u+w $out
    cd $out
    patch -p1 < ${./common-mk-generator-paths.patch}
  '';
  gnRoot = ''
    buildconfig = "//common-mk/BUILDCONFIG.gn"
    root = "//common-mk/gn_root"
  '';
  cxxbridge = pkgs.rustPlatform.buildRustPackage {
    pname = "floss-cxxbridge"; version = "1.0.202";
    src = pkgs.fetchurl {
      name = "cxxbridge-cmd-1.0.202.tar.gz";
      url = "https://static.crates.io/crates/cxxbridge-cmd/cxxbridge-cmd-1.0.202.crate";
      hash = "sha256-B7rokjbIEf1NCO00QXWaxKyY11LL/T3jQTFboWrSDsM=";
    };
    cargoHash = "sha256-vtull3vfDg2KEWQ1Bm+cGzCSaeOvF2VSjuZppfcyPXU=";
    doCheck = false;
  };
  modp = pkgs.stdenv.mkDerivation {
    pname = "floss-modp-b64"; version = "68a503f";
    src = archives.modp-b64;
    unpackPhase = ''mkdir source; cp -r $src/. source/; chmod -R u+w source; cd source'';
    dontConfigure = true;
    buildPhase = ''$CC -Imodp_b64 -fPIC -shared -Wl,-soname,libmodp_b64.so -o libmodp_b64.so modp_b64.cc'';
    installPhase = ''
      mkdir -p $out/{lib/pkgconfig,include}
      cp libmodp_b64.so $out/lib/
      cp -r modp_b64 $out/include/
      cat > $out/lib/pkgconfig/libmodp_b64.pc <<PC
      Name: modp_b64
      Description: Base64 codec used by Floss
      Version: 1
      Libs: -L$out/lib -lmodp_b64
      Cflags: -I$out/include
      PC
    '';
    doCheck = false;
  };
  libchromeCore = pkgs.llvmPackages.stdenv.mkDerivation {
    pname = "floss-libchrome"; version = "1094370";
    src = archives.libchrome;
    nativeBuildInputs = [ pkgs.gn pkgs.ninja pkgs.pkg-config pkgs.python3 ];
    buildInputs = [ modp pkgs.abseil-cpp pkgs.glib pkgs.libevent pkgs.double-conversion pkgs.nss pkgs.openssl pkgs.re2 pkgs.dbus pkgs.protobuf pkgs.gtest ];
    unpackPhase = ''
      mkdir -p source/libchrome
      cp -r $src/. source/libchrome/
      cd source
      cp -r ${commonMk} common-mk
      chmod -R u+w .
      cat > .gn <<'GN'
      ${gnRoot}
      GN
    '';
    postPatch = ''
      cd libchrome
      for p in libchrome_tools/patches/*.patch; do patch -p1 < "$p"; done
      cd ..
      substituteInPlace libchrome/BUILD.gn --replace-fail 'args = [ "--output" ] + outputs' 'args = [ "--output" ] + rebase_path(outputs, root_build_dir)'
      sed -i '1i#include <limits>' libchrome/base/memory/ref_counted.h
      sed -i '1i#include <cstring>' libchrome/base/hash/md5_nacl.cc
      substituteInPlace libchrome/BUILD.gn --replace-fail '-Xclang-only=-Wno-char-subscripts' '-Wno-char-subscripts'
      substituteInPlace libchrome/BUILD.gn --replace-fail '"absl"' '"absl_base", "absl_flat_hash_map", "absl_hash", "absl_strings", "absl_synchronization"' --replace-fail '-I/usr/include/libchrome' "-I$out/include/libchrome"
    '';
    configurePhase = ''
      runHook preConfigure
      gn gen out/Release --args="platform_subdir=\"libchrome\" platform2_root=\"$PWD\" pkg_config=\"pkg-config\" cc=\"$CC\" cxx=\"$CXX\" ar=\"$AR\" clang_cc=true clang_cxx=true libbase_ver=\"1094370\" libdir=\"$out/lib\" enable_werror=false external_cxxflags=[\"-DNDEBUG\",\"-Wno-unknown-warning-option\"] use={lto_experiment=false function_elimination_experiment=false clang=true proto_force_optimize_speed=false mojo=false asan=false msan=false ubsan=false coverage=false crypto=true dbus=true fuzzer=false timers=true cros_host=false cros_debug=false profiling=false tcmalloc=false test=false}"
      runHook postConfigure
    '';
    buildPhase = ''ninja -C out/Release -j$NIX_BUILD_CORES libchrome:libchrome'';
    installPhase = ''
      mkdir -p $out/lib/pkgconfig $out/include/libchrome
      cp out/Release/lib/* $out/lib/
      cp out/Release/obj/libchrome/libchrome.pc $out/lib/pkgconfig/
      sed -i "s|Libs: |Libs: -L$out/lib |" $out/lib/pkgconfig/libchrome.pc
      cd libchrome
      find base build crypto dbus third_party components -name '*.h' -exec cp --parents '{}' $out/include/libchrome/ \;
      cd ../out/Release/gen/libchrome
      find . -name '*.h' -exec cp --parents '{}' $out/include/libchrome/ \;
    '';
    doCheck = false;
  };
  libchrome = pkgs.symlinkJoin {
    name = "floss-libchrome-with-public-headers";
    paths = [ libchromeCore ];
    postBuild = ''
      mkdir -p $out/include/libchrome/testing
      cp -r ${archives.libchrome}/testing/. $out/include/libchrome/testing/
      rm $out/lib/pkgconfig/libchrome.pc
      sed "s|${libchromeCore}|$out|g" ${libchromeCore}/lib/pkgconfig/libchrome.pc > $out/lib/pkgconfig/libchrome.pc
    '';
  };
in { inherit archives commonMk commonMkFloss gnRoot modp libchrome cxxbridge; }
