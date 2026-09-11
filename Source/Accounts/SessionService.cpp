#include "SessionService.h"
#include "Crypto.h"
#include "stdafx.h"

namespace Accounts
{
SessionService::SessionService(AccountRepository& repository, int idleSeconds)
	: m_Repository(repository),
	  m_IdleSeconds(idleSeconds)
{
}

Json::Value SessionService::Validate(const std::string& token, const std::string& csrf, bool mutation)
{
	if (token.size() != 64)
		throw RequestError(401, "Please sign in");
	const auto hash = TokenHash(token);
	const auto session = m_Repository.LockActiveSession(hash, m_IdleSeconds);
	if (!session)
		throw RequestError(401, "Please sign in");
	if (mutation && (csrf.size() != 64 || !ConstantEqual(csrf, session->csrfToken)))
		throw RequestError(403, "Invalid request token");
	m_Repository.TouchSession(hash);
	Json::Value user(Json::objectValue);
	user["id"] = session->userId;
	user["email"] = session->email;
	user["displayName"] = session->displayName;
	user["phone"] = session->phone;
	user["locale"] = session->locale;
	user["role"] = session->role;
	user["enabled"] = session->enabled;
	user["version"] = session->version;
	user["csrfToken"] = session->csrfToken;
	return user;
}

void SessionService::Revoke(const std::string& token)
{
	m_Repository.DeleteSession(TokenHash(token));
}

void SessionService::RevokeAll(const std::string& user)
{
	m_Repository.DeleteUserSessions(user);
}
} // namespace Accounts
