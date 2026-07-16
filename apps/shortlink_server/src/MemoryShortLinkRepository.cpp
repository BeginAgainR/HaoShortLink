#include "shortlink/MemoryShortLinkRepository.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace shortlink
{

namespace
{

const char kBase62Chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr std::size_t kBase62Size = 62;
constexpr std::size_t kMinCodeLength = 6;

} // namespace

std::optional<ShortLinkRepository::ShortLinkRecord>
MemoryShortLinkRepository::create(const std::string& originalUrl,
                                  std::optional<std::int64_t> expiresAt)
{
    std::lock_guard<std::mutex> lock(mutex_);

    const std::uint64_t id = nextId_++;
    const std::string code = encodeBase62(id);
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();

    const ShortLinkRecord record {
        id,
        code,
        originalUrl,
        Status::Active,
        expiresAt,
        now,
        now
    };
    links_[code] = record;
    return record;
}

ShortLinkRepository::LookupResult MemoryShortLinkRepository::findByCode(
    const std::string& code) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = links_.find(code);
    if (it == links_.end())
    {
        return { std::nullopt, LookupSource::Memory };
    }
    return { it->second, LookupSource::Memory };
}

std::vector<ShortLinkRepository::ShortLinkRecord> MemoryShortLinkRepository::list(
    const ListQuery& query) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ShortLinkRecord> records;
    records.reserve(links_.size());
    for (const auto& item : links_)
    {
        if (item.second.id <= query.cursor || (query.status && item.second.status != *query.status))
        {
            continue;
        }
        records.push_back(item.second);
    }
    std::sort(records.begin(), records.end(), [](const ShortLinkRecord& left,
                                                  const ShortLinkRecord& right) {
        return left.id < right.id;
    });
    if (records.size() > query.limit)
    {
        records.resize(query.limit);
    }
    return records;
}

std::optional<ShortLinkRepository::ShortLinkRecord> MemoryShortLinkRepository::updateLifecycle(
    const std::string& code,
    const LifecycleUpdate& update)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = links_.find(code);
    if (it == links_.end())
    {
        return std::nullopt;
    }
    if (update.status)
    {
        it->second.status = *update.status;
    }
    if (update.expiresAtProvided)
    {
        it->second.expiresAt = update.expiresAt;
    }
    it->second.updatedAt = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    return it->second;
}

std::string MemoryShortLinkRepository::encodeBase62(std::uint64_t value)
{
    if (value == 0)
    {
        return std::string(kMinCodeLength, '0');
    }

    std::string encoded;
    while (value > 0)
    {
        encoded.push_back(kBase62Chars[value % kBase62Size]);
        value /= kBase62Size;
    }

    while (encoded.size() < kMinCodeLength)
    {
        encoded.push_back('0');
    }

    std::reverse(encoded.begin(), encoded.end());
    return encoded;
}

} // namespace shortlink
