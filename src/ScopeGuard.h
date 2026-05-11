#pragma once

#include <utility>

namespace firebolt::adbc
{

template <typename Fn>
class ScopeGuard
{
public:
    explicit ScopeGuard(Fn && fn)
        : fn_(std::move(fn))
    {
    }

    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard & operator=(const ScopeGuard &) = delete;
    ScopeGuard(ScopeGuard &&) = delete;
    ScopeGuard & operator=(ScopeGuard &&) = delete;

    ~ScopeGuard() { fn_(); }

private:
    Fn fn_;
};

} // namespace firebolt::adbc

#define FIREBOLT_INTERNAL_SCOPE_GUARD_CONCAT_INNER(x, y) x##y
#define FIREBOLT_INTERNAL_SCOPE_GUARD_CONCAT(x, y) FIREBOLT_INTERNAL_SCOPE_GUARD_CONCAT_INNER(x, y)
#define FIREBOLT_SCOPE_GUARD(...) \
    ::firebolt::adbc::ScopeGuard FIREBOLT_INTERNAL_SCOPE_GUARD_CONCAT(_firebolt_scope_guard_, __COUNTER__)([&]() { __VA_ARGS__; })
