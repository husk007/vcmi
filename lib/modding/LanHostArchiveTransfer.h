/*
 * LanHostArchiveTransfer.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include "ModVerificationInfo.h"
#include "LanModArchiveCache.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

// One verified, pinned host archive per registered lobby guest. The previous
// stateless chunk reader rehashed the entire archive for every 64 KiB packet:
// a 1 GiB mod required hashing approximately 16 TiB before the guest could
// finish downloading. This reader hashes once and streams the pinned file.
// Guest-side SHA-256 verification remains mandatory before cache publication.
class LanHostArchiveTransfer
{
#ifdef _WIN32
    HANDLE archive = INVALID_HANDLE_VALUE;
#else
    std::FILE * archive = nullptr;
#endif
    TModID rootId;
    std::string digest;
    std::int64_t totalBytes = 0;
    std::int64_t nextOffset = 0;

    LanHostArchiveTransfer() = default;

    bool rewindArchive()
    {
#ifdef _WIN32
        LARGE_INTEGER start{};
        return SetFilePointerEx(archive, start, nullptr, FILE_BEGIN) != 0;
#else
        return std::fseek(archive, 0, SEEK_SET) == 0;
#endif
    }

    // A pinned handle prevents switching the source to a symlink between
    // chunks. Hash the bytes of that same handle once before serving them.
    bool verifyPinnedArchive(std::string_view expected, std::uint64_t size)
    {
        ModArchiveDigest::Sha256 hasher;
        std::array<char, 64 * 1024> buffer{};
        std::uint64_t counted = 0;
        while(true)
        {
            std::size_t got = 0;
#ifdef _WIN32
            DWORD count = 0;
            if(!ReadFile(archive, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
                return false;
            got = count;
#else
            got = std::fread(buffer.data(), 1, buffer.size(), archive);
            if(got < buffer.size() && std::ferror(archive))
                return false;
#endif
            if(got == 0)
                break;
            if(got > size - counted)
                return false;
            counted += got;
            hasher.update(buffer.data(), got);
        }
        return counted == size && ModArchiveDigest::sameSha256(expected, hasher.finish()) && rewindArchive();
    }

public:
    LanHostArchiveTransfer(const LanHostArchiveTransfer &) = delete;
    LanHostArchiveTransfer & operator=(const LanHostArchiveTransfer &) = delete;

    ~LanHostArchiveTransfer()
    {
#ifdef _WIN32
        if(archive != INVALID_HANDLE_VALUE)
            CloseHandle(archive);
#else
        if(archive)
            std::fclose(archive);
#endif
    }

    static std::unique_ptr<LanHostArchiveTransfer> open(const ModCompatibilityInfo & activeMods,
        const std::filesystem::path & userDataPath, const TModID & requestedRoot,
        const std::string & requestedSha256)
    {
        if(requestedRoot.empty() || !ModArchiveDigest::isSha256(requestedSha256))
            return {};

        // A parent root may be absent from the active list when its submods
        // are enabled. Bind the request to that active root's subtree only;
        // never authorize an unrelated root or hash every shared archive.
        ModCompatibilityInfo selected;
        for(const auto & [modId, info] : activeMods)
        {
            if(modId == requestedRoot
                || (modId.size() > requestedRoot.size()
                    && modId.compare(0, requestedRoot.size(), requestedRoot) == 0
                    && modId[requestedRoot.size()] == '.'))
                selected.emplace(modId, info);
        }
        if(selected.empty())
            return {};
        const auto offers = ModVerificationInfo::buildHostArchiveManifest(selected, userDataPath);
        const auto offered = offers.find(requestedRoot);
        if(offered == offers.end() || !ModArchiveDigest::sameSha256(offered->second.sha256, requestedSha256)
            || offered->second.bytes == 0
            || offered->second.bytes > LanModArchiveCache::maxArchiveBytes)
            return {};

        auto transfer = std::unique_ptr<LanHostArchiveTransfer>(new LanHostArchiveTransfer);
        const auto path = userDataPath / "lan-mod-shares" / (requestedRoot + ".zip");
#ifdef _WIN32
        transfer->archive = ModArchiveDigest::detail::openRegularArchiveNoFollow(path);
        if(transfer->archive == INVALID_HANDLE_VALUE)
            return {};
#else
        const int fd = ModArchiveDigest::detail::openRegularArchiveNoFollow(path);
        if(fd < 0)
            return {};
        transfer->archive = ::fdopen(fd, "rb");
        if(!transfer->archive)
        {
            ::close(fd);
            return {};
        }
#endif
        if(!transfer->verifyPinnedArchive(requestedSha256, offered->second.bytes))
            return {};
        transfer->rootId = requestedRoot;
        transfer->digest = requestedSha256;
        transfer->totalBytes = static_cast<std::int64_t>(offered->second.bytes);
        return transfer;
    }

    std::optional<ModArchiveChunk> next(const TModID & requestedRoot,
        const std::string & requestedSha256, std::int64_t offset)
    {
        if(requestedRoot != rootId || !ModArchiveDigest::sameSha256(digest, requestedSha256)
            || offset != nextOffset || offset >= totalBytes)
            return std::nullopt;
        const auto count = static_cast<std::size_t>((std::min)(std::int64_t(64 * 1024), totalBytes - offset));
        std::string bytes(count, '\0');
#ifdef _WIN32
        DWORD readCount = 0;
        if(!ReadFile(archive, bytes.data(), static_cast<DWORD>(count), &readCount, nullptr)
            || readCount != count)
            return std::nullopt;
#else
        if(std::fread(bytes.data(), 1, count, archive) != count)
            return std::nullopt;
#endif
        nextOffset += static_cast<std::int64_t>(count);
        return ModArchiveChunk{totalBytes, std::move(bytes)};
    }

    bool complete() const { return nextOffset == totalBytes; }
};
