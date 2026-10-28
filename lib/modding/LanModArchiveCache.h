/*
 * LanModArchiveCache.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include "ModArchiveDigest.h"
#include "LanRelaunchToken.h"
#include "AtomicModSettings.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace LanModArchiveCache
{
/// Maximum size of one received LAN mod archive before extraction.
constexpr std::uintmax_t maxArchiveBytes = 2ULL * 1024 * 1024 * 1024;

/// Do not hash, publish, or reuse oversized archives or non-regular files.
inline bool isAllowedArchiveSize(const std::filesystem::path & archive)
{
    std::error_code error;
    if(!std::filesystem::is_regular_file(std::filesystem::symlink_status(archive, error)) || error)
        return false;
    const std::uintmax_t size = std::filesystem::file_size(archive, error);
    return !error && size <= maxArchiveBytes;
}

/// Cache path for a canonical root mod ID and archive digest, below the persistent user data directory.
inline std::optional<std::filesystem::path> archivePath(const std::filesystem::path & userDataPath,
    std::string_view rootModId, std::string_view expectedSha256)
{
    if(userDataPath.empty() || rootModId.empty() || rootModId.find('.') != std::string_view::npos
        || !ModArchiveDigest::isSha256(expectedSha256)
        || std::any_of(rootModId.begin(), rootModId.end(), [](unsigned char ch){
            return ch < 0x20 || ch == 0x7f || (ch >= 'A' && ch <= 'Z')
                || ch == '/' || ch == '\\';
        }))
        return std::nullopt;

    ModArchiveDigest::Sha256 identity;
    identity.update(rootModId.data(), rootModId.size());

    std::string archiveName;
    archiveName.reserve(68);
    constexpr char hex[] = "0123456789abcdef";
    for(char digit : expectedSha256)
        archiveName.push_back(hex[ModArchiveDigest::hexDigit(digit)]);
    archiveName += ".zip";

    return userDataPath / "lan-mod-archives" / identity.finish() / archiveName;
}

/// Cache metadata must not follow symlinks outside the user's persistent archive store.
inline bool isTrustedCacheDirectory(const std::filesystem::path & path)
{
    std::error_code error;
    return std::filesystem::is_directory(std::filesystem::symlink_status(path, error)) && !error;
}

/// A cached archive is reusable only after hashing its bytes against the host's expected digest.
inline std::optional<std::filesystem::path> findVerifiedArchive(const std::filesystem::path & userDataPath,
    std::string_view rootModId, std::string_view expectedSha256)
{
    const auto candidate = archivePath(userDataPath, rootModId, expectedSha256);
    if(!candidate || !isTrustedCacheDirectory(userDataPath / "lan-mod-archives")
        || !isTrustedCacheDirectory(candidate->parent_path())
        || !isAllowedArchiveSize(*candidate)
        || !ModArchiveDigest::matchesFileSha256(*candidate, expectedSha256, maxArchiveBytes))
        return std::nullopt;
    return candidate;
}

/// Return only host-manifest entries whose persisted bytes have been rehashed successfully.
inline std::map<std::string, std::string> verifiedArchiveDigests(const std::filesystem::path & userDataPath,
    const std::map<std::string, std::string> & expectedSha256)
{
    std::map<std::string, std::string> verified;
    for(const auto & [modId, digest] : expectedSha256)
        if(findVerifiedArchive(userDataPath, modId, digest))
            verified.emplace(modId, digest);
    return verified;
}

namespace detail
{
#ifdef _WIN32
/// Use a pinned regular-file handle and exclusive destination when staging on Windows.
inline bool copyStagedArchiveBounded(const std::filesystem::path & source,
    const std::filesystem::path & destination, std::uintmax_t maximumBytes)
{
    const HANDLE sourceHandle = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if(sourceHandle == INVALID_HANDLE_VALUE)
        return false;

    BY_HANDLE_FILE_INFORMATION info{};
    const bool regularSource = GetFileType(sourceHandle) == FILE_TYPE_DISK
        && GetFileInformationByHandle(sourceHandle, &info)
        && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    if(!regularSource)
    {
        CloseHandle(sourceHandle);
        return false;
    }

    const HANDLE destinationHandle = CreateFileW(destination.c_str(), GENERIC_WRITE,
        0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(destinationHandle == INVALID_HANDLE_VALUE)
    {
        CloseHandle(sourceHandle);
        return false;
    }

    std::array<char, 64 * 1024> buffer{};
    std::uintmax_t copiedBytes = 0;
    bool success = true;
    while(success)
    {
        DWORD count = 0;
        if(!ReadFile(sourceHandle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
        {
            success = false;
            break;
        }
        if(count == 0)
            break;
        if(static_cast<std::uintmax_t>(count) > maximumBytes - copiedBytes)
        {
            success = false;
            break;
        }
        copiedBytes += static_cast<std::uintmax_t>(count);
        DWORD written = 0;
        while(written < count)
        {
            DWORD next = 0;
            if(!WriteFile(destinationHandle, buffer.data() + written, count - written, &next, nullptr)
                || next == 0)
            {
                success = false;
                break;
            }
            written += next;
        }
    }
    if(success && !FlushFileBuffers(destinationHandle))
        success = false;
    const bool destinationClosed = CloseHandle(destinationHandle) != 0;
    const bool sourceClosed = CloseHandle(sourceHandle) != 0;
    return success && destinationClosed && sourceClosed;
}
#else
/// Copy a staged archive without following a replaced source symlink or writing beyond the limit.
inline bool copyStagedArchiveBounded(const std::filesystem::path & source,
    const std::filesystem::path & destination, std::uintmax_t maximumBytes)
{
    const int sourceFd = ModArchiveDigest::detail::openRegularArchiveNoFollow(source);
    if(sourceFd < 0)
        return false;

    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int destinationFd = ::open(destination.c_str(), flags, 0600);
    if(destinationFd < 0)
    {
        ::close(sourceFd);
        return false;
    }

    std::array<char, 64 * 1024> buffer{};
    std::uintmax_t copiedBytes = 0;
    bool success = true;
    while(success)
    {
        const auto count = ::read(sourceFd, buffer.data(), buffer.size());
        if(count < 0 && errno == EINTR)
            continue;
        if(count < 0)
        {
            success = false;
            break;
        }
        if(count == 0)
            break;
        if(static_cast<std::uintmax_t>(count) > maximumBytes - copiedBytes)
        {
            success = false;
            break;
        }
        copiedBytes += static_cast<std::uintmax_t>(count);
        std::size_t written = 0;
        while(written < static_cast<std::size_t>(count))
        {
            const auto result = ::write(destinationFd, buffer.data() + written,
                static_cast<std::size_t>(count) - written);
            if(result < 0 && errno == EINTR)
                continue;
            if(result <= 0)
            {
                success = false;
                break;
            }
            written += static_cast<std::size_t>(result);
        }
    }
    if(::close(sourceFd) != 0)
        success = false;
    if(::close(destinationFd) != 0)
        success = false;
    return success;
}

/// Sync staged archive bytes and cache directory entries before reporting durable publication.
inline bool syncCacheEntry(const std::filesystem::path & target, bool directory)
{
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#else
    std::error_code error;
    if(std::filesystem::is_symlink(std::filesystem::symlink_status(target, error)) || error)
        return false;
#endif
#ifdef O_DIRECTORY
    if(directory)
        flags |= O_DIRECTORY;
#endif
    const int fd = ::open(target.c_str(), flags);
    if(fd < 0)
        return false;
    struct stat metadata{};
    const bool expectedType = ::fstat(fd, &metadata) == 0
        && (directory ? S_ISDIR(metadata.st_mode) : S_ISREG(metadata.st_mode));
    const bool synced = expectedType && AtomicModSettings::syncDescriptorWithRetry(fd, ::fsync);
    const bool closed = ::close(fd) == 0;
    return synced && closed;
}
#endif

/// POSIX rename may overwrite a file created after an earlier existence check.
/// A hard link creates the destination atomically only if it is still absent.
/// The staged file resides alongside the destination, on the same filesystem.
inline bool publishWithoutReplacing(const std::filesystem::path & temporary, const std::filesystem::path & destination)
{
    std::error_code error;
    std::filesystem::create_hard_link(temporary, destination, error);
    return !error;
}
}

/// Publish a complete staged archive atomically; never install it into the player's personal mods.
inline std::optional<std::filesystem::path> publishVerifiedArchive(const std::filesystem::path & userDataPath,
    std::string_view rootModId, std::string_view expectedSha256, const std::filesystem::path & stagedArchive)
{
    const auto destination = archivePath(userDataPath, rootModId, expectedSha256);
    if(!destination || !isAllowedArchiveSize(stagedArchive)
        || !ModArchiveDigest::matchesFileSha256(stagedArchive, expectedSha256, maxArchiveBytes))
        return std::nullopt;

    if(const auto existing = findVerifiedArchive(userDataPath, rootModId, expectedSha256))
        return existing;

    std::error_code error;
    const auto cacheRoot = userDataPath / "lan-mod-archives";
    // Reject linked cache roots before creating the mod-specific directory.
    std::filesystem::create_directories(cacheRoot, error);
    if(error)
        return std::nullopt;

    // A link in the cache path could redirect received content outside the user data directory.
    if(!isTrustedCacheDirectory(cacheRoot))
        return std::nullopt;
    error.clear();
    std::filesystem::create_directories(destination->parent_path(), error);
    if(error || !isTrustedCacheDirectory(destination->parent_path()))
        return std::nullopt;

#ifndef _WIN32
    // Flush directory names created for this cache entry before publishing its contents.
    if(!detail::syncCacheEntry(userDataPath, true) || !detail::syncCacheEntry(cacheRoot, true))
        return std::nullopt;
#endif

    // Never silently replace an existing cache entry that failed integrity verification.
    if(std::filesystem::exists(std::filesystem::symlink_status(*destination, error)))
        return std::nullopt;
    if(error && error != std::errc::no_such_file_or_directory)
        return std::nullopt;

    std::filesystem::path temporary = *destination;
    temporary += "." + LanRelaunchToken::generate() + ".part";
    error.clear();
    const bool copied = detail::copyStagedArchiveBounded(stagedArchive, temporary, maxArchiveBytes);
    if(!copied)
    {
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return std::nullopt;
    }

    if(!isAllowedArchiveSize(temporary)
        || !ModArchiveDigest::matchesFileSha256(temporary, expectedSha256, maxArchiveBytes))
    {
        std::filesystem::remove(temporary, error);
        return std::nullopt;
    }

#ifndef _WIN32
    if(!detail::syncCacheEntry(temporary, false))
    {
        std::filesystem::remove(temporary, error);
        return std::nullopt;
    }
#endif

    // An earlier lookup cannot prevent a concurrent process from creating this destination.
    // Never replace that entry, even if it is corrupt; a verified competing writer may be reused.
    if(!detail::publishWithoutReplacing(temporary, *destination))
    {
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return findVerifiedArchive(userDataPath, rootModId, expectedSha256);
    }
    std::error_code cleanupError;
    std::filesystem::remove(temporary, cleanupError);
#ifndef _WIN32
    if(!detail::syncCacheEntry(destination->parent_path(), true))
        return std::nullopt;
#endif
    return findVerifiedArchive(userDataPath, rootModId, expectedSha256);
}
}
