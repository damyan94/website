#include "ReservationCatalog.h"
#include "Accounts/Crypto.h"
#include <sstream>
#include <utility>

namespace Reservations
{
namespace
{
Json::Value Parse(const std::string& bytes)
{
	Json::CharReaderBuilder builder;
	Json::Value				value;
	std::string				errors;
	std::istringstream		input(bytes);
	if (!Json::parseFromStream(builder, input, &value, &errors))
		throw std::runtime_error("Invalid reservations JSON");
	return value;
}

} // namespace

ReservationCatalog::ReservationCatalog(const Json::Value& settings, std::shared_ptr<PublicContent::Snapshot> content)
	: m_Settings(settings),
	  m_Content(std::move(content))
{
}

ReservationCatalog::Snapshot ReservationCatalog::Read() const
{
	const auto	bytes	 = m_Content->Get();
	const auto	document = Parse(*bytes);
	Snapshot result;
	result.revision = Accounts::TokenHash(*bytes);
	result.services = Json::arrayValue;
	for (const auto& rule : m_Settings["services"])
		for (const auto& service : document["services"])
		{
			if (service["id"] != rule["id"] || !service.get("available", true).asBool())
				continue;
			Json::Value item;
			item["id"]		   = service["id"];
			item["title"]	   = service["title"];
			item["therapists"] = rule["therapists"];
			item["variants"]   = Json::arrayValue;
			for (const auto& variant : service["variants"])
			{
				if (!variant["priceMinor"].isInt64() || variant["priceMinor"].asInt64() < 0 ||
					variant["priceMinor"].asInt64() > 1000000000 || !variant["durationMinutes"].isInt() ||
					variant["durationMinutes"].asInt() < 5 || variant["durationMinutes"].asInt() > 240 ||
					!variant["currency"].isString() || variant["currency"].asString().size() != 3)
					continue;
				item["variants"].append(variant);
			}
			if (!item["variants"].empty())
				result.services.append(item);
		}
	return result;
}

std::optional<ReservationCatalog::Offer> ReservationCatalog::FindOffer(
	const Snapshot& snapshot, const std::string& service, int duration) const
{
	for (const auto& item : snapshot.services)
		if (item["id"].asString() == service)
			for (const auto& variant : item["variants"])
				if (variant["durationMinutes"].asInt() == duration)
					for (const auto& rule : m_Settings["services"])
						if (rule["id"].asString() == service)
							return Offer{item["title"],
									duration,
									variant["priceMinor"].asInt64(),
									variant["currency"].asString(),
									rule};
	return std::nullopt;
}

} // namespace Reservations
