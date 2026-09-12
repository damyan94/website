#pragma once

#include "Database.h"
#include <optional>

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

struct UserAccessState
{
	std::string role;
	bool enabled;
	std::string version;
};

struct SessionRecord
{
	std::string userId;
	std::string email;
	std::string displayName;
	std::string phone;
	std::string locale;
	std::string role;
	bool enabled;
	std::string version;
	std::string csrfToken;
};

struct LoginCredentials
{
	std::string userId;
	std::string passwordHash;
	bool enabled;
};

struct LoginUser
{
	std::string id;
	std::string email;
	std::string displayName;
	std::string phone;
	std::string locale;
	std::string role;
	bool enabled;
	std::string version;
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

	bool UpdateProfile(const std::string& user,
					   const std::string& displayName,
					   const std::string& phone,
					   const std::string& locale,
					   const std::string& version);
	std::string CreateUser(const std::string& email,
						   const std::string& passwordHash,
						   const std::string& displayName,
						   const std::string& phone,
						   const std::string& locale,
						   const std::string& role);
	std::optional<UserAccessState> LockUserAccess(const std::string& user);
	long long CountEnabledAdmins();
	void UpdateUserAccess(const std::string& user, const std::string& role, bool enabled);
	void Audit(const std::string& actor, const std::string& subject, const char* action);

	std::optional<SessionRecord> LockActiveSession(const std::string& tokenHash, int idleSeconds);
	void TouchSession(const std::string& tokenHash);

	void DeleteSession(const std::string& tokenHash);
	void DeleteUserSessions(const std::string& user);

	std::string PasswordHash(const std::string& user);
	void UpdatePassword(const std::string& user, const std::string& passwordHash);

	std::optional<std::string> ResetPasswordByEmail(const std::string& email, const std::string& passwordHash);

	std::optional<LoginCredentials> FindLoginCredentials(const std::string& email);
	std::optional<LoginUser> LockLoginUser(const std::string& user, const std::string& passwordHash);
	void DeleteExpiredSessions(int idleSeconds);
	void DeleteOlderSessionsForLogin(const std::string& user);
	void CreateSession(const std::string& tokenHash,
					   const std::string& user,
					   const std::string& csrf,
					   int absoluteSeconds);
	void UpdateLastLogin(const std::string& user);

private:
	Database& m_Database;
};
} // namespace Accounts
