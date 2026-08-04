#ifndef DAGE_RESOURCE_LEASE_HPP
#define DAGE_RESOURCE_LEASE_HPP
#include "dage/result.hpp"
#include <cstdint>
#include <memory>
#include <map>
#include <string>
namespace dage {
class CancellationToken;
struct ResourceRequest {
    std::string run_id,node_id,node_type,executor;
    std::uint32_t units=1;
    std::map<std::string,std::uint64_t> resources;
    std::int32_t priority=0;
    std::string fairness_key;
    std::uint64_t lease_ttl_ms=30000;
    std::uint64_t deadline_remaining_ms=0;
};
class ResourceLease {
public:
    virtual ~ResourceLease()=default;
    virtual const std::string& lease_id()const noexcept=0;
    virtual std::uint64_t fencing_token()const noexcept=0;
    virtual std::uint64_t expires_at_unix_ms()const noexcept=0;
    virtual Result<std::uint64_t> renew(std::uint64_t ttl_ms)=0;
};
class ResourceLeaseProvider {
public:
    virtual ~ResourceLeaseProvider()=default;
    virtual Result<std::unique_ptr<ResourceLease>> acquire(
        const ResourceRequest&,const CancellationToken*)=0;
};
class SemaphoreLeaseProvider final : public ResourceLeaseProvider {
public:
    class Impl;
    explicit SemaphoreLeaseProvider(std::uint32_t capacity);
    Result<std::unique_ptr<ResourceLease>> acquire(
        const ResourceRequest&,const CancellationToken*) override;
    std::uint32_t capacity()const noexcept;
    std::uint32_t available()const noexcept;
private:
    std::shared_ptr<Impl> impl_;
};
class FairResourceLeaseProvider final : public ResourceLeaseProvider {
public:
    class Impl;
    explicit FairResourceLeaseProvider(std::map<std::string,std::uint64_t> capacities);
    Result<std::unique_ptr<ResourceLease>> acquire(
        const ResourceRequest&,const CancellationToken*) override;
    std::map<std::string,std::uint64_t> capacities()const;
    std::map<std::string,std::uint64_t> available()const;
private:
    std::shared_ptr<Impl> impl_;
};
}
#endif
