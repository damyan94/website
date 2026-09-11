#pragma once

#include "Database.h"

namespace Accounts
{
struct UserListQuery;

// Read projections preserve identifiers, versions and UTC timestamp text from PostgreSQL.
struct UserSummary
{
	std::string id;
	std::string email;
	std::string displayName;
	std::string phone;
	std::string locale;
	std::string role;
	bool enabled;
	std::string version;
	std::string createdAt;
	std::string lastLoginAt;
	bool emailVerified;
};

struct ProfileEmailState
{
	bool verified;
	bool subscribed;
};

// Borrows the current worker's connection and participates in its caller's transaction.
class AccountRepository
{
public:
	explicit AccountRepository(Database& database);
	ProfileEmailState ProfileEmail(const std::string& user);
	std::vector<UserSummary> ListUsersAfter(const std::string& cursor);
	long long CountUsers(const UserListQuery& query);
	std::vector<UserSummary> ListUsersPage(const UserListQuery& query, long long offset);

private:
	Database& m_Database;
};
} // namespace Accounts
