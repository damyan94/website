#pragma once

#include "Accounts/Types.h"
#include "ReservationCatalog.h"
#include "ReservationAvailability.h"
#include "ReservationNotifications.h"
#include <utility>

namespace Reservations
{
// HTTP workflows use the caller's authenticated transaction. Catalog interpretation,
// slot calculation, appointment persistence and notices delegate to concrete feature components.
class ReservationService
{
public:
	ReservationService(const Json::Value& settings,
					   std::shared_ptr<PublicContent::Snapshot> content,
					   Accounts::StoreSettings accounts);
	Json::Value GetOptions(Accounts::Database& db);
	Json::Value GetSlots(Accounts::Database& db, const Accounts::Identity& actor, const Json::Value& body);
	Json::Value ListAppointments(Accounts::Database&	   db,
									 const Accounts::Identity& actor,
									 const Json::Value&		   body,
									 bool					   calendar);
	// Returns the appointment and whether this request created it (rather than replayed it).
	std::pair<Json::Value, bool> CreateAppointment(Accounts::Database& db, const Accounts::Identity& actor, const Json::Value& body);
	Json::Value UpdateAppointment(Accounts::Database& db, const Accounts::Identity& actor, const Json::Value& body);

	bool RemindersEnabled() const;
	void QueueReminders(Accounts::Database& db) const;
	bool ReminderEligible(Accounts::Database& db, const std::string& job) const;

private:
	using Offer = ReservationCatalog::Offer;

	struct BookingContact
	{
		std::string userId;
		std::string name;
		std::string email;
		std::string phone;
		std::string locale;
	};

	Json::Value		Catalog() const;
	Offer			FindOffer(const std::string& service, int duration, const std::string& revision) const;
	Json::Value		Availability(Accounts::Database& db,
								 const Json::Value&	 query,
								 const Offer&		 offer,
								 const std::string&	 exclude = "") const;
	Json::Value		Read(Accounts::Database& db, const std::string& id) const;
	BookingContact	ContactForBooking(Accounts::Database&		db,
									  const Accounts::Identity& actor,
									  const Json::Value&		body) const;
	Json::Value		SelectSlot(Accounts::Database& db,
							   const Json::Value&  body,
							   const Offer&		   offer,
							   const std::string&  exclude = "") const;
	void			Reschedule(Accounts::Database&		 db,
							   const Accounts::Identity& actor,
							   const Json::Value&		 appointment,
							   const Json::Value&		 body) const;
	void			ChangeState(Accounts::Database&		  db,
								const Accounts::Identity& actor,
								const Json::Value&		  appointment,
								const Json::Value&		  body) const;
	std::string		Notify(Accounts::Database& db, const Json::Value& appointment, const std::string& action) const;
	const Json::Value& m_Settings;
	ReservationCatalog m_Catalog;
	ReservationAvailability m_Availability;
	Accounts::StoreSettings m_Accounts;
	ReservationNotifications m_Notifications;
};
} // namespace Reservations
