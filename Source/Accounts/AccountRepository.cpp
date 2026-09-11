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
} // namespace Accounts
