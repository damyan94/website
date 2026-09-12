#include "ReservationService.h"
#include "ReservationRepository.h"
#include "Validation.h"
#include "Accounts/Crypto.h"
#include "InputValidation.h"
#include <algorithm>

namespace Reservations
{
namespace
{
using Accounts::RequestError;
using namespace Validation;

std::string Serialize(const Json::Value& value)
{
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	return Json::writeString(builder, value);
}

std::string RecordId(const Json::Value& value)
{
	auto id = Text(value, 1, 18);
	if (!InputValidation::IsDecimalIdentifier(id))
		throw RequestError(400, "Invalid record identifier");
	return id;
}

bool Contains(const Json::Value& array, const std::string& value)
{
	return std::any_of(
		array.begin(), array.end(), [&](const auto& item) { return item.isString() && item.asString() == value; });
}

bool Staff(const Accounts::Identity& actor)
{
	return actor.role == "admin" || actor.role == "operator";
}

std::string Phone(const Json::Value& value)
{
	auto phone = Text(value, 0, 32);
	if (!InputValidation::HasPhoneCharacters(phone))
		throw RequestError(400, "Invalid phone number");
	return phone;
}
} // namespace

ReservationService::ReservationService(const Json::Value& settings,
									   std::shared_ptr<PublicContent::Snapshot> content,
									   Accounts::StoreSettings accounts)
	: m_Settings(settings),
	  m_Catalog(settings, std::move(content)),
	  m_Availability(settings),
	  m_Accounts(std::move(accounts))
{
	Catalog();
}

Json::Value ReservationService::Catalog() const
{
	auto catalog = m_Catalog.Read();
	Json::Value result(Json::objectValue);
	result["revision"]	  = catalog.revision;
	result["timezone"]	  = m_Settings["timezone"];
	result["cancelHours"] = m_Settings["cancel_hours"];
	result["services"]	  = std::move(catalog.services);
	result["resources"]	  = Json::arrayValue;
	for (const auto& resource : m_Settings["resources"])
	{
		Json::Value item;
		for (const auto* key : {"id", "kind", "name"})
			item[key] = resource[key];
		result["resources"].append(item);
	}
	return result;
}

ReservationService::Offer ReservationService::FindOffer(const std::string& service, int duration, const std::string& revision) const
{
	const auto catalog = m_Catalog.Read();
	if (revision != catalog.revision)
		throw RequestError(409, "The service menu changed; reload booking options");
	auto offer = m_Catalog.FindOffer(catalog, service, duration);
	if (offer)
		return std::move(*offer);
	throw RequestError(400, "This service or duration is not available for native booking");
}

Json::Value ReservationService::Availability(Accounts::Database& db,
								 const Json::Value&	 query,
								 const Offer&		 offer,
								 const std::string&	 exclude) const
{
	const auto date = Date(query["date"]), timezone = m_Settings["timezone"].asString();
	const auto choice = Text(query.get("therapistId", ""), 0, 64);
	if (!choice.empty() && !Contains(offer.rule["therapists"], choice))
		throw RequestError(400, "Therapist cannot perform this service");
	const auto clock = db.Query("SELECT floor(extract(epoch FROM now()))::bigint, (now() AT TIME ZONE $1)::date::text, "
								"((now() AT TIME ZONE $1)::date+$2::int)::text",
								{timezone, m_Settings["horizon_days"].asString()});
	Json::Value slots(Json::arrayValue);
	if (date < clock.Get(0, 1) || date > clock.Get(0, 2))
		return slots;
	const auto now = std::stoll(clock.Get(0, 0));
	// Walk real UTC minutes, then inspect local wall-clock minutes. Missing DST
	// times never appear; repeated times keep distinct UTC identities. Checking
	// every occupied minute also enforces breaks across clock transitions.
	const auto timeline = db.Query(R"SQL(
        SELECT extract(epoch FROM t)::bigint, extract(isodow FROM t AT TIME ZONE $2)::int,
               (extract(hour FROM t AT TIME ZONE $2)*60+extract(minute FROM t AT TIME ZONE $2))::int,
               to_char(t AT TIME ZONE 'UTC','YYYY-MM-DD"T"HH24:MI:SS"Z"')
        FROM generate_series($1::date::timestamp AT TIME ZONE $2,
             ($1::date+1)::timestamp AT TIME ZONE $2 - interval '1 minute', interval '1 minute') t
        ORDER BY t
    )SQL",
								   {date, timezone});
	const auto occupied = db.Query(
		"SELECT resource_id,extract(epoch FROM lower(during))::bigint,extract(epoch FROM upper(during))::bigint "
		"FROM reservations.occupancy WHERE during && tstzrange($1::date::timestamp AT TIME ZONE "
		"$2,($1::date+1)::timestamp AT TIME ZONE $2,'[)') "
		"AND appointment_id<>COALESCE(NULLIF($3,'')::bigint,0)",
		{date, timezone, exclude});

	std::vector<ReservationAvailability::Tick> ticks;
	for (int i = 0; i < timeline.Count(); ++i)
		ticks.push_back({std::stoll(timeline.Get(i, 0)),
						 std::stoi(timeline.Get(i, 1)),
						 std::stoi(timeline.Get(i, 2)),
						 timeline.Get(i, 3)});
	std::vector<ReservationAvailability::OccupiedRange> ranges;
	for (int i = 0; i < occupied.Count(); ++i)
		ranges.push_back({occupied.Get(i, 0), std::stoll(occupied.Get(i, 1)), std::stoll(occupied.Get(i, 2))});
	const auto available = m_Availability.Calculate(date, choice, offer.duration, offer.rule, now, ticks, ranges);
	for (const auto& item : available)
	{
		Json::Value slot;
		slot["startsAt"] = item.startsAt;
		slot["therapistId"] = item.therapistId;
		slot["roomId"] = item.roomId;
		slots.append(slot);
	}
	return slots;
}

Json::Value ReservationService::Read(Accounts::Database& db, const std::string& id) const
{
	ReservationRepository repository(db);
	auto appointment = repository.Read(id, m_Settings["cancel_hours"].asString());
	if (!appointment)
		throw RequestError(404, "Reservation not found");
	return std::move(*appointment);
}

std::string ReservationService::Notify(Accounts::Database& db, const Json::Value& appointment, const std::string& action) const
{
	if (!m_Accounts.email.enabled || appointment["contactEmail"].asString().empty())
		return "";
	auto recipient = appointment["contactEmail"].asString();
	if (!appointment["userId"].isNull())
	{
		// Keep the booking's contact snapshot for staff/history, but send new notices
		// to the account's current verified address after an email change.
		const auto account = db.Query(
			"SELECT email FROM accounts.users WHERE id=$1::bigint AND enabled AND email_verified_at IS NOT NULL",
			{appointment["userId"].asString()});
		if (!account.Count())
			return "";
		recipient = account.Get(0, 0);
	}
	const auto locale = appointment["locale"].asString();
	const bool bg	  = locale == "bg";
	const auto title  = appointment["serviceTitle"].get(locale, appointment["serviceTitle"].get("en", "")).asString();
	const auto local  = db.Query("SELECT to_char(starts_at AT TIME ZONE $2,'YYYY-MM-DD HH24:MI') FROM "
								 "reservations.appointments WHERE id=$1::bigint",
								 {appointment["id"].asString(), m_Settings["timezone"].asString()})
						   .Get(0, 0);
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
					 m_Accounts.ui.supportText.get(locale, "").asString()
				   : (bg ? "Управление на резервации: " : "Manage reservations: ") + m_Accounts.email.origin +
					 "/profile?lang=" + locale + "#section-reservations";
	const auto body = m_Accounts.ui.siteName + "\n\n" + appointment["contactName"].asString() + "\n" + title + "\n" +
					  local + " (" + m_Settings["timezone"].asString() + ")\n" + status + "\n\n" +
					  (bg ? "Номер: " : "Reference: ") + appointment["id"].asString() + "\n" + management;
	return Accounts::QueueEmail(
		db, recipient, subject, body, appointment["userId"].isNull() ? "" : appointment["userId"].asString());
}

bool ReservationService::RemindersEnabled() const
{
	return m_Accounts.email.enabled && m_Settings.get("reminders_enabled", false).asBool();
}

void ReservationService::QueueReminders(Accounts::Database& db) const
{
	Accounts::Transaction transaction(db);
	db.Query("SELECT pg_advisory_xact_lock(70123002)");
	const auto pending =
		std::stoi(db.Query("SELECT count(*) FROM accounts.mail_jobs WHERE state IN ('queued','processing')").Get(0, 0));
	const auto capacity = std::min(20, 10000 - pending);
	if (capacity <= 0)
	{
		transaction.Commit();
		return;
	}
	// Bookings made/changed after the reminder time already have a fresh confirmation.
	// Do not follow it with an immediate reminder, or send reminders after the visit starts.
	const auto rows =
		db.Query("SELECT a.id,a.version FROM reservations.appointments a LEFT JOIN accounts.users u ON u.id=a.user_id "
				 "WHERE a.state='confirmed' AND a.starts_at>now()+interval '5 minutes' "
				 "AND a.starts_at<=now()+$1::integer*interval '1 hour' AND "
				 "a.updated_at<=a.starts_at-$1::integer*interval '1 hour' "
				 "AND ((a.user_id IS NULL AND a.contact_email<>'') OR (u.enabled AND u.email_verified_at IS NOT NULL)) "
				 "AND NOT EXISTS(SELECT 1 FROM reservations.reminders r WHERE r.appointment_id=a.id AND "
				 "r.appointment_version=a.version) "
				 "ORDER BY a.starts_at,a.id LIMIT $2::integer FOR UPDATE OF a SKIP LOCKED",
				 {m_Settings.get("reminder_hours", 24).asString(), std::to_string(capacity)});
	for (int i = 0; i < rows.Count(); ++i)
	{
		const auto id  = rows.Get(i, 0);
		const auto job = Notify(db, Read(db, id), "reminder");
		if (job.empty())
			continue;
		db.Query("UPDATE accounts.mail_jobs SET kind='reminder',expires_at=(SELECT starts_at FROM "
				 "reservations.appointments WHERE id=$2::bigint) WHERE id=$1::bigint",
				 {job, id});
		db.Query("INSERT INTO reservations.reminders(appointment_id,appointment_version,mail_job_id) "
				 "VALUES($1::bigint,$2::bigint,$3::bigint)",
				 {id, rows.Get(i, 1), job});
	}
	transaction.Commit();
}

bool ReservationService::ReminderEligible(Accounts::Database& db, const std::string& job) const
{
	return db.Query(
				 "SELECT a.id FROM reservations.reminders r JOIN reservations.appointments a ON a.id=r.appointment_id "
				 "JOIN accounts.mail_jobs j ON j.id=r.mail_job_id LEFT JOIN accounts.users u ON u.id=a.user_id "
				 "WHERE j.id=$1::bigint AND a.state='confirmed' AND a.version=r.appointment_version AND "
				 "a.starts_at>now() "
				 "AND ((a.user_id IS NULL AND j.recipient=a.contact_email) OR (u.enabled AND u.email_verified_at IS "
				 "NOT NULL AND j.recipient=u.email))",
				 {job})
			   .Count() != 0;
}

Json::Value ReservationService::GetOptions(Accounts::Database& db)
{
	auto result = Catalog();
	const auto dates =
		db.Query("SELECT (now() AT TIME ZONE $1)::date::text,((now() AT TIME ZONE $1)::date+$2::int)::text",
				 {m_Settings["timezone"].asString(), m_Settings["horizon_days"].asString()});
	result["today"]	  = dates.Get(0, 0);
	result["maxDate"] = dates.Get(0, 1);
	return result;
}

Json::Value ReservationService::GetSlots(Accounts::Database& db, const Accounts::Identity& actor, const Json::Value& body)
{
	auto exclude = body.isMember("excludeId") ? RecordId(body["excludeId"]) : "";
	if (!exclude.empty() && !Staff(actor))
		throw RequestError(403, "Only staff can reschedule");
	auto offer =
		FindOffer(Id(body["serviceId"]), QueryNumber(body["durationMinutes"], 5, 240), Text(body["revision"], 64, 64));
	return Availability(db, body, offer, exclude);
}

Json::Value ReservationService::ListAppointments(Accounts::Database&	   db,
										 const Accounts::Identity& actor,
										 const Json::Value&		   body,
										 bool					   calendar)
{
	Json::Value result(Json::objectValue);
	const int requested = body.isMember("page") ? QueryNumber(body["page"], 1, 1000000) : 1;
	bool history = false;
	std::string date, timezone;
	int days = 0;
	if (!calendar)
	{
		const auto scope = Text(body.get("scope", "upcoming"));
		if (scope != "upcoming" && scope != "history")
			throw RequestError(400, "Invalid history scope");
		history = scope == "history";
	}
	else
	{
		if (!Staff(actor))
			throw RequestError(403, "Staff access required");
		days = QueryNumber(body["days"], 1, 7);
		date = Date(body["date"]);
		timezone = m_Settings["timezone"].asString();
	}
	ReservationRepository repository(db);
	const auto total = calendar ? repository.CountCalendar(date, timezone, days)
							   : repository.CountForUser(actor.id, history);
	const int pages = std::max(1, (total + 49) / 50), page = std::min(requested, pages);
	const auto ids = calendar ? repository.ListCalendarIds(date, timezone, days, (page - 1) * 50)
							 : repository.ListIdsForUser(actor.id, history, (page - 1) * 50);
	result["appointments"] = Json::arrayValue;
	for (const auto& id : ids)
		result["appointments"].append(Read(db, id));
	result["page"]	= page;
	result["pages"] = pages;
	result["total"] = total;
	return result;
}

std::pair<Json::Value, bool> ReservationService::CreateAppointment(Accounts::Database&		db,
										  const Accounts::Identity& actor,
										  const Json::Value&		body)
{
	const bool guestBooking = body.isMember("guest");
	if (guestBooking && !Staff(actor))
		throw RequestError(403, "Only staff can create guest reservations");
	if (!guestBooking && actor.role != "customer")
		throw RequestError(403, "Only customers can book for themselves; staff must enter a guest");
	const auto key = Id(body["requestKey"]), hash = Accounts::TokenHash(Serialize(body));
	ReservationRepository repository(db);
	const auto existing = repository.FindRequest(actor.id, key);
	if (existing)
	{
		if (existing->requestHash != hash)
			throw RequestError(409, "Request key was already used for different reservation details");
		return {Read(db, existing->id), false};
	}
	const auto contact	= ContactForBooking(db, actor, body);
	const auto service	= Id(body["serviceId"]);
	const auto offer	= FindOffer(service, Number(body["durationMinutes"], 5, 240), Text(body["revision"], 64, 64));
	const auto start	= Text(body["startsAt"], 20, 20);
	const auto selected = SelectSlot(db, body, offer);
	AppointmentCreation creation;
	creation.actorId = actor.id;
	creation.requestKey = key;
	creation.requestHash = hash;
	creation.userId = contact.userId;
	creation.contactName = contact.name;
	creation.contactEmail = contact.email;
	creation.contactPhone = contact.phone;
	creation.locale = contact.locale;
	creation.serviceId = service;
	creation.serviceTitle = offer.title;
	creation.durationMinutes = offer.duration;
	creation.priceMinor = offer.price;
	creation.currency = offer.currency;
	creation.startsAt = start;
	creation.therapistId = selected["therapistId"].asString();
	creation.roomId = selected["roomId"].asString();
	creation.bufferBefore = offer.rule["buffer_before"].asInt();
	creation.bufferAfter = offer.rule["buffer_after"].asInt();
	const auto id = repository.CreateAppointment(creation);
	repository.InsertOccupancy(id);
	repository.RecordEvent(id, actor.id, "created");
	auto appointment = Read(db, id);
	Notify(db, appointment, "confirmed");
	return {std::move(appointment), true};
}

Json::Value ReservationService::UpdateAppointment(Accounts::Database&		db,
										  const Accounts::Identity& actor,
										  const Json::Value&		body)
{
	const auto id = RecordId(body["id"]), version = RecordId(body["version"]), change = Text(body["action"]);
	auto	   appointment = Read(db, id);
	if (!Staff(actor) && appointment["userId"].asString() != actor.id)
		throw RequestError(404, "Reservation not found");
	if (appointment["version"].asString() != version)
		throw RequestError(409, "Reservation changed; refresh before editing");
	if (appointment["state"] != "confirmed")
		throw RequestError(409, "This reservation is already finalized");
	if (change == "reschedule")
		Reschedule(db, actor, appointment, body);
	else
		ChangeState(db, actor, appointment, body);
	ReservationRepository repository(db);
	repository.SkipQueuedReminders(id);
	repository.RecordEvent(id, actor.id, change);
	appointment = Read(db, id);
	Notify(db, appointment, change == "reschedule" ? "rescheduled" : change);
	return appointment;
}

ReservationService::BookingContact ReservationService::ContactForBooking(Accounts::Database&	   db,
												 const Accounts::Identity& actor,
												 const Json::Value&		   body) const
{
	BookingContact contact;
	contact.userId = actor.id;
	if (body.isMember("guest"))
	{
		const auto& guest = body["guest"];
		Keys(guest, {"name", "email", "phone", "locale"});
		contact.userId = "";
		contact.name   = Text(guest["name"], 1, 100);
		contact.email  = Text(guest["email"], 0, 254);
		if (!contact.email.empty())
			contact.email = Accounts::NormalizedEmail(contact.email);
		contact.phone  = Phone(guest["phone"]);
		contact.locale = Text(guest["locale"], 1, 20);
		if (contact.email.empty() && contact.phone.empty())
			throw RequestError(400, "A guest needs an email or phone number");
		if (std::find(m_Accounts.locales.begin(), m_Accounts.locales.end(), contact.locale) == m_Accounts.locales.end())
			throw RequestError(400, "Unsupported contact language");
	}
	else
	{
		ReservationRepository repository(db);
		const auto customer = repository.ReadBookingCustomer(contact.userId);
		if (!customer.emailVerified)
			throw RequestError(403, "Verify your email in My profile before booking");
		contact.name = customer.name;
		contact.email = customer.email;
		contact.phone = customer.phone;
		contact.locale = customer.locale;
		const auto count = repository.CountUpcomingForCustomer(contact.userId);
		if (count >= m_Settings["max_future_per_customer"].asInt())
			throw RequestError(409, "Maximum number of upcoming reservations reached");
	}
	return contact;
}

Json::Value ReservationService::SelectSlot(Accounts::Database& db,
							   const Json::Value&  body,
							   const Offer&		   offer,
							   const std::string&  exclude) const
{
	const auto slots = Availability(db, body, offer, exclude);
	const auto start = Text(body["startsAt"], 20, 20);
	for (const auto& slot : slots)
		if (slot["startsAt"].asString() == start)
			return slot;
	throw RequestError(409,
					   exclude.empty() ? "This time is no longer available; choose another slot"
									   : "This time is no longer available");
}

void ReservationService::Reschedule(Accounts::Database&		  db,
						const Accounts::Identity& actor,
						const Json::Value&		  appointment,
						const Json::Value&		  body) const
{
	const auto id = appointment["id"].asString();
	if (!Staff(actor))
		throw RequestError(403, "Only staff can reschedule");
	Keys(body, {"id", "version", "action", "date", "startsAt", "therapistId", "revision"});
	const auto offer = FindOffer(
		appointment["serviceId"].asString(), appointment["durationMinutes"].asInt(), Text(body["revision"], 64, 64));
	const auto start	= Text(body["startsAt"], 20, 20);
	const auto selected = SelectSlot(db, body, offer, id);
	ReservationRepository repository(db);
	repository.DeleteOccupancy(id);
	// Rescheduling preserves the originally agreed service and price snapshot.
	repository.UpdateSchedule(id,
							  start,
							  selected["therapistId"].asString(),
							  selected["roomId"].asString(),
							  offer.rule["buffer_before"].asInt(),
							  offer.rule["buffer_after"].asInt());
	repository.InsertOccupancy(id);
}

void ReservationService::ChangeState(Accounts::Database&	   db,
						 const Accounts::Identity& actor,
						 const Json::Value&		   appointment,
						 const Json::Value&		   body) const
{
	const auto id = appointment["id"].asString(), change = body["action"].asString();
	Keys(body, {"id", "version", "action"});
	if (change != "cancelled" && change != "completed" && change != "no_show")
		throw RequestError(400, "Invalid reservation action");
	if (!Staff(actor) && (change != "cancelled" || !appointment["canCancel"].asBool()))
		throw RequestError(403, "Cancellation deadline passed; contact staff");
	ReservationRepository repository(db);
	const auto timing = repository.ReadTiming(id);
	if ((change == "completed" && !timing.hasEnded) || (change == "no_show" && !timing.hasStarted))
		throw RequestError(409, "Appointment has not reached that stage yet");
	repository.UpdateState(id, change);
	if (change == "cancelled")
		repository.DeleteOccupancy(id);
}

} // namespace Reservations
