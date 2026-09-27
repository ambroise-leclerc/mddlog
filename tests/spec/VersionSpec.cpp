/**
 * @brief Version traceability (docs/release-process.md, step 3).
 *
 * The literal below is deliberately hard-coded. project(mddlog VERSION ...) is the single place
 * the version is defined and getVersion() derives from it, so comparing getVersion() with a value
 * derived the same way would assert nothing. A release moves this literal together with
 * project(), which is how a build that reports a version other than the declared one is caught.
 */
import std;
import speclab;
import mddlog;

namespace {

constexpr std::string_view declaredVersion = "0.1.0";

const speclab::Register reportedVersionMatchesDeclaredRelease{"The library reports the version this release declares", "unit", [] {
                                                                  return speclab::Test("version-traceability")
                                                                      .Then("getVersion() returns the declared release version",
                                                                            [] {
                                                                                speclab::core::Checks checks;
                                                                                checks.expect(mddlog::getVersion() == declaredVersion,
                                                                                              "getVersion() matches the declared release version");
                                                                                checks.raise();
                                                                            })
                                                                      .Execute();
                                                              }};

}  // namespace
