#include "ReservationNotifications.h"
#include "ReservationRepository.h"
#include <algorithm>

namespace Reservations
{
ReservationNotifications::ReservationNotifications(const Json::Value& settings,
													 const Accounts::EmailSettings& email,
													 const Accounts::UiSettings& ui)
	: m_Settings(settings),
	  m_EmailEnabled(email.enabled),
	  m_Origin(email.origin),
	  m_Ui(ui)
{
}

std::string ReservationNotifications::Notify(Accounts::Database& db, const Json::Value& appointment, const std::string& action) const
{
	if (!m_EmailEnabled || appointment["contactEmail"].asString().empty())
		return "";
	ReservationRepository repository(db);
	auto recipient = appointment["contactEmail"].asString();
	if (!appointment["userId"].isNull())
	{
		// Keep the booking's contact snapshot for staff/history, but send new notices
		// to the account's current verified address after an email change.
		const auto account = repository.CurrentNotificationEmail(appointment["userId"].asString());
		if (!account)
			return "";
		recipient = *account;
	}
	const auto locale = appointment["locale"].asString();
	const bool bg	  = locale == "bg";
	const auto title  = appointment["serviceTitle"].get(locale, appointment["serviceTitle"].get("en", "")).asString();
	const auto local  = repository.LocalAppointmentStart(appointment["id"].asString(), m_Settings["timezone"].asString());
	const std::string status  = bg ? (action == "reminder"		? "Напомняне"
									  : action == "confirmed"	? "Потвърдена"
									  : action == "rescheduled" ? "Променена"
									  : action == "cancelled"	? "Отказана"
									  : action == "completed"	? "Завършена"
																: "Неявяване")
								   : action;
	const auto		  subject = (bg ? "Резервация: " : "Reservation: ") + status;
	const auto		  management =
		   appointment["userId"].isNull()
				   ? (bg ? "За промяна се свържете със служител. " : "Contact staff to change this reservation. ") +
					 m_Ui.supportText.get(locale, "").asString()
				   : (bg ? "Управление на резервации: " : "Manage reservations: ") + m_Origin +
					 "/profile?lang=" + locale + "#section-reservations";
	const auto body = m_Ui.siteName + "\n\n" + appointment["contactName"].asString() + "\n" + title + "\n" +
					  local + " (" + m_Settings["timezone"].asString() + ")\n" + status + "\n\n" +
					  (bg ? "Номер: " : "Reference: ") + appointment["id"].asString() + "\n" + management;
	return Accounts::QueueEmail(
		db, recipient, subject, body, appointment["userId"].isNull() ? "" : appointment["userId"].asString());
}

bool ReservationNotifications::RemindersEnabled() const
{
	return m_EmailEnabled && m_Settings.get("reminders_enabled", false).asBool();
}

void ReservationNotifications::QueueReminders(Accounts::Database& db) const
{
	ReservationRepository repository(db);
	const auto pending = repository.CountPendingMailJobs();
	const auto capacity = std::min(20, 10000 - pending);
	if (capacity <= 0)
		return;
	// Bookings made/changed after the reminder time already have a fresh confirmation.
	// Do not follow it with an immediate reminder, or send reminders after the visit starts.
	const auto reminders = repository.DueReminders(m_Settings.get("reminder_hours", 24).asString(), capacity);
	for (const auto& reminder : reminders)
	{
		const auto& id = reminder.id;
		auto appointment = repository.Read(id, m_Settings["cancel_hours"].asString());
		if (!appointment)
			throw Accounts::RequestError(404, "Reservation not found");
		const auto job = Notify(db, *appointment, "reminder");
		if (job.empty())
			continue;
		repository.MarkReminderJob(job, id);
		repository.RecordReminder(id, reminder.version, job);
	}
}

bool ReservationNotifications::ReminderEligible(Accounts::Database& db, const std::string& job) const
{
	ReservationRepository repository(db);
	return repository.ReminderEligible(job);
}

} // namespace Reservations
