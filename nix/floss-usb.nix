{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "floss-usb";
  version = "0.1.0";
  src = ../floss-usb;
  nativeBuildInputs = [ pkgs.rustc pkgs.pkg-config ];
  buildInputs = [ pkgs.libusb1 ];
  doCheck = true;
  installFlags = [ "PREFIX=$(out)" ];
  meta = {
    description = "Userspace USB HCI/SCO transport for Floss on Realtek 0bda:c123";
    license = pkgs.lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
    mainProgram = "floss-usb";
  };
}
