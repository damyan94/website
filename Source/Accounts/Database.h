#pragma once

#include <libpq-fe.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace Accounts
{
class DatabaseError : public std::runtime_error
{
public:
	explicit DatabaseError(std::string state)
		: std::runtime_error("Accounts database operation failed"),
		  sqlState(std::move(state))
	{
	}

	std::string sqlState;
};

class Rows
{
public:
	explicit Rows(PGresult* result);
	int			Count() const;
	std::string Get(int row, int column) const;

private:
	std::unique_ptr<PGresult, decltype(&PQclear)> m_Result;
};

// Owned by one accounts worker; never shared between threads.
class Database
{
public:
	explicit Database(const std::string& connection);
	Rows Query(const char* sql, const std::vector<std::string>& parameters = {});
	void Script(const std::string& sql);
	void CheckSchema();
	void ReconnectIfNeeded();

private:
	std::unique_ptr<PGconn, decltype(&PQfinish)> m_Connection;
};

class Transaction
{
public:
	explicit Transaction(Database& database);
	~Transaction();
	void Commit();
	Transaction(const Transaction&)			   = delete;
	Transaction& operator=(const Transaction&) = delete;

private:
	Database& m_Database;
	bool	  m_Committed = false;
};
} // namespace Accounts
