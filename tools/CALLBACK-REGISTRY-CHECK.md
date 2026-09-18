# Offline Floss callback registry check

`test-floss-callback-registry.py` compiles the actual exporter macro crate and
runs the production callback registry's Rust unit tests with a tiny native-stack
message-channel stub. It verifies same-owner deduplication, separate IDs for
different owners sharing an object path, a separate local-proxy namespace, and
removal isolation. The real `RPCProxy` trait is copied into the harness.

Provide already installed Cargo, Rust (including its standard library), a C
linker, and the cached Floss Cargo vendor directory:

```sh
python3 tools/test-floss-callback-registry.py \
  --cargo /path/to/cargo --rustc /path/to/rustc --cc /path/to/cc \
  --vendor /path/to/cached-floss-cargo-vendor-dir
```

The runner forces Cargo offline, uses a temporary directory that is removed
afterward, and records tool versions, production source hashes, test counts and
the compiler/test log in `runtime-results/socket-registry/`. It clears any old
summary before starting. No dependencies are downloaded.

This check does not instantiate native Bluetooth, exercise socket cleanup, or
prove authorization in actual exported D-Bus methods. The separate isolated
real-daemon SocketManager checks cover authenticated cross-client requests.
Hardware transport remains outside both tests.
