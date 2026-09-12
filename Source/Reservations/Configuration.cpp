#include "Configuration.h"
#include "Validation.h"
#include "Accounts/Database.h"
#include "stdafx.h"

#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace Reservations
{
using namespace Validation;

Configuration::Configuration(Json::Value settings,
							 const std::filesystem::path& configDirectory,
							 const std::vector<std::string>& locales)
	: m_Settings(std::move(settings))
{
	Keys(m_Settings,
		 {"enabled",
		  "schema_file",
		  "timezone",
		  "slot_minutes",
		  "lead_minutes",
		  "horizon_days",
		  "cancel_hours",
		  "max_future_per_customer",
		  "business_hours",
		  "resources",
		  "services",
		  "closures"},
		 {"reminders_enabled", "reminder_hours"});
	if (m_Settings.isMember("reminders_enabled") && !m_Settings["reminders_enabled"].isBool())
		throw RequestError(400, "reminders_enabled must be boolean");
	if (m_Settings.isMember("reminder_hours"))
		Number(m_Settings["reminder_hours"], 1, 168);
	m_Schema = (configDirectory / Text(m_Settings["schema_file"], 1, 1000)).lexically_normal();
	Text(m_Settings["timezone"], 1, 100);
	const int step = Number(m_Settings["slot_minutes"], 5, 60);
	if (60 % step)
		throw std::runtime_error("slot_minutes must divide 60");
	Number(m_Settings["lead_minutes"], 0, 10080);
	Number(m_Settings["horizon_days"], 1, 365);
	Number(m_Settings["cancel_hours"], 0, 168);
	Number(m_Settings["max_future_per_customer"], 1, 50);
	auto hours = [](const Json::Value& windows)
	{
		if (!windows.isArray() || windows.empty() || windows.size() > 28)
			throw RequestError(400, "Invalid opening hours");
		for (const auto& window : windows)
		{
			Keys(window, {"days", "start", "end"});
			if (!window["days"].isArray() || window["days"].empty() || window["days"].size() > 7 ||
				Minute(window["end"]) <= Minute(window["start"]))
				throw RequestError(400, "Invalid opening window; overnight windows are not supported");
			for (const auto& day : window["days"])
				Number(day, 1, 7);
		}
	};
	hours(m_Settings["business_hours"]);
	if (!m_Settings["resources"].isArray() || m_Settings["resources"].empty() || m_Settings["resources"].size() > 20)
		throw RequestError(400, "Configure 1–20 resources");
	std::map<std::string, std::string> resources;
	for (const auto& resource : m_Settings["resources"])
	{
		Keys(resource, {"id", "kind", "name", "hours"});
		const auto id = Id(resource["id"]), kind = Text(resource["kind"]);
		if ((kind != "therapist" && kind != "room") || !resources.emplace(id, kind).second)
			throw RequestError(400, "Invalid or duplicate resource");
		if (!resource["name"].isObject())
			throw RequestError(400, "Resource names must be localized");
		for (const auto& locale : locales)
			Text(resource["name"][locale], 1, 100);
		hours(resource["hours"]);
	}
	if (!m_Settings["services"].isArray() || m_Settings["services"].empty() || m_Settings["services"].size() > 100)
		throw RequestError(400, "Configure 1–100 bookable services");
	std::set<std::string> services;
	for (const auto& service : m_Settings["services"])
	{
		Keys(service, {"id", "therapists", "rooms", "buffer_before", "buffer_after"});
		if (!services.insert(Id(service["id"])).second)
			throw RequestError(400, "Duplicate service rule");
		Number(service["buffer_before"], 0, 60);
		Number(service["buffer_after"], 0, 60);
		for (const auto* field : {"therapists", "rooms"})
		{
			if (!service[field].isArray() || service[field].empty() || service[field].size() > 10)
				throw RequestError(400, "Configure 1–10 eligible resources per service");
			std::set<std::string> unique;
			for (const auto& value : service[field])
			{
				const auto id = Id(value);
				if (!resources.contains(id) ||
					resources.at(id) != (std::string(field) == "therapists" ? "therapist" : "room") ||
					!unique.insert(id).second)
					throw RequestError(400, "Service references an invalid resource");
			}
		}
	}
	if (!m_Settings["closures"].isArray() || m_Settings["closures"].size() > 1000)
		throw RequestError(400, "Invalid closures");
	for (const auto& closure : m_Settings["closures"])
	{
		Keys(closure, {"date", "resources"});
		Date(closure["date"]);
		if (!closure["resources"].isArray() || closure["resources"].size() > 20)
			throw RequestError(400, "Invalid closure resources");
		for (const auto& id : closure["resources"])
			if (!resources.contains(Id(id)))
				throw RequestError(400, "Unknown closure resource");
	}
}

void Configuration::Prepare(const std::string& connection, bool migrate)
{
	Accounts::Database db(connection);
	db.CheckSchema();
	// Validate the configured zone directly; enumerating pg_timezone_names scans
	// every zone file and is unnecessarily slow on some local filesystems.
	db.Query("SELECT now() AT TIME ZONE $1", {m_Settings["timezone"].asString()});
	if (migrate)
	{
		db.Query("SELECT pg_advisory_lock(70123003)");
		if (db.Query("SELECT to_regclass('reservations.schema_version') IS NOT NULL").Get(0, 0) != "t")
		{
			std::ifstream	   file(m_Schema);
			std::ostringstream bytes;
			bytes << file.rdbuf();
			if (!file || bytes.str().size() > 1024 * 1024)
				throw std::runtime_error("Cannot read reservation migration");
			db.Script(bytes.str());
		}
		const auto version = db.Query("SELECT version FROM reservations.schema_version");
		if (version.Count() == 1 && version.Get(0, 0) == "1")
		{
			std::ifstream	   file(m_Schema.parent_path() / "002_reminders.sql");
			std::ostringstream bytes;
			bytes << file.rdbuf();
			if (!file || bytes.str().size() > 1024 * 1024)
				throw std::runtime_error("Cannot read reminder migration");
			db.Script(bytes.str());
		}
	}
	const auto schema = db.Query("SELECT version FROM reservations.schema_version");
	if (schema.Count() != 1 || schema.Get(0, 0) != "2")
		throw std::runtime_error("Run --migrate-reservations");
	Accounts::Transaction transaction(db);
	db.Query("SELECT pg_advisory_xact_lock(70123002)");
	for (const auto& resource : m_Settings["resources"])
	{
		const auto id = resource["id"].asString(), kind = resource["kind"].asString();
		if (migrate)
			db.Query("INSERT INTO reservations.resources(id,kind) VALUES($1,$2) ON CONFLICT DO NOTHING", {id, kind});
		const auto stored = db.Query("SELECT kind FROM reservations.resources WHERE id=$1", {id});
		if (!stored.Count() || stored.Get(0, 0) != kind)
			throw std::runtime_error("Resource missing or kind changed; run --migrate-reservations for new IDs");
	}
	transaction.Commit();
}

} // namespace Reservations
