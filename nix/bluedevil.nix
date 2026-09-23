{ pkgs, sources }:
let
  source = sources.bluedevil;
  kde = pkgs.kdePackages;
in
pkgs.stdenv.mkDerivation {
  pname = "bluedevil-floss";
  version = "6.8.80-${builtins.substring 0 12 source.rev}";
  src = pkgs.fetchzip {
    url = "https://invent.kde.org/plasma/bluedevil/-/archive/${source.rev}/bluedevil-${source.rev}.tar.gz";
    sha256 = "sha256-sqwakSfP2v5X5D+ZM8BLp7Y1ThQybrEe5NEdmRI5fdo=";
  };
  patches = [ ../patches/bluedevil-floss.patch ];

  # This development snapshot uses the next release's minimum versions. The
  # Floss implementation is built against the pinned, released KDE libraries.
  postPatch = ''
    substituteInPlace CMakeLists.txt \
      --replace-fail 'set(PROJECT_DEP_VERSION "6.7.90")' 'set(PROJECT_DEP_VERSION "6.7.4")' \
      --replace-fail 'set(KF6_MIN_VERSION "6.30.0")' 'set(KF6_MIN_VERSION "6.29.0")'
  '';
  nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config kde.extra-cmake-modules kde.wrapQtAppsHook ];
  buildInputs = with kde; [ qtbase qtdeclarative kcoreaddons ki18n kcmutils libplasma kirigami ];
  cmakeFlags = [ "-DBUILD_TESTING=OFF" ];
  doCheck = false;
  doInstallCheck = false;
  meta = {
    description = "KDE Bluetooth controls using Floss directly";
    license = pkgs.lib.licenses.gpl2Plus;
    platforms = pkgs.lib.platforms.linux;
  };
}
