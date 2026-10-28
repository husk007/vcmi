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
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#ifndef _WIN32
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
        || !ModArchiveDigest::matchesFileSha256(*candidate, expectedSha256))
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
#ifndef _WIN32
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
        || !ModArchiveDigest::matchesFileSha256(stagedArchive, expectedSha256))
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
    if(!std::filesystem::copy_file(stagedArchive, temporary, std::filesystem::copy_options::none, error))
    {
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return std::nullopt;
    }

    if(!isAllowedArchiveSize(temporary)
        || !ModArchiveDigest::matchesFileSha256(temporary, expectedSha256))
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
