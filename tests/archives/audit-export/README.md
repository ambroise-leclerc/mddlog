# Original v0.2 audit corpus

`v0.2.0.json` contains raw segment bytes and witness fields produced by mddlog v0.2.0
(`e7012f299b3c976b37b43b53add43e5e6e4636b8`) on 2026-10-08, Clang/libc++ 21.1.8,
CMake 4.2.3, Ninja, Linux x86_64 Release. It holds a closed ledger and three producer
events, canonical/layout/anchor version 1. It is test evidence with no authentic
external witness or hardware durability assertion.

Regenerate from that immutable revision: unpack `git archive v0.2.0` outside this tree,
copy [GenerateV02.cpp](GenerateV02.cpp), append a `generate_v02` executable linked to
`mddlog::mddlog` in the archived CMakeLists, configure the archived `ninja-clang` preset
with examples/tests OFF, build `generate_v02` and redirect its output to a new JSON file.
Compare SHA-256 and segment bytes before replacing the fixture. It must not be regenerated
with the current library and called old-version evidence.

[scripts/run-audit-tools.py](../../../scripts/run-audit-tools.py) independently encodes
the binary evidence envelope around these original bytes and verifies them with the CLI;
it also writes a private legacy file directory without an allocator and inspects it.
It decodes projections in Python, checks event count/version and anchored verdicts.
Unknown/truncated/over-budget mutations are generated during testing, outside Git.
The v0.3 fixture below completes the two currently published audit-producing releases.
Future releases, the final format-support duration and the archive policy remain #121.
This fixture proves the stated v0.2 reading case, not every historical archive.

Fixture SHA-256: `b1d7f485eb046e6d95a1fa288e2904c7b6ad76ac7291e86d8ba393eeb39c532d`.

## Original v0.3 audit corpus (#120)

`v0.3.0.json` was produced on 2026-10-08 by rebuilding release v0.3.0 at its peeled
immutable revision `073761b7a6d5ed29ed87bc37c85967db72386d3c`, with
[GenerateV03.cpp](GenerateV03.cpp) as an external consumer. The archived source tree,
module objects and generator were built separately from the current library, using
Clang/libc++ 21.1.8, CMake 4.2.3, Ninja and Linux x86_64 Release. The generator uses
InMemoryStorageMedium and InMemoryAnchorProvider to capture actual release-produced
segment bytes and anchors; it does not provide hardware durability or an authentic
external witness. It emits five events with a closed producer and ledger. Formats remain
canonical/layout/anchor version 1.

Reproduce from the repository root with the same Clang/libc++ tuple (CMake 4.2.3
or a compatible version supporting `import std`). The generator emits every provenance
field; no JSON editing or current-library encoding is needed:

```bash
archive_dir=$(mktemp -d /tmp/mddlog-v03-XXXXXXXX)
git archive 073761b7a6d5ed29ed87bc37c85967db72386d3c | tar -x -C "$archive_dir"
cp tests/archives/audit-export/GenerateV03.cpp "$archive_dir/GenerateV03.cpp"
cat >> "$archive_dir/CMakeLists.txt" <<'CMAKE'
add_executable(generate_archive GenerateV03.cpp)
target_link_libraries(generate_archive PRIVATE mddlog::mddlog)
CMAKE
cmake --preset ninja-clang -S "$archive_dir" \
  -DMDDLOG_BUILD_EXAMPLES=OFF -DMDDLOG_BUILD_TESTS=OFF
cmake --build "$archive_dir/build-clang" --target generate_archive --parallel 4
"$archive_dir/build-clang/generate_archive" > "$archive_dir/v0.3.0.json"
sha256sum "$archive_dir/v0.3.0.json"
cmp tests/archives/audit-export/v0.3.0.json "$archive_dir/v0.3.0.json"
```

This external consumer is deliberately absent from the current release's CMake targets:
compiling it against the current library would not reproduce old-release evidence.

Fixture SHA-256: `0852b676669ab7570753c61b139d16524b7e4ec1ce8c721116c73585052761e2`.
Generator binary SHA-256 for this local tuple:
`2e26d2e26126f2275d5d835af75bec779da702ddd82f7d091af183b59bbbc431`.
The latter is provenance of this run, not a required hash across different builds.

`audit.tools` and `audit.tools.v0.3.0` replay the original v0.2/v0.3 bytes as a binary
package and a legacy file directory. They require anchored coverage, exact producer
count/order and canonical version, and show the file-only inspection as unanchored.
The chain supervisor runs both when its CLI/reference options are supplied. The four
`real-v02-*`/`real-v03-*` fuzz seeds reproduce these two corpora byte for byte.
