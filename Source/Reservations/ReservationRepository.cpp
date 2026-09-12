#include "ReservationRepository.h"
#include <sstream>

namespace Reservations
{
namespace
{
std::string UserCondition(bool history)
{
	return "user_id=$1::bigint AND " + std::string(history ? "NOT " : "") +
		   "(state='confirmed' AND ends_at>now())";
}

constexpr const char* CalendarCondition =
	"starts_at>=($1::date::timestamp AT TIME ZONE $2) AND starts_at<(($1::date+$3::int)::timestamp "
	"AT TIME ZONE $2)";
} // namespace

ReservationRepository::ReservationRepository(Accounts::Database& database)
	: m_Database(database)
{
}

std::optional<Json::Value> ReservationRepository::Read(const std::string& id, const std::string& cancelHours)
{
	const auto rows = m_Database.Query(R"SQL(
        SELECT jsonb_build_object('id',id::text,'userId',user_id::text,'contactName',contact_name,
        'contactEmail',contact_email,'contactPhone',contact_phone,'locale',locale,'serviceId',service_id,
        'serviceTitle',service_title,'durationMinutes',duration_minutes,'priceMinor',price_minor,'currency',currency,
        'startsAt',to_char(starts_at AT TIME ZONE 'UTC','YYYY-MM-DD"T"HH24:MI:SS"Z"'),
        'endsAt',to_char(ends_at AT TIME ZONE 'UTC','YYYY-MM-DD"T"HH24:MI:SS"Z"'),
        'therapistId',therapist_id,'roomId',room_id,'state',state,'version',version::text,
        'canCancel',state='confirmed' AND starts_at>=now()+make_interval(hours=>$2::int))::text
        FROM reservations.appointments WHERE id=$1::bigint
    )SQL",
							   {id, cancelHours});
	if (!rows.Count())
		return std::nullopt;
	Json::CharReaderBuilder builder;
	Json::Value				value;
	std::string				errors;
	std::istringstream		input(rows.Get(0, 0));
	if (!Json::parseFromStream(builder, input, &value, &errors))
		throw std::runtime_error("Invalid reservations JSON");
	return value;
}

int ReservationRepository::CountForUser(const std::string& user, bool history)
{
	return Count(UserCondition(history), {user});
}

std::vector<std::string> ReservationRepository::ListIdsForUser(const std::string& user, bool history, int offset)
{
	return ListIds(UserCondition(history), {user}, history, offset);
}

int ReservationRepository::CountCalendar(const std::string& date, const std::string& timezone, int days)
{
	return Count(CalendarCondition, {date, timezone, std::to_string(days)});
}

std::vector<std::string> ReservationRepository::ListCalendarIds(const std::string& date,
																 const std::string& timezone,
																 int days,
																 int offset)
{
	return ListIds(CalendarCondition, {date, timezone, std::to_string(days)}, false, offset);
}

std::optional<BookingRequest> ReservationRepository::FindRequest(const std::string& actor, const std::string& key)
{
	const auto rows = m_Database.Query("SELECT id,request_hash FROM reservations.appointments WHERE actor_id=$1::bigint AND request_key=$2",
				 {actor, key});
	if (!rows.Count())
		return std::nullopt;
	return BookingRequest{rows.Get(0, 0), rows.Get(0, 1)};
}

BookingCustomer ReservationRepository::ReadBookingCustomer(const std::string& user)
{
	const auto row = m_Database.Query("SELECT display_name,email,phone,locale,email_verified_at IS NOT NULL FROM "
								  "accounts.users WHERE id=$1::bigint",
								  {user});
	return {row.Get(0, 4) == "t", row.Get(0, 0), row.Get(0, 1), row.Get(0, 2), row.Get(0, 3)};
}

int ReservationRepository::CountUpcomingForCustomer(const std::string& user)
{
	const auto count = m_Database.Query("SELECT count(*) FROM reservations.appointments WHERE user_id=$1::bigint AND "
									"state='confirmed' AND ends_at>now()",
									{user});
	return std::stoi(count.Get(0, 0));
}

std::string ReservationRepository::CreateAppointment(const AppointmentCreation& appointment)
{
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	const auto title = Json::writeString(builder, appointment.serviceTitle);
	const auto row = m_Database.Query(R"SQL(
            INSERT INTO reservations.appointments(actor_id,request_key,request_hash,user_id,contact_name,contact_email,contact_phone,locale,
                service_id,service_title,duration_minutes,price_minor,currency,starts_at,ends_at,therapist_id,room_id,buffer_before,buffer_after)
            VALUES($1::bigint,$2,$3,NULLIF($4,'')::bigint,$5,$6,$7,$8,$9,$10::jsonb,$11::int,$12::bigint,$13,$14::timestamptz,
                $14::timestamptz+make_interval(mins=>$11::int),$15,$16,$17::int,$18::int) RETURNING id
        )SQL",
							   {appointment.actorId,
									appointment.requestKey,
									appointment.requestHash,
									appointment.userId,
									appointment.contactName,
									appointment.contactEmail,
									appointment.contactPhone,
									appointment.locale,
									appointment.serviceId,
									title,
									std::to_string(appointment.durationMinutes),
									std::to_string(appointment.priceMinor),
									appointment.currency,
									appointment.startsAt,
									appointment.therapistId,
									appointment.roomId,
									std::to_string(appointment.bufferBefore),
									std::to_string(appointment.bufferAfter)});
	return row.Get(0, 0);
}

void ReservationRepository::InsertOccupancy(const std::string& id)
{
	m_Database.Query("INSERT INTO reservations.occupancy(appointment_id,resource_id,during) "
			 "SELECT "
			 "id,resource,tstzrange(starts_at-make_interval(mins=>buffer_before),ends_at+make_interval(mins=>buffer_"
			 "after),'[)') "
			 "FROM reservations.appointments CROSS JOIN LATERAL unnest(ARRAY[therapist_id,room_id]) resource WHERE "
			 "id=$1::bigint",
			 {id});
}

void ReservationRepository::DeleteOccupancy(const std::string& id)
{
	m_Database.Query("DELETE FROM reservations.occupancy WHERE appointment_id=$1::bigint", {id});
}

void ReservationRepository::UpdateSchedule(const std::string& id,
										   const std::string& startsAt,
										   const std::string& therapist,
										   const std::string& room,
										   int bufferBefore,
										   int bufferAfter)
{
	m_Database.Query("UPDATE reservations.appointments SET "
			 "starts_at=$2::timestamptz,ends_at=$2::timestamptz+make_interval(mins=>duration_minutes),"
			 "therapist_id=$3,room_id=$4,buffer_before=$5::int,buffer_after=$6::int,version=version+1,updated_at="
			 "now() WHERE id=$1::bigint",
			 {id,
			  startsAt,
			  therapist,
			  room,
			  std::to_string(bufferBefore),
			  std::to_string(bufferAfter)});
}

AppointmentTiming ReservationRepository::ReadTiming(const std::string& id)
{
	const auto row = m_Database.Query("SELECT starts_at<=now(),ends_at<=now() FROM reservations.appointments WHERE id=$1::bigint", {id});
	return {row.Get(0, 0) == "t", row.Get(0, 1) == "t"};
}

void ReservationRepository::UpdateState(const std::string& id, const std::string& state)
{
	m_Database.Query("UPDATE reservations.appointments SET state=$2,version=version+1,updated_at=now() WHERE id=$1::bigint",
			 {id, state});
}

void ReservationRepository::SkipQueuedReminders(const std::string& id)
{
	m_Database.Query("UPDATE accounts.mail_jobs SET state='skipped',last_error='appointment_changed',body='',request_body=NULL,"
			 "unsubscribe_token=NULL,finished_at=now() WHERE state='queued' AND id IN "
			 "(SELECT mail_job_id FROM reservations.reminders WHERE appointment_id=$1::bigint)",
			 {id});
}

void ReservationRepository::RecordEvent(const std::string& id,
										const std::string& actor,
										const std::string& action)
{
	m_Database.Query("INSERT INTO reservations.events(appointment_id,actor_id,action) VALUES($1::bigint,$2::bigint,$3)",
			 {id, actor, action});
}

std::optional<std::string> ReservationRepository::CurrentNotificationEmail(const std::string& user)
{
	const auto account = m_Database.Query(
			"SELECT email FROM accounts.users WHERE id=$1::bigint AND enabled AND email_verified_at IS NOT NULL",
			{user});
	if (!account.Count())
		return std::nullopt;
	return account.Get(0, 0);
}

std::string ReservationRepository::LocalAppointmentStart(const std::string& id, const std::string& timezone)
{
	return m_Database.Query("SELECT to_char(starts_at AT TIME ZONE $2,'YYYY-MM-DD HH24:MI') FROM "
								 "reservations.appointments WHERE id=$1::bigint",
								 {id, timezone})
						   .Get(0, 0);
}

int ReservationRepository::CountPendingMailJobs()
{
	return std::stoi(m_Database.Query("SELECT count(*) FROM accounts.mail_jobs WHERE state IN ('queued','processing')").Get(0, 0));
}

std::vector<ReminderCandidate> ReservationRepository::DueReminders(const std::string& hours, int limit)
{
	const auto rows = m_Database.Query("SELECT a.id,a.version FROM reservations.appointments a LEFT JOIN accounts.users u ON u.id=a.user_id "
				 "WHERE a.state='confirmed' AND a.starts_at>now()+interval '5 minutes' "
				 "AND a.starts_at<=now()+$1::integer*interval '1 hour' AND "
				 "a.updated_at<=a.starts_at-$1::integer*interval '1 hour' "
				 "AND ((a.user_id IS NULL AND a.contact_email<>'') OR (u.enabled AND u.email_verified_at IS NOT NULL)) "
				 "AND NOT EXISTS(SELECT 1 FROM reservations.reminders r WHERE r.appointment_id=a.id AND "
				 "r.appointment_version=a.version) "
				 "ORDER BY a.starts_at,a.id LIMIT $2::integer FOR UPDATE OF a SKIP LOCKED",
				 {hours, std::to_string(limit)});
	std::vector<ReminderCandidate> reminders;
	for (int i = 0; i < rows.Count(); ++i)
		reminders.push_back({rows.Get(i, 0), rows.Get(i, 1)});
	return reminders;
}

void ReservationRepository::MarkReminderJob(const std::string& job, const std::string& id)
{
	m_Database.Query("UPDATE accounts.mail_jobs SET kind='reminder',expires_at=(SELECT starts_at FROM "
				 "reservations.appointments WHERE id=$2::bigint) WHERE id=$1::bigint",
				 {job, id});
}

void ReservationRepository::RecordReminder(const std::string& id, const std::string& version, const std::string& job)
{
	m_Database.Query("INSERT INTO reservations.reminders(appointment_id,appointment_version,mail_job_id) "
				 "VALUES($1::bigint,$2::bigint,$3::bigint)",
				 {id, version, job});
}

bool ReservationRepository::ReminderEligible(const std::string& job)
{
	return m_Database.Query(
				 "SELECT a.id FROM reservations.reminders r JOIN reservations.appointments a ON a.id=r.appointment_id "
				 "JOIN accounts.mail_jobs j ON j.id=r.mail_job_id LEFT JOIN accounts.users u ON u.id=a.user_id "
				 "WHERE j.id=$1::bigint AND a.state='confirmed' AND a.version=r.appointment_version AND "
				 "a.starts_at>now() "
				 "AND ((a.user_id IS NULL AND j.recipient=a.contact_email) OR (u.enabled AND u.email_verified_at IS "
				 "NOT NULL AND j.recipient=u.email))",
				 {job})
			   .Count() != 0;
}

int ReservationRepository::Count(const std::string& condition, const std::vector<std::string>& args)
{
	return std::stoi(
		m_Database.Query(("SELECT count(*) FROM reservations.appointments WHERE " + condition).c_str(), args).Get(0, 0));
}

std::vector<std::string> ReservationRepository::ListIds(const std::string& condition,
														 std::vector<std::string> args,
														 bool descending,
														 int offset)
{
	args.push_back(std::to_string(offset));
	const auto rows = m_Database.Query(("SELECT id FROM reservations.appointments WHERE " + condition + " ORDER BY starts_at " +
										   (descending ? "DESC" : "ASC") +
										   ",id LIMIT 50 OFFSET $" + std::to_string(args.size()) + "::int")
										  .c_str(),
									  args);
	std::vector<std::string> ids;
	for (int i = 0; i < rows.Count(); ++i)
		ids.push_back(rows.Get(i, 0));
	return ids;
}
} // namespace Reservations
