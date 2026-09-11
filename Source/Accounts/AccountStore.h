#pragma once

#include "Database.h"
#include "Email.h"
#include <functional>
#include <initializer_list>
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

class AccountStore
{
public:
	AccountStore(const std::string& connection, StoreSettings settings);
	Reply Handle(Action				  action,
				 const Json::Value&	  body,
				 const std::string&	  token,
				 const std::string&	  csrf,
				 const std::string&	  cursor,
				 const UserListQuery& users = {});
	void  Bootstrap(const std::string& email, const std::string& password);
	void  ResetPassword(const std::string& email, const std::string& password);
	// Only the signature-verified provider handler may call this entry point.
	Reply ReceiveEmailEvent(const std::string& eventId, const Json::Value& body);
	Reply RunDatabaseOperation(const std::string&		token,
							   const std::string&		csrf,
							   bool						mutation,
							   const std::string&		requiredRole,
							   const Json::Value&		body,
							   const DatabaseOperation& operation);
	Reply RunProtected(const std::string&		 token,
					   const std::string&		 csrf,
					   bool						 mutation,
					   const std::string&		 requiredRole,
					   const Json::Value&		 body,
					   const ProtectedOperation& operation);

private:
	struct EmailChallenge
	{
		std::string purpose;
		std::string user;
		std::string email;
		std::string locale;
	};

	Reply EmailOptions() const;
	Reply RequestEmailChallenge(Action action, const Json::Value& body);
	Reply UnsubscribeLink(const Json::Value& body);
	Reply CompleteEmailChallenge(Action action, const Json::Value& body);
	Reply RegisterCustomer(const EmailChallenge& challenge, const Json::Value& body, const std::string& password);
	Reply RecoverPassword(const EmailChallenge& challenge, const std::string& password);
	Reply ConfirmEmailChange(const EmailChallenge& challenge);
	Reply ConfirmEmailVerification(const EmailChallenge& challenge);
	Reply ConfirmSubscription(const EmailChallenge& challenge);
	Reply ListCampaigns();
	Reply PreviewCampaign(const Json::Value& body, const std::string& id);
	Reply SendCampaign(const Json::Value& body, const std::string& id);

	// Helpers participate in the caller's transaction; they never commit independently.
	static void	  RequireFields(const Json::Value& body, std::initializer_list<const char*> allowed);
	Reply		  CurrentProfile(Json::Value user);
	Reply		  UpdateProfile(const Json::Value& body, const std::string& actor);
	Reply		  ChangePassword(const Json::Value& body, const std::string& actor);
	Reply		  CreateUser(const Json::Value& body, const std::string& actor);
	Reply		  UpdateUser(const Json::Value& body, const std::string& actor);
	void		  RevokeSessions(const std::string& user);
	Reply		  ListUsers(const std::string& cursor, const UserListQuery& query);
	Json::Value	  Session(const std::string& token, const std::string& csrf, bool mutation);
	Reply		  Login(const Json::Value& body, const std::string& oldToken);
	std::string	  Locale(const Json::Value& body) const;
	void		  Audit(const std::string& actor, const std::string& subject, const char* action);
	Reply		  PublicEmail(Action action, const Json::Value& body);
	Reply		  AccountEmail(Action action, const Json::Value& body, const Json::Value& user);
	void		  IssueEmail(const std::string& purpose,
							 const std::string& email,
							 const std::string& locale,
							 const std::string& user			  = "",
							 const std::string& credentialVersion = "");
	void		  Subscribe(const std::string& user, const std::string& email, const char* source);
	void		  UnsubscribeUser(const std::string& user, const char* source);
	std::string	  EmailContext(std::string body, const std::string& locale) const;
	Database	  m_Database;
	StoreSettings m_Settings;
};

std::string NormalizedEmail(const std::string& email);
} // namespace Accounts
