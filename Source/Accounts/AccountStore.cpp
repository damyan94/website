#include "AccountStore.h"
#include "AccountQueryService.h"
#include "AccountWriteService.h"
#include "SessionService.h"
#include "PasswordService.h"
#include "LoginService.h"
#include "Crypto.h"
#include "stdafx.h"

#include <algorithm>
#include <initializer_list>

namespace Accounts
{
namespace
{
std::string Field(const Json::Value& body, const char* key, int minimum, int maximum)
{
	if (!body[key].isString())
		throw RequestError(400, "Invalid text field");
	auto	   value  = body[key].asString();
	const auto length = TextLength(value);
	if (length < minimum || length > maximum)
		throw RequestError(400, "Text field length or encoding is invalid");
	return value;
}

} // namespace

void AccountStore::RequireFields(const Json::Value& body, std::initializer_list<const char*> allowed)
{
	if (!body.isObject() || body.size() != allowed.size())
		throw RequestError(400, "Unexpected or missing fields");
	for (const auto* key : allowed)
		if (!body.isMember(key))
			throw RequestError(400, "Unexpected or missing fields");
}

std::string NormalizedEmail(const std::string& email)
{
	if (email.empty() || email.size() > 254)
		throw RequestError(400, "Invalid email address");
	std::string result = email;
	for (auto& c : result)
		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
	const auto at = result.find('@');
	if (at == 0 || at == std::string::npos || at > 64 || result.find('@', at + 1) != std::string::npos)
		throw RequestError(400, "Invalid email address");
	const auto local = result.substr(0, at);
	if (local.front() == '.' || local.back() == '.' || local.find("..") != std::string::npos)
		throw RequestError(400, "Invalid email address");
	for (char c : local)
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			  std::string_view("._+-%").find(c) != std::string_view::npos))
			throw RequestError(400, "Use an ASCII email address");
	const auto domain = result.substr(at + 1);
	if (domain.empty() || domain.find('.') == std::string::npos)
		throw RequestError(400, "Invalid email domain");
	std::size_t start = 0;
	while (start < domain.size())
	{
		const auto end	 = domain.find('.', start);
		const auto label = domain.substr(start, end == std::string::npos ? end : end - start);
		if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
			throw RequestError(400, "Invalid email domain");
		for (char c : label)
			if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
				throw RequestError(400, "Invalid email domain");
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	if (domain.back() == '.')
		throw RequestError(400, "Invalid email domain");
	return result;
}

AccountStore::AccountStore(const std::string& connection, StoreSettings settings)
	: m_Database(connection),
	  m_Settings(std::move(settings))
{
	m_Database.CheckSchema();
}

std::string AccountStore::Locale(const Json::Value& body) const
{
	const auto locale = Field(body, "locale", 1, 20);
	if (std::find(m_Settings.locales.begin(), m_Settings.locales.end(), locale) == m_Settings.locales.end())
		throw RequestError(400, "Unsupported language");
	return locale;
}

void AccountStore::Audit(const std::string& actor, const std::string& subject, const char* action)
{
	m_Database.Query(
		"INSERT INTO accounts.audit(actor_id, subject_id, action) VALUES(NULLIF($1,'')::bigint,$2::bigint,$3)",
		{actor, subject, action});
}

Json::Value AccountStore::Session(const std::string& token, const std::string& csrf, bool mutation)
{
	AccountRepository repository(m_Database);
	SessionService service(repository, m_Settings.idleSeconds);
	return service.Validate(token, csrf, mutation);
}

Reply AccountStore::Login(const Json::Value& body, const std::string& oldToken)
{
	AccountRepository repository(m_Database);
	LoginService service(repository, m_Settings.dummyPasswordHash, m_Settings.idleSeconds, m_Settings.absoluteSeconds);
	const auto login = service.VerifyCredentials(body);
	Transaction transaction(m_Database);
	m_Database.Query("SELECT pg_advisory_xact_lock(70123002)");
	auto reply = service.IssueSession(login, oldToken);
	transaction.Commit();
	return reply;
}

Reply AccountStore::ListUsers(const std::string& cursor, const UserListQuery& query)
{
	AccountRepository repository(m_Database);
	AccountQueryService service(repository, m_Settings.locales);
	return service.ListUsers(cursor, query);
}

Reply AccountStore::Handle(Action				action,
						   const Json::Value&	body,
						   const std::string&	token,
						   const std::string&	csrf,
						   const std::string&	cursor,
						   const UserListQuery& users)
{
	// Recover before a new request only; never automatically replay a write whose
	// commit may already have succeeded before a connection failure.
	m_Database.ReconnectIfNeeded();
	if (action == Action::Login)
		return Login(body, token);
	if (action == Action::EmailOptions || action == Action::RegisterEmail || action == Action::CompleteRegistration ||
		action == Action::ForgotPassword || action == Action::CompleteReset || action == Action::ConfirmEmail ||
		action == Action::Unsubscribe)
		return PublicEmail(action, body);
	// Account mutations are uncommon. A database transaction lock gives consistent
	// role checks, session revocation and last-admin protection across processes.
	Transaction transaction(m_Database);
	m_Database.Query("SELECT pg_advisory_xact_lock(70123002)");
	const bool mutation = action != Action::Me && action != Action::ListUsers && action != Action::ListCampaigns;
	auto	   user		= Session(token, csrf, mutation);
	const auto actor	= user["id"].asString();
	Reply	   reply;
	if (action == Action::VerifyEmail || action == Action::ChangeEmail || action == Action::NewsletterPreference ||
		action == Action::ListCampaigns || action == Action::PreviewCampaign || action == Action::SendCampaign)
	{
		reply = AccountEmail(action, body, user);
		transaction.Commit();
		return reply;
	}
	if (action == Action::Me)
		reply = CurrentProfile(std::move(user));
	else if (action == Action::Logout)
	{
		RequireFields(body, {});
		RevokeSession(token);
		reply.clearCookie = true;
	}
	else if (action == Action::UpdateMe)
		reply = UpdateProfile(body, actor);
	else if (action == Action::ChangePassword)
		reply = ChangePassword(body, actor);
	else
	{
		if (user["role"].asString() != "admin")
			throw RequestError(403, "Administrator access required");
		if (action == Action::ListUsers)
			reply = ListUsers(cursor, users);
		else if (action == Action::CreateUser)
			reply = CreateUser(body, actor);
		else if (action == Action::UpdateUser)
			reply = UpdateUser(body, actor);
	}
	transaction.Commit();
	return reply;
}

Reply AccountStore::CurrentProfile(Json::Value user)
{
	AccountRepository repository(m_Database);
	AccountQueryService service(repository, m_Settings.locales);
	return service.CurrentProfile(std::move(user));
}

Reply AccountStore::UpdateProfile(const Json::Value& body, const std::string& actor)
{
	AccountRepository repository(m_Database);
	AccountWriteService service(repository, m_Settings.locales);
	return service.UpdateProfile(body, actor);
}

Reply AccountStore::ChangePassword(const Json::Value& body, const std::string& actor)
{
	AccountRepository repository(m_Database);
	SessionService sessions(repository, m_Settings.idleSeconds);
	PasswordService service(repository, sessions);
	return service.ChangePassword(body, actor);
}

Reply AccountStore::CreateUser(const Json::Value& body, const std::string& actor)
{
	AccountRepository repository(m_Database);
	AccountWriteService service(repository, m_Settings.locales);
	return service.CreateUser(body, actor);
}

Reply AccountStore::UpdateUser(const Json::Value& body, const std::string& actor)
{
	AccountRepository repository(m_Database);
	AccountWriteService service(repository, m_Settings.locales);
	return service.UpdateUser(body, actor,
							  [this](const std::string& id, bool enabled)
							  {
								  RevokeSessions(id);
								  if (!enabled)
									  UnsubscribeUser(id, "account-disabled-v1");
							  });
}

Reply AccountStore::RunProtected(const std::string&		   token,
								 const std::string&		   csrf,
								 bool					   mutation,
								 const std::string&		   requiredRole,
								 const Json::Value&		   body,
								 const ProtectedOperation& operation)
{
	m_Database.ReconnectIfNeeded();
	Identity identity;
	{
		Transaction transaction(m_Database);
		m_Database.Query("SELECT pg_advisory_xact_lock(70123002)");
		const auto user = Session(token, csrf, mutation);
		identity		= {user["id"].asString(), user["role"].asString()};
		if (identity.role != requiredRole)
			throw RequestError(403, "Insufficient permissions");
		transaction.Commit();
	}
	// Authorization is complete before entering another module. Its filesystem
	// changes are not part of the accounts SQL transaction. Revocation prevents
	// subsequent requests; it does not cancel an already authorized operation.
	return operation(identity, body);
}

Reply AccountStore::RunDatabaseOperation(const std::string&		  token,
										 const std::string&		  csrf,
										 bool					  mutation,
										 const std::string&		  requiredRole,
										 const Json::Value&		  body,
										 const DatabaseOperation& operation)
{
	m_Database.ReconnectIfNeeded();
	Transaction transaction(m_Database);
	m_Database.Query("SELECT pg_advisory_xact_lock(70123002)");
	const auto user = Session(token, csrf, mutation);
	Identity   identity{user["id"].asString(), user["role"].asString()};
	if ((!requiredRole.empty() && requiredRole != "staff" && identity.role != requiredRole) ||
		(requiredRole == "staff" && identity.role != "admin" && identity.role != "operator"))
		throw RequestError(403, "Insufficient permissions");
	auto reply = operation(m_Database, identity, body);
	transaction.Commit();
	return reply;
}

void AccountStore::RevokeSession(const std::string& token)
{
	AccountRepository repository(m_Database);
	SessionService service(repository, m_Settings.idleSeconds);
	service.Revoke(token);
}

void AccountStore::RevokeSessions(const std::string& user)
{
	AccountRepository repository(m_Database);
	SessionService service(repository, m_Settings.idleSeconds);
	service.RevokeAll(user);
}

void AccountStore::Bootstrap(const std::string& email, const std::string& password)
{
	const auto normalized = NormalizedEmail(email);
	if (!ValidPassword(password))
		throw RequestError(400, "Password must contain 15 to 128 characters without control characters");
	const auto	encoded = HashPassword(password);
	Transaction transaction(m_Database);
	m_Database.Query("SELECT pg_advisory_xact_lock(70123002)");
	const auto existing = m_Database.Query("SELECT id FROM accounts.users WHERE role='admin' LIMIT 1");
	if (existing.Count())
		throw RequestError(409, "An administrator already exists; use the admin panel to create additional accounts");
	const auto rows = m_Database.Query("INSERT INTO accounts.users(email,password_hash,display_name,locale,role) "
									   "VALUES($1,$2,'Administrator',$3,'admin') RETURNING id",
									   {normalized, encoded, m_Settings.locales.front()});
	Audit("", rows.Get(0, 0), "admin.bootstrapped");
	transaction.Commit();
}

void AccountStore::ResetPassword(const std::string& email, const std::string& password)
{
	// Keep validation and hashing outside the owner-reset transaction.
	const auto reset = PasswordService::PrepareOwnerReset(email, password);
	Transaction transaction(m_Database);
	m_Database.Query("SELECT pg_advisory_xact_lock(70123002)");
	AccountRepository repository(m_Database);
	SessionService sessions(repository, m_Settings.idleSeconds);
	PasswordService service(repository, sessions);
	service.ResetByOwner(reset);
	transaction.Commit();
}
} // namespace Accounts
