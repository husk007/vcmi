/*
 * LanModArchiveCache.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include "ModArchiveDigest.h"
#include "LanRelaunchToken.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace LanModArchiveCache
{
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
    if(!destination || !ModArchiveDigest::matchesFileSha256(stagedArchive, expectedSha256))
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

    if(!ModArchiveDigest::matchesFileSha256(temporary, expectedSha256))
    {
        std::filesystem::remove(temporary, error);
        return std::nullopt;
    }

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
    return findVerifiedArchive(userDataPath, rootModId, expectedSha256);
}
}
