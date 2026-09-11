#pragma once

#include "Accounts/Module.h"
#include "PublicContent.h"
#include <map>

namespace Reservations
{
// Site-specific schedules are configuration; appointments and their immutable
// commercial/contact snapshots are private database records.
class Module : public std::enable_shared_from_this<Module>
{
public:
	Module(Json::Value								settings,
		   std::filesystem::path					configDirectory,
		   std::shared_ptr<PublicContent::Snapshot> content,
		   Accounts::StoreSettings					accounts);
	void						  Prepare(const std::string& connection, bool migrate);
	void						  RegisterHandlers(Accounts::Module& accounts);
	Accounts::ScheduledEmailHooks EmailHooks();

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
	Accounts::Reply Handle(Accounts::Database&		 db,
						   const Accounts::Identity& actor,
						   const Json::Value&		 body,
						   const std::string&		 action);
	Accounts::Reply GetOptions(Accounts::Database& db, const Json::Value& body);
	Accounts::Reply GetSlots(Accounts::Database& db, const Accounts::Identity& actor, const Json::Value& body);
	Accounts::Reply ListAppointments(Accounts::Database&	   db,
									 const Accounts::Identity& actor,
									 const Json::Value&		   body,
									 bool					   calendar);
	Accounts::Reply CreateAppointment(Accounts::Database& db, const Accounts::Identity& actor, const Json::Value& body);
	Accounts::Reply UpdateAppointment(Accounts::Database& db, const Accounts::Identity& actor, const Json::Value& body);
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
	void			QueueReminders(Accounts::Database& db) const;
	bool			ReminderEligible(Accounts::Database& db, const std::string& job) const;
	bool			Open(const Json::Value& windows, int weekday, int minute) const;
	bool			Closed(const std::string& date, const std::string& resource) const;
	Json::Value		m_Settings;
	std::filesystem::path					 m_Schema;
	std::shared_ptr<PublicContent::Snapshot> m_Content;
	Accounts::StoreSettings					 m_Accounts;
};
} // namespace Reservations
