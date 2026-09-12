#pragma once

#include "Accounts/Types.h"

namespace Reservations
{
// Queues reservation notices inside the caller's transaction; never performs delivery I/O.
class ReservationNotifications
{
public:
	ReservationNotifications(const Json::Value& settings,
							 const Accounts::EmailSettings& email,
							 const Accounts::UiSettings& ui);
	std::string Notify(Accounts::Database& db, const Json::Value& appointment, const std::string& action) const;

	bool RemindersEnabled() const;
	// The caller holds the reminder-scan transaction and advisory lock.
	void QueueReminders(Accounts::Database& db) const;
	bool ReminderEligible(Accounts::Database& db, const std::string& job) const;

private:
	const Json::Value& m_Settings;
	bool m_EmailEnabled;
	std::string m_Origin;
	Accounts::UiSettings m_Ui;
};
} // namespace Reservations
