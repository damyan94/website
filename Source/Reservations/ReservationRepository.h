#pragma once

#include "Accounts/Database.h"
#include <json/json.h>
#include <optional>

namespace Reservations
{
struct BookingRequest
{
	std::string id;
	std::string requestHash;
};

struct BookingCustomer
{
	bool emailVerified;
	std::string name;
	std::string email;
	std::string phone;
	std::string locale;
};

struct AppointmentCreation
{
	std::string actorId;
	std::string requestKey;
	std::string requestHash;
	std::string userId;
	std::string contactName;
	std::string contactEmail;
	std::string contactPhone;
	std::string locale;
	std::string serviceId;
	Json::Value serviceTitle;
	int durationMinutes;
	long long priceMinor;
	std::string currency;
	std::string startsAt;
	std::string therapistId;
	std::string roomId;
	int bufferBefore;
	int bufferAfter;
};

struct AppointmentTiming
{
	bool hasStarted;
	bool hasEnded;
};

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

	std::optional<BookingRequest> FindRequest(const std::string& actor, const std::string& key);
	BookingCustomer ReadBookingCustomer(const std::string& user);
	int CountUpcomingForCustomer(const std::string& user);
	std::string CreateAppointment(const AppointmentCreation& appointment);

	void InsertOccupancy(const std::string& id);
	void DeleteOccupancy(const std::string& id);
	void UpdateSchedule(const std::string& id,
						const std::string& startsAt,
						const std::string& therapist,
						const std::string& room,
						int bufferBefore,
						int bufferAfter);
	AppointmentTiming ReadTiming(const std::string& id);
	void UpdateState(const std::string& id, const std::string& state);
	void SkipQueuedReminders(const std::string& id);
	void RecordEvent(const std::string& id, const std::string& actor, const std::string& action);

private:
	int Count(const std::string& condition, const std::vector<std::string>& args);
	std::vector<std::string> ListIds(const std::string& condition,
								   std::vector<std::string> args,
								   bool descending,
								   int offset);
	Accounts::Database& m_Database;
};
} // namespace Reservations
