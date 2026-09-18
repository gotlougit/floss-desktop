{ pkgs, sources }:
let
  inherit (pkgs) lib;
  deps = import ./floss-dependencies.nix { inherit pkgs; };
  source = sources.bluetooth;
  version = "0.1-${builtins.substring 0 12 source.rev}";
  src = pkgs.fetchzip {
    stripRoot = false;
    extension = "tar.gz";
    url = "https://android.googlesource.com/platform/packages/modules/Bluetooth/+archive/${source.rev}.tar.gz";
    hash = "sha256-CYP+7fD7C4YvwaExpd4q8ozvZv5dg1uoIWaZ0mNWxpI=";
  };
  patches = [ ../patches/bluetooth-floss.patch ];
  native = import ./floss-native.nix { inherit pkgs src patches deps generators version; };
  codec = import ./floss-codec.nix {
    inherit pkgs src deps generators version;
    patches = patches ++ [ ../patches/bluetooth-codec-service.patch ];
  };
  generators = import ./floss-generators.nix { inherit pkgs; };
in (pkgs.rustPlatform.buildRustPackage.override { stdenv = pkgs.llvmPackages.stdenv; }) {
  pname = "floss";
  inherit version;

  unpackPhase = ''
    runHook preUnpack
    mkdir -p source/bt
    cp -r $src/. source/bt/
    chmod -R u+w source/bt
    sourceRoot=source/bt
    runHook postUnpack
  '';
  inherit src;
  patches = patches ++ [ ../patches/bluetooth-service-runtime.patch ];
  cargoLock.lockFile = ./floss-Cargo.lock;
  postPatch = ''
    cp ${./floss-Cargo.lock} Cargo.lock
    substituteInPlace system/gd/rust/linux/{service,mgmt}/build.rs \
      --replace-fail 'cargo:rustc-link-lib=c++' 'cargo:rustc-link-lib=stdc++'
  '';
  dontUseCmakeConfigure = true;
  dontUseNinjaBuild = true;
  dontUseNinjaInstall = true;
  nativeBuildInputs = with pkgs; [ pkg-config protobuf rustfmt rustPlatform.bindgenHook ];
  buildInputs = with pkgs; [ deps.libchrome deps.modp dbus openssl tinyxml-2 fmt liblc3 flatbuffers protobuf zlib gtest abseil-cpp libevent glib double-conversion re2 nss ];
  preConfigure = ''
    export CXX_ROOT_PATH="$PWD"
    export CXX_OUTDIR="${native}/lib"
    export CARGO_TARGET_DIR="$PWD/target"
    export CROS_SYSTEM_API_ROOT="${deps.archives.system-api}"
    export RUSTFLAGS="-L ${native}/lib"
  '';
  preBuild = ''
    mkdir -p "$CARGO_TARGET_DIR"
    cp ${native}/lib/libbluetooth-static.a "$CARGO_TARGET_DIR/"
  '';
  postInstall = ''
    ln -s ${codec}/bin/mmc_service "$out/bin/mmc_service"
    for program in btmanagerd btadapterd btclient; do
      test -x "$out/bin/$program"
    done
    install -Dm644 system/conf/bt_stack.conf "$out/share/floss/bt_stack.conf"
    install -Dm644 system/gd/rust/linux/HFP_PCM_BRIDGE.md "$out/share/doc/floss/HFP_PCM_BRIDGE.md"
    install -Dm644 system/build/dpkg/floss/package/etc/dbus-1/system.d/org.chromium.bluetooth.conf \
      "$out/share/doc/floss/dbus-policy.upstream-example.conf"
  '';
  cargoBuildFlags = [ "-p" "btadapterd" "-p" "manager_service" "-p" "client" ];
  doCheck = false;
  passthru = { inherit native codec; };
  meta = { description = "Android Floss Bluetooth daemon for Linux"; platforms = [ "x86_64-linux" ]; license = lib.licenses.asl20; };
}
