#pragma once

#include "AccountRepository.h"
#include "Types.h"

namespace Accounts
{
// AccountStore owns the transaction and calls this service on the connection's worker.
class SessionService
{
public:
	SessionService(AccountRepository& repository, int idleSeconds);
	Json::Value Validate(const std::string& token, const std::string& csrf, bool mutation);

	void Revoke(const std::string& token);
	void RevokeAll(const std::string& user);

private:
	AccountRepository& m_Repository;
	int m_IdleSeconds;
};
} // namespace Accounts
