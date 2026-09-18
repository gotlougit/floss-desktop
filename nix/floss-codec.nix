# Independent daemon derivation: codec implementation fixes do not rebuild the
# Bluetooth native archive. Only this process links FFmpeg.
args: import ./floss-native.nix (args // { buildCodec = true; })
