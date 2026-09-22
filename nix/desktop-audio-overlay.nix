# Repair KDE microphone-test cancellation.
# Applied to the host package set so existing desktop/Home Manager selections
# inherit the repairs when they share the NixOS package set.
final: prev: {
  kdePackages = prev.kdePackages.overrideScope (kfinal: kprev: {
    plasma-pa = kprev.plasma-pa.overrideAttrs (old: {
      patches = (old.patches or []) ++ [ ../patches/plasma-pa-microphone-lifecycle.patch ];
    });
  });
}
