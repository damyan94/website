#include "Database.h"
#include "stdafx.h"

namespace Accounts
{
Rows::Rows(PGresult* result)
	: m_Result(result, &PQclear)
{
	if (!result)
		throw DatabaseError("08000");
	const auto status = PQresultStatus(result);
	if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK)
	{
		const auto state = PQresultErrorField(result, PG_DIAG_SQLSTATE);
		throw DatabaseError(state ? state : "08000");
	}
}

int Rows::Count() const
{
	return PQntuples(m_Result.get());
}

std::string Rows::Get(int row, int column) const
{
	return std::string(PQgetvalue(m_Result.get(), row, column), PQgetlength(m_Result.get(), row, column));
}

Database::Database(const std::string& connection)
	: m_Connection(nullptr, &PQfinish)
{
	// A final connect_timeout overrides a value supplied inside the connection string.
	const char* keys[]	 = {"dbname", "connect_timeout", nullptr};
	const char* values[] = {connection.c_str(), "5", nullptr};
	m_Connection.reset(PQconnectdbParams(keys, values, 1));
	if (!m_Connection || PQstatus(m_Connection.get()) != CONNECTION_OK)
		throw DatabaseError("08000");
	Script("SET statement_timeout = '5s'; SET lock_timeout = '3s'; SET idle_in_transaction_session_timeout = '10s'; "
		   "SET client_encoding = 'UTF8';");
}

Rows Database::Query(const char* sql, const std::vector<std::string>& parameters)
{
	std::vector<const char*> values;
	values.reserve(parameters.size());
	for (const auto& value : parameters)
	{
		if (value.find('\0') != std::string::npos)
			throw std::invalid_argument("Embedded null in database parameter");
		values.push_back(value.c_str());
	}
	return Rows(PQexecParams(m_Connection.get(), sql, values.size(), nullptr, values.data(), nullptr, nullptr, 0));
}

void Database::Script(const std::string& sql)
{
	Rows result(PQexec(m_Connection.get(), sql.c_str()));
}

void Database::CheckSchema()
{
	const auto rows = Query("SELECT version FROM accounts.schema_version");
	if (rows.Count() != 1 || rows.Get(0, 0) != "4")
		throw std::runtime_error("Unsupported accounts schema; run the accounts migration");
}

void Database::ReconnectIfNeeded()
{
	if (PQstatus(m_Connection.get()) == CONNECTION_OK)
		return;
	PQreset(m_Connection.get());
	if (PQstatus(m_Connection.get()) != CONNECTION_OK)
		throw DatabaseError("08000");
	Script("SET statement_timeout = '5s'; SET lock_timeout = '3s'; SET idle_in_transaction_session_timeout = '10s'; "
		   "SET client_encoding = 'UTF8';");
	CheckSchema();
}

Transaction::Transaction(Database& database)
	: m_Database(database)
{
	m_Database.Query("BEGIN");
}

Transaction::~Transaction()
{
	if (!m_Committed)
	{
		try
		{
			m_Database.Query("ROLLBACK");
		}
		catch (...)
		{ /* Connection failure is reported by the operation. */
		}
	}
}

void Transaction::Commit()
{
	m_Database.Query("COMMIT");
	m_Committed = true;
}
} // namespace Accounts
