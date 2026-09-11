#include "PasswordService.h"
#include "Crypto.h"
#include "SessionService.h"
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

} // namespace

PasswordService::PasswordService(AccountRepository& repository, SessionService& sessions)
	: m_Repository(repository),
	  m_Sessions(sessions)
{
}

Reply PasswordService::ChangePassword(const Json::Value& body, const std::string& actor)
{
	Reply reply;
	RequireFields(body, {"currentPassword", "newPassword"});
	const auto old = m_Repository.PasswordHash(actor);
	if (!VerifyPassword(Field(body, "currentPassword", 1, 128), old))
		throw RequestError(403, "Current password is incorrect");
	const auto encoded = HashPassword(Field(body, "newPassword", 15, 128));
	m_Repository.UpdatePassword(actor, encoded);
	m_Sessions.RevokeAll(actor);
	m_Repository.Audit(actor, actor, "password.changed");
	reply.clearCookie = true;
	return reply;
}

OwnerPasswordReset PasswordService::PrepareOwnerReset(const std::string& email, const std::string& password)
{
	const auto normalized = NormalizedEmail(email);
	if (!ValidPassword(password))
		throw RequestError(400, "Password must contain 15 to 128 characters without control characters");
	const auto encoded = HashPassword(password);
	return {normalized, encoded};
}

void PasswordService::ResetByOwner(const OwnerPasswordReset& reset)
{
	const auto user = m_Repository.ResetPasswordByEmail(reset.email, reset.passwordHash);
	if (!user)
		throw RequestError(404, "Account not found");
	m_Sessions.RevokeAll(*user);
	m_Repository.Audit("", *user, "password.reset_by_owner");
}
} // namespace Accounts
