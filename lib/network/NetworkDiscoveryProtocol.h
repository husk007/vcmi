/*
 * NetworkDiscoveryProtocol.h, part of VCMI engine
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// UDP discovery capability probe. Do not change the legacy probe/reply:
/// older clients and hosts must continue discovering one another.
namespace LanModDiscovery
{
inline constexpr std::string_view legacyProbe = "VCMI_DISCOVERY";
inline constexpr std::string_view previewProbe = "VCMI_DISCOVERY;lanModSync=preview";
inline constexpr std::string_view replyPrefix = "VCMI_SERVER:";
inline constexpr std::string_view previewSuffix = ";lanModSync=preview";

struct Reply
{
	std::uint16_t port = 0;
	bool supportsModPreview = false;
};

inline bool retainPreviewCapability(bool previouslyAdvertised, bool newlyAdvertised)
{
	return previouslyAdvertised || newlyAdvertised;
}

inline std::string replyForProbe(std::string_view probe, std::uint16_t port)
{
	if(!port || (probe != legacyProbe && probe != previewProbe))
		return {};

	std::string reply = std::string(replyPrefix) + std::to_string(port);
	if(probe == previewProbe)
		reply += previewSuffix;
	return reply;
}

inline std::optional<Reply> parseReply(std::string_view message)
{
	if(!message.starts_with(replyPrefix))
		return std::nullopt;

	message.remove_prefix(replyPrefix.size());
	const auto suffixStart = message.find(';');
	const auto portText = message.substr(0, suffixStart);
	std::uint32_t port = 0;
	const auto [last, error] = std::from_chars(portText.data(), portText.data() + portText.size(), port);
	if(error != std::errc{} || last != portText.data() + portText.size() || port == 0 || port > 65535)
		return std::nullopt;

	// Unknown future capability suffixes still permit ordinary discovery.
	const bool supportsModPreview = suffixStart != std::string_view::npos
		&& message.substr(suffixStart) == previewSuffix;
	return Reply{static_cast<std::uint16_t>(port), supportsModPreview};
}
}
