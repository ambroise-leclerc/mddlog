/** @brief Deliberate throw reference; compiled only as a scan negative control. */
// NOLINTNEXTLINE(misc-use-internal-linkage): keep this deliberate scan violation emitted in optimized builds.
void forbiddenThrow() {
    // NOLINTNEXTLINE(hicpp-exception-baseclass): primitive exception isolates the throw entry point from allocating library helpers.
    throw 1;
}
