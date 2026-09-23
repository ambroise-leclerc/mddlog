# Migrating to the CamelCase `LogLevel` enumerators

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
| `LogLevel::AUDIT` | `LogLevel::Audit` | 6 | `"AUDIT"` |

What did **not** change:

- Numeric values and the underlying type (`std::uint8_t`), so comparisons, filtering thresholds and
  any stored or transmitted numeric level behave exactly as before.
- Produced and accepted text: `toString()` still returns the upper-case strings above (and
  `"UNKNOWN"` outside the enumeration); `fromString()` still accepts them and still falls back to
  `Info` for an unrecognised string. Console output and messages are identical.

A mechanical migration of call sites is a word-bounded search and replace of the seven qualified
names in the first two columns, for example `perl -pi -e 's/\bLogLevel::INFO\b/LogLevel::Info/g' <files>`
(Perl rather than `sed`, whose `\b` word boundary is not portable to BSD/macOS `sed`).
Do not touch string literals such as `"INFO"`.
