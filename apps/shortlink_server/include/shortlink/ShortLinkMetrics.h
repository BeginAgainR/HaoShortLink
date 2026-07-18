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
        Disabled,
        Expired,
        Error,
        Count
    };

    enum class CacheOperation : std::size_t
    {
        Get,
        Set,
        Delete,
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
        List,
        Update,
        Get,
        Set,
        Delete,
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

    enum class AccessEventEnqueueResult : std::size_t
    {
        Accepted,
        QueueFull,
        Error,
        Count
    };

    enum class AccessEventDeliveryResult : std::size_t
    {
        Success,
        Failure,
        Count
    };

    ShortLinkMetrics();

    void recordCreate(CreateResult result, Storage storage) noexcept;
    void recordRedirect(RedirectResult result, RedirectSource source) noexcept;
    void recordCache(CacheOperation operation, CacheResult result) noexcept;
    void recordBackendError(Backend backend, BackendOperation operation) noexcept;
    void recordRateLimit(RateLimitResult result) noexcept;
    void recordAccessEventEnqueue(AccessEventEnqueueResult result) noexcept;
    void recordAccessEventDelivery(AccessEventDeliveryResult result) noexcept;
    void setAccessEventQueueSize(std::uint64_t size) noexcept;
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
    static constexpr std::size_t kAccessEventEnqueueResultCount =
        static_cast<std::size_t>(AccessEventEnqueueResult::Count);
    static constexpr std::size_t kAccessEventDeliveryResultCount =
        static_cast<std::size_t>(AccessEventDeliveryResult::Count);

    using CreateCounters = std::array<std::atomic<std::uint64_t>, kStorageCount * kCreateResultCount>;
    using RedirectCounters = std::array<std::atomic<std::uint64_t>, kRedirectSourceCount * kRedirectResultCount>;
    using CacheCounters = std::array<std::atomic<std::uint64_t>, kCacheOperationCount * kCacheResultCount>;
    using BackendErrorCounters = std::array<std::atomic<std::uint64_t>, kBackendCount * kBackendOperationCount>;
    using RateLimitCounters = std::array<std::atomic<std::uint64_t>, kRateLimitResultCount>;
    using AccessEventEnqueueCounters =
        std::array<std::atomic<std::uint64_t>, kAccessEventEnqueueResultCount>;
    using AccessEventDeliveryCounters =
        std::array<std::atomic<std::uint64_t>, kAccessEventDeliveryResultCount>;

    CreateCounters createCounters_;
    RedirectCounters redirectCounters_;
    CacheCounters cacheCounters_;
    BackendErrorCounters backendErrorCounters_;
    RateLimitCounters rateLimitCounters_;
    AccessEventEnqueueCounters accessEventEnqueueCounters_;
    AccessEventDeliveryCounters accessEventDeliveryCounters_;
    std::atomic<std::uint64_t> accessEventQueueSize_ { 0 };
};

} // namespace shortlink
