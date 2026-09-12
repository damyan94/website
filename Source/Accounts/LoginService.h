#pragma once

#include "AccountRepository.h"
#include "Types.h"

namespace Accounts
{
struct VerifiedLogin
{
	std::string userId;
	std::string passwordHash;
};

// AccountStore verifies before its transaction, then issues the session under the login lock.
class LoginService
{
public:
	LoginService(AccountRepository& repository,
				 const std::string& dummyPasswordHash,
				 int idleSeconds,
				 int absoluteSeconds);
	VerifiedLogin VerifyCredentials(const Json::Value& body);
	Reply IssueSession(const VerifiedLogin& login, const std::string& oldToken);

private:
	AccountRepository& m_Repository;
	const std::string& m_DummyPasswordHash;
	int m_IdleSeconds;
	int m_AbsoluteSeconds;
};
} // namespace Accounts
