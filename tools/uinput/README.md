# Floss uinput regression checks

Run from the repository root with an existing Rust compiler, C linker, Python,
and `patch` (no package manager or network is used):

```sh
python3 tools/uinput/test-uinput.py \
  --source Bluetooth/system/gd/rust/linux/utils/src/uinput.rs \
  --rustc /path/to/rustc --linker /path/to/cc
```

The source argument is the upstream, unpatched uinput file. The runner copies it
to a temporary directory and applies the uinput section of the packaged
`bluetooth-service-runtime.patch`. It never modifies the reference checkout.
It then compiles that implementation with mock libc calls and equivalents of
nix's ioctl-number macros. The fixture targets Linux x86_64, matching the module.

The mocks substitute only open/ioctl/write: successful opens allocate real
`/dev/null` descriptors so `OwnedFd` cleanup is real. They never create input
devices or inject keys. Ten tests cover:

- Failed opens retain the permission error even when legacy paths are missing.
- Every setup ioctl failure releases its descriptor.
- Short writes fail and interrupted writes retry whole records.
- UTF-8 names fit with a terminating NUL; physical addresses use the correct ABI.
- Repeated cleanup and failed device destruction cannot double-close an FD.
- Closing then dropping a device cannot close a newly reused descriptor.
- Collection destruction closes each child once.
- Invalid addresses and repeated initialization are rejected.
- Supported keys emit press/release plus SYN_REPORT; invalid keys/states fail.

These checks cover ownership and syscall contracts, not kernel acceptance,
KWin media-key routing, or physical headset behavior. Also compile the module
against real libc/nix when cached dependencies are available. AVRCP absolute
volume synchronization is separate and is not implemented by this patch.
