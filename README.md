# enzim-webkit

A WPE-focused fork of [WebKit](https://github.com/WebKit/WebKit) for embedding
web content in native Linux applications.

The downstream changes cover rendering, font and canvas handling, network
scheduling, process and memory policy, media playback and host embedding APIs.

The companion [enzim-webkit-rs](https://github.com/enz1m/enzim-webkit-rs)
repository contains the Rust engine API and enzUI widget adapter. It keeps the
`enzui-webkit` and `enzui-webview` crate names. Native dependency recipes and
runtime packaging live in the separate `enzim-webkit-flatpak` project.

## Branches

- `enzim`: maintained WPE integration for browsers and embedding applications.
- `upstream/wpe-2.55`: original upstream base with its history.

See [fork provenance](FORK.md), [build requirements](BUILDING.md) and the
original [WebKit README](ReadMe.md).

## License

WebKit and its components retain their upstream licenses and file notices.
Consult the license files in each component before distributing source or binaries.
