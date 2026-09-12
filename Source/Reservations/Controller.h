#pragma once

#include "ReservationService.h"

namespace Reservations
{
// Invoked by the existing Accounts dispatcher after authentication, inside its transaction.
class Controller
{
public:
	explicit Controller(ReservationService& service);
	Accounts::Reply Handle(Accounts::Database& db,
					   const Accounts::Identity& actor,
					   const Json::Value& body,
					   const std::string& action);

private:
	ReservationService& m_Service;
};
} // namespace Reservations
