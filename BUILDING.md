# Build requirements

The maintained target is Linux x86-64 with WPE, Skia and GStreamer. Other
platforms, architectures and backend configurations need their own validation.

Start with the upstream [build documentation](ReadMe.md). The WPE target needs
its native development dependencies and a matching CMake configuration. Runtime
dependency recipes and packaging are maintained separately in
`enzim-webkit-flatpak`; the Rust embedding and launch requirements are documented
in [enzim-webkit-rs](https://github.com/enz1m/enzim-webkit-rs).

A source checkout is not a ready-to-run runtime. Distributors must provide the
engine libraries, process helpers, resources, sandbox support and required
third-party notices. WebRTC-enabled builds must include LibWebRTC's licenses.

PGO training profiles are not distributed here. Reproducing an optimized runtime
requires compatible compiler flags, dependencies and training data as well as
the source. Application developers can use a matching prebuilt runtime and SDK;
they do not need to rebuild WebKit for every application.
