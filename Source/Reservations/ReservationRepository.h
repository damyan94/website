#pragma once

#include "Accounts/Database.h"
#include <json/json.h>
#include <optional>

namespace Reservations
{
// Borrows the current worker's connection and participates in its caller's transaction.
class ReservationRepository
{
public:
	explicit ReservationRepository(Accounts::Database& database);
	std::optional<Json::Value> Read(const std::string& id, const std::string& cancelHours);
	int CountForUser(const std::string& user, bool history);
	std::vector<std::string> ListIdsForUser(const std::string& user, bool history, int offset);
	int CountCalendar(const std::string& date, const std::string& timezone, int days);
	std::vector<std::string> ListCalendarIds(const std::string& date,
										   const std::string& timezone,
										   int days,
										   int offset);

private:
	int Count(const std::string& condition, const std::vector<std::string>& args);
	std::vector<std::string> ListIds(const std::string& condition,
								   std::vector<std::string> args,
								   bool descending,
								   int offset);
	Accounts::Database& m_Database;
};
} // namespace Reservations
