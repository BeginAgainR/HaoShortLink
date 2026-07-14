#include "shortlink/ShortLinkMetrics.h"

#include <sstream>

namespace shortlink
{
namespace
{

const std::array<const char*, 2> kStorageLabels { "memory", "mysql" };
const std::array<const char*, 3> kCreateResultLabels { "success", "invalid", "error" };
const std::array<const char*, 3> kRedirectSourceLabels { "memory", "mysql", "redis" };
const std::array<const char*, 3> kRedirectResultLabels { "success", "not_found", "error" };
const std::array<const char*, 2> kCacheOperationLabels { "get", "set" };
const std::array<const char*, 4> kCacheResultLabels { "hit", "miss", "success", "error" };
const std::array<const char*, 2> kBackendLabels { "mysql", "redis" };
const std::array<const char*, 4> kBackendOperationLabels { "create", "find", "get", "set" };

template <typename Counters>
void resetCounters(Counters* counters)
{
    for (std::atomic<std::uint64_t>& counter : *counters)
    {
        counter.store(0, std::memory_order_relaxed);
    }
}

} // namespace

ShortLinkMetrics::ShortLinkMetrics()
{
    resetCounters(&createCounters_);
    resetCounters(&redirectCounters_);
    resetCounters(&cacheCounters_);
    resetCounters(&backendErrorCounters_);
}

void ShortLinkMetrics::recordCreate(CreateResult result, Storage storage) noexcept
{
    const std::size_t index = static_cast<std::size_t>(storage) * kCreateResultCount +
                              static_cast<std::size_t>(result);
    createCounters_[index].fetch_add(1, std::memory_order_relaxed);
}

void ShortLinkMetrics::recordRedirect(RedirectResult result, RedirectSource source) noexcept
{
    const std::size_t index = static_cast<std::size_t>(source) * kRedirectResultCount +
                              static_cast<std::size_t>(result);
    redirectCounters_[index].fetch_add(1, std::memory_order_relaxed);
}

void ShortLinkMetrics::recordCache(CacheOperation operation, CacheResult result) noexcept
{
    const std::size_t index = static_cast<std::size_t>(operation) * kCacheResultCount +
                              static_cast<std::size_t>(result);
    cacheCounters_[index].fetch_add(1, std::memory_order_relaxed);
}

void ShortLinkMetrics::recordBackendError(Backend backend, BackendOperation operation) noexcept
{
    const std::size_t index = static_cast<std::size_t>(backend) * kBackendOperationCount +
                              static_cast<std::size_t>(operation);
    backendErrorCounters_[index].fetch_add(1, std::memory_order_relaxed);
}

std::string ShortLinkMetrics::renderPrometheus() const
{
    std::ostringstream output;
    output << "# HELP haohttp_shortlink_create_total Short link creation results.\n"
           << "# TYPE haohttp_shortlink_create_total counter\n";
    for (std::size_t storage = 0; storage < kStorageLabels.size(); ++storage)
    {
        for (std::size_t result = 0; result < kCreateResultLabels.size(); ++result)
        {
            const std::size_t index = storage * kCreateResultCount + result;
            output << "haohttp_shortlink_create_total{result=\"" << kCreateResultLabels[result]
                   << "\",storage=\"" << kStorageLabels[storage] << "\"} "
                   << createCounters_[index].load(std::memory_order_relaxed) << '\n';
        }
    }

    output << "# HELP haohttp_shortlink_redirect_total Short link redirect lookup results.\n"
           << "# TYPE haohttp_shortlink_redirect_total counter\n";
    for (std::size_t source = 0; source < kRedirectSourceLabels.size(); ++source)
    {
        for (std::size_t result = 0; result < kRedirectResultLabels.size(); ++result)
        {
            const std::size_t index = source * kRedirectResultCount + result;
            output << "haohttp_shortlink_redirect_total{result=\"" << kRedirectResultLabels[result]
                   << "\",source=\"" << kRedirectSourceLabels[source] << "\"} "
                   << redirectCounters_[index].load(std::memory_order_relaxed) << '\n';
        }
    }

    output << "# HELP haohttp_shortlink_cache_operations_total Redis cache operation results.\n"
           << "# TYPE haohttp_shortlink_cache_operations_total counter\n";
    for (std::size_t operation = 0; operation < kCacheOperationLabels.size(); ++operation)
    {
        for (std::size_t result = 0; result < kCacheResultLabels.size(); ++result)
        {
            const bool validCombination =
                (operation == static_cast<std::size_t>(CacheOperation::Get) &&
                 result != static_cast<std::size_t>(CacheResult::Success)) ||
                (operation == static_cast<std::size_t>(CacheOperation::Set) &&
                 (result == static_cast<std::size_t>(CacheResult::Success) ||
                  result == static_cast<std::size_t>(CacheResult::Error)));
            if (!validCombination)
            {
                continue;
            }

            const std::size_t index = operation * kCacheResultCount + result;
            output << "haohttp_shortlink_cache_operations_total{operation=\""
                   << kCacheOperationLabels[operation]
                   << "\",result=\"" << kCacheResultLabels[result] << "\"} "
                   << cacheCounters_[index].load(std::memory_order_relaxed) << '\n';
        }
    }

    output << "# HELP haohttp_shortlink_backend_errors_total Short link backend operation errors.\n"
           << "# TYPE haohttp_shortlink_backend_errors_total counter\n";
    for (std::size_t backend = 0; backend < kBackendLabels.size(); ++backend)
    {
        for (std::size_t operation = 0; operation < kBackendOperationLabels.size(); ++operation)
        {
            const bool validCombination =
                (backend == static_cast<std::size_t>(Backend::Mysql) &&
                 operation <= static_cast<std::size_t>(BackendOperation::Find)) ||
                (backend == static_cast<std::size_t>(Backend::Redis) &&
                 operation >= static_cast<std::size_t>(BackendOperation::Get));
            if (!validCombination)
            {
                continue;
            }

            const std::size_t index = backend * kBackendOperationCount + operation;
            output << "haohttp_shortlink_backend_errors_total{backend=\"" << kBackendLabels[backend]
                   << "\",operation=\"" << kBackendOperationLabels[operation] << "\"} "
                   << backendErrorCounters_[index].load(std::memory_order_relaxed) << '\n';
        }
    }

    return output.str();
}

} // namespace shortlink
