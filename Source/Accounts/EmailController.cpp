#include "EmailController.h"
#include "Crypto.h"
#include "EmailStatusService.h"
#include "InputValidation.h"
#include "stdafx.h"
#include <algorithm>
#include <regex>

namespace Accounts
{
namespace
{
std::string Text(const Json::Value& body, const char* key, int maximum)
{
	if (!body[key].isString())
		throw RequestError(400, "Expected text");
	const auto result = body[key].asString();
	const auto length = TextLength(result);
	if (length < 1 || length > maximum || result.find_first_not_of(' ') == std::string::npos)
		throw RequestError(400, "Invalid text length or encoding");
	return result;
}

std::string Number(const Json::Value& body, const char* key)
{
	const auto value = Text(body, key, 18);
	if (!InputValidation::IsDecimalIdentifier(value))
		throw RequestError(400, "Invalid identifier");
	return value;
}
} // namespace

EmailController::EmailController(const Configuration& configuration, Controller& accounts)
	: m_Configuration(configuration),
	  m_Accounts(accounts)
{
}

void EmailController::DispatchWebhook(const drogon::HttpRequestPtr& request, Callback callback, const SubmitJob& submit)
{
	try
	{
		const auto& id = request->getHeader("svix-id");
		if (request->getHeader("host") != m_Configuration.authority ||
			!VerifyEmailWebhook(m_Configuration.store.email.webhookKey,
								id,
								request->getHeader("svix-timestamp"),
								request->getHeader("svix-signature"),
								request->body()))
			throw RequestError(400, "Invalid email webhook");
		Json::CharReaderBuilder builder;
		builder["collectComments"]	   = false;
		builder["allowComments"]	   = false;
		builder["allowTrailingCommas"] = false;
		builder["rejectDupKeys"]	   = true;
		builder["failIfExtra"]		   = true;
		builder["stackLimit"]		   = 8;
		std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
		Json::Value						  body;
		std::string						  errors;
		const auto						  bytes = request->body();
		if (!reader->parse(bytes.data(), bytes.data() + bytes.size(), &body, &errors) || !body.isObject())
			throw RequestError(400, "Invalid email webhook");
		submit(
			[this, id, body = std::move(body), callback](AccountStore& store)
			{
				try
				{
					m_Accounts.Respond(callback, ReceiveEvent(store, id, body));
				}
				catch (const RequestError& error)
				{
					m_Accounts.Respond(callback, Controller::Error(error.status, error.what()));
				}
				catch (const DatabaseError& error)
				{
					m_Accounts.Respond(callback,
								  error.sqlState.starts_with("22")
									  ? Controller::Error(400, "Invalid email event")
									  : Controller::Error(503, "Email event temporarily unavailable"));
				}
				catch (const std::exception&)
				{
					m_Accounts.Respond(callback, Controller::Error(503, "Email event temporarily unavailable"));
				}
			});
	}
	catch (const RequestError& error)
	{
		m_Accounts.Respond(callback, Controller::Error(error.status, error.what()));
	}
	catch (const std::exception&)
	{
		m_Accounts.Respond(callback, Controller::Error(400, "Invalid email webhook"));
	}
}

Reply EmailController::ReceiveEvent(AccountStore& store, const std::string& eventId, const Json::Value& body) const
{
	if (!m_Configuration.store.email.enabled || m_Configuration.store.email.transport != "resend")
		throw RequestError(404, "Email delivery is disabled");
	const auto type = Text(body, "type", 64);
	Reply	   reply;
	if (type != "email.sent" && type != "email.delivered" && type != "email.delivery_delayed" &&
		type != "email.bounced" && type != "email.complained" && type != "email.failed" && type != "email.suppressed")
		return reply;
	const auto providerId = Text(body["data"], "email_id", 128);
	const auto occurred	  = Text(body, "created_at", 40);
	if (!std::regex_match(providerId, std::regex("[A-Za-z0-9_-]{1,128}")) ||
		!std::regex_match(
			occurred,
			std::regex(
				R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]{1,9})?(Z|[+-][0-9]{2}:[0-9]{2})$)")))
		throw RequestError(400, "Invalid email event");
	if (!store.ReceiveEmailEvent(eventId, type, providerId, occurred))
		throw RequestError(400, "Conflicting email event");
	return reply;
}

Json::Value EmailController::DeliveryStatus(Database& database, const Json::Value& query, const std::string& transport)
{
	for (const auto& key : query.getMemberNames())
		if (key != "before" && key != "state")
			throw RequestError(400, "Unknown email filter");
	const auto before = query.isMember("before") ? Number(query, "before") : "";
	const auto state  = query.isMember("state") ? Text(query, "state", 20) : "";
	if (!state.empty() && state != "queued" && state != "processing" && state != "accepted" && state != "delivered" &&
		state != "delayed" && state != "bounced" && state != "complained" && state != "skipped" && state != "failed")
		throw RequestError(400, "Invalid email state");
	EmailStatusService service(database);
	const auto snapshot = service.ReadStatus(before, state);
	Json::Value result;
	result["transport"] = transport;
	result["counts"]	= Json::objectValue;
	const auto& totals	= snapshot.counts;
	for (const auto& total : totals)
		result["counts"][total.state] = total.count;
	const auto& worker = snapshot.worker;
	result["worker"]["heartbeatAt"] = worker.heartbeatAt;
	result["worker"]["healthy"]		= worker.healthy;
	result["worker"]["error"]		= worker.error;
	result["worker"]["paused"]		= transport == "resend" && worker.paused;
	result["worker"]["pauseUntil"]	= worker.pauseUntil;
	result["jobs"]					= Json::arrayValue;
	const auto& jobs				= snapshot.jobs;
	for (std::size_t i = 0; i < std::min(jobs.size(), std::size_t{50}); ++i)
	{
		const auto& job = jobs[i];
		Json::Value item;
		item["id"]			= job.id;
		item["kind"]		= job.kind;
		item["recipient"]	= job.recipient;
		item["subject"]		= job.subject;
		item["state"]		= job.state;
		item["attempts"]	= job.attempts;
		item["transport"]	= job.transport;
		item["error"]		= job.error;
		item["statusCode"]	= job.statusCode;
		item["providerId"]	= job.providerId;
		item["createdAt"]	= job.createdAt;
		item["availableAt"] = job.availableAt;
		result["jobs"].append(item);
	}
	result["nextBefore"] = jobs.size() > 50 ? jobs[49].id : "";
	return result;
}

} // namespace Accounts
