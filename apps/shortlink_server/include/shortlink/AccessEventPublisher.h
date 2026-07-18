#pragma once

#include "shortlink/AccessEvent.h"

namespace shortlink
{

class AccessEventPublisher
{
public:
    virtual ~AccessEventPublisher() = default;
    virtual void publish(const AccessEvent& event) noexcept = 0;
};

class NoopAccessEventPublisher final : public AccessEventPublisher
{
public:
    void publish(const AccessEvent& event) noexcept override
    {
        (void)event;
    }
};

} // namespace shortlink
