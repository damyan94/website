#pragma once

#include "Accounts/Types.h"
#include "PublicContent.h"
#include <utility>

namespace Reservations
{
// HTTP workflows use the caller's authenticated transaction. Appointment reads
// delegate to ReservationRepository; mutation, availability and reminder SQL stay here.
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
	struct Offer
	{
		Json::Value title;
		int			duration;
		long long	price;
		std::string currency;
		Json::Value rule;
	};

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
	void			RecordEvent(Accounts::Database&		  db,
								const Accounts::Identity& actor,
								const std::string&		  id,
								const std::string&		  action) const;
	void			Reschedule(Accounts::Database&		 db,
							   const Accounts::Identity& actor,
							   const Json::Value&		 appointment,
							   const Json::Value&		 body) const;
	void			ChangeState(Accounts::Database&		  db,
								const Accounts::Identity& actor,
								const Json::Value&		  appointment,
								const Json::Value&		  body) const;
	void			Occupy(Accounts::Database& db, const std::string& id) const;
	std::string		Notify(Accounts::Database& db, const Json::Value& appointment, const std::string& action) const;
	bool			Open(const Json::Value& windows, int weekday, int minute) const;
	bool			Closed(const std::string& date, const std::string& resource) const;
	const Json::Value& m_Settings;
	std::shared_ptr<PublicContent::Snapshot> m_Content;
	Accounts::StoreSettings m_Accounts;
};
} // namespace Reservations
