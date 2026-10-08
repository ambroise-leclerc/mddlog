# Audit CLI (#119)

Build the optional Linux tool with the normal adapter library:

```sh
cmake --preset ninja-clang -DMDDLOG_BUILD_AUDIT_TOOLS=ON
cmake --build --preset ninja-clang --parallel
build-clang/mddlog-audit --help
```

`MDDLOG_BUILD_AUDIT_TOOLS` defaults to OFF and requires
`MDDLOG_BUILD_FILE_STORAGE=ON`. `cmake --install build-clang --prefix PREFIX`
installs `bin/mddlog-audit`. No CLI, Linux API or witness dependency reaches
`mddlog::core`. Portable evidence/projection modules belong to `mddlog::mddlog`.

## Everyday use

Use a reader-owned copy/snapshot of the private journal directory. The file backend
requires directory/segment ownership by the invoking UID and private modes, not merely
an ACL granting read access to another account's journal. An original journal can be
inspected under its owner account; independent reader custody needs an independently
owned consistent copy and reader checkpoint. Inspection
holds the file backend's shared lock; a cooperating writer must release its
exclusive lock first. Copy or snapshot the journal with host-controlled consistent
backup procedures if the writer cannot stop. The tool never repairs the source.

```sh
mddlog-audit inspect --source /srv/reader/journal
mddlog-audit inspect --config reader.conf --format json
mddlog-audit export --config reader.conf --format evidence --output /srv/reader/archive.mda
mddlog-audit verify --config replay.conf --archive /srv/reader/archive.mda
```

For `verify`, use `replay.conf`: copy the provider/retained/budget settings from
`reader.conf`, omit `source` and select `format=human` or `format=json`. Unknown, repeated or irrelevant input options are errors; command
line values override configuration values. `--output` creates a new file with mode
0600, refuses replacement/symlinks, synchronizes file and directory, and removes a
partial output on failure. An output in the source journal directory is refused to preserve its strict inventory.
Destination confidentiality and filesystem durability
remain host responsibilities. A nonzero exit can accompany a useful report/archive:
inspect the exit policy before using shell `&&` for a partially covered journal.

The configuration is byte-oriented (UTF-8 recommended), maximum 64 KiB, strict `key=value` lines with no whitespace
normalization; empty lines and lines starting with `#` are ignored. Values may contain
`=`. There is no interpolation, environment substitution, include or secret storage.
Relative file paths resolve against the current working directory; socket paths must be absolute.
Paths containing newlines can only be passed as named command line values. Boolean
configuration values are `true`/`false`; on the command line the two flags take no value.

```ini
version=1
source=/srv/reader/journal
format=json
socket=/srv/witness/anchor.sock
provider-id=site-witness
server-uid=1201
timeout-ms=1000
retained=/srv/reader/checkpoint
max-segments=4096
max-segment-bytes=1048576
max-total-bytes=67108864
read-chunk-bytes=65536
max-read-bytes=134217728
max-streams=4096
max-records=262144
max-provider-entries=4096
max-provider-text-bytes=1024
max-provider-calls=32768
max-integrity-faults=4096
max-archive-bytes=150994944
```

`--max-anchor-age-ns N` declares a nonnegative age bound;
`--verification-time-ns N` supplies signed Unix epoch nanoseconds. No clock is invented
when omitted: age is NotChecked without a bound, Unknown if a bound cannot be evaluated.
Packages retain this time/bound; explicit replay options override them. Input/archive,
metadata, decoded record, provider and output budgets are finite. A package cannot raise
local resource limits: to replay a larger profile, deliberately supply all necessary
larger named limits. The byte budget limits encoded size, not peak process RSS; snapshots,
LogImage buffers and projection encoding allocate within the declared profile, and multiple
buffers coexist. No wall-clock/WCET guarantee is inferred from work budgets. Provider
calls use the configured transport deadline. Choose smaller limits for the host deployment.

## Trust and reader state

An authenticated Unix provider is selected by socket, exact provider identity and peer
UID; reader ACL/deadline failures are actionable diagnostics with AnchorUnavailable
coverage. Its deployment must establish independence from the writer and the host's
authority. Transport authentication alone does not qualify that independence; see
[the witness contract](independent-witness.md). The report labels it
`authenticated-unix-provider` and records socket/UID/identity provenance.

Offline replay ignores embedded anchors by default. To make a deliberate assumption:

```sh
mddlog-audit verify --archive archive.mda --accept-embedded-provider --format json
```

The report then says `accepted-embedded-assumption`. Anyone able to rewrite a package
can replace both log and witness snapshot and recalculate its checksum; this command
provides no authenticity proof. Alternatively use `--socket`, `--provider-id`,
`--server-uid` to consult an independent authority at replay time. Its newer state may
intentionally produce a different report. No signature is implemented (#123).

`retainedTrust` distinguishes `embedded-untrusted-snapshot`, `external-reader-store`
and `none`. Embedded checkpoints reproduce the earlier run's inputs but do not establish
independent rollback protection. `rollbackNotExcluded` is the library's input-relative
finding and must be read with this provenance. Use a reader-owned external store for
independent rollback checks. For initial enrollment provision an empty private directory
outside the writer's authority, then explicitly run:

```sh
mddlog-audit checkpoint-init --retained /srv/reader/checkpoint --provider-id site-witness
mddlog-audit inspect --config reader.conf --update-retained
```

Ordinary inspection loads the store and verifies a copy; it never saves it. Export includes
the position **before** verification. `--update-retained` saves only an eligible candidate,
requires an available authenticated Unix provider, refuses impossible/adverse results
and the embedded-snapshot assumption, and uses #115's generation
comparison. Restore failures never become empty state or automatic enrollment. Reload and
reconcile after save failure. Output and checkpoint save are separate operations: an output
may already exist when a subsequent checkpoint save fails; the error then returns 2.

## Projection and completeness

`inspect --format json` and `export --format json` produce the
[projection-v1 schema](audit-export-format.md). `--stream ID` selects only that stream's
events and sets `selection.complete=false`, even if the selection happens to match every
event. The full journal report is retained so the selection does not conceal other streams.
No filtered projection is a complete journal or an importable evidence package.
`export --format evidence` preserves every original segment and refuses filtering.
Events include canonical bytes/digests and decoded fields; unknown record versions are
never guessed. Payload strings are byte-preserving, with documented JSON encoding.

Actors, targets, detail and correlation/requirement/risk references can disclose personal
or operational information. Decide which data may be logged, who may inspect them, output
permissions, destination, encryption and retention in the host deployment. Filtering is
selection, not redaction; the full report still contains stream/ledger identities and
anchors. Complete evidence cannot omit sensitive bytes without losing its completeness.
The tool does not send journal data to a remote service.

## Exit policy

| Code | Meaning |
|---|---|
| 0 | Complete anchored/retired coverage, no adverse finding or coverage reservation in the exit policy. Retained rollback assumptions still appear separately. |
| 2 | Operation impossible: configuration, I/O, package/schema/version, resource refusal or CannotVerify. |
| 3 | Inconsistency, alteration, rollback, conflict, incomplete history or adverse ledger boundary. |
| 4 | Absent/partial coverage, unavailable witness, stale/unknown age, residual/headerless bytes or unclosed/uncheckable history. |

Priority is 2, then 3, then 4, then 0. Filtering changes selection metadata, not the whole-log
exit. There is no `valid` field. Human reports always show present/anchored/unanchored
positions, cause, retained outcome, age, rollback caveat and residual boundaries. JSON
contains stable IDs for detailed causes and all ledger boundary notes. These are technical
integrity findings, not a regulatory attestation.

## Compatibility and external use

v0.2 canonical/layout contracts remain readable; [the corpus](../tests/archives/audit-export/README.md)
was generated by the published v0.2.0 library. Legacy file directories lacking an allocator
can be inspected read-only. Unknown canonical/layout versions yield CannotVerify/exit 2;
unknown package/configuration versions are refused. Final maintenance duration, broader
intermediate-version corpus and schema acceptance belong to #120/#121 and final adoption
to #122. The schema remains a candidate pending maintainer review.

An external Python consumer can read a projection without C++ modules:

```python
import json
with open('projection.json', encoding='utf-8') as stream:
    projection = json.load(stream)
assert projection['schema'] == 'mddlog.audit.projection' and projection['version'] == 1
for event in projection['events']:
    canonical = bytes.fromhex(event['canonicalHex'])
    if event['fields'] is not None:
        action_bytes = event['fields']['action'].encode('latin1')
        print(event['streamId'], event['fields']['sequence'], action_bytes)
print(projection['selection'], projection['report'])
```

See [local validation and limits](audit-tools-validation.md) and
[ADR-006](adr/ADR-006-audit-tools-and-export.md).
