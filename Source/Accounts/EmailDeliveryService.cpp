#include "EmailDeliveryService.h"
#include "Crypto.h"
#include "Email.h"
#include "MailRepository.h"
#include "stdafx.h"
#include <charconv>
#include <json/json.h>

namespace Accounts
{
EmailDeliveryService::EmailDeliveryService(const EmailSettings& settings,
										   const ScheduledEmailHooks& hooks,
										   const ResendTransport& resend,
										   const LocalOutboxTransport& localOutbox)
	: m_Settings(settings),
	  m_Hooks(hooks),
	  m_ResendTransport(resend),
	  m_LocalOutboxTransport(localOutbox)
{
}

bool EmailDeliveryService::DeliverOne(Database& database)
{
#ifdef _WIN32
	(void)database;
	return false;
#else
	MailRepository repository(database);
	std::string id, subject, body, recipient, kind, unsubscribe, deliveryKey, requestBody;
	int			attempts = 0;
	Json::Value message;
	{
		Transaction transaction(database);
		const auto	row = repository.LockDueJob(m_Settings.transport);
		if (!row)
		{
			transaction.Commit();
			return false;
		}
		id					 = row->id;
		kind				 = row->kind;
		recipient			 = row->recipient;
		subject				 = row->subject;
		body				 = row->body;
		unsubscribe			 = row->unsubscribe;
		attempts			 = row->attempts;
		deliveryKey			 = row->deliveryKey;
		requestBody			 = row->requestBody;
		bool		eligible = !row->expired;
		std::string reason	 = eligible ? "ineligible" : "expired";
		if (!row->transport.empty() && row->transport != m_Settings.transport)
		{
			eligible = false;
			reason	 = "transport_changed";
		}
		if (repository.IsSuppressed(recipient))
		{
			eligible = false;
			reason	 = "recipient_suppressed";
		}
		if (kind == "reminder")
			eligible = eligible && m_Hooks.eligible && m_Hooks.eligible(database, id);
		if (kind == "challenge")
			eligible =
				eligible &&
				repository.HasCurrentChallenge(row->challenge, m_Settings.registration, m_Settings.newsletters);
		if (kind == "newsletter")
		{
			eligible =
				eligible && m_Settings.newsletters &&
				repository.HasCurrentSubscription(row->user, row->generation, recipient, id);
			if (eligible && unsubscribe.empty())
			{
				unsubscribe = RandomToken();
				repository.InsertNewsletterLink(TokenHash(unsubscribe), row->user, row->generation);
				body += "\n\nUnsubscribe / Отписване:\n" + m_Settings.origin +
						"/email#action=unsubscribe&token=" + unsubscribe;
			}
		}
		const bool retryExpired = m_Settings.transport == "resend" && row->retryWindowExpired;
		if (!eligible || attempts >= 5 || retryExpired)
		{
			repository.FinishUnsentJob(id,
									   eligible ? "failed" : "skipped",
									   retryExpired ? "retry_window_expired"
									   : eligible	? "attempts_exhausted"
													: reason);
			transaction.Commit();
			return true;
		}
		message["id"]	   = id;
		message["kind"]	   = kind;
		message["from"]	   = m_Settings.sender;
		message["to"]	   = recipient;
		message["subject"] = subject;
		message["text"]	   = body;
		if (!unsubscribe.empty())
		{
			message["headers"]["List-Unsubscribe"] =
				"<" + m_Settings.origin + "/api/v1/newsletter/one-click?token=" + unsubscribe + ">";
			message["headers"]["List-Unsubscribe-Post"] = "List-Unsubscribe=One-Click";
		}
		if (deliveryKey.empty())
			deliveryKey = RandomToken();
		if (requestBody.empty() && m_Settings.transport == "resend")
		{
			auto payload = message;
			payload.removeMember("id");
			payload.removeMember("kind");
			payload["to"] = Json::arrayValue;
			payload["to"].append(recipient);
			Json::StreamWriterBuilder writer;
			writer["indentation"] = "";
			requestBody			  = Json::writeString(writer, payload);
		}
		repository.RecordAttempt(id, body, unsubscribe, m_Settings.transport, deliveryKey, requestBody);
		transaction.Commit();
	}
	// No SQL transaction is held during network/filesystem I/O.
	bool		accepted = false, retry = true;
	int			status = 0, delay = 30 * (1 << attempts);
	std::string providerId, error;
	try
	{
		if (m_Settings.transport == "local_outbox")
		{
			m_LocalOutboxTransport.Deliver(m_Settings.outbox / (id + ".json"), message);
			accepted = true;
		}
		else
		{
			const auto [result, response] =
				m_ResendTransport.Send(m_Settings.apiKey, deliveryKey, requestBody, m_Settings.requestSeconds);
			if (result != drogon::ReqResult::Ok || !response)
				error = result == drogon::ReqResult::Timeout ? "provider_timeout" : "provider_connection_failed";
			else
			{
				status = response->statusCode();
				const auto json = ResendTransport::DecodeResponse(response);
				if (status >= 200 && status < 300)
				{
					if (json && (*json)["id"].isString())
						providerId = (*json)["id"].asString();
					accepted =
						!providerId.empty() && providerId.size() <= 128 &&
						providerId.find_first_not_of(
							"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
					if (!accepted)
						error = "provider_invalid_response";
				}
				else
				{
					const auto name = json && (*json)["name"].isString() ? (*json)["name"].asString() : "";
					retry			= status == 408 || status == 429 || status >= 500 ||
							(status == 409 && name == "concurrent_idempotent_requests");
					error = status == 401 || status == 403 ? "provider_configuration"
							: status == 429				   ? "provider_rate_limited"
							: status >= 500				   ? "provider_unavailable"
														   : "provider_rejected";
					if (name == "daily_quota_exceeded" || name == "monthly_quota_exceeded")
						error = "provider_quota_exceeded";
					const auto& after	= response->getHeader("retry-after");
					int			seconds = 0;
					const auto	parsed	= std::from_chars(after.data(), after.data() + after.size(), seconds);
					if (parsed.ec == std::errc() && parsed.ptr == after.data() + after.size() && seconds > 0)
						delay = std::max(delay, std::min(seconds, 86400));
					if (error == "provider_quota_exceeded")
						delay = std::max(delay, 3600);
				}
			}
		}
	}
	catch (const std::exception&)
	{
		error = m_Settings.transport == "local_outbox" ? "outbox_unavailable" : "provider_connection_failed";
	}
	Transaction transaction(database);
	if (accepted)
	{
		if (!providerId.empty())
			repository.LockProvider(providerId);
		repository.CompleteDelivery(
			id, m_Settings.transport == "resend" ? "accepted" : "delivered", providerId, status);
		if (!providerId.empty())
			ApplyEmailEvents(database, providerId);
	}
	else
	{
		const bool failed = !retry || attempts + 1 >= 5;
		repository.RecordDeliveryFailure(id, failed, error, status, delay);
		if (status == 429 || status == 401 || status == 403 || status >= 500)
			repository.ExtendProviderPause(status == 401 || status == 403 ? 300 : delay);
	}
	transaction.Commit();
	return true;
#endif
}
} // namespace Accounts
