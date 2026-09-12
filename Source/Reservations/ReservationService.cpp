#include "ReservationService.h"
#include "Validation.h"
#include "Accounts/Crypto.h"
#include "InputValidation.h"
#include <algorithm>
#include <sstream>

namespace Reservations
{
namespace
{
using Accounts::RequestError;
using namespace Validation;

Json::Value Parse(const std::string& bytes)
{
	Json::CharReaderBuilder builder;
	Json::Value				value;
	std::string				errors;
	std::istringstream		input(bytes);
	if (!Json::parseFromStream(builder, input, &value, &errors))
		throw std::runtime_error("Invalid reservations JSON");
	return value;
}

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
	  m_Content(std::move(content)),
	  m_Accounts(std::move(accounts))
{
	Catalog();
}

Json::Value ReservationService::Catalog() const
{
	const auto	bytes	 = m_Content->Get();
	const auto	document = Parse(*bytes);
	Json::Value result(Json::objectValue);
	result["revision"]	  = Accounts::TokenHash(*bytes);
	result["timezone"]	  = m_Settings["timezone"];
	result["cancelHours"] = m_Settings["cancel_hours"];
	result["services"]	  = Json::arrayValue;
	result["resources"]	  = Json::arrayValue;
	for (const auto& resource : m_Settings["resources"])
	{
		Json::Value item;
		for (const auto* key : {"id", "kind", "name"})
			item[key] = resource[key];
		result["resources"].append(item);
	}
	for (const auto& rule : m_Settings["services"])
		for (const auto& service : document["services"])
		{
			if (service["id"] != rule["id"] || !service.get("available", true).asBool())
				continue;
			Json::Value item;
			item["id"]		   = service["id"];
			item["title"]	   = service["title"];
			item["therapists"] = rule["therapists"];
			item["variants"]   = Json::arrayValue;
			for (const auto& variant : service["variants"])
			{
				if (!variant["priceMinor"].isInt64() || variant["priceMinor"].asInt64() < 0 ||
					variant["priceMinor"].asInt64() > 1000000000 || !variant["durationMinutes"].isInt() ||
					variant["durationMinutes"].asInt() < 5 || variant["durationMinutes"].asInt() > 240 ||
					!variant["currency"].isString() || variant["currency"].asString().size() != 3)
					continue;
				item["variants"].append(variant);
			}
			if (!item["variants"].empty())
				result["services"].append(item);
		}
	return result;
}

ReservationService::Offer ReservationService::FindOffer(const std::string& service, int duration, const std::string& revision) const
{
	const auto catalog = Catalog();
	if (revision != catalog["revision"].asString())
		throw RequestError(409, "The service menu changed; reload booking options");
	for (const auto& item : catalog["services"])
		if (item["id"].asString() == service)
			for (const auto& variant : item["variants"])
				if (variant["durationMinutes"].asInt() == duration)
					for (const auto& rule : m_Settings["services"])
						if (rule["id"].asString() == service)
							return {item["title"],
									duration,
									variant["priceMinor"].asInt64(),
									variant["currency"].asString(),
									rule};
	throw RequestError(400, "This service or duration is not available for native booking");
}

bool ReservationService::Open(const Json::Value& windows, int weekday, int minute) const
{
	for (const auto& window : windows)
		if (std::any_of(
				window["days"].begin(), window["days"].end(), [&](const auto& d) { return d.asInt() == weekday; }) &&
			minute >= Minute(window["start"]) && minute < Minute(window["end"]))
			return true;
	return false;
}

bool ReservationService::Closed(const std::string& date, const std::string& resource) const
{
	for (const auto& closure : m_Settings["closures"])
		if (closure["date"].asString() == date &&
			(closure["resources"].empty() || Contains(closure["resources"], resource)))
			return true;
	return false;
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
	const auto earliest = std::stoll(clock.Get(0, 0)) + m_Settings["lead_minutes"].asInt() * 60;
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

	struct Tick
	{
		long long	instant;
		int			weekday;
		int			minute;
		std::string stamp;
	};

	std::vector<Tick> ticks;
	for (int i = 0; i < timeline.Count(); ++i)
		ticks.push_back({std::stoll(timeline.Get(i, 0)),
						 std::stoi(timeline.Get(i, 1)),
						 std::stoi(timeline.Get(i, 2)),
						 timeline.Get(i, 3)});
	const int before = offer.rule["buffer_before"].asInt(), after = offer.rule["buffer_after"].asInt();
	for (int i = before; i + offer.duration + after <= static_cast<int>(ticks.size()); ++i)
	{
		if (ticks[i].instant < earliest || ticks[i].minute % m_Settings["slot_minutes"].asInt())
			continue;
		const int begin = i - before, end = i + offer.duration + after;
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
			for (int k = 0; k < occupied.Count(); ++k)
				if (occupied.Get(k, 0) == id && starts < std::stoll(occupied.Get(k, 2)) &&
					ends > std::stoll(occupied.Get(k, 1)))
					return false;
			return true;
		};
		std::string therapist, room;
		for (const auto& id : offer.rule["therapists"])
			if ((choice.empty() || choice == id.asString()) && free(id.asString()))
			{
				therapist = id.asString();
				break;
			}
		if (therapist.empty())
			continue;
		for (const auto& id : offer.rule["rooms"])
			if (free(id.asString()))
			{
				room = id.asString();
				break;
			}
		if (room.empty())
			continue;
		Json::Value slot;
		slot["startsAt"]	= ticks[i].stamp;
		slot["therapistId"] = therapist;
		slot["roomId"]		= room;
		slots.append(slot);
	}
	return slots;
}

Json::Value ReservationService::Read(Accounts::Database& db, const std::string& id) const
{
	const auto rows = db.Query(R"SQL(
        SELECT jsonb_build_object('id',id::text,'userId',user_id::text,'contactName',contact_name,
        'contactEmail',contact_email,'contactPhone',contact_phone,'locale',locale,'serviceId',service_id,
        'serviceTitle',service_title,'durationMinutes',duration_minutes,'priceMinor',price_minor,'currency',currency,
        'startsAt',to_char(starts_at AT TIME ZONE 'UTC','YYYY-MM-DD"T"HH24:MI:SS"Z"'),
        'endsAt',to_char(ends_at AT TIME ZONE 'UTC','YYYY-MM-DD"T"HH24:MI:SS"Z"'),
        'therapistId',therapist_id,'roomId',room_id,'state',state,'version',version::text,
        'canCancel',state='confirmed' AND starts_at>=now()+make_interval(hours=>$2::int))::text
        FROM reservations.appointments WHERE id=$1::bigint
    )SQL",
							   {id, m_Settings["cancel_hours"].asString()});
	if (!rows.Count())
		throw RequestError(404, "Reservation not found");
	return Parse(rows.Get(0, 0));
}

void ReservationService::Occupy(Accounts::Database& db, const std::string& id) const
{
	db.Query("INSERT INTO reservations.occupancy(appointment_id,resource_id,during) "
			 "SELECT "
			 "id,resource,tstzrange(starts_at-make_interval(mins=>buffer_before),ends_at+make_interval(mins=>buffer_"
			 "after),'[)') "
			 "FROM reservations.appointments CROSS JOIN LATERAL unnest(ARRAY[therapist_id,room_id]) resource WHERE "
			 "id=$1::bigint",
			 {id});
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
	const int				 requested = body.isMember("page") ? QueryNumber(body["page"], 1, 1000000) : 1;
	std::string				 condition;
	std::vector<std::string> args;
	if (!calendar)
	{
		const auto scope = Text(body.get("scope", "upcoming"));
		if (scope != "upcoming" && scope != "history")
			throw RequestError(400, "Invalid history scope");
		condition = "user_id=$1::bigint AND " + std::string(scope == "history" ? "NOT " : "") +
					"(state='confirmed' AND ends_at>now())";
		args = {actor.id};
	}
	else
	{
		if (!Staff(actor))
			throw RequestError(403, "Staff access required");
		const int days = QueryNumber(body["days"], 1, 7);
		condition = "starts_at>=($1::date::timestamp AT TIME ZONE $2) AND starts_at<(($1::date+$3::int)::timestamp "
					"AT TIME ZONE $2)";
		args	  = {Date(body["date"]), m_Settings["timezone"].asString(), std::to_string(days)};
	}
	const auto total = std::stoi(
		db.Query(("SELECT count(*) FROM reservations.appointments WHERE " + condition).c_str(), args).Get(0, 0));
	const int pages = std::max(1, (total + 49) / 50), page = std::min(requested, pages);
	args.push_back(std::to_string((page - 1) * 50));
	const auto rows = db.Query(("SELECT id FROM reservations.appointments WHERE " + condition + " ORDER BY starts_at " +
								(!calendar && body.get("scope", "upcoming") == "history" ? "DESC" : "ASC") +
								",id LIMIT 50 OFFSET $" + std::to_string(args.size()) + "::int")
								   .c_str(),
							   args);
	result["appointments"] = Json::arrayValue;
	for (int i = 0; i < rows.Count(); ++i)
		result["appointments"].append(Read(db, rows.Get(i, 0)));
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
	const auto existing =
		db.Query("SELECT id,request_hash FROM reservations.appointments WHERE actor_id=$1::bigint AND request_key=$2",
				 {actor.id, key});
	if (existing.Count())
	{
		if (existing.Get(0, 1) != hash)
			throw RequestError(409, "Request key was already used for different reservation details");
		return {Read(db, existing.Get(0, 0)), false};
	}
	const auto contact	= ContactForBooking(db, actor, body);
	const auto service	= Id(body["serviceId"]);
	const auto offer	= FindOffer(service, Number(body["durationMinutes"], 5, 240), Text(body["revision"], 64, 64));
	const auto start	= Text(body["startsAt"], 20, 20);
	const auto selected = SelectSlot(db, body, offer);
	const auto row		= db.Query(R"SQL(
            INSERT INTO reservations.appointments(actor_id,request_key,request_hash,user_id,contact_name,contact_email,contact_phone,locale,
                service_id,service_title,duration_minutes,price_minor,currency,starts_at,ends_at,therapist_id,room_id,buffer_before,buffer_after)
            VALUES($1::bigint,$2,$3,NULLIF($4,'')::bigint,$5,$6,$7,$8,$9,$10::jsonb,$11::int,$12::bigint,$13,$14::timestamptz,
                $14::timestamptz+make_interval(mins=>$11::int),$15,$16,$17::int,$18::int) RETURNING id
        )SQL",
							   {actor.id,
									key,
									hash,
									contact.userId,
									contact.name,
									contact.email,
									contact.phone,
									contact.locale,
									service,
									Serialize(offer.title),
									std::to_string(offer.duration),
									std::to_string(offer.price),
									offer.currency,
									start,
									selected["therapistId"].asString(),
									selected["roomId"].asString(),
									offer.rule["buffer_before"].asString(),
									offer.rule["buffer_after"].asString()});
	const auto id		= row.Get(0, 0);
	Occupy(db, id);
	RecordEvent(db, actor, id, "created");
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
	db.Query("UPDATE accounts.mail_jobs SET state='skipped',last_error='appointment_changed',body='',request_body=NULL,"
			 "unsubscribe_token=NULL,finished_at=now() WHERE state='queued' AND id IN "
			 "(SELECT mail_job_id FROM reservations.reminders WHERE appointment_id=$1::bigint)",
			 {id});
	RecordEvent(db, actor, id, change);
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
		const auto row = db.Query("SELECT display_name,email,phone,locale,email_verified_at IS NOT NULL FROM "
								  "accounts.users WHERE id=$1::bigint",
								  {contact.userId});
		if (row.Get(0, 4) != "t")
			throw RequestError(403, "Verify your email in My profile before booking");
		contact.name	 = row.Get(0, 0);
		contact.email	 = row.Get(0, 1);
		contact.phone	 = row.Get(0, 2);
		contact.locale	 = row.Get(0, 3);
		const auto count = db.Query("SELECT count(*) FROM reservations.appointments WHERE user_id=$1::bigint AND "
									"state='confirmed' AND ends_at>now()",
									{contact.userId});
		if (std::stoi(count.Get(0, 0)) >= m_Settings["max_future_per_customer"].asInt())
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

void ReservationService::RecordEvent(Accounts::Database&	   db,
						 const Accounts::Identity& actor,
						 const std::string&		   id,
						 const std::string&		   action) const
{
	db.Query("INSERT INTO reservations.events(appointment_id,actor_id,action) VALUES($1::bigint,$2::bigint,$3)",
			 {id, actor.id, action});
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
	db.Query("DELETE FROM reservations.occupancy WHERE appointment_id=$1::bigint", {id});
	// Rescheduling preserves the originally agreed service and price snapshot.
	db.Query("UPDATE reservations.appointments SET "
			 "starts_at=$2::timestamptz,ends_at=$2::timestamptz+make_interval(mins=>duration_minutes),"
			 "therapist_id=$3,room_id=$4,buffer_before=$5::int,buffer_after=$6::int,version=version+1,updated_at="
			 "now() WHERE id=$1::bigint",
			 {id,
			  start,
			  selected["therapistId"].asString(),
			  selected["roomId"].asString(),
			  offer.rule["buffer_before"].asString(),
			  offer.rule["buffer_after"].asString()});
	Occupy(db, id);
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
	const auto timing =
		db.Query("SELECT starts_at<=now(),ends_at<=now() FROM reservations.appointments WHERE id=$1::bigint", {id});
	if ((change == "completed" && timing.Get(0, 1) != "t") || (change == "no_show" && timing.Get(0, 0) != "t"))
		throw RequestError(409, "Appointment has not reached that stage yet");
	db.Query("UPDATE reservations.appointments SET state=$2,version=version+1,updated_at=now() WHERE id=$1::bigint",
			 {id, change});
	if (change == "cancelled")
		db.Query("DELETE FROM reservations.occupancy WHERE appointment_id=$1::bigint", {id});
}

} // namespace Reservations
