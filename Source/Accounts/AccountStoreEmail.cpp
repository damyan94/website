#include "AccountStore.h"
#include "EmailStatusService.h"
#include "Crypto.h"
#include "InputValidation.h"
#include "stdafx.h"
#include <algorithm>
#include <regex>

namespace Accounts
{
namespace
{
std::string Text(const Json::Value& body, const char* key, int maximum, bool multiline = false)
{
	if (!body[key].isString())
		throw RequestError(400, "Expected text");
	const auto result	 = body[key].asString();
	auto	   validated = result;
	if (multiline)
		for (auto& c : validated)
			if (c == '\n' || c == '\t')
				c = ' ';
	const auto length = TextLength(validated);
	if (length < 1 || length > maximum || validated.find_first_not_of(' ') == std::string::npos)
		throw RequestError(400, "Invalid text length or encoding");
	return result;
}

std::string Token(const Json::Value& body)
{
	const auto token = Text(body, "token", 64);
	if (token.size() != 64 || !std::all_of(token.begin(),
										   token.end(),
										   [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
		throw RequestError(400, "Invalid or expired link");
	return token;
}

Reply Accepted()
{
	Reply result;
	result.status		   = 202;
	result.body["message"] = "If the request is eligible, an email will arrive shortly";
	return result;
}

std::string Number(const Json::Value& body, const char* key)
{
	const auto value = Text(body, key, 18);
	if (!InputValidation::IsDecimalIdentifier(value))
		throw RequestError(400, "Invalid identifier");
	return value;
}
} // namespace

std::string AccountStore::EmailContext(std::string body, const std::string& locale) const
{
	const auto& ui = m_Settings.ui;
	if (!ui.siteName.empty())
		body = ui.siteName + "\n\n" + body;
	const auto support	= ui.supportText.get(locale, "").asString();
	const auto fallback = ui.supportText.get(ui.defaultLocale, "").asString();
	if (!support.empty() || !fallback.empty())
		body += "\n\n" + (support.empty() ? fallback : support);
	return body;
}

void AccountStore::IssueEmail(const std::string& purpose,
							  const std::string& email,
							  const std::string& locale,
							  const std::string& user,
							  const std::string& credentialVersion)
{
	// Persistent recipient throttling supplements the bounded HTTP limiter.
	const auto recent =
		m_Database.Query("SELECT count(*), count(*) FILTER (WHERE created_at>now()-interval '1 minute') "
						 "FROM accounts.email_challenges WHERE email=$1 AND created_at>now()-interval '1 hour'",
						 {email});
	if (std::stoi(recent.Get(0, 0)) >= 5 || recent.Get(0, 1) != "0")
		return;
	const auto token	 = RandomToken();
	const auto challenge = m_Database.Query(
		"INSERT INTO accounts.email_challenges(token_hash,purpose,user_id,credential_version,email,locale) "
		"VALUES($1,$2,NULLIF($3,'')::bigint,NULLIF($4,'')::bigint,$5,$6) RETURNING id",
		{TokenHash(token), purpose, user, credentialVersion, email, locale});
	const auto	link = m_Settings.email.origin + "/email?lang=" + locale + "#action=" + purpose + "&token=" + token;
	const bool	bg	 = locale == "bg";
	std::string subject, body;
	if (purpose == "register")
		subject = bg ? "Създаване на профил" : "Create your account";
	else if (purpose == "reset")
		subject = bg ? "Възстановяване на парола" : "Reset your password";
	else if (purpose == "subscribe")
		subject = bg ? "Потвърдете абонамента си" : "Confirm your newsletter subscription";
	else
		subject = bg ? "Потвърдете имейл адреса си" : "Verify your email address";
	body = subject + "\n\n" + link + "\n\n" +
		   (bg ? "Връзката е валидна 20 минути и може да се използва веднъж. Ако не сте поискали това, игнорирайте "
				 "съобщението."
			   : "This link expires in 20 minutes and can be used once. If you did not request this, ignore this "
				 "message.");
	QueueEmail(m_Database, email, subject, EmailContext(std::move(body), locale), user, challenge.Get(0, 0));
}

void AccountStore::Subscribe(const std::string& user, const std::string& email, const char* source)
{
	m_Database.Query("INSERT INTO accounts.subscriptions(user_id,email,confirmed_at) VALUES($1::bigint,$2,now()) "
					 "ON CONFLICT(user_id) DO UPDATE SET "
					 "email=excluded.email,confirmed_at=now(),generation=accounts.subscriptions.generation+1",
					 {user, email});
	m_Database.Query(
		"INSERT INTO accounts.subscription_events(user_id,email,action,source) VALUES($1::bigint,$2,'subscribed',$3)",
		{user, email, source});
}

void AccountStore::UnsubscribeUser(const std::string& user, const char* source)
{
	const auto row = m_Database.Query("UPDATE accounts.subscriptions SET confirmed_at=NULL,generation=generation+1 "
									  "WHERE user_id=$1::bigint AND confirmed_at IS NOT NULL RETURNING email",
									  {user});
	if (row.Count())
		m_Database.Query("INSERT INTO accounts.subscription_events(user_id,email,action,source) "
						 "VALUES($1::bigint,$2,'unsubscribed',$3)",
						 {user, row.Get(0, 0), source});
	m_Database.Query("UPDATE accounts.email_challenges SET consumed_at=now() WHERE user_id=$1::bigint AND "
					 "purpose='subscribe' AND consumed_at IS NULL",
					 {user});
	m_Database.Query("UPDATE accounts.mail_jobs SET "
					 "state='skipped',body='',request_body=NULL,unsubscribe_token=NULL,finished_at=now() "
					 "WHERE user_id=$1::bigint AND kind='newsletter' AND state='queued'",
					 {user});
}

Reply AccountStore::PublicEmail(Action action, const Json::Value& body)
{
	if (action == Action::EmailOptions)
		return EmailOptions();
	if (!m_Settings.email.enabled)
		throw RequestError(404, "Email features are disabled");
	if ((action == Action::RegisterEmail || action == Action::CompleteRegistration) && !m_Settings.email.registration)
		throw RequestError(404, "Registration is disabled");
	if (action == Action::RegisterEmail || action == Action::ForgotPassword)
		return RequestEmailChallenge(action, body);
	if (action == Action::Unsubscribe)
		return UnsubscribeLink(body);
	return CompleteEmailChallenge(action, body);
}

Reply AccountStore::EmailOptions() const
{
	Reply reply;
	// Only explicitly public presentation fields are exposed, never server configuration.
	reply.body["ui"]["siteName"]	  = m_Settings.ui.siteName;
	reply.body["ui"]["defaultLocale"] = m_Settings.ui.defaultLocale;
	reply.body["ui"]["supportText"]	  = m_Settings.ui.supportText;
	reply.body["email"]				  = m_Settings.email.enabled;
	reply.body["registration"]		  = m_Settings.email.registration;
	reply.body["newsletters"]		  = m_Settings.email.newsletters;
	reply.body["transport"]			  = m_Settings.email.enabled ? m_Settings.email.transport : "disabled";
	reply.body["locales"]			  = Json::arrayValue;
	for (const auto& locale : m_Settings.locales)
		reply.body["locales"].append(locale);
	return reply;
}

Reply AccountStore::RequestEmailChallenge(Action action, const Json::Value& body)
{
	RequireFields(body, {"email", "locale"});
	const auto email  = NormalizedEmail(Text(body, "email", 254));
	const auto locale = Locale(body);
	// Both existing and unknown addresses perform the same expensive work.
	HashPassword(RandomToken());
	Transaction transaction(m_Database);
	m_Database.Query("SELECT pg_advisory_xact_lock(70123002)");
	const auto user = m_Database.Query(
		"SELECT id,credential_version,enabled,email_verified_at IS NOT NULL FROM accounts.users WHERE email=$1",
		{email});
	if (action == Action::RegisterEmail && !user.Count())
		IssueEmail("register", email, locale);
	if (action == Action::ForgotPassword && user.Count() && user.Get(0, 2) == "t" && user.Get(0, 3) == "t")
		IssueEmail("reset", email, locale, user.Get(0, 0), user.Get(0, 1));
	transaction.Commit();
	return Accepted();
}

Reply AccountStore::UnsubscribeLink(const Json::Value& body)
{
	RequireFields(body, {"token"});
	const auto	hash = TokenHash(Token(body));
	Transaction transaction(m_Database);
	m_Database.Query("SELECT pg_advisory_xact_lock(70123002)");
	const auto row = m_Database.Query("SELECT l.user_id FROM accounts.newsletter_links l JOIN accounts.subscriptions s "
									  "ON s.user_id=l.user_id AND s.generation=l.generation WHERE l.token_hash=$1",
									  {hash});
	if (row.Count())
		UnsubscribeUser(row.Get(0, 0), "email-link-v1");
	transaction.Commit();
	Reply reply;
	reply.body["message"] = "Newsletter subscription removed";
	return reply;
}

Reply AccountStore::CompleteEmailChallenge(Action action, const Json::Value& body)
{
	if (action == Action::CompleteRegistration)
		RequireFields(body, {"token", "displayName", "phone", "password", "newsletter"});
	else if (action == Action::CompleteReset)
		RequireFields(body, {"token", "password"});
	else
		RequireFields(body, {"token"});
	const auto	hash = TokenHash(Token(body));
	std::string password;
	if (action == Action::CompleteRegistration || action == Action::CompleteReset)
	{
		password = Text(body, "password", 128);
		if (!ValidPassword(password))
			throw RequestError(400, "Password must contain 15 to 128 characters without control characters");
		password = HashPassword(password);
	}
	Transaction transaction(m_Database);
	m_Database.Query("SELECT pg_advisory_xact_lock(70123002)");
	const auto row = m_Database.Query(
		"SELECT c.id,c.purpose,COALESCE(c.user_id::text,''),c.email,c.locale "
		"FROM accounts.email_challenges c LEFT JOIN accounts.users u ON u.id=c.user_id "
		"WHERE c.token_hash=$1 AND c.consumed_at IS NULL AND c.expires_at>now() "
		"AND (c.user_id IS NULL OR (u.enabled AND u.credential_version=c.credential_version)) FOR UPDATE OF c",
		{hash});
	if (!row.Count())
		throw RequestError(400, "Invalid or expired link");
	const EmailChallenge challenge{row.Get(0, 1), row.Get(0, 2), row.Get(0, 3), row.Get(0, 4)};
	if ((action == Action::CompleteRegistration && challenge.purpose != "register") ||
		(action == Action::CompleteReset && challenge.purpose != "reset") ||
		(action == Action::ConfirmEmail && challenge.purpose != "verify" && challenge.purpose != "change" &&
		 challenge.purpose != "subscribe"))
		throw RequestError(400, "Invalid or expired link");
	Reply reply;
	if (challenge.purpose == "register")
		reply = RegisterCustomer(challenge, body, password);
	else if (challenge.purpose == "reset")
		reply = RecoverPassword(challenge, password);
	else if (challenge.purpose == "change")
		reply = ConfirmEmailChange(challenge);
	else if (challenge.purpose == "verify")
		reply = ConfirmEmailVerification(challenge);
	else
		reply = ConfirmSubscription(challenge);
	m_Database.Query(
		"UPDATE accounts.email_challenges SET consumed_at=now() WHERE email=$1 AND purpose=$2 AND consumed_at IS NULL",
		{challenge.email, challenge.purpose});
	transaction.Commit();
	reply.body["message"] = "Request completed";
	return reply;
}

Reply AccountStore::RegisterCustomer(const EmailChallenge& challenge,
									 const Json::Value&	   body,
									 const std::string&	   password)
{
	Reply	   reply;
	const auto name = Text(body, "displayName", 100);
	if (!body["phone"].isString() || TextLength(body["phone"].asString()) < 0 || body["phone"].asString().size() > 32 ||
		!InputValidation::HasPhoneCharacters(body["phone"].asString()) || !body["newsletter"].isBool())
		throw RequestError(400, "Invalid phone number or subscription choice");
	if (body["newsletter"].asBool() && !m_Settings.email.newsletters)
		throw RequestError(400, "Newsletters are disabled");
	if (m_Database.Query("SELECT id FROM accounts.users WHERE email=$1", {challenge.email}).Count())
		throw RequestError(409, "This link cannot create an account; use sign in or recovery");
	const auto created = m_Database.Query(
		"INSERT INTO accounts.users(email,password_hash,display_name,phone,locale,role,email_verified_at) "
		"VALUES($1,$2,$3,$4,$5,'customer',now()) RETURNING id",
		{challenge.email, password, name, body["phone"].asString(), challenge.locale});
	if (body["newsletter"].asBool())
		Subscribe(created.Get(0, 0), challenge.email, "registration-newsletter-v1");
	Audit("", created.Get(0, 0), "user.registered");
	reply.status = 201;
	return reply;
}

Reply AccountStore::RecoverPassword(const EmailChallenge& challenge, const std::string& password)
{
	Reply reply;
	m_Database.Query("UPDATE accounts.users SET "
					 "password_hash=$1,credential_version=credential_version+1,version=version+1,updated_at=now() "
					 "WHERE id=$2::bigint AND email=$3 AND email_verified_at IS NOT NULL",
					 {password, challenge.user, challenge.email});
	RevokeSessions(challenge.user);
	Audit(challenge.user, challenge.user, "password.recovered");
	QueueEmail(m_Database,
			   challenge.email,
			   challenge.locale == "bg" ? "Паролата Ви е сменена" : "Your password changed",
			   EmailContext(challenge.locale == "bg"
								? "Паролата Ви е сменена. Ако не сте направили това, свържете се с поддръжката."
								: "Your password was changed. If you did not do this, contact support.",
							challenge.locale),
			   challenge.user);
	reply.clearCookie = true;
	return reply;
}

Reply AccountStore::ConfirmEmailChange(const EmailChallenge& challenge)
{
	Reply reply;
	if (m_Database
			.Query("SELECT id FROM accounts.users WHERE email=$1 AND id<>$2::bigint", {challenge.email, challenge.user})
			.Count())
		throw RequestError(409, "This email change cannot be completed");
	const auto old = m_Database.Query("SELECT email FROM accounts.users WHERE id=$1::bigint", {challenge.user});
	UnsubscribeUser(challenge.user, "email-change-v1");
	m_Database.Query("UPDATE accounts.users SET "
					 "email=$1,email_verified_at=now(),credential_version=credential_version+1,version=version+1,"
					 "updated_at=now() WHERE id=$2::bigint",
					 {challenge.email, challenge.user});
	RevokeSessions(challenge.user);
	QueueEmail(
		m_Database,
		old.Get(0, 0),
		challenge.locale == "bg" ? "Имейл адресът Ви е сменен" : "Your email address changed",
		EmailContext(challenge.locale == "bg"
						 ? "Имейл адресът за вход е сменен. Ако не сте направили това, свържете се с поддръжката."
						 : "Your sign-in email changed. If you did not do this, contact support.",
					 challenge.locale),
		challenge.user);
	Audit(challenge.user, challenge.user, "email.changed");
	reply.clearCookie = true;
	return reply;
}

Reply AccountStore::ConfirmEmailVerification(const EmailChallenge& challenge)
{
	Reply reply;
	m_Database.Query(
		"UPDATE accounts.users SET email_verified_at=now(),version=version+1 WHERE id=$1::bigint AND email=$2",
		{challenge.user, challenge.email});
	Audit(challenge.user, challenge.user, "email.verified");
	return reply;
}

Reply AccountStore::ConfirmSubscription(const EmailChallenge& challenge)
{
	Reply reply;
	if (!m_Settings.email.newsletters)
		throw RequestError(404, "Newsletters are disabled");
	const auto valid = m_Database.Query(
		"SELECT id FROM accounts.users WHERE id=$1::bigint AND email=$2 AND email_verified_at IS NOT NULL",
		{challenge.user, challenge.email});
	if (!valid.Count())
		throw RequestError(400, "Invalid or expired link");
	Subscribe(challenge.user, challenge.email, "profile-newsletter-v1");
	return reply;
}

Reply AccountStore::AccountEmail(Action action, const Json::Value& body, const Json::Value& user)
{
	if (!m_Settings.email.enabled)
		throw RequestError(404, "Email features are disabled");
	const auto id = user["id"].asString(), email = user["email"].asString(), locale = user["locale"].asString();
	const auto details = m_Database.Query(
		"SELECT credential_version,email_verified_at IS NOT NULL,password_hash FROM accounts.users WHERE id=$1::bigint",
		{id});
	if (action == Action::VerifyEmail)
	{
		RequireFields(body, {});
		if (details.Get(0, 1) != "t")
			IssueEmail("verify", email, locale, id, details.Get(0, 0));
		return Accepted();
	}
	if (action == Action::ChangeEmail)
	{
		RequireFields(body, {"email", "currentPassword"});
		const auto target = NormalizedEmail(Text(body, "email", 254));
		if (!VerifyPassword(Text(body, "currentPassword", 128), details.Get(0, 2)))
			throw RequestError(403, "Current password is incorrect");
		if (details.Get(0, 1) != "t")
			throw RequestError(409, "Verify your current email before changing it");
		if (target != email && !m_Database.Query("SELECT id FROM accounts.users WHERE email=$1", {target}).Count())
			IssueEmail("change", target, locale, id, details.Get(0, 0));
		return Accepted();
	}
	if (!m_Settings.email.newsletters)
		throw RequestError(404, "Newsletters are disabled");
	if (action == Action::NewsletterPreference)
	{
		RequireFields(body, {"subscribed"});
		if (!body["subscribed"].isBool())
			throw RequestError(400, "Subscription choice must be a boolean");
		if (!body["subscribed"].asBool())
		{
			UnsubscribeUser(id, "profile-v1");
			return Reply{};
		}
		if (details.Get(0, 1) != "t")
			throw RequestError(409, "Verify your email before subscribing");
		if (!m_Database
				 .Query(
					 "SELECT user_id FROM accounts.subscriptions WHERE user_id=$1::bigint AND confirmed_at IS NOT NULL",
					 {id})
				 .Count())
			IssueEmail("subscribe", email, locale, id, details.Get(0, 0));
		return Accepted();
	}
	if (user["role"] != "admin")
		throw RequestError(403, "Administrator access required");
	if (action == Action::ListCampaigns)
		return ListCampaigns();
	if (action == Action::PreviewCampaign)
		return PreviewCampaign(body, id);
	return SendCampaign(body, id);
}

bool AccountStore::ReceiveEmailEvent(const std::string& eventId,
									const std::string& type,
									const std::string& providerId,
									const std::string& occurred)
{
	EmailStatusService service(m_Database);
	return service.ReceiveEvent(eventId, type, providerId, occurred);
}

Reply AccountStore::ListCampaigns()
{
	Reply	   reply;
	const auto rows = m_Database.Query(
		"SELECT c.id,c.subject,c.locale,c.state,count(j.id),count(j.id) FILTER(WHERE j.state='delivered'),"
		"count(j.id) FILTER(WHERE j.state IN ('failed','bounced','complained')),count(j.id) FILTER(WHERE "
		"j.state='skipped'),"
		"count(j.id) FILTER(WHERE j.state IN ('accepted','delayed')),"
		"count(j.id) FILTER(WHERE j.state='delivered' AND j.transport='local_outbox') FROM "
		"accounts.campaigns c "
		"LEFT JOIN accounts.mail_jobs j ON j.campaign_id=c.id GROUP BY c.id ORDER BY c.id DESC LIMIT 50");
	reply.body["campaigns"] = Json::arrayValue;
	for (int i = 0; i < rows.Count(); ++i)
	{
		Json::Value item;
		const char* keys[] = {
			"id", "subject", "locale", "state", "recipients", "delivered", "failed", "skipped", "accepted", "outbox"};
		for (int j = 0; j < 10; ++j)
			item[keys[j]] = rows.Get(i, j);
		reply.body["campaigns"].append(item);
	}
	return reply;
}

Reply AccountStore::PreviewCampaign(const Json::Value& body, const std::string& id)
{
	Reply reply;
	RequireFields(body, {"requestKey", "locale", "subject", "text"});
	const auto key = Text(body, "requestKey", 64);
	if (!std::regex_match(key, std::regex("[A-Za-z0-9-]{16,64}")))
		throw RequestError(400, "Invalid request key");
	const auto language = Locale(body), subject = Text(body, "subject", 200), message = Text(body, "text", 5000, true);
	m_Database.Query("INSERT INTO accounts.campaigns(request_key,actor_id,locale,subject,body) "
					 "VALUES($1,$2::bigint,$3,$4,$5) ON CONFLICT(request_key) DO NOTHING",
					 {key, id, language, subject, message});
	const auto draft = m_Database.Query(
		"SELECT id,actor_id,locale,subject,body,state FROM accounts.campaigns WHERE request_key=$1", {key});
	if (draft.Get(0, 1) != id || draft.Get(0, 2) != language || draft.Get(0, 3) != subject ||
		draft.Get(0, 4) != message)
		throw RequestError(409, "Request key already used for different content");
	const auto count =
		m_Database.Query("SELECT count(*) FROM accounts.subscriptions s JOIN accounts.users u ON u.id=s.user_id "
						 "WHERE s.confirmed_at IS NOT NULL AND s.email=u.email AND u.email_verified_at IS NOT NULL "
						 "AND u.enabled AND u.locale=$1 AND NOT EXISTS "
						 "(SELECT 1 FROM accounts.mail_suppressions m WHERE m.recipient=u.email)",
						 {language});
	reply.body["id"]		 = draft.Get(0, 0);
	reply.body["recipients"] = std::stoi(count.Get(0, 0));
	reply.body["subject"]	 = subject;
	reply.body["text"] =
		message + "\n\n" + m_Settings.email.footer + "\n\n[Unsubscribe link is added for each recipient]";
	reply.body["transport"] = m_Settings.email.transport;
	return reply;
}

Reply AccountStore::SendCampaign(const Json::Value& body, const std::string& id)
{
	Reply reply;
	RequireFields(body, {"id", "expectedRecipients"});
	const auto campaign = Number(body, "id");
	if (!body["expectedRecipients"].isUInt())
		throw RequestError(400, "Invalid recipient count");
	const auto draft = m_Database.Query(
		"SELECT locale,subject,body,state FROM accounts.campaigns WHERE id=$1::bigint FOR UPDATE", {campaign});
	if (!draft.Count())
		throw RequestError(404, "Campaign not found");
	if (draft.Get(0, 3) == "queued")
	{
		reply.body["id"] = campaign;
		return reply;
	}
	const auto eligible =
		m_Database.Query("SELECT count(*) FROM accounts.subscriptions s JOIN accounts.users u ON u.id=s.user_id "
						 "WHERE s.confirmed_at IS NOT NULL AND s.email=u.email AND u.email_verified_at IS NOT NULL AND "
						 "u.enabled AND u.locale=$1 AND NOT EXISTS "
						 "(SELECT 1 FROM accounts.mail_suppressions m WHERE m.recipient=u.email)",
						 {draft.Get(0, 0)});
	const auto count = std::stoul(eligible.Get(0, 0));
	if (count != body["expectedRecipients"].asUInt())
		throw RequestError(409, "The audience changed; preview again before sending");
	if (!count || count > 1000)
		throw RequestError(400, "A campaign needs between 1 and 1000 eligible subscribers");
	if (std::stoul(m_Database.Query("SELECT count(*) FROM accounts.mail_jobs WHERE state IN ('queued','processing')")
					   .Get(0, 0)) +
			count >
		10000)
		throw RequestError(503, "Email queue is full; try again later");
	m_Database.Query(
		"INSERT INTO accounts.mail_jobs(kind,user_id,campaign_id,subscription_generation,recipient,subject,body) "
		"SELECT 'newsletter',u.id,$1::bigint,s.generation,u.email,$2,$3 FROM accounts.subscriptions s JOIN "
		"accounts.users u ON u.id=s.user_id "
		"WHERE s.confirmed_at IS NOT NULL AND s.email=u.email AND u.email_verified_at IS NOT NULL AND u.enabled AND "
		"u.locale=$4 AND NOT EXISTS (SELECT 1 FROM accounts.mail_suppressions m WHERE m.recipient=u.email)",
		{campaign, draft.Get(0, 1), draft.Get(0, 2) + "\n\n" + m_Settings.email.footer, draft.Get(0, 0)});
	m_Database.Query("UPDATE accounts.campaigns SET state='queued',queued_at=now() WHERE id=$1::bigint", {campaign});
	Audit(id, id, "newsletter.queued");
	reply.body["id"] = campaign;
	reply.status	 = 202;
	return reply;
}
} // namespace Accounts
