# Fork provenance

- Upstream: https://github.com/WebKit/WebKit
- Upstream base: `b74592135f6378beaeec14c85c8fa2937a30473b` (`324516@main`, development version 2.55.0).
- Maintainer: Bernard, [enz1m](https://github.com/enz1m).

The `enzim` branch descends directly from the original WebKit commit recorded
above. Its original upstream ancestry and authorship are retained, followed by
separate signed downstream commits. `upstream/wpe-2.55` points to that original
upstream base. The GitHub repository belongs to the `WebKit/WebKit` fork network.

## Downstream scope

- WPE embedding APIs, permissions, accessibility, clipboard and inspector support.
- GPU composition, buffer ownership, hidden-page resource release and bounded caches.
- Font matching, canvas rendering, CSS invalidation and resource discovery.
- Network scheduling, process preparation and background memory management.
- GStreamer playback, screen capture, audio output selection and WebRTC integration.

Rust bindings and the enzUI adapter belong in `enzim-webkit-rs`. Native dependency
recipes and runtime packaging belong in `enzim-webkit-flatpak`. The Rust
repository's `runtime/engine.json` selects the engine source and compatible builds.

Upstream component licenses, authorship and file notices are retained. Downstream
commit authorship does not replace the notices in imported or modified files.
