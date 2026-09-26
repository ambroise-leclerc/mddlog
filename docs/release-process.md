# Releasing mddlog

How a version gets cut, and why each step is a step rather than a habit. This is MduX's procedure
([`docs/release-process.md`](https://github.com/ambroise-leclerc/MduX/blob/master/docs/release-process.md)),
adopted by #53 and adapted to what this repository has: mddlog commits no baked artifacts, so MduX's
re-bake step has no counterpart here, and `README.md`'s `(planned)` markers play the part of MduX's
roadmap.

For software shaped by medical-device concerns a release is not a convenience. IEC 62304 asks a
manufacturer to know precisely what is being released — every included item, its known anomalies,
and the configuration it was built from — and that changes to that composition be controlled. The
steps below are how this repository answers those questions for a version. None of it certifies
mddlog or validates it for medical-device use.

## The branch topology

```text
  NNN-slug ──▶ develop ──▶ release/vX.Y.Z ──▶ master  (tagged vX.Y.Z)
                  ▲                              │
                  └───────────── back-merge ─────┘
```

A working branch is `<issue-number>-<slug>`, the scheme GitHub's "create a branch for this issue"
button generates.

- **`develop`** integrates and is the default branch. Every change arrives by pull request and is
  squash-merged, so each pull request lands as one commit (see [`CONTRIBUTING.md`](../CONTRIBUTING.md)).
- **`release/vX.Y.Z`** exists so that release-only changes — the version, the changelog date — are
  reviewable as a diff of their own.
- **`master`** carries releases and nothing else. Its tip is the most recent tag. The
  [`Branch Topology`](../.github/workflows/branch-topology.yml) check accepts only same-repository
  `release/vX.Y.Z` pull requests targeting it.

`master` was called `main` until #53 renamed it; GitHub redirects the old name, but a clone made
before the rename must update its local branch (`git branch -m main master` then
`git branch -u origin/master master`).

## The procedure

### 1. Confirm `develop` is releasable

```console
$ git switch develop && git pull --ff-only    # fails rather than merging, if local has drifted
$ gh pr list --state open --base develop      # nothing half-landed
$ gh run list --branch develop --limit 8      # every workflow green on the tip
```

Every epic the release claims must be closed on GitHub, not merely merged, and checked against its
own closure criteria: an epic with an open child issue is a release note that overstates.

### 2. Open the release branch

```console
$ git switch -c release/vX.Y.Z
```

### 3. Move the version, in the one place it is defined and the places that assert it

`CMakeLists.txt`'s `project(mddlog VERSION X.Y.Z ...)` is the single definition. It reaches the code
through `MDDLOG_VERSION_STRING` (`cmake/CompilerSettings.cmake`), which `mddlog::getVersion()`
returns, and the package version file through `install(EXPORT ...)`. Never repeat the literal in the
code.

The assertions do repeat it, on purpose:

```console
$ grep -rn 'declaredVersion =' tests/
tests/spec/VersionSpec.cpp:      1 literal
```

A test that derived its expectation from the same macro would assert nothing; the hard-coded literal
is what catches a build reporting a version other than the one declared. `InstallTreeConsumer`
separately checks that the installed module reports `PROJECT_VERSION`.

### 4. Finish the changelog entry

[`CHANGELOG.md`](../CHANGELOG.md)'s top entry moves from `unreleased` to the date. Its **known
limits** section is not optional: a release note that omits the anomalies known at release describes
a different release. If a limit was discovered after the entry was drafted, it goes in now.

### 5. Check the README against what shipped

Features the release delivers lose their `(planned)` marker; features it does not deliver keep it.
This happens on the release branch, before the merge, so the tag never points at a tree that
misdescribes its own content.

### 6. Run the full suite locally if you can, and let CI decide

```console
$ cmake --preset <your-preset>
$ cmake --build --preset <your-preset>
$ ctest --preset <your-preset> -L governed --verbose --no-tests=error
$ ctest --preset <your-preset> --output-on-failure --no-tests=error
```

Report configuration, build and test results separately: a successful configuration is not a
successful build.

### 7. Merge to `master`, tag there, and publish the release

```console
$ gh pr create --base master --head release/vX.Y.Z --title "Release vX.Y.Z"
# after review and a green run - --merge, never --squash, see step 8:
$ gh pr merge <pr> --merge
# then, without checking master out:
$ git fetch origin
$ git tag -a vX.Y.Z -m "mddlog vX.Y.Z" origin/master
$ git push origin vX.Y.Z
$ gh release create vX.Y.Z --verify-tag --title "vX.Y.Z — <subject>" --notes-file <notes>
```

**A green check is not evidence that CI ran.** GitHub does not run `pull_request` workflows when it
cannot construct a merge commit, so an unmergeable release pull request can show a passing review bot
with every build and test absent. Every workflow must be present and successful before merging.

**Merge with `--merge`, on the command line.** The web UI remembers whichever method was used last,
and `develop` pull requests are squash-merged, so relying on the default is how a release gets
squashed. `master`'s protection must therefore allow merge commits.

**Tag `origin/master`, not a checked-out `master`,** so a stale local branch cannot be tagged.

**The tag is annotated,** so it records its own author and date: it identifies a configuration.

**Publish a GitHub release too,** titled `vX.Y.Z — <subject>`, where the subject names the epic or
theme the version closes (`v0.1.0 — Epic #8: Allocation-free governed logging core`). Its notes are
the changelog entry's highlights *and its known-limits section*.

### 8. Back-merge, so the histories do not drift

```console
$ git switch develop && git pull --ff-only && git merge --no-ff origin/master && git push
$ git rev-list --left-right --count origin/master...origin/develop
0	N
```

The left column must be `0`: `master` carries nothing `develop` lacks. The
[`Branch Topology`](../.github/workflows/branch-topology.yml) check enforces this on every push to
`master` or `develop` and once a day. `develop`'s protection must allow this merge commit.

**Merge the release pull request, do not squash it.** A squash gives the release commit a single
parent, so the back-merge falls back to an older common ancestor and conflicts on every path both
sides changed since. If it was squashed anyway, resolve per path against the commit the release
branched from, as MduX's procedure describes; never resolve globally to one side.

## What is deliberately not automated

No workflow cuts a release. Whether an epic is genuinely complete and whether the known-limits
section is honest are judgement calls a script cannot make. Everything mechanical — builds on every
toolchain, sanitizers, the governed boundary checks, formatting and the branch topology — runs on
every mergeable pull request, including the release one.
