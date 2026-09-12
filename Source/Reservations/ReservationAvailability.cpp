#include "ReservationAvailability.h"
#include "Validation.h"
#include <algorithm>

namespace Reservations
{
namespace
{
using Validation::Minute;

bool Contains(const Json::Value& array, const std::string& value)
{
	return std::any_of(
		array.begin(), array.end(), [&](const auto& item) { return item.isString() && item.asString() == value; });
}

} // namespace

ReservationAvailability::ReservationAvailability(const Json::Value& settings)
	: m_Settings(settings)
{
}

bool ReservationAvailability::Open(const Json::Value& windows, int weekday, int minute) const
{
	for (const auto& window : windows)
		if (std::any_of(
				window["days"].begin(), window["days"].end(), [&](const auto& d) { return d.asInt() == weekday; }) &&
			minute >= Minute(window["start"]) && minute < Minute(window["end"]))
			return true;
	return false;
}

bool ReservationAvailability::Closed(const std::string& date, const std::string& resource) const
{
	for (const auto& closure : m_Settings["closures"])
		if (closure["date"].asString() == date &&
			(closure["resources"].empty() || Contains(closure["resources"], resource)))
			return true;
	return false;
}

std::vector<ReservationAvailability::Slot> ReservationAvailability::Calculate(
	const std::string& date,
	const std::string& choice,
	int duration,
	const Json::Value& rule,
	long long now,
	const std::vector<Tick>& ticks,
	const std::vector<OccupiedRange>& occupied) const
{
	std::vector<Slot> slots;
	const auto earliest = now + m_Settings["lead_minutes"].asInt() * 60;
	const int before = rule["buffer_before"].asInt(), after = rule["buffer_after"].asInt();
	for (int i = before; i + duration + after <= static_cast<int>(ticks.size()); ++i)
	{
		if (ticks[i].instant < earliest || ticks[i].minute % m_Settings["slot_minutes"].asInt())
			continue;
		const int begin = i - before, end = i + duration + after;
		auto	  free = [&](const std::string& id)
		{
			if (Closed(date, id))
				return false;
			const Json::Value* resource = nullptr;
			for (const auto& item : m_Settings["resources"])
				if (item["id"].asString() == id)
					resource = &item;
			if (!resource)
				return false;
			for (int k = begin; k < end; ++k)
				if (!Open(m_Settings["business_hours"], ticks[k].weekday, ticks[k].minute) ||
					!Open((*resource)["hours"], ticks[k].weekday, ticks[k].minute))
					return false;
			const auto starts = ticks[begin].instant, ends = ticks[end - 1].instant + 60;
			for (const auto& range : occupied)
				if (range.resource == id && starts < range.ends && ends > range.starts)
					return false;
			return true;
		};
		std::string therapist, room;
		for (const auto& id : rule["therapists"])
			if ((choice.empty() || choice == id.asString()) && free(id.asString()))
			{
				therapist = id.asString();
				break;
			}
		if (therapist.empty())
			continue;
		for (const auto& id : rule["rooms"])
			if (free(id.asString()))
			{
				room = id.asString();
				break;
			}
		if (room.empty())
			continue;
		slots.push_back({ticks[i].stamp, therapist, room});
	}
	return slots;
}
} // namespace Reservations
