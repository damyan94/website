#pragma once

#include "AccountRepository.h"
#include "Types.h"

namespace Accounts
{
class SessionService;

struct OwnerPasswordReset
{
	std::string email;
	std::string passwordHash;
};

// AccountStore owns each transaction and authenticates password changes.
class PasswordService
{
public:
	PasswordService(AccountRepository& repository, SessionService& sessions);
	Reply ChangePassword(const Json::Value& body, const std::string& actor);

	static OwnerPasswordReset PrepareOwnerReset(const std::string& email, const std::string& password);
	void ResetByOwner(const OwnerPasswordReset& reset);

private:
	AccountRepository& m_Repository;
	SessionService& m_Sessions;
};
} // namespace Accounts
