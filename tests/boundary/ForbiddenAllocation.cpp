/** @brief Deliberate allocator references; compiled only as a scan negative control. */
#include <cstdlib>
#include <new>

// NOLINTNEXTLINE(misc-use-internal-linkage): keep this deliberate scan violation emitted in optimized builds.
void* forbiddenNew(std::size_t size) {
    return ::operator new(size);
}

// NOLINTNEXTLINE(misc-use-internal-linkage): keep this deliberate scan violation emitted in optimized builds.
void* forbiddenMalloc(std::size_t size) {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc,hicpp-no-malloc): negative control; never linked or executed.
    return std::malloc(size);
}
