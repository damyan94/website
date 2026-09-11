#include "AccountQueryService.h"
#include "Crypto.h"
#include "InputValidation.h"
#include "stdafx.h"

#include <algorithm>

namespace Accounts
{
AccountQueryService::AccountQueryService(AccountRepository& repository, const std::vector<std::string>& locales)
	: m_Repository(repository),
	  m_Locales(locales)
{
}

Reply AccountQueryService::CurrentProfile(Json::Value user)
{
	Reply reply;
	const auto actor = user["id"].asString();
	reply.body["csrfToken"] = user["csrfToken"];
	user.removeMember("csrfToken");
	const auto email = m_Repository.ProfileEmail(actor);
	user["emailVerified"] = email.verified;
	user["newsletterSubscribed"] = email.subscribed;
	reply.body["user"] = user;
	reply.body["locales"] = Json::arrayValue;
	for (const auto& locale : m_Locales)
		reply.body["locales"].append(locale);
	return reply;
}

Reply AccountQueryService::ListUsers(const std::string& cursor, const UserListQuery& query)
{
	Reply reply;
	std::vector<UserSummary> users;
	int limit = 50;
	if (!query.paginated)
	{
		if (!cursor.empty() && !InputValidation::IsDecimalIdentifier(cursor))
			throw RequestError(400, "Invalid ID or version");
		users = m_Repository.ListUsersAfter(cursor.empty() ? "0" : cursor);
	}
	else
	{
		if (!cursor.empty() || query.page < 1 || query.page > 1000000 || TextLength(query.search) < 0 ||
			TextLength(query.search) > 100 ||
			(!query.role.empty() && query.role != "admin" && query.role != "operator" && query.role != "customer") ||
			(!query.enabled.empty() && query.enabled != "true" && query.enabled != "false") ||
			(query.order != "asc" && query.order != "desc"))
			throw RequestError(400, "Invalid user filters or pagination");
		if (query.sort != "createdAt" && query.sort != "lastLoginAt" && query.sort != "email")
			throw RequestError(400, "Invalid user sort");
		const auto total = m_Repository.CountUsers(query);
		limit = 25;
		const auto pages = std::max(1LL, (total + limit - 1) / limit);
		const auto page = std::min(static_cast<long long>(query.page), pages);
		reply.body["page"] = Json::Int64(page);
		reply.body["pageSize"] = limit;
		reply.body["pages"] = Json::Int64(pages);
		reply.body["total"] = Json::Int64(total);
		users = m_Repository.ListUsersPage(query, (page - 1) * limit);
	}
	reply.body["users"] = Json::arrayValue;
	const auto count = static_cast<int>(users.size());
	for (int i = 0; i < std::min(count, limit); ++i)
	{
		const auto& row = users[i];
		Json::Value user(Json::objectValue);
		user["id"] = row.id;
		user["email"] = row.email;
		user["displayName"] = row.displayName;
		user["phone"] = row.phone;
		user["locale"] = row.locale;
		user["role"] = row.role;
		user["enabled"] = row.enabled;
		user["version"] = row.version;
		user["createdAt"] = row.createdAt;
		user["lastLoginAt"] = row.lastLoginAt.empty() ? Json::Value() : Json::Value(row.lastLoginAt);
		user["emailVerified"] = row.emailVerified;
		reply.body["users"].append(std::move(user));
	}
	if (!query.paginated)
		reply.body["nextCursor"] = count > limit ? users[limit - 1].id : "";
	return reply;
}
} // namespace Accounts
