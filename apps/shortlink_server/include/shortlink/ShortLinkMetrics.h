#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace shortlink
{

class ShortLinkMetrics
{
public:
    enum class Storage : std::size_t
    {
        Memory,
        Mysql,
        Count
    };

    enum class CreateResult : std::size_t
    {
        Success,
        Invalid,
        Error,
        Count
    };

    enum class RedirectSource : std::size_t
    {
        Memory,
        Mysql,
        Redis,
        Count
    };

    enum class RedirectResult : std::size_t
    {
        Success,
        NotFound,
        Error,
        Count
    };

    enum class CacheOperation : std::size_t
    {
        Get,
        Set,
        Count
    };

    enum class CacheResult : std::size_t
    {
        Hit,
        Miss,
        Success,
        Error,
        Count
    };

    enum class Backend : std::size_t
    {
        Mysql,
        Redis,
        Count
    };

    enum class BackendOperation : std::size_t
    {
        Create,
        Find,
        Get,
        Set,
        RateLimit,
        Count
    };

    enum class RateLimitResult : std::size_t
    {
        Allowed,
        Limited,
        Error,
        Count
    };

    ShortLinkMetrics();

    void recordCreate(CreateResult result, Storage storage) noexcept;
    void recordRedirect(RedirectResult result, RedirectSource source) noexcept;
    void recordCache(CacheOperation operation, CacheResult result) noexcept;
    void recordBackendError(Backend backend, BackendOperation operation) noexcept;
    void recordRateLimit(RateLimitResult result) noexcept;
    std::string renderPrometheus() const;

private:
    static constexpr std::size_t kStorageCount = static_cast<std::size_t>(Storage::Count);
    static constexpr std::size_t kCreateResultCount = static_cast<std::size_t>(CreateResult::Count);
    static constexpr std::size_t kRedirectSourceCount = static_cast<std::size_t>(RedirectSource::Count);
    static constexpr std::size_t kRedirectResultCount = static_cast<std::size_t>(RedirectResult::Count);
    static constexpr std::size_t kCacheOperationCount = static_cast<std::size_t>(CacheOperation::Count);
    static constexpr std::size_t kCacheResultCount = static_cast<std::size_t>(CacheResult::Count);
    static constexpr std::size_t kBackendCount = static_cast<std::size_t>(Backend::Count);
    static constexpr std::size_t kBackendOperationCount = static_cast<std::size_t>(BackendOperation::Count);
    static constexpr std::size_t kRateLimitResultCount = static_cast<std::size_t>(RateLimitResult::Count);

    using CreateCounters = std::array<std::atomic<std::uint64_t>, kStorageCount * kCreateResultCount>;
    using RedirectCounters = std::array<std::atomic<std::uint64_t>, kRedirectSourceCount * kRedirectResultCount>;
    using CacheCounters = std::array<std::atomic<std::uint64_t>, kCacheOperationCount * kCacheResultCount>;
    using BackendErrorCounters = std::array<std::atomic<std::uint64_t>, kBackendCount * kBackendOperationCount>;
    using RateLimitCounters = std::array<std::atomic<std::uint64_t>, kRateLimitResultCount>;

    CreateCounters createCounters_;
    RedirectCounters redirectCounters_;
    CacheCounters cacheCounters_;
    BackendErrorCounters backendErrorCounters_;
    RateLimitCounters rateLimitCounters_;
};

} // namespace shortlink
