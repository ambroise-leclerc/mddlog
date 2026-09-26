# Governed boundary evidence (issue #39)

These checks implement ADR-001 Decision 6. They run after compilation with tests enabled:

```sh
ctest --test-dir <build-directory> -L governed --verbose --no-tests=error
```

Python 3.9+ is a test-only dependency. No Python, test fixture, or inspection tool is linked into
the installed library. The checks have both `build` and `governed` CTest labels. A missing tool,
missing object or compiler scan, empty inventory, unknown format, or unexpected dependency fails
the check. A build with `MDDLOG_BUILD_TESTS=OFF` **does not execute these checks** and provides no
boundary-check result. Configuration success alone provides none either.

## What is checked

| Check | Evidence | Limit |
| --- | --- | --- |
| `build.core.graph` | Compiler-generated P1689 `.ddi` files for every object of `mddlog-core`, exact module/import inventory, and generation-time evaluated direct/interface CMake link libraries/options of the core and its project dependencies | Standard-library module/runtime internals are trusted toolchain inputs; this is not a whole-program call graph |
| `build.core.source` | Every file recursively under `include/mddlog/core`, including files not registered in CMake, checked for forbidden C++ tokens | Lexical check, not a C++ semantic proof; indirect calls, aliases and arbitrary external code are not resolved |
| `build.core.allocation` | Undefined allocator/deallocator references in every governed object plus a representative template-instantiation object | No listed emitted reference in these objects; cannot establish absence of allocation through indirect calls, runtime internals, uninstantiated templates, eliminated code or a different build |
| `build.core.exception` | A separate scan for exception-runtime entry points and standard-library throw helpers in the same objects | No listed emitted throw reference; does not prove the absence of every exceptional path or establish allocation freedom |
| `build.core.negativeControls` | Each production check runs against a deliberate violation and must fail with the expected diagnostic; real allocation and throw fixtures are compiled but never linked | Validates the checked violations, not completeness of the forbidden lists |

`cmake/governed-policy.json` pins the exact direct imports of each governed module. Changing an
import even to another existing core module requires an explicit policy review. Sink, adapter and
unknown module imports fail. Target dependencies form a closed reviewed set: `mddlog_options`,
`mddlog_warnings`, and `Threads::Threads`. Their dependencies are checked too, including PUBLIC,
PRIVATE and INTERFACE links after generator-expression evaluation. `__CMAKE::CXX23` is the
toolchain's standard-module target. `-pthread`, `--coverage` and the project's sanitizer link
options are explicit build-instrumentation exceptions, not permission to link another library.
Arbitrary global compiler/linker flags and changes to the toolchain remain build inputs requiring
review; this check is not a sandbox against a malicious build configuration.

Module interface objects often contain only module initializers. `CoreInstantiation.cpp` therefore
emits `RingLog<1>`, `RingLog<3>`, `InlineString<messageCapacity>` and record assignment operations.
The extra object is scanned separately from the target's objects and is never installed. This is
representative template coverage, not exhaustive coverage of all possible capacities/importers.

## Source policy and exceptions

The authoritative list is `FORBIDDEN` in `scripts/check-governed.py`. It covers exception keywords,
allocation/deallocation, owning strings/containers and allocation helpers, locks and waits,
threads/futures, stream/formatting APIs, allocator functions, and calendar/clock APIs. `::now()`
is also forbidden. Preprocessor directives (`#`) are rejected so governed sources cannot conceal
textual dependencies or define local macros. Identifiers are checked irrespective of qualification,
so `using namespace std; vector<int>` is rejected too. `std::string_view` is a distinct allowed
token; it is not an owning `std::string`.

The only syntactic exception is **`= delete;`**, which disables an operation instead of releasing
heap memory. Comments, ordinary/character literals, and raw string literals are ignored because
their contents are data/documentation. Line splicing is handled before tokenization. There are no
per-file exceptions or inline suppression annotations. New exceptions must be justified here and
accompanied by a negative control. This token policy can intentionally reject a harmless identifier
that shares a forbidden API's name; renaming it is preferable to weakening the rule.

## Toolchain coverage and execution status

| CI configuration | Dependency/source checks | Allocation and exception scans |
| --- | --- | --- |
| Linux GCC 16.1 / libstdc++ / CMake 4.1.1 | P1689 scans and Python | GNU/LLVM `nm -u -C`, ELF objects |
| Linux Clang 21 / libc++ / CMake 4.1.1 | P1689 scans and Python | LLVM `nm -u -C`, ELF objects |
| macOS arm64 Clang 21.1.8 / libc++ / CMake 4.3.1 | P1689 scans and Python | LLVM `nm -u -C`, Mach-O objects |
| Windows MSVC / CMake 4.1.1 | CMake-normalized P1689 scans and Python | `dumpbin /SYMBOLS`, only `UNDEF` entries in COFF objects |

These are configured CI gates, not a claim that an unexecuted platform passed. Each of the four
build workflows prints the checks and negative-control results in a dedicated verbose step. The
ASan/UBSan suite also includes them; TSan executes them in a separate step after the RingLog tests.
Only a completed job's logs establish execution for its revision/configuration. Missing tooling is
a failure, never a skipped or successful scan. No unsupported-platform scan is silently substituted.

The allocation patterns include C++ scalar/array new/delete (including decorated MSVC spellings),
C allocation functions and exception-storage allocation. The exception profile includes
`__cxa_throw`, rethrow, libstdc++/libc++ throw helpers and MSVC's `_CxxThrowException`/`_X...`
helpers. Unlike MduX's report-only treatment for some allocating governed objects, this bounded
core has no throw-symbol exemption: an inlined library throw fails too. Compiler-generated unwind
metadata/personality symbols alone are not throw sites and are not forbidden.

## Four different properties

* **Allocation:** source restrictions and the allocation symbol profile provide complementary,
  scoped evidence. A successful allocation scan says nothing about exceptions or blocking.
* **Exceptions:** source restrictions and the separate exception profile detect their listed
  constructs/references. We do not enable or promise `-fno-exceptions`: `import std` BMI dialect
  compatibility must be demonstrated independently on each compiler/library tuple.
* **Blocking:** forbidden lock/wait APIs are a guard, not a nonblocking proof. The ring's
  `is_always_lock_free` assertion and SPSC design supply separate evidence; neither proves that
  the OS will schedule a producer promptly. Spin loops can contain none of the forbidden tokens.
* **Time bounds:** none of these scans establishes a worst-case execution time. Capacity-bounded
  copies and loops need algorithm review, and target timing needs separate measurement/analysis.

The implementation draws on [MduXNoHeapScan.cmake at the pinned reference revision](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/cmake/MduXNoHeapScan.cmake),
but keeps allocation and exception profiles independent and makes no whole-program guarantee.
