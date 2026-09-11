#pragma once

#include "Database.h"
#include "Email.h"
#include <functional>
#include <json/json.h>

namespace Accounts
{
enum class Action
{
	Login,
	Logout,
	Me,
	UpdateMe,
	ChangePassword,
	ListUsers,
	CreateUser,
	UpdateUser,
	EmailOptions,
	RegisterEmail,
	CompleteRegistration,
	ForgotPassword,
	CompleteReset,
	VerifyEmail,
	ChangeEmail,
	ConfirmEmail,
	NewsletterPreference,
	Unsubscribe,
	ListCampaigns,
	PreviewCampaign,
	SendCampaign
};

class RequestError : public std::runtime_error
{
public:
	RequestError(int statusCode, const char* message)
		: std::runtime_error(message),
		  status(statusCode)
	{
	}

	int status;
};

struct UiSettings
{
	std::string siteName;
	std::string defaultLocale = "en";
	Json::Value supportText{Json::objectValue};
};

struct StoreSettings
{
	int						 idleSeconds	 = 3600;
	int						 absoluteSeconds = 43200;
	std::vector<std::string> locales		 = {"en"};
	std::string				 dummyPasswordHash;
	EmailSettings			 email;
	UiSettings				 ui;
};

struct Reply
{
	int			status = 200;
	Json::Value body{Json::objectValue};
	std::string sessionToken;
	bool		clearCookie = false;
};

struct Identity
{
	std::string id;
	std::string role;
};

struct UserListQuery
{
	bool		paginated = false;
	int			page	  = 1;
	std::string search;
	std::string role;
	std::string enabled;
	std::string sort  = "createdAt";
	std::string order = "desc";
};

using ProtectedOperation = std::function<Reply(const Identity&, const Json::Value&)>;
// SQL-backed modules run authorization and their operation in one transaction.
using DatabaseOperation = std::function<Reply(Database&, const Identity&, const Json::Value&)>;

} // namespace Accounts
