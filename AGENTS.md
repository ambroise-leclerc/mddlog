# mddlog contributor guide

This file is the canonical, tool-neutral guidance for contributors and coding agents.

## Published project state

mddlog is a C++23 module library. Treat `CMakeLists.txt` as the source of truth for the module
file set and supported compiler versions; distinguish features implemented under `include/mddlog/`
from features explicitly marked as planned in `README.md`.

The normal out-of-source workflow is:

```bash
cmake -S . -B build -G Ninja -DMDDLOG_BUILD_EXAMPLES=OFF -DMDDLOG_BUILD_TESTS=OFF
cmake --build build --parallel
```

The published branch includes `examples/` and `tests/`. Enable their CMake options to build the
examples and run CTest; tests fetch a pinned SpecLab revision. `SourceTreeCoreConsumer` and
`InstallTreeCoreConsumer` separately verify the governed-only target in source and installed
contexts. See `docs/migration/governed-core.md` before moving direct imports or producers.
`import std` support is experimental and tied to the CMake/compiler tuple. Report configuration or
module-import failures honestly; do not claim a build passed from configuration success alone.

## Working rules

- Register every new module interface in the `FILE_SET cxx_modules` list in dependency order.
- Keep generated build output outside version control.
- Follow `CONTRIBUTING.md` for naming, formatting, licensing, and review conventions.
- Target `develop` from an `<issue-number>-<slug>` branch. `master` carries releases only; cut a
  version with `docs/release-process.md`, never by tagging or pushing to `master` directly.
- Do not describe the library as certified or production-validated solely from its medical-device
  design goals or documentation.
- Keep tool-specific assistant settings local and ignored. Commit messages, PR descriptions, and
  code comments contain no assistant attribution.
