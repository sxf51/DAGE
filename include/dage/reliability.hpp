#ifndef DAGE_RELIABILITY_HPP
#define DAGE_RELIABILITY_HPP

#include "dage/result.hpp"

#include <map>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <string>
#include <vector>

namespace dage {

class CancellationToken {
public:
    bool is_cancelled() const noexcept;
    std::string reason() const;
    void request_cancel(const std::string& reason = "cancelled") noexcept;
    bool wait_for(std::uint64_t timeout_ms) const;
private:
    std::atomic<bool> cancelled_{false};
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::string reason_;
};

struct StateRecord {
    std::uint64_t version = 0;
    std::uint64_t epoch = 0;
    std::string owner;
    std::string checkpoint;
};

class StateStore {
public:
    virtual ~StateStore() = default;
    virtual Result<bool> put(const std::string& run_id, const std::string& checkpoint) = 0;
    virtual Result<std::string> get(const std::string& run_id) const = 0;
    virtual Result<bool> erase(const std::string& run_id) = 0;
    virtual Result<StateRecord> load(const std::string& run_id) const = 0;
    virtual Result<std::uint64_t> compare_exchange(const std::string& run_id,
                                                   std::uint64_t expected_version,
                                                   const std::string& checkpoint) = 0;
    virtual Result<StateRecord> claim(const std::string& run_id,std::uint64_t expected_version,
                                      const std::string& owner) = 0;
    virtual Result<std::vector<std::string>> list(const std::string& prefix) const = 0;
};

class MemoryStateStore final : public StateStore {
public:
    Result<bool> put(const std::string& run_id, const std::string& checkpoint) override;
    Result<std::string> get(const std::string& run_id) const override;
    Result<bool> erase(const std::string& run_id) override;
    Result<StateRecord> load(const std::string& run_id) const override;
    Result<std::uint64_t> compare_exchange(const std::string& run_id,
                                           std::uint64_t expected_version,
                                           const std::string& checkpoint) override;
    Result<StateRecord> claim(const std::string&,std::uint64_t,const std::string&) override;
    Result<std::vector<std::string>> list(const std::string&) const override;
private:
    mutable std::mutex mutex_;
    std::map<std::string, StateRecord> states_;
};

// Durable reference implementation. Each record is replaced atomically and updates use
// optimistic version checks. The directory may be shared by cooperating processes.
class FileStateStore final : public StateStore {
public:
    explicit FileStateStore(std::string directory);
    Result<bool> put(const std::string& run_id, const std::string& checkpoint) override;
    Result<std::string> get(const std::string& run_id) const override;
    Result<bool> erase(const std::string& run_id) override;
    Result<StateRecord> load(const std::string& run_id) const override;
    Result<std::uint64_t> compare_exchange(const std::string& run_id,
                                           std::uint64_t expected_version,
                                           const std::string& checkpoint) override;
    Result<StateRecord> claim(const std::string&,std::uint64_t,const std::string&) override;
    Result<std::vector<std::string>> list(const std::string&) const override;
    const std::string& directory() const noexcept;
private:
    std::string directory_;
};

class EffectCommitter {
public:
    virtual ~EffectCommitter() = default;
    virtual void commit() noexcept = 0;
    virtual bool committed() const noexcept = 0;
};

} // namespace dage
#endif
