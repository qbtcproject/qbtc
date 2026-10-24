#pragma once
#include <string>
namespace qbtc::storage {
class RunLock {
#ifdef _WIN32
    void *handle_;
#else
    int fd_;
#endif
  public:
    explicit RunLock(const std::string &database);
    ~RunLock();
    RunLock(const RunLock &) = delete;
    RunLock &operator=(const RunLock &) = delete;
};
} // namespace qbtc::storage
