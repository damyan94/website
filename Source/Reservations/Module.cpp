#include "Module.h"

namespace Reservations
{
Module::Module(Json::Value								settings,
			   std::filesystem::path					configDirectory,
			   std::shared_ptr<PublicContent::Snapshot> content,
			   Accounts::StoreSettings					accounts)
	: m_Configuration(std::move(settings), configDirectory, accounts.locales),
	  m_Service(m_Configuration.Settings(), std::move(content), std::move(accounts)),
	  m_Controller(m_Service)
{
}

void Module::Prepare(const std::string& connection, bool migrate)
{
	m_Configuration.Prepare(connection, migrate);
}

Accounts::ScheduledEmailHooks Module::EmailHooks()
{
	if (!m_Service.RemindersEnabled())
		return {};
	const auto self = shared_from_this();
	return {[self](Accounts::Database& db) { self->m_Service.QueueReminders(db); },
			[self](Accounts::Database& db, const std::string& job) { return self->m_Service.ReminderEligible(db, job); }};
}

void Module::RegisterHandlers(Accounts::Module& accounts)
{
	const auto self	 = shared_from_this();
	auto	   route = [&](const char* path, drogon::HttpMethod method, const char* role, const char* action)
	{
		accounts.RegisterDatabaseHandler(
			path,
			method,
			role,
			[self, action](Accounts::Database& db, const Accounts::Identity& actor, const Json::Value& body)
			{ return self->m_Controller.Handle(db, actor, body, action); });
	};
	route("/api/v1/reservations/options", drogon::Get, "", "options");
	route("/api/v1/reservations/availability", drogon::Get, "", "slots");
	route("/api/v1/me/reservations", drogon::Get, "", "mine");
	route("/api/v1/admin/reservations", drogon::Get, "staff", "calendar");
	route("/api/v1/reservations", drogon::Post, "", "create");
	route("/api/v1/reservations", drogon::Patch, "", "update");
	accounts.RegisterUiAsset("/accounts-assets/reservations.js", "reservations.js", "text/javascript; charset=utf-8");
}
} // namespace Reservations
