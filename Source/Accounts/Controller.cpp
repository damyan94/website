#include "Controller.h"
#include "Crypto.h"
#include "stdafx.h"

#include <charconv>
#include <drogon/Cookie.h>

namespace Accounts
{
namespace
{
void Headers(const drogon::HttpResponsePtr& response)
{
	response->addHeader("Cache-Control", "no-store");
	response->addHeader("X-Content-Type-Options", "nosniff");
	response->addHeader("X-Frame-Options", "DENY");
	response->addHeader("Referrer-Policy", "no-referrer");
	response->addHeader("Content-Security-Policy",
						"default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; "
						"object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'");
}

UserListQuery UsersQuery(const drogon::HttpRequestPtr& request)
{
	UserListQuery query;
	const auto&	  parameters = request->getParameters();
	for (const auto* key : {"page", "q", "role", "enabled", "sort", "order"})
		if (parameters.find(key) != parameters.end())
			query.paginated = true;
	if (!query.paginated)
		return query;
	if (parameters.find("after") != parameters.end())
		throw RequestError(400, "Do not combine page and cursor pagination");
	query.search  = request->getParameter("q");
	query.role	  = request->getParameter("role");
	query.enabled = request->getParameter("enabled");
	if (query.search.size() > 400 || query.role.size() > 20 || query.enabled.size() > 5)
		throw RequestError(400, "Invalid user filters");
	if (parameters.find("sort") != parameters.end())
		query.sort = request->getParameter("sort");
	if (parameters.find("order") != parameters.end())
		query.order = request->getParameter("order");
	if (parameters.find("page") != parameters.end())
	{
		const auto& value  = request->getParameter("page");
		const auto	parsed = std::from_chars(value.data(), value.data() + value.size(), query.page);
		if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size())
			throw RequestError(400, "Invalid page number");
	}
	return query;
}
} // namespace

Reply Controller::Error(int status, const char* message)
{
	Reply reply;
	reply.status		= status;
	reply.body["error"] = message;
	reply.clearCookie	= status == 401;
	return reply;
}

Controller::Controller(const Configuration& configuration)
	: m_Configuration(configuration)
{
}

bool Controller::AllowPasswordWork(const std::string& peer, const std::string& identity)
{
	std::lock_guard lock(m_LimitMutex);
	const auto		now = std::chrono::steady_clock::now();
	for (auto it = m_Limits.begin(); it != m_Limits.end();)
		if (it->second.expires <= now)
			it = m_Limits.erase(it);
		else
			++it;
	const std::pair<std::string, int> limits[] = {
		{"global", 120}, {"peer:" + peer, 60}, {"identity:" + TokenHash(identity), 10}};
	for (const auto& [key, maximum] : limits)
	{
		auto it = m_Limits.find(key);
		if (it != m_Limits.end() && it->second.count >= maximum)
			return false;
		if (it == m_Limits.end() && m_Limits.size() >= 2048)
			return false;
	}
	for (const auto& [key, maximum] : limits)
	{
		(void)maximum;
		auto [it, inserted] = m_Limits.try_emplace(key, Limit{now + std::chrono::seconds(60), 0});
		(void)inserted;
		++it->second.count;
	}
	return true;
}

void Controller::Respond(const Callback& callback, Reply reply) const
{
	auto response = drogon::HttpResponse::newHttpJsonResponse(reply.body);
	response->setStatusCode(static_cast<drogon::HttpStatusCode>(reply.status));
	Headers(response);
	if (reply.status == 429 || reply.status == 503)
		response->addHeader("Retry-After", "60");
	if (!reply.sessionToken.empty() || reply.clearCookie)
	{
		drogon::Cookie cookie(m_Configuration.cookieName, reply.clearCookie ? "" : reply.sessionToken);
		cookie.setPath("/");
		cookie.setHttpOnly(true);
		cookie.setSecure(m_Configuration.secure);
		cookie.setSameSite(drogon::Cookie::SameSite::kLax);
		cookie.setMaxAge(reply.clearCookie ? 0 : m_Configuration.store.absoluteSeconds);
		response->addCookie(cookie);
	}
	callback(response);
}

void Controller::Dispatch(Action						action,
					  const drogon::HttpRequestPtr& request,
					  Callback						callback,
					  const SubmitJob&				submit,
					  ProtectedOperation			operation,
					  std::string					requiredRole,
					  std::size_t					maximumBodyBytes,
					  bool							oneClick,
					  DatabaseOperation				databaseOperation)
{
	try
	{
		auto pending			  = ParseRequest(action, request, bool(databaseOperation), oneClick, maximumBodyBytes);
		pending.operation		  = std::move(operation);
		pending.databaseOperation = std::move(databaseOperation);
		pending.requiredRole	  = std::move(requiredRole);
		submit([this, pending = std::move(pending), callback](AccountStore& store)
			   { Execute(store, pending, callback); });
	}
	catch (const RequestError& error)
	{
		Respond(callback, Error(error.status, error.what()));
	}
	catch (const std::exception&)
	{
		Respond(callback, Error(400, "Invalid account request"));
	}
}

Json::Value Controller::ParseBody(const drogon::HttpRequestPtr& request,
							  bool							mutation,
							  bool							databaseOperation,
							  bool							oneClick,
							  std::size_t					maximumBodyBytes) const
{
	if (request->getHeader("host") != m_Configuration.authority ||
		(!oneClick && request->getHeader("sec-fetch-site") == "cross-site"))
		throw RequestError(403, "Request origin is not allowed");
	Json::Value body(Json::objectValue);
	if (databaseOperation && !mutation)
	{
		if (request->getParameters().size() > 10)
			throw RequestError(400, "Too many query fields");
		for (const auto& [key, value] : request->getParameters())
		{
			if (key.size() > 40 || value.size() > 400)
				throw RequestError(400, "Query field too long");
			body[key] = value;
		}
	}
	if (oneClick)
	{
		if (request->body() != "List-Unsubscribe=One-Click" ||
			request->getHeader("content-type") != "application/x-www-form-urlencoded")
			throw RequestError(400, "Invalid unsubscribe request");
		body["token"] = request->getParameter("token");
	}
	else if (mutation)
	{
		if (request->getHeader("origin") != m_Configuration.origin || request->getHeader("x-accounts-request") != "1")
			throw RequestError(403, "Request origin is not allowed");
		const auto& type = request->getHeader("content-type");
		if (type != "application/json" && type != "application/json; charset=utf-8")
			throw RequestError(415, "Use application/json");
		const auto bytes = request->body();
		if (bytes.size() > maximumBodyBytes)
			throw RequestError(413, "Request body is too large");
		Json::CharReaderBuilder builder;
		builder["collectComments"]	   = false;
		builder["allowComments"]	   = false;
		builder["allowTrailingCommas"] = false;
		builder["rejectDupKeys"]	   = true;
		builder["failIfExtra"]		   = true;
		builder["stackLimit"]		   = 8;
		std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
		std::string						  errors;
		if (!reader->parse(bytes.data(), bytes.data() + bytes.size(), &body, &errors) || !body.isObject())
			throw RequestError(400, "Invalid JSON object");
	}
	return body;
}

Controller::PendingRequest Controller::ParseRequest(Action						  action,
											const drogon::HttpRequestPtr& request,
											bool						  databaseOperation,
											bool						  oneClick,
											std::size_t					  maximumBodyBytes)
{
	const bool mutation = action != Action::Me && action != Action::ListUsers && action != Action::EmailOptions &&
						  action != Action::ListCampaigns;
	PendingRequest parsed;
	parsed.action	= action;
	parsed.mutation = mutation;
	parsed.body		= ParseBody(request, mutation, databaseOperation, oneClick, maximumBodyBytes);
	auto token		= request->getCookie(m_Configuration.cookieName);
	auto csrf		= request->getHeader("x-csrf-token");
	if (token.size() > 64 || csrf.size() > 64)
		throw RequestError(400, "Invalid request token");
	CheckRequestLimit(action, request, parsed.body, token);
	auto cursor = request->getParameter("after");
	if (cursor.size() > 18)
		throw RequestError(400, "Invalid pagination cursor");
	auto users	  = action == Action::ListUsers ? UsersQuery(request) : UserListQuery{};
	parsed.token  = std::move(token);
	parsed.csrf	  = std::move(csrf);
	parsed.cursor = std::move(cursor);
	parsed.users  = std::move(users);
	return parsed;
}

void Controller::CheckRequestLimit(Action						 action,
							   const drogon::HttpRequestPtr& request,
							   const Json::Value&			 body,
							   const std::string&			 token)
{
	if (action == Action::Login || action == Action::CreateUser || action == Action::ChangePassword ||
		action == Action::RegisterEmail || action == Action::ForgotPassword || action == Action::CompleteRegistration ||
		action == Action::CompleteReset || action == Action::ConfirmEmail || action == Action::VerifyEmail ||
		action == Action::ChangeEmail || action == Action::NewsletterPreference || action == Action::Unsubscribe ||
		action == Action::PreviewCampaign || action == Action::SendCampaign)
	{
		const bool address =
			action == Action::Login || action == Action::RegisterEmail || action == Action::ForgotPassword;
		const auto identity = address		  ? NormalizedEmail(body.get("email", "").asString())
							  : token.empty() ? body.get("token", request->peerAddr().toIp()).asString()
											  : token;
		if (!AllowPasswordWork(request->peerAddr().toIp(), identity))
			throw RequestError(429, "Too many attempts; try again in one minute");
	}
}

void Controller::Execute(AccountStore& store, const PendingRequest& request, const Callback& callback) const
{
	const auto& [action, body, token, csrf, cursor, users, mutation, operation, databaseOperation, requiredRole] =
		request;
	try
	{
		if (databaseOperation)
		{
			Respond(callback, store.RunDatabaseOperation(token, csrf, mutation, requiredRole, body, databaseOperation));
			return;
		}
		Respond(callback,
				operation ? store.RunProtected(token, csrf, mutation, requiredRole, body, operation)
						  : store.Handle(action, body, token, csrf, cursor, users));
	}
	catch (const RequestError& error)
	{
		Respond(callback, Error(error.status, error.what()));
	}
	catch (const DatabaseError& error)
	{
		if (databaseOperation)
		{
			Respond(callback,
					error.sqlState == "23P01" || error.sqlState == "23505"
						? Error(409, "Reservation conflict; refresh availability")
						: Error(503, "Reservations are temporarily unavailable"));
			return;
		}
		Respond(callback,
				error.sqlState == "23505" ? Error(409, "An account with that email already exists")
										  : Error(503, "Accounts are temporarily unavailable"));
	}
	catch (const std::exception&)
	{
		Respond(callback, Error(500, "Account operation failed"));
	}
}

void Controller::RespondWithAsset(const std::string& data, const std::string& type, const Callback& callback)
{
	auto response = drogon::HttpResponse::newHttpResponse();
	response->setContentTypeString(type);
	response->setBody(data);
	Headers(response);
	callback(response);
}
} // namespace Accounts
