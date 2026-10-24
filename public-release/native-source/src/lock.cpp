#include "qbtc/platform.hpp"
#include "qbtc/lock.hpp"
#include "qbtc/consensus.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <filesystem>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace qbtc::storage {
RunLock::RunLock(const std::string &database) {
    auto path = database + ".lock";
#ifdef _WIN32
    handle_ = CreateFileW(file_path(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE)
        throw Invalid("database is already in use or cannot be locked");
#else
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd_ < 0)
        throw Invalid("cannot open database lock");
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd_);
        throw Invalid("database is already in use");
    }
#endif
}
RunLock::~RunLock() {
#ifdef _WIN32
    CloseHandle(handle_);
#else
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
#endif
}
} // namespace qbtc::storage
