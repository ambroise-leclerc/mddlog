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
Additional intermediate-version fixtures and final duration of format support remain
#120/#121. This fixture proves the stated v0.2 reading case, not every historical archive.

Fixture SHA-256: `b1d7f485eb046e6d95a1fa288e2904c7b6ad76ac7291e86d8ba393eeb39c532d`.
