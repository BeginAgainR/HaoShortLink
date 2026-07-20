#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace shortlink
{
namespace consumer
{

class ConsumerMetrics
{
public:
    enum class MessageResult : std::size_t
    {
        Aggregated,
        Duplicate,
        Ignored,
        Dlq,
        Error,
        Count
    };

    enum class RetryOperation : std::size_t
    {
        Mysql,
        OffsetCommit,
        DlqPublish,
        Count
    };

    enum class DlqResult : std::size_t
    {
        Success,
        Failure,
        Count
    };

    ConsumerMetrics();

    void recordMessage(MessageResult result) noexcept;
    void recordRetry(RetryOperation operation) noexcept;
    void recordDlq(DlqResult result) noexcept;
    void setLag(std::int32_t partition, std::int64_t lag);
    void replaceLag(const std::map<std::int32_t, std::int64_t>& lag);
    void recordSuccess() noexcept;
    void setHealthy(bool healthy) noexcept;
    bool healthy() const noexcept;
    std::string renderPrometheus() const;

private:
    static constexpr std::size_t kMessageResultCount =
        static_cast<std::size_t>(MessageResult::Count);
    static constexpr std::size_t kRetryOperationCount =
        static_cast<std::size_t>(RetryOperation::Count);
    static constexpr std::size_t kDlqResultCount =
        static_cast<std::size_t>(DlqResult::Count);

    std::array<std::atomic<std::uint64_t>, kMessageResultCount> messageCounters_;
    std::array<std::atomic<std::uint64_t>, kRetryOperationCount> retryCounters_;
    std::array<std::atomic<std::uint64_t>, kDlqResultCount> dlqCounters_;
    mutable std::mutex lagMutex_;
    std::map<std::int32_t, std::int64_t> lag_;
    std::atomic<std::int64_t> lastSuccessUnixTime_ { 0 };
    std::atomic<bool> healthy_ { false };
};

} // namespace consumer
} // namespace shortlink
