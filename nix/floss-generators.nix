{ pkgs }:
{
  sysprop_cpp = import ./floss-sysprop.nix { inherit pkgs; };
  aconfig = pkgs.rustPlatform.buildRustPackage {
    pname = "android-aconfig";
    version = "0.1.0-5ff19f40c704";
    src = pkgs.fetchzip {
      url = "https://android.googlesource.com/platform/build/+archive/5ff19f40c7047cd490f255a34ec10f786f81abf1/tools/aconfig.tar.gz";
      hash = "sha256-iSi6OcH6LfDiRVSgtB2ugrTqSs9WROLgjzkdx4n6G/8=";
      stripRoot = false;
    };
    postPatch = ''
      # Exclude unrelated Android device tools requiring other source trees.
      cat > Cargo.toml <<'EOF'
      [workspace]
      members = ["aconfig", "aconfig_protos", "aconfig_storage_file"]
      resolver = "2"
      EOF
      cp ${./aconfig-Cargo.lock} Cargo.lock
    '';
    # Android 14 QPR3 retains the server_configurable_flags API supplied by
    # Floss; newer aconfig revisions require Android on-device flag storage.
    cargoLock.lockFile = ./aconfig-Cargo.lock;
    cargoBuildFlags = [ "-p" "aconfig" ];
    doCheck = false;
    doInstallCheck = false;
    meta = {
      description = "Official Android aconfig generator for Floss C++ flags";
      license = pkgs.lib.licenses.asl20;
      platforms = pkgs.lib.platforms.linux;
    };
  };
}
