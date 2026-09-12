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
