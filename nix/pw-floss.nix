{ pkgs, pipewire }:

pkgs.stdenv.mkDerivation {
  pname = "pw-floss";
  version = "0.1.0";
  src = ../pw-floss;

  strictDeps = true;
  nativeBuildInputs = with pkgs; [ meson ninja pkg-config ];
  buildInputs = [ pipewire pkgs.dbus ];

  meta = with pkgs.lib; {
    description = "Standalone PipeWire PCM bridge for the Floss Bluetooth stack";
    homepage = "https://github.com/gotlougit/floss-desktop";
    license = licenses.mit;
    mainProgram = "pw-floss";
    platforms = platforms.linux;
  };
}
