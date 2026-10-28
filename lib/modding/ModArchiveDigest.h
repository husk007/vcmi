/*
 * ModArchiveDigest.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <memory>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <cstddef>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/// Comparison of canonical SHA-256 archive digests supplied by the LAN manifest and verified cache.
namespace ModArchiveDigest
{
inline int hexDigit(char digit)
{
	if(digit >= '0' && digit <= '9')
		return digit - '0';
	if(digit >= 'a' && digit <= 'f')
		return digit - 'a' + 10;
	if(digit >= 'A' && digit <= 'F')
		return digit - 'A' + 10;
	return -1;
}

inline bool isSha256(std::string_view digest)
{
	if(digest.size() != 64)
		return false;

	for(char digit : digest)
		if(hexDigit(digit) < 0)
			return false;

	return true;
}

inline bool sameSha256(std::string_view expected, std::string_view cached)
{
	if(!isSha256(expected) || !isSha256(cached))
		return false;

	for(std::size_t index = 0; index < 64; ++index)
		if(hexDigit(expected[index]) != hexDigit(cached[index]))
			return false;

	return true;
}

/// Incremental SHA-256 for validating complete archive bytes without loading them into memory.
class Sha256
{
    std::array<std::uint32_t, 8> state = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    std::array<std::uint8_t, 64> pending{};
    std::size_t pendingSize = 0;
    std::uint64_t totalBytes = 0;

    void transform(const std::uint8_t * block)
    {
        static constexpr std::array<std::uint32_t, 64> round = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
        };

        std::array<std::uint32_t, 64> words{};
        for(std::size_t i = 0; i < 16; ++i)
            words[i] = (std::uint32_t(block[4 * i]) << 24)
                | (std::uint32_t(block[4 * i + 1]) << 16)
                | (std::uint32_t(block[4 * i + 2]) << 8)
                | std::uint32_t(block[4 * i + 3]);
        for(std::size_t i = 16; i < 64; ++i)
        {
            const auto s0 = std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const auto s1 = std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        auto a = state[0], b = state[1], c = state[2], d = state[3];
        auto e = state[4], f = state[5], g = state[6], h = state[7];
        for(std::size_t i = 0; i < 64; ++i)
        {
            const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto temp1 = h + s1 + choose + round[i] + words[i];
            const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

public:
    void update(const void * bytes, std::size_t size)
    {
        if(size > std::numeric_limits<std::uint64_t>::max() / 8 - totalBytes)
            throw std::length_error("LAN archive exceeds SHA-256 length limit");
        totalBytes += size;
        const auto * input = static_cast<const std::uint8_t *>(bytes);
        while(size)
        {
            const std::size_t take = std::min(size, pending.size() - pendingSize);
            std::memcpy(pending.data() + pendingSize, input, take);
            pendingSize += take;
            input += take;
            size -= take;
            if(pendingSize == pending.size())
            {
                transform(pending.data());
                pendingSize = 0;
            }
        }
    }

    std::string finish()
    {
        const std::uint64_t bitLength = totalBytes * 8;
        const std::uint8_t marker = 0x80;
        update(&marker, 1);
        const std::uint8_t zero = 0;
        while(pendingSize != 56)
            update(&zero, 1);
        std::array<std::uint8_t, 8> length{};
        for(std::size_t i = 0; i < length.size(); ++i)
            length[i] = static_cast<std::uint8_t>(bitLength >> (56 - 8 * i));
        update(length.data(), length.size());

        static constexpr char hex[] = "0123456789abcdef";
        std::string result(64, '0');
        for(std::size_t i = 0; i < state.size(); ++i)
            for(std::size_t j = 0; j < 4; ++j)
            {
                const auto value = static_cast<std::uint8_t>(state[i] >> (24 - 8 * j));
                result[8 * i + 2 * j] = hex[value >> 4];
                result[8 * i + 2 * j + 1] = hex[value & 0x0f];
            }
        return result;
    }
};

namespace detail
{
#ifdef _WIN32
/// Open a pinned archive handle and refuse Windows reparse points after opening.
inline HANDLE openRegularArchiveNoFollow(const std::filesystem::path & archive)
{
    const HANDLE handle = CreateFileW(archive.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if(handle == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;

    BY_HANDLE_FILE_INFORMATION metadata{};
    if(GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &metadata)
        || (metadata.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
    {
        CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

struct FileCloser
{
    void operator()(HANDLE handle) const
    {
        if(handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};
#else
/// Anchor hashing to a regular file descriptor, never a symlink swapped into the pathname.
inline int openRegularArchiveNoFollow(const std::filesystem::path & archive)
{
#ifndef O_NOFOLLOW
    (void)archive;
    return -1;
#else
    int flags = O_RDONLY | O_NONBLOCK | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(archive.c_str(), flags);
    if(descriptor < 0)
        return -1;

    struct stat metadata{};
    if(::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode))
    {
        ::close(descriptor);
        return -1;
    }
    return descriptor;
#endif
}

struct FileCloser
{
    void operator()(std::FILE * file) const
    {
        if(file)
            std::fclose(file);
    }
};
#endif
}

/// Returns no digest on unreadable or non-regular files. Symlinks are not trusted cache archives.
inline std::optional<std::string> sha256File(const std::filesystem::path & archive,
    std::uintmax_t maximumBytes = std::numeric_limits<std::uintmax_t>::max())
{
    std::error_code error;
    if(!std::filesystem::is_regular_file(std::filesystem::symlink_status(archive, error)) || error)
        return std::nullopt;

#ifdef _WIN32
    const HANDLE handle = detail::openRegularArchiveNoFollow(archive);
    if(handle == INVALID_HANDLE_VALUE)
        return std::nullopt;
    std::unique_ptr<void, detail::FileCloser> input(handle);
#else
    const int descriptor = detail::openRegularArchiveNoFollow(archive);
    if(descriptor < 0)
        return std::nullopt;
    std::FILE * raw = ::fdopen(descriptor, "rb");
    if(!raw)
    {
        ::close(descriptor);
        return std::nullopt;
    }
    std::unique_ptr<std::FILE, detail::FileCloser> input(raw);
#endif

    Sha256 hasher;
    std::array<char, 64 * 1024> buffer{};
    std::uintmax_t bytesRead = 0;
#ifdef _WIN32
    while(true)
    {
        DWORD count = 0;
        if(!ReadFile(input.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
            return std::nullopt;
        if(count == 0)
            break;
        if(static_cast<std::uintmax_t>(count) > maximumBytes - bytesRead)
            return std::nullopt;
        bytesRead += static_cast<std::uintmax_t>(count);
        hasher.update(buffer.data(), static_cast<std::size_t>(count));
    }
#else
    while(true)
    {
        const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), input.get());
        if(count > 0)
        {
            if(static_cast<std::uintmax_t>(count) > maximumBytes - bytesRead)
                return std::nullopt;
            bytesRead += static_cast<std::uintmax_t>(count);
            hasher.update(buffer.data(), count);
        }
        if(count < buffer.size())
        {
            if(std::ferror(input.get()))
                return std::nullopt;
            break;
        }
    }
#endif

    return hasher.finish();
}

inline bool matchesFileSha256(const std::filesystem::path & archive, std::string_view expected,
    std::uintmax_t maximumBytes = std::numeric_limits<std::uintmax_t>::max())
{
    if(!isSha256(expected))
        return false;
    const auto calculated = sha256File(archive, maximumBytes);
    return calculated && sameSha256(expected, *calculated);
}
}
