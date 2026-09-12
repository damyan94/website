#include "AccountRepository.h"
#include "Types.h"
#include "stdafx.h"

namespace Accounts
{
namespace
{
constexpr const char* Columns = "SELECT id,email,display_name,phone,locale,role,enabled,version,"
									   "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'),"
									   "to_char(last_login_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'),"
									   "email_verified_at IS NOT NULL FROM accounts.users ";
constexpr const char* Filters = "WHERE ($1='' OR strpos(lower(email),lower($1))>0 OR strpos(lower(display_name),lower($1))>0) "
			"AND ($2='' OR role=$2) AND ($3='' OR enabled=($3='true')) ";

std::vector<UserSummary> ReadUsers(const Rows& rows)
{
	std::vector<UserSummary> users;
	users.reserve(rows.Count());
	for (int i = 0; i < rows.Count(); ++i)
		users.push_back({rows.Get(i, 0),
						rows.Get(i, 1),
						rows.Get(i, 2),
						rows.Get(i, 3),
						rows.Get(i, 4),
						rows.Get(i, 5),
						rows.Get(i, 6) == "t",
						rows.Get(i, 7),
						rows.Get(i, 8),
						rows.Get(i, 9),
						rows.Get(i, 10) == "t"});
	return users;
}
} // namespace

AccountRepository::AccountRepository(Database& database)
	: m_Database(database)
{
}

ProfileEmailState AccountRepository::ProfileEmail(const std::string& user)
{
	const auto email =
		m_Database.Query("SELECT email_verified_at IS NOT NULL, EXISTS(SELECT 1 FROM accounts.subscriptions "
						 "WHERE user_id=$1::bigint AND confirmed_at IS NOT NULL AND email=accounts.users.email) "
						 "FROM accounts.users WHERE id=$1::bigint",
						 {user});
	return {email.Get(0, 0) == "t", email.Get(0, 1) == "t"};
}

std::vector<UserSummary> AccountRepository::ListUsersAfter(const std::string& cursor)
{
	const auto sql = std::string(Columns) + "WHERE id>$1::bigint ORDER BY id LIMIT 51";
	return ReadUsers(m_Database.Query(sql.c_str(), {cursor}));
}

long long AccountRepository::CountUsers(const UserListQuery& query)
{
	const auto sql = std::string("SELECT count(*) FROM accounts.users ") + Filters;
	const auto count = m_Database.Query(sql.c_str(), {query.search, query.role, query.enabled});
	return std::stoll(count.Get(0, 0));
}

std::vector<UserSummary> AccountRepository::ListUsersPage(const UserListQuery& query, long long offset)
{
	// Only these fixed column names and directions ever become SQL syntax.
	const char* sort = query.sort == "createdAt" ? "created_at"
					 : query.sort == "lastLoginAt" ? "last_login_at"
					 : query.sort == "email" ? "email"
					 : nullptr;
	if (!sort)
		throw std::invalid_argument("Invalid user sort");
	const std::string direction = query.order == "asc" ? " ASC" : " DESC";
	const auto sql = std::string(Columns) + Filters + "ORDER BY " + sort + direction + " NULLS LAST, id" +
					 direction + " LIMIT 25 OFFSET $4::bigint";
	return ReadUsers(m_Database.Query(
		sql.c_str(), {query.search, query.role, query.enabled, std::to_string(offset)}));
}

bool AccountRepository::UpdateProfile(const std::string& user,
									  const std::string& displayName,
									  const std::string& phone,
									  const std::string& locale,
									  const std::string& version)
{
	const auto rows = m_Database.Query(
		"UPDATE accounts.users SET display_name=$1,phone=$2,locale=$3,version=version+1,updated_at=now() "
		"WHERE id=$4::bigint AND version=$5::bigint RETURNING id",
		{displayName, phone, locale, user, version});
	return rows.Count() != 0;
}

std::string AccountRepository::CreateUser(const std::string& email,
										  const std::string& passwordHash,
										  const std::string& displayName,
										  const std::string& phone,
										  const std::string& locale,
										  const std::string& role)
{
	const auto rows = m_Database.Query("INSERT INTO accounts.users(email,password_hash,display_name,phone,locale,role) "
									   "VALUES($1,$2,$3,$4,$5,$6) RETURNING id",
									   {email, passwordHash, displayName, phone, locale, role});
	return rows.Get(0, 0);
}

std::optional<UserAccessState> AccountRepository::LockUserAccess(const std::string& user)
{
	const auto current =
		m_Database.Query("SELECT role,enabled,version FROM accounts.users WHERE id=$1::bigint FOR UPDATE", {user});
	if (!current.Count())
		return std::nullopt;
	return UserAccessState{current.Get(0, 0), current.Get(0, 1) == "t", current.Get(0, 2)};
}

long long AccountRepository::CountEnabledAdmins()
{
	const auto admins = m_Database.Query("SELECT count(*) FROM accounts.users WHERE role='admin' AND enabled");
	return std::stoll(admins.Get(0, 0));
}

void AccountRepository::UpdateUserAccess(const std::string& user, const std::string& role, bool enabled)
{
	m_Database.Query("UPDATE accounts.users SET "
					 "role=$1,enabled=$2::boolean,credential_version=credential_version+1,version=version+1,"
					 "updated_at=now() "
					 "WHERE id=$3::bigint",
					 {role, enabled ? "true" : "false", user});
}

void AccountRepository::Audit(const std::string& actor, const std::string& subject, const char* action)
{
	m_Database.Query(
		"INSERT INTO accounts.audit(actor_id, subject_id, action) VALUES(NULLIF($1,'')::bigint,$2::bigint,$3)",
		{actor, subject, action});
}

std::optional<SessionRecord> AccountRepository::LockActiveSession(const std::string& tokenHash, int idleSeconds)
{
	const auto rows =
		m_Database.Query("SELECT u.id,u.email,u.display_name,u.phone,u.locale,u.role,u.enabled,u.version,s.csrf_token "
						 "FROM accounts.users u JOIN accounts.sessions s ON s.user_id=u.id "
						 "WHERE s.token_hash=$1 AND u.enabled AND s.expires_at>now() "
						 "AND s.last_seen>now()-make_interval(secs=>$2::int) FOR UPDATE OF u,s",
						 {tokenHash, std::to_string(idleSeconds)});
	if (!rows.Count())
		return std::nullopt;
	return SessionRecord{rows.Get(0, 0),
						 rows.Get(0, 1),
						 rows.Get(0, 2),
						 rows.Get(0, 3),
						 rows.Get(0, 4),
						 rows.Get(0, 5),
						 rows.Get(0, 6) == "t",
						 rows.Get(0, 7),
						 rows.Get(0, 8)};
}

void AccountRepository::TouchSession(const std::string& tokenHash)
{
	m_Database.Query("UPDATE accounts.sessions SET last_seen=now() WHERE token_hash=$1", {tokenHash});
}

void AccountRepository::DeleteSession(const std::string& tokenHash)
{
	m_Database.Query("DELETE FROM accounts.sessions WHERE token_hash=$1", {tokenHash});
}

void AccountRepository::DeleteUserSessions(const std::string& user)
{
	m_Database.Query("DELETE FROM accounts.sessions WHERE user_id=$1::bigint", {user});
}

std::string AccountRepository::PasswordHash(const std::string& user)
{
	const auto rows = m_Database.Query("SELECT password_hash FROM accounts.users WHERE id=$1::bigint", {user});
	return rows.Get(0, 0);
}

void AccountRepository::UpdatePassword(const std::string& user, const std::string& passwordHash)
{
	m_Database.Query("UPDATE accounts.users SET "
					 "password_hash=$1,credential_version=credential_version+1,version=version+1,updated_at=now() "
					 "WHERE id=$2::bigint",
					 {passwordHash, user});
}

std::optional<std::string> AccountRepository::ResetPasswordByEmail(const std::string& email,
																 const std::string& passwordHash)
{
	const auto rows = m_Database.Query("UPDATE accounts.users SET "
									   "password_hash=$1,credential_version=credential_version+1,version=version+1,"
									   "updated_at=now() WHERE email=$2 RETURNING id",
									   {passwordHash, email});
	if (!rows.Count())
		return std::nullopt;
	return rows.Get(0, 0);
}

std::optional<LoginCredentials> AccountRepository::FindLoginCredentials(const std::string& email)
{
	const auto rows = m_Database.Query("SELECT id,password_hash,enabled FROM accounts.users WHERE email=$1", {email});
	if (!rows.Count())
		return std::nullopt;
	return LoginCredentials{rows.Get(0, 0), rows.Get(0, 1), rows.Get(0, 2) == "t"};
}

std::optional<LoginUser> AccountRepository::LockLoginUser(const std::string& user, const std::string& passwordHash)
{
	const auto rows =
		m_Database.Query("SELECT id,email,display_name,phone,locale,role,enabled,version FROM accounts.users "
						 "WHERE id=$1::bigint AND password_hash=$2 AND enabled FOR UPDATE",
						 {user, passwordHash});
	if (!rows.Count())
		return std::nullopt;
	return LoginUser{rows.Get(0, 0),
					 rows.Get(0, 1),
					 rows.Get(0, 2),
					 rows.Get(0, 3),
					 rows.Get(0, 4),
					 rows.Get(0, 5),
					 rows.Get(0, 6) == "t",
					 rows.Get(0, 7)};
}

void AccountRepository::DeleteExpiredSessions(int idleSeconds)
{
	m_Database.Query(
		"DELETE FROM accounts.sessions WHERE expires_at<=now() OR last_seen<=now()-make_interval(secs=>$1::int)",
		{std::to_string(idleSeconds)});
}

void AccountRepository::DeleteOlderSessionsForLogin(const std::string& user)
{
	m_Database.Query("DELETE FROM accounts.sessions WHERE token_hash IN (SELECT token_hash FROM accounts.sessions "
					 "WHERE user_id=$1::bigint ORDER BY created_at DESC OFFSET 4)",
					 {user});
}

void AccountRepository::CreateSession(const std::string& tokenHash,
									  const std::string& user,
									  const std::string& csrf,
									  int absoluteSeconds)
{
	m_Database.Query("INSERT INTO accounts.sessions(token_hash,user_id,csrf_token,expires_at) "
					 "VALUES($1,$2::bigint,$3,now()+make_interval(secs=>$4::int))",
					 {tokenHash, user, csrf, std::to_string(absoluteSeconds)});
}

void AccountRepository::UpdateLastLogin(const std::string& user)
{
	m_Database.Query("UPDATE accounts.users SET last_login_at=now() WHERE id=$1::bigint", {user});
}
} // namespace Accounts
