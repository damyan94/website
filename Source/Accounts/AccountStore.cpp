#include "AccountStore.h"
#include "AccountQueryService.h"
#include "AccountWriteService.h"
#include "SessionService.h"
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

std::string Password(const Json::Value& body, const char* field)
{
	return Field(body, field, 15, 128);
}

Json::Value User(const Rows& rows, int row)
{
	Json::Value user(Json::objectValue);
	const char* keys[] = {"id", "email", "displayName", "phone", "locale", "role"};
	for (int i = 0; i < 6; ++i)
		user[keys[i]] = rows.Get(row, i);
	user["enabled"] = rows.Get(row, 6) == "t";
	user["version"] = rows.Get(row, 7);
	return user;
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
	RequireFields(body, {"email", "password"});
	const auto email	= NormalizedEmail(Field(body, "email", 3, 254));
	const auto password = Field(body, "password", 1, 128);
	const auto credentials =
		m_Database.Query("SELECT id,password_hash,enabled FROM accounts.users WHERE email=$1", {email});
	const auto encoded =
		credentials.Count() && !credentials.Get(0, 1).empty() ? credentials.Get(0, 1) : m_Settings.dummyPasswordHash;
	const bool correct = VerifyPassword(password, encoded);
	if (!correct || !credentials.Count() || credentials.Get(0, 2) != "t")
		throw RequestError(401, "Email or password is incorrect");

	Transaction transaction(m_Database);
	m_Database.Query("SELECT pg_advisory_xact_lock(70123002)");
	const auto rows =
		m_Database.Query("SELECT id,email,display_name,phone,locale,role,enabled,version FROM accounts.users "
						 "WHERE id=$1::bigint AND password_hash=$2 AND enabled FOR UPDATE",
						 {credentials.Get(0, 0), encoded});
	if (!rows.Count())
		throw RequestError(401, "Email or password is incorrect");
	m_Database.Query(
		"DELETE FROM accounts.sessions WHERE expires_at<=now() OR last_seen<=now()-make_interval(secs=>$1::int)",
		{std::to_string(m_Settings.idleSeconds)});
	if (!oldToken.empty())
		m_Database.Query("DELETE FROM accounts.sessions WHERE token_hash=$1", {TokenHash(oldToken)});
	// Keep at most five devices per account, including the new session.
	m_Database.Query("DELETE FROM accounts.sessions WHERE token_hash IN (SELECT token_hash FROM accounts.sessions "
					 "WHERE user_id=$1::bigint ORDER BY created_at DESC OFFSET 4)",
					 {rows.Get(0, 0)});
	Reply reply;
	reply.sessionToken = RandomToken();
	const auto csrf	   = RandomToken();
	m_Database.Query("INSERT INTO accounts.sessions(token_hash,user_id,csrf_token,expires_at) "
					 "VALUES($1,$2::bigint,$3,now()+make_interval(secs=>$4::int))",
					 {TokenHash(reply.sessionToken), rows.Get(0, 0), csrf, std::to_string(m_Settings.absoluteSeconds)});
	Audit(rows.Get(0, 0), rows.Get(0, 0), "login");
	// Activity does not change the version used for optimistic profile edits.
	m_Database.Query("UPDATE accounts.users SET last_login_at=now() WHERE id=$1::bigint", {rows.Get(0, 0)});
	reply.body["user"]		= User(rows, 0);
	reply.body["csrfToken"] = csrf;
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
	Reply reply;
	RequireFields(body, {"currentPassword", "newPassword"});
	const auto old = m_Database.Query("SELECT password_hash FROM accounts.users WHERE id=$1::bigint", {actor});
	if (!VerifyPassword(Field(body, "currentPassword", 1, 128), old.Get(0, 0)))
		throw RequestError(403, "Current password is incorrect");
	const auto encoded = HashPassword(Password(body, "newPassword"));
	m_Database.Query("UPDATE accounts.users SET "
					 "password_hash=$1,credential_version=credential_version+1,version=version+1,updated_at=now() "
					 "WHERE id=$2::bigint",
					 {encoded, actor});
	RevokeSessions(actor);
	Audit(actor, actor, "password.changed");
	reply.clearCookie = true;
	return reply;
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
	const auto normalized = NormalizedEmail(email);
	if (!ValidPassword(password))
		throw RequestError(400, "Password must contain 15 to 128 characters without control characters");
	const auto	encoded = HashPassword(password);
	Transaction transaction(m_Database);
	m_Database.Query("SELECT pg_advisory_xact_lock(70123002)");
	const auto rows = m_Database.Query("UPDATE accounts.users SET "
									   "password_hash=$1,credential_version=credential_version+1,version=version+1,"
									   "updated_at=now() WHERE email=$2 RETURNING id",
									   {encoded, normalized});
	if (!rows.Count())
		throw RequestError(404, "Account not found");
	RevokeSessions(rows.Get(0, 0));
	Audit("", rows.Get(0, 0), "password.reset_by_owner");
	transaction.Commit();
}
} // namespace Accounts
