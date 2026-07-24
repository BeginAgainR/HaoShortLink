#pragma once

#include <optional>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace shortlink
{

class ShortLinkRepository
{
public:
    enum class Status
    {
        Active,
        Disabled
    };

    enum class LookupSource
    {
        Memory,
        Mysql,
        Redis
    };

    struct ShortLinkRecord
    {
        std::uint64_t id { 0 };
        std::uint64_t ownerId { 0 };
        std::string code;
        std::string originalUrl;
        Status status { Status::Active };
        std::optional<std::int64_t> expiresAt;
        std::int64_t createdAt { 0 };
        std::int64_t updatedAt { 0 };
    };

    struct LookupResult
    {
        std::optional<ShortLinkRecord> record;
        LookupSource source { LookupSource::Memory };
    };

    struct ListQuery
    {
        std::uint64_t cursor { 0 };
        std::size_t limit { 50 };
        std::optional<std::uint64_t> ownerId;
        std::optional<Status> status;
    };

    struct LifecycleUpdate
    {
        std::optional<Status> status;
        bool expiresAtProvided { false };
        std::optional<std::int64_t> expiresAt;
        std::optional<std::uint64_t> ownerId;
    };

    class ShortCodeConflict : public std::runtime_error
    {
    public:
        ShortCodeConflict()
            : std::runtime_error("Short code already exists")
        {}
    };

    virtual ~ShortLinkRepository() = default;

    virtual std::optional<ShortLinkRecord> create(
        const std::string& originalUrl,
        std::optional<std::int64_t> expiresAt = std::nullopt,
        std::uint64_t ownerId = 1,
        std::optional<std::string> customCode = std::nullopt) = 0;
    virtual LookupResult findByCode(const std::string& code) const = 0;
    virtual std::optional<ShortLinkRecord> findByCodeForOwner(
        const std::string& code,
        std::uint64_t ownerId) const = 0;
    virtual LookupSource defaultLookupSource() const noexcept = 0;
    virtual std::vector<ShortLinkRecord> list(const ListQuery& query) const = 0;
    virtual std::optional<ShortLinkRecord> updateLifecycle(
        const std::string& code,
        const LifecycleUpdate& update) = 0;
};

const char* statusToString(ShortLinkRepository::Status status) noexcept;
std::optional<ShortLinkRepository::Status> parseStatus(const std::string& value) noexcept;

} // namespace shortlink
