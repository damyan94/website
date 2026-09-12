#pragma once

#include "PublicContent.h"
#include <json/json.h>
#include <optional>

namespace Reservations
{
class ReservationCatalog
{
public:
	struct Offer
	{
		Json::Value title;
		int			duration;
		long long	price;
		std::string currency;
		Json::Value rule;
	};

	struct Snapshot
	{
		std::string revision;
		Json::Value services;
	};

	ReservationCatalog(const Json::Value& settings, std::shared_ptr<PublicContent::Snapshot> content);
	Snapshot Read() const;
	std::optional<Offer> FindOffer(const Snapshot& snapshot, const std::string& service, int duration) const;

private:
	const Json::Value& m_Settings;
	std::shared_ptr<PublicContent::Snapshot> m_Content;
};
} // namespace Reservations
