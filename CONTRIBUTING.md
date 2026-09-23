# Contributing Guidelines

## Invitation-only contributions

This project accepts code and documentation contributions only from collaborators explicitly
invited by a maintainer. Opening an issue or pull request does not constitute an invitation, and
unsolicited pull requests may be closed without review.

Before contributing, an invited collaborator must accept the [Contributor Licence
Agreement](CLA.md) in the GitHub issue designated by the maintainer. Only collaborators with
repository access may submit changes for review. Every change remains subject to maintainer review;
an invitation does not guarantee merge.

## Coding Style

We follow the **C++ Core Guidelines** to ensure conformance with modern C++23 generic programming. Please refer to the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines) for detailed rules. Key points are summarized below:

### Naming Conventions

- **Classes/Structs:**
  - Use `UpperCamelCase` (e.g., `MyClass`, `DataProcessor`).
- **Functions/Methods:**
  - Use `lowerCamelCase` (e.g., `processData()`, `getValue()`).
- **Variables (including const, constexpr, and constinit variables):**
  - Use `lowerCamelCase` (e.g., `dataBuffer`, `isReady`).
- **Class/struct data members (public, protected, and private):**
  - Use `lowerCamelCase` with no leading or trailing underscore (e.g., `minLevel`, `enabled`),
    same as any other variable. `.clang-tidy`'s `readability-identifier-naming` enforces this
    strictly - a trailing `_` is a violation, not an accepted suffix.
  - A constructor or setter parameter must not be given the exact same name as a member it
    initializes or assigns: `-Werror=shadow` rejects that at compile time regardless of whether
    the body still behaves correctly. Rename the parameter instead (e.g.
    `void setEnabled(bool value) { enabled.store(value); }`, `explicit Sink(LogLevel
    initialMinLevel) : minLevel(initialMinLevel) {}`).
- **Enumeration values (enumerators):**
  - Use `UpperCamelCase` (e.g., `LogLevel::Info`, `LogLevel::Audit`), enforced by
    `readability-identifier-naming.EnumConstantCase: CamelCase` in `.clang-tidy`.
  - This is a deliberate deviation from MduX, whose `.clang-tidy` at the imported revision uses
    `camelBack` for enumerators; mddlog's severity levels read as named states, like the enum type
    itself. Old ALL_CAPS spellings (`LogLevel::INFO`, ...) no longer exist; see
    [the migration table](docs/migration/loglevel-rename.md).
- **Namespaces:**
  - Use `lowercase` (e.g., `mddlog::core`, `mddlog::sinks`).
- **Macros:**
  - Use `ALL_CAPS_WITH_UNDERSCORES`. But do not use macros.

### Formatting

- Indent with 4 spaces, no tabs.
- Place pointer/reference symbols next to the type (e.g., `int* ptr`, `const std::string& name`).
- Use `nullptr` instead of `NULL`.

### Atomics

- Access a `std::atomic<T>` member through its explicit API (`.load()`, `.store(value)`,
  `.exchange(value)`, `.fetch_add(n)`, ...) rather than relying on the implicit conversion to
  `T` or on `operator=`. Both compile and behave identically to the explicit form, but the
  explicit form makes the atomic access visible at every call site instead of reading like a
  plain variable.

### File Organization

- Module files: `.cppm`
- Source files: `.cpp` (for examples and tests only)
- **File naming:** Use `UpperCamelCase` for `.cppm` and `.cpp` files (e.g., `Logger.cppm`, `DataProcessor.cpp`).

### Documentation

We use **Doxygen** syntax for code documentation. Follow these guidelines:

- **File-level documentation:** Use `@brief` only, no `@file` or `@author` tags.
- **Class documentation:** Include `@brief` with detailed description and usage examples.
- **Method documentation:**
  - Use compact notation `/** @brief Description */` for simple one-line descriptions.
  - Use full format with `@param`, `@return`, `@note` for complex methods.
  - Include usage examples with `@code` blocks when helpful.
- **Template parameters:** Document with `@tparam` when non-obvious.
- **Private members:** Generally no documentation needed unless complex.

Example formats:
```cpp
/** @brief Simple one-line description */
void simpleFunction();

/**
 * @brief Complex function with detailed documentation
 *
 * Detailed description of what the function does, including
 * important implementation details and usage patterns.
 *
 * @param param1 Description of first parameter
 * @param param2 Description of second parameter
 * @return Description of return value
 *
 * @note Important notes about usage or behavior
 *
 * @code
 * // Usage example
 * auto result = complexFunction(value1, value2);
 * @endcode
 */
ReturnType complexFunction(Type1 param1, Type2 param2);
```

## Tooling

To ensure consistency, we use `.clang-format` and `.clang-tidy` to enforce our coding style. These configuration files are included in the root of the repository.

- **Clang-Format:** Automatically formats your code to match our style guidelines.
- **Clang-Tidy:** Detects and warns about style violations, bugs, and non-modern C++ practices.

Please run these tools on your code before submitting a pull request.

### Origin and reference versions

`.clang-format` and `.clang-tidy` are imported from MduX at the immutable revision
`d972d77bc5cefdbe105ad7933ee61746fb5eb45b` ("Bump SpecLab to v0.4.0 [#365] (#374)"), then adapted
to mddlog. Every deviation is commented in the file itself; the notable ones are `Standard: Latest`
(clang-format has no literal `c++23` value), a project-specific `HeaderFilterRegex`, a POSIX-valid
third-party include category, `EnumConstantCase: CamelCase`, and the strict no-`_` member rule
above. Replacing the revision requires citing the new SHA and justifying each difference.

The reference tool version is **LLVM 21** (the same toolchain `clang-build.yml` installs):
`clang-format` 21.x and `clang-tidy` 21.x. Other versions may format or diagnose differently.

### Formatting a change

Run clang-format 21 over the formatted scope (`include/**/*.cppm`, `tests/**/*.cpp`,
`tests/**/*.hpp`, `examples/**/*.cpp`):

```bash
clang-format-21 --style=file -i \
  $(find include -name '*.cppm') \
  $(find tests -name '*.cpp' -o -name '*.hpp') \
  $(find examples -name '*.cpp')
```

To check without modifying anything, use the same entry point CI runs (added by the formatting-check
change, #18):

```bash
scripts/check-format.sh
```

It verifies at run time that clang-format is present and is major version 21, checks every file in
the scope above with `--dry-run --Werror`, and fails (rather than passing) if the tool is missing or
the scope resolves to no files. Override the binary with `CLANG_FORMAT=/path/to/clang-format`. The
`Format Check` workflow (`.github/workflows/format-check.yml`) runs it on every pull request and
prints the effective clang-format version in its log.

### Analysing a change

Run the reference analysis with the same entry point CI uses (added by the clang-tidy CI change, #19):

```bash
cmake --preset ninja-clang && cmake --build --preset ninja-clang   # builds the module interfaces
scripts/run-clang-tidy.sh build-clang
```

- **Version**: clang-tidy 21 (`clang-tidy-21`), verified at run time; any other major version fails.
- **Scope** (translation units): `include/mddlog/**/*.cppm`, `tests/spec/*.cpp`, `examples/*.cpp`.
  `tests/framework/*.hpp` is a header, analysed through the specs that include it.
- **What fails the run**: any diagnostic (findings are promoted to errors), any tool failure, a
  missing or unbuilt Clang preset, a file of the scope absent from `compile_commands.json`, or an
  empty scope. The report (tool version, units analysed, `.cppm` units covered,
  `clang-diagnostic-error` count) is printed and, in CI, added to the job summary.
- **Known limitations**: C++ named modules (`import std`, `mddlog.*`) can only be analysed against
  BMIs built by a matching Clang, so the analysis needs the *built* Clang preset; a GCC build's
  `compile_commands.json`, or a clang-tidy of another LLVM major, fails on the module interfaces
  (`module 'std' not found`, or a BMI built by another compiler build) and is rejected up front.
- **Suppressions**: fix a finding when possible. Otherwise use a targeted, justified
  `NOLINT(check-name): reason` at the site (e.g. the module's exported `using` declarations, `main`
  in examples, the deliberately out-of-range enum cast in `LogLevelSpec.cpp`), never a blanket one.
  `tests/.clang-tidy` and `examples/.clang-tidy` (inheriting the root config) turn off only the two
  magic-number checks there, because scenario data are literals; `include/` keeps them.

## Pull Requests

- **One commit per pull request.**
- Keep purely mechanical changes (reformatting, renames) in their own pull request, separate from
  semantic changes, and merge them first so the semantic diff stays readable.
- The pull request (PR) title must reference the related issue or feature (e.g., `Add CameraManager class [#42]`).
- Provide a clear description of the changes and the motivation.
- Ensure your branch is up to date with `main` before submitting.
- All code must pass CI checks and tests before merging.
- Request a review from at least one maintainer.

## Additional Notes

- Write clear, descriptive commit messages.
- Add or update documentation as needed.
- Run all tests locally before submitting your PR.

