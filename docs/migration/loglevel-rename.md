# Migrating to the CamelCase `LogLevel` enumerators

This page records the earlier #7 rename. Since #58, `LogLevel::Audit` has been
removed: audit is a separate `AuditEvent`, not a diagnostic severity. See
[audit admission migration](audit-admission.md). `fromString()` now returns
`std::optional<LogLevel>` and rejects `"AUDIT"` or any unknown input.

`mddlog::LogLevel` enumerators were renamed to `UpperCamelCase` to satisfy
`readability-identifier-naming.EnumConstantCase: CamelCase` (see `CONTRIBUTING.md`).

**This is a source-compatibility break.** The old identifiers no longer exist and **no deprecated
alias is provided**: code using `LogLevel::INFO` and friends stops compiling until it is updated.

| Before | After | Numeric value | Text produced by `toString()` |
|---|---|---|---|
| `LogLevel::TRACE` | `LogLevel::Trace` | 0 | `"TRACE"` |
| `LogLevel::DEBUG` | `LogLevel::Debug` | 1 | `"DEBUG"` |
| `LogLevel::INFO`  | `LogLevel::Info`  | 2 | `"INFO"`  |
| `LogLevel::WARN`  | `LogLevel::Warn`  | 3 | `"WARN"`  |
| `LogLevel::ERROR` | `LogLevel::Error` | 4 | `"ERROR"` |
| `LogLevel::FATAL` | `LogLevel::Fatal` | 5 | `"FATAL"` |
| `LogLevel::AUDIT` | `LogLevel::Audit` (historical; removed in #58) | 6 (historical) | `"AUDIT"` (historical) |

What did **not** change in the #7 rename (before the #58 audit migration):

- Numeric values and the underlying type (`std::uint8_t`), so comparisons, filtering thresholds and
  any stored or transmitted numeric level behave exactly as before.
- At that time, `toString()` still returned the upper-case strings above and
  `fromString()` still fell back to `Info` for an unrecognised string. Since #58,
  `"AUDIT"` is rejected and all unknown strings yield `nullopt`; diagnostic console
  output retains the six remaining spellings.

A mechanical migration of the remaining diagnostic call sites is a word-bounded search and replace of the six qualified
names in the first two columns, for example `perl -pi -e 's/\bLogLevel::INFO\b/LogLevel::Info/g' <files>`
(Perl rather than `sed`, whose `\b` word boundary is not portable to BSD/macOS `sed`).
Do not touch string literals such as `"INFO"`.
