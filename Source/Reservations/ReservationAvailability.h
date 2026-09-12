#pragma once

#include <json/json.h>
#include <string>
#include <vector>

namespace Reservations
{
// Calculates slots from PostgreSQL's UTC minute timeline and occupied ranges.
class ReservationAvailability
{
public:
	struct Tick
	{
		long long instant;
		int weekday;
		int minute;
		std::string stamp;
	};

	struct OccupiedRange
	{
		std::string resource;
		long long starts;
		long long ends;
	};

	struct Slot
	{
		std::string startsAt;
		std::string therapistId;
		std::string roomId;
	};

	explicit ReservationAvailability(const Json::Value& settings);
	std::vector<Slot> Calculate(const std::string& date,
							const std::string& choice,
							int duration,
							const Json::Value& rule,
							long long now,
							const std::vector<Tick>& ticks,
							const std::vector<OccupiedRange>& occupied) const;

private:
	bool Open(const Json::Value& windows, int weekday, int minute) const;
	bool Closed(const std::string& date, const std::string& resource) const;
	const Json::Value& m_Settings;
};
} // namespace Reservations
