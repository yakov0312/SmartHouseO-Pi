// Created by yakov on 02/08/2026.

#include "DataBase.h"

#include "Logging/Logger.h"

DataBase::DataBase()
{
	m_dbCon = PQconnectdb( "host=localhost dbname=home_server user=HomeServer password=Test");

	if (PQstatus(m_dbCon) != CONNECTION_OK)
	{
		const std::string err = PQerrorMessage(m_dbCon);
		PQfinish(m_dbCon);
		throw std::runtime_error(err);
	}

	const char* query = R"(
		CREATE TABLE IF NOT EXISTS users (
			id SERIAL PRIMARY KEY,
			username TEXT UNIQUE NOT NULL,
			password TEXT NOT NULL); )";

	const PGresult* result = PQexec(m_dbCon, query);

	if (PQresultStatus(result) != PGRES_COMMAND_OK)
	{
		PQfinish(m_dbCon);
		throw std::runtime_error("Failed to create table users. try creating it yourself.");
	}
}

uint64_t DataBase::createUser(std::string_view username, std::string_view password)
{
	const char* params[] = {
		username.data(),
		password.data()
	};

	PGresult* result;

	{
		std::lock_guard lock(m_dbMutex);
		result = PQexecParams(
			m_dbCon,
			"INSERT INTO users (username, password) VALUES ($1, $2) RETURNING id;",
			2,
			nullptr,
			params,
			nullptr,
			nullptr,
			0
		);
	}

	uint64_t id = 0;
	if (PQresultStatus(result) == PGRES_TUPLES_OK)
		id = std::stoi(PQgetvalue(result, 0, 0));
	else
		Logger::get().error(PQerrorMessage(m_dbCon));

	PQclear(result);

	return id;
}

uint64_t DataBase::validateUser(std::string_view username, std::string_view password)
{
	const char* params[] = {
		username.data(),
		password.data()
	};

	PGresult* result;

	{
		std::lock_guard lock(m_dbMutex);
		result = PQexecParams(
			m_dbCon,
			"SELECT id FROM users WHERE username = $1 AND password = $2;",
			2,
			nullptr,
			params,
			nullptr,
			nullptr,
			0
		);
	}

	uint64_t id = 0;
	if (PQresultStatus(result) == PGRES_TUPLES_OK)
	{
		if (PQntuples(result) == 1)
			id = std::stoi(PQgetvalue(result, 0, 0));
	}
	else
		Logger::get().error(PQresultErrorMessage(result));

	PQclear(result);

	return id;
}