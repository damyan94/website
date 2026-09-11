#include "AccountWriteService.h"
#include "Crypto.h"
#include "InputValidation.h"
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

std::string Identifier(const std::string& value)
{
	if (!InputValidation::IsDecimalIdentifier(value))
		throw RequestError(400, "Invalid ID or version");
	return value;
}

std::string Role(const Json::Value& body)
{
	const auto role = Field(body, "role", 1, 20);
	if (role != "admin" && role != "operator" && role != "customer")
		throw RequestError(400, "Invalid role");
	return role;
}

std::string Phone(const Json::Value& body)
{
	const auto phone = Field(body, "phone", 0, 32);
	if (!InputValidation::HasPhoneCharacters(phone))
		throw RequestError(400, "Invalid phone number");
	return phone;
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

AccountWriteService::AccountWriteService(AccountRepository& repository, const std::vector<std::string>& locales)
	: m_Repository(repository),
	  m_Locales(locales)
{
}

std::string AccountWriteService::Locale(const Json::Value& body) const
{
	const auto locale = Field(body, "locale", 1, 20);
	if (std::find(m_Locales.begin(), m_Locales.end(), locale) == m_Locales.end())
		throw RequestError(400, "Unsupported language");
	return locale;
}

Reply AccountWriteService::UpdateProfile(const Json::Value& body, const std::string& actor)
{
	Reply reply;
	RequireFields(body, {"displayName", "phone", "locale", "version"});
	const auto name = Field(body, "displayName", 1, 100);
	const auto phone = Phone(body);
	const auto locale = Locale(body);
	const auto version = Identifier(Field(body, "version", 1, 18));
	if (!m_Repository.UpdateProfile(actor, name, phone, locale, version))
		throw RequestError(409, "Profile changed elsewhere; reload before saving");
	m_Repository.Audit(actor, actor, "profile.updated");
	return reply;
}

Reply AccountWriteService::CreateUser(const Json::Value& body, const std::string& actor)
{
	Reply reply;
	RequireFields(body, {"email", "password", "displayName", "phone", "locale", "role"});
	const auto email   = NormalizedEmail(Field(body, "email", 3, 254));
	const auto name	   = Field(body, "displayName", 1, 100);
	const auto phone   = Phone(body);
	const auto locale  = Locale(body);
	const auto role	   = Role(body);
	const auto encoded = HashPassword(Field(body, "password", 15, 128));
	const auto id = m_Repository.CreateUser(email, encoded, name, phone, locale, role);
	m_Repository.Audit(actor, id, "user.created");
	reply.status	 = 201;
	reply.body["id"] = id;
	return reply;
}

Reply AccountWriteService::UpdateUser(
	const Json::Value& body,
	const std::string& actor,
	const std::function<void(const std::string&, bool)>& invalidateAccess)
{
	Reply reply;
	RequireFields(body, {"id", "role", "enabled", "version"});
	const auto id = Identifier(Field(body, "id", 1, 18));
	const auto role = Role(body);
	if (!body["enabled"].isBool())
		throw RequestError(400, "Enabled must be a boolean");
	const bool enabled = body["enabled"].asBool();
	if (id == actor && (!enabled || role != "admin"))
		throw RequestError(409, "You cannot disable or demote your own administrator account");
	const auto current = m_Repository.LockUserAccess(id);
	if (!current)
		throw RequestError(404, "Account not found");
	if (current->version != Identifier(Field(body, "version", 1, 18)))
		throw RequestError(409, "Account changed elsewhere; reload before saving");
	if (current->role == "admin" && current->enabled && (!enabled || role != "admin"))
	{
		if (m_Repository.CountEnabledAdmins() == 1)
			throw RequestError(409, "At least one active administrator is required");
	}
	m_Repository.UpdateUserAccess(id, role, enabled);
	// The compatibility boundary retains the existing session and email invalidation workflows.
	invalidateAccess(id, enabled);
	m_Repository.Audit(actor, id, "user.access_changed");
	if (id == actor)
		reply.clearCookie = true;
	return reply;
}

} // namespace Accounts
