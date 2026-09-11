#pragma once

#include "AccountRepository.h"
#include "Types.h"

namespace Accounts
{
// AccountStore authenticates and owns the transaction before calling these write workflows.
class AccountWriteService
{
public:
	AccountWriteService(AccountRepository& repository, const std::vector<std::string>& locales);
	Reply UpdateProfile(const Json::Value& body, const std::string& actor);

	Reply CreateUser(const Json::Value& body, const std::string& actor);
	Reply UpdateUser(const Json::Value& body,
					 const std::string& actor,
					 const std::function<void(const std::string&, bool)>& invalidateAccess);

private:
	std::string Locale(const Json::Value& body) const;

	AccountRepository& m_Repository;
	const std::vector<std::string>& m_Locales;
};
} // namespace Accounts
