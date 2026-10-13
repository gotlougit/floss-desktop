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
    sha256 = "sha256-V87qK0ad3wdopN8uK8chmzY0khrAbfCcZ76V/GZ2rnU=";
  };
  # The pinned development snapshot's minimum versions are lowered to the
  # released KDE libraries by bluedevil-floss.patch.
  patches = [ ../patches/bluedevil-floss.patch ];
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
