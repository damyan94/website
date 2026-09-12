#include "LoginService.h"
#include "Crypto.h"
#include "stdafx.h"

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

void RequireFields(const Json::Value& body, std::initializer_list<const char*> allowed)
{
	if (!body.isObject() || body.size() != allowed.size())
		throw RequestError(400, "Unexpected or missing fields");
	for (const auto* key : allowed)
		if (!body.isMember(key))
			throw RequestError(400, "Unexpected or missing fields");
}

Json::Value User(const LoginUser& account)
{
	Json::Value user(Json::objectValue);
	user["id"] = account.id;
	user["email"] = account.email;
	user["displayName"] = account.displayName;
	user["phone"] = account.phone;
	user["locale"] = account.locale;
	user["role"] = account.role;
	user["enabled"] = account.enabled;
	user["version"] = account.version;
	return user;
}
} // namespace

LoginService::LoginService(AccountRepository& repository,
						   const std::string& dummyPasswordHash,
						   int idleSeconds,
						   int absoluteSeconds)
	: m_Repository(repository),
	  m_DummyPasswordHash(dummyPasswordHash),
	  m_IdleSeconds(idleSeconds),
	  m_AbsoluteSeconds(absoluteSeconds)
{
}

VerifiedLogin LoginService::VerifyCredentials(const Json::Value& body)
{
	RequireFields(body, {"email", "password"});
	const auto email	= NormalizedEmail(Field(body, "email", 3, 254));
	const auto password = Field(body, "password", 1, 128);
	const auto credentials = m_Repository.FindLoginCredentials(email);
	const auto encoded =
		credentials && !credentials->passwordHash.empty() ? credentials->passwordHash : m_DummyPasswordHash;
	const bool correct = VerifyPassword(password, encoded);
	if (!correct || !credentials || !credentials->enabled)
		throw RequestError(401, "Email or password is incorrect");
	return {credentials->userId, encoded};
}

Reply LoginService::IssueSession(const VerifiedLogin& login, const std::string& oldToken)
{
	const auto user = m_Repository.LockLoginUser(login.userId, login.passwordHash);
	if (!user)
		throw RequestError(401, "Email or password is incorrect");
	m_Repository.DeleteExpiredSessions(m_IdleSeconds);
	if (!oldToken.empty())
		m_Repository.DeleteSession(TokenHash(oldToken));
	// Keep at most five devices per account, including the new session.
	m_Repository.DeleteOlderSessionsForLogin(user->id);
	Reply reply;
	reply.sessionToken = RandomToken();
	const auto csrf	   = RandomToken();
	m_Repository.CreateSession(TokenHash(reply.sessionToken), user->id, csrf, m_AbsoluteSeconds);
	m_Repository.Audit(user->id, user->id, "login");
	// Activity does not change the version used for optimistic profile edits.
	m_Repository.UpdateLastLogin(user->id);
	reply.body["user"]		= User(*user);
	reply.body["csrfToken"] = csrf;
	return reply;
}
} // namespace Accounts
