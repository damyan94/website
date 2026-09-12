#include "Controller.h"
#include "Validation.h"
#include "stdafx.h"

namespace Reservations
{
Controller::Controller(ReservationService& service)
	: m_Service(service)
{
}

Accounts::Reply Controller::Handle(Accounts::Database& db,
								  const Accounts::Identity& actor,
								  const Json::Value& body,
								  const std::string& action)
{
	using Validation::Keys;
	Accounts::Reply reply;
	if (action == "options")
	{
		Keys(body, {});
		reply.body = m_Service.GetOptions(db);
	}
	else if (action == "slots")
	{
		Keys(body, {"serviceId", "durationMinutes", "revision", "date"}, {"therapistId", "excludeId"});
		reply.body["slots"] = m_Service.GetSlots(db, actor, body);
	}
	else if (action == "mine" || action == "calendar")
	{
		const bool calendar = action == "calendar";
		if (!calendar)
			Keys(body, {}, {"scope", "page"});
		else
			Keys(body, {"date", "days"}, {"page"});
		reply.body = m_Service.ListAppointments(db, actor, body, calendar);
	}
	else if (action == "create")
	{
		Keys(body,
			 {"serviceId", "durationMinutes", "revision", "date", "startsAt", "therapistId", "requestKey"},
			 {"guest"});
		auto [appointment, created] = m_Service.CreateAppointment(db, actor, body);
		reply.status = created ? 201 : 200;
		reply.body["appointment"] = std::move(appointment);
	}
	else if (action == "update")
	{
		Keys(body, {"id", "version", "action"}, {"date", "startsAt", "therapistId", "revision"});
		reply.body["appointment"] = m_Service.UpdateAppointment(db, actor, body);
	}
	else
		throw std::logic_error("Unknown reservation handler");
	return reply;
}
} // namespace Reservations
