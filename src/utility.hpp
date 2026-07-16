#ifndef ZANE_UTILITY_H
#define ZANE_UTILITY_H

#include <cstdint>
#include <cassert>
#include <limits>
#include <type_traits>
#include <utility>

// ============================================================================
// LIFETIME_BOUND — annotate that a reference's lifetime must outlive the caller
// ============================================================================

#if __has_cpp_attribute(msvc::lifetimebound)
    #define LIFETIME_BOUND [[msvc::lifetimebound]]
#elif __has_cpp_attribute(clang::lifetimebound)
    #define LIFETIME_BOUND [[clang::lifetimebound]]
#else
    #define LIFETIME_BOUND
#endif

namespace zane {
namespace utility {

// ============================================================================
// narrow — checked narrowing cast (from GSL)
// Throws std::overflow_error if narrowing loses data.
// ============================================================================

template<typename T, typename U>
T narrow(U v) {
    static_assert(std::is_arithmetic_v<T>, "T must be arithmetic");
    static_assert(std::is_arithmetic_v<U>, "U must be arithmetic");

    T t = static_cast<T>(v);
    if (static_cast<U>(t) != v) {
        throw std::overflow_error("zane::utility::narrow: value out of range");
    }

    // Signed/unsigned mismatch — check sign
    if constexpr (std::is_signed_v<T> != std::is_signed_v<U>) {
        if constexpr (std::is_signed_v<T>) {
            // T is signed, U is unsigned — ensure v fits in T's positive range
            if (v > static_cast<U>(std::numeric_limits<T>::max())) {
                throw std::overflow_error("zane::utility::narrow: value out of range (positive)");
            }
        } else {
            // T is unsigned, U is signed — ensure v is non-negative
            if (v < 0) {
                throw std::overflow_error("zane::utility::narrow: value out of range (negative)");
            }
        }
    }

    return t;
}

// ============================================================================
// narrow_cast — unchecked narrow (for hot paths where bounds are guaranteed)
// ============================================================================

template<typename T, typename U>
T narrow_cast(U v) {
    static_assert(std::is_arithmetic_v<T>, "T must be arithmetic");
    static_assert(std::is_arithmetic_v<U>, "U must be arithmetic");
    return static_cast<T>(v);
}

// ============================================================================
// scope_guard — execute a function on scope exit (gsl::finally equivalent)
// ============================================================================

template<typename F>
class ScopeGuard {
public:
    explicit ScopeGuard(F fn) : m_fn(std::move(fn)) {}

    ~ScopeGuard() noexcept(noexcept(std::declval<F&>())) {
        m_fn();
    }

    // Non-copyable, non-movable
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard(ScopeGuard&&) = delete;
    ScopeGuard& operator=(ScopeGuard&&) = delete;

private:
    F m_fn;
};

// Deduction guide
template<typename F>
ScopeGuard(F) -> ScopeGuard<F>;

// Helper to create a ScopeGuard with auto type deduction
template<typename F>
ScopeGuard<F> makeScopeGuard(F fn) {
    return ScopeGuard<F>(std::move(fn));
}

// ============================================================================
// not_null — lightweight wrapper (minimal GSL::not_null equivalent)
// Only for pointer parameters. SKIP for V8 handles.
// ============================================================================

template<typename T>
class NotNull {
public:
    explicit NotNull(T p) : m_ptr(p) {
        assert(m_ptr != nullptr && "NotNull: pointer cannot be null");
    }

    T get() const noexcept { return m_ptr; }
    operator T() const noexcept { return m_ptr; }
    T operator->() const noexcept { return m_ptr; }
    auto& operator*() const noexcept { return *m_ptr; }

private:
    T m_ptr;
};

} // namespace utility
} // namespace zane

#endif // ZANE_UTILITY_H
