#pragma once

#include "AccountRepository.h"
#include "Types.h"

namespace Accounts
{
// AccountStore authenticates and owns the transaction before calling these read workflows.
class AccountQueryService
{
public:
	AccountQueryService(AccountRepository& repository, const std::vector<std::string>& locales);
	Reply CurrentProfile(Json::Value user);
	Reply ListUsers(const std::string& cursor, const UserListQuery& query);

private:
	AccountRepository& m_Repository;
	const std::vector<std::string>& m_Locales;
};
} // namespace Accounts
