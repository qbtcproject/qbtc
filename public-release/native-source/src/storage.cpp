#include "qbtc/platform.hpp"
#include "qbtc/storage.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <openssl/rand.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <sddl.h>
// clang-format on
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace qbtc::storage {
namespace {
void need(bool ok, const char *msg) {
    if (!ok)
        throw Invalid(msg);
}
void directory_sync(const std::string &path) {
#ifndef _WIN32
    auto parent = file_path(path).parent_path();
    if (parent.empty())
        parent = ".";
    int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    need(fd >= 0, "directory open for sync");
    int result = ::fsync(fd);
    int closed = ::close(fd);
    need(result == 0 && closed == 0, "directory sync failure");
#else
    (void)path;
#endif
}
} // namespace
Bytes read_bounded(const std::string &path, std::size_t maximum) {
    std::ifstream f(file_path(path), std::ios::binary | std::ios::ate);
    need(bool(f), "file open");
    auto n = f.tellg();
    need(n >= 0 && static_cast<std::uint64_t>(n) <= maximum, "file size limit");
    Bytes b(static_cast<std::size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(b.size()));
    need(bool(f), "file read");
    return b;
}
void write_new(const std::string &path, std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    need(ConvertStringSecurityDescriptorToSecurityDescriptorW(
             L"D:P(A;;GA;;;OW)(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr) != 0,
         "wallet ACL creation");
    SECURITY_ATTRIBUTES attributes{static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)), descriptor,
                                   FALSE};
    auto p = file_path(path);
    HANDLE file = CreateFileW(p.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(descriptor);
    need(file != INVALID_HANDLE_VALUE, "cannot create file; existing files are never overwritten");
    DWORD written = 0;
    bool ok =
        bytes.size() <= MAXDWORD &&
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 &&
        written == bytes.size() && FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(p.c_str());
        throw Invalid("private file write failure");
    }
#else
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    need(fd >= 0, "cannot create file; existing files are never overwritten");
    std::size_t written = 0;
    bool ok = true;
    while (written < bytes.size()) {
        auto n = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (n <= 0) {
            ok = false;
            break;
        }
        written += static_cast<std::size_t>(n);
    }
    if (ok)
        ok = ::fsync(fd) == 0;
    int closed = ::close(fd);
    if (!ok || closed != 0) {
        ::unlink(path.c_str());
        throw Invalid("private file write failure");
    }
#endif
    directory_sync(path);
}
void append_durable(const std::string &path, std::span<const std::uint8_t> bytes,
                    std::uint64_t observed_size, std::uint64_t committed_size) {
    need(committed_size <= observed_size, "journal cursor range");
#ifdef _WIN32
    auto file =
        CreateFileW(file_path(path).c_str(), GENERIC_WRITE | GENERIC_READ,
                    FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    need(file != INVALID_HANDLE_VALUE, "journal append open");
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != 0 && size.QuadPart >= 0 &&
              static_cast<std::uint64_t>(size.QuadPart) == observed_size;
    // Match POSIX: reject an externally changed file before seeking or truncating.
    if (!ok) {
        CloseHandle(file);
        throw Invalid("journal changed since load");
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(committed_size);
    ok =
        ok && SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != 0 && SetEndOfFile(file) != 0;
    std::size_t done = 0;
    while (ok && done < bytes.size()) {
        DWORD written = 0;
        auto n = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, MAXDWORD));
        ok = WriteFile(file, bytes.data() + done, n, &written, nullptr) != 0 && written > 0;
        done += written;
    }
    if (ok)
        ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    need(ok, "journal append/flush failed; restart to recover");
#else
    int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    need(fd >= 0, "journal append open");
    struct stat info{};
    bool ok = ::fstat(fd, &info) == 0 && info.st_size >= 0 &&
              static_cast<std::uint64_t>(info.st_size) == observed_size;
    // A mismatching size means another writer; never truncate in that case.
    if (!ok) {
        ::close(fd);
        throw Invalid("journal changed since load");
    }
    ok = ::ftruncate(fd, static_cast<off_t>(committed_size)) == 0;
    std::size_t done = 0;
    while (ok && done < bytes.size()) {
        auto n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n <= 0)
            ok = false;
        else
            done += static_cast<std::size_t>(n);
    }
    if (ok)
        ok = ::fsync(fd) == 0;
    int closed = ::close(fd);
    need(ok && closed == 0, "journal append/flush failed; restart to recover");
#endif
}
void replace_atomic(const std::string &path, std::span<const std::uint8_t> bytes) {
    Bytes random(16);
    need(RAND_bytes(random.data(), 16) == 1, "snapshot temporary-name randomness");
    auto temporary = path + ".tmp." + hex(random);
    try {
        write_new(temporary, bytes);
#ifdef _WIN32
        need(MoveFileExW(file_path(temporary).c_str(),
                         file_path(path).c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
             "atomic snapshot replacement");
#else
        need(::rename(temporary.c_str(), path.c_str()) == 0, "atomic snapshot replacement");
#endif
        directory_sync(path);
    } catch (...) {
        std::error_code error;
        std::filesystem::remove(file_path(temporary), error);
        throw;
    }
}
} // namespace qbtc::storage
