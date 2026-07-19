#include "shortlink/ConsumerMetrics.h"

#include <chrono>
#include <sstream>

namespace shortlink
{
namespace consumer
{
namespace
{

const std::array<const char*, 5> kMessageResultLabels {
    "aggregated", "duplicate", "ignored", "dlq", "error"
};
const std::array<const char*, 3> kRetryOperationLabels {
    "mysql", "offset_commit", "dlq_publish"
};
const std::array<const char*, 2> kDlqResultLabels { "success", "failure" };

template <typename Counters>
void resetCounters(Counters* counters)
{
    for (std::atomic<std::uint64_t>& counter : *counters)
    {
        counter.store(0, std::memory_order_relaxed);
    }
}

} // namespace

ConsumerMetrics::ConsumerMetrics()
{
    resetCounters(&messageCounters_);
    resetCounters(&retryCounters_);
    resetCounters(&dlqCounters_);
}

void ConsumerMetrics::recordMessage(MessageResult result) noexcept
{
    messageCounters_[static_cast<std::size_t>(result)].fetch_add(
        1,
        std::memory_order_relaxed);
}

void ConsumerMetrics::recordRetry(RetryOperation operation) noexcept
{
    retryCounters_[static_cast<std::size_t>(operation)].fetch_add(
        1,
        std::memory_order_relaxed);
}

void ConsumerMetrics::recordDlq(DlqResult result) noexcept
{
    dlqCounters_[static_cast<std::size_t>(result)].fetch_add(
        1,
        std::memory_order_relaxed);
}

void ConsumerMetrics::setLag(std::int32_t partition, std::int64_t lag)
{
    std::lock_guard<std::mutex> lock(lagMutex_);
    lag_[partition] = lag;
}

void ConsumerMetrics::replaceLag(const std::map<std::int32_t, std::int64_t>& lag)
{
    std::lock_guard<std::mutex> lock(lagMutex_);
    lag_ = lag;
}

void ConsumerMetrics::recordSuccess() noexcept
{
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    lastSuccessUnixTime_.store(now, std::memory_order_relaxed);
}

void ConsumerMetrics::setHealthy(bool healthy) noexcept
{
    healthy_.store(healthy, std::memory_order_relaxed);
}

bool ConsumerMetrics::healthy() const noexcept
{
    return healthy_.load(std::memory_order_relaxed);
}

std::string ConsumerMetrics::renderPrometheus() const
{
    std::ostringstream output;
    output << "# HELP haohttp_shortlink_access_consumer_messages_total Access event consumer results.\n"
           << "# TYPE haohttp_shortlink_access_consumer_messages_total counter\n";
    for (std::size_t index = 0; index < kMessageResultLabels.size(); ++index)
    {
        output << "haohttp_shortlink_access_consumer_messages_total{result=\""
               << kMessageResultLabels[index] << "\"} "
               << messageCounters_[index].load(std::memory_order_relaxed) << '\n';
    }

    output << "# HELP haohttp_shortlink_access_consumer_retries_total Access event consumer retries.\n"
           << "# TYPE haohttp_shortlink_access_consumer_retries_total counter\n";
    for (std::size_t index = 0; index < kRetryOperationLabels.size(); ++index)
    {
        output << "haohttp_shortlink_access_consumer_retries_total{operation=\""
               << kRetryOperationLabels[index] << "\"} "
               << retryCounters_[index].load(std::memory_order_relaxed) << '\n';
    }

    output << "# HELP haohttp_shortlink_access_consumer_dlq_total Access event DLQ delivery results.\n"
           << "# TYPE haohttp_shortlink_access_consumer_dlq_total counter\n";
    for (std::size_t index = 0; index < kDlqResultLabels.size(); ++index)
    {
        output << "haohttp_shortlink_access_consumer_dlq_total{result=\""
               << kDlqResultLabels[index] << "\"} "
               << dlqCounters_[index].load(std::memory_order_relaxed) << '\n';
    }

    output << "# HELP haohttp_shortlink_access_consumer_lag Uncommitted access events by partition.\n"
           << "# TYPE haohttp_shortlink_access_consumer_lag gauge\n";
    {
        std::lock_guard<std::mutex> lock(lagMutex_);
        for (const auto& item : lag_)
        {
            output << "haohttp_shortlink_access_consumer_lag{partition=\""
                   << item.first << "\"} " << item.second << '\n';
        }
    }

    output << "# HELP haohttp_shortlink_access_consumer_last_success_unixtime Last successfully committed event time.\n"
           << "# TYPE haohttp_shortlink_access_consumer_last_success_unixtime gauge\n"
           << "haohttp_shortlink_access_consumer_last_success_unixtime "
           << lastSuccessUnixTime_.load(std::memory_order_relaxed) << '\n';
    return output.str();
}

} // namespace consumer
} // namespace shortlink
