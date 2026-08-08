// Created by yakov on 02/08/2026.

#pragma once

#include <libpq-fe.h>
#include <mutex>
#include <string>

class DataBase
{
public:
	DataBase();
	uint64_t createUser(std::string_view username, std::string_view password);
	uint64_t validateUser(std::string_view username, std::string_view password);

private:
	PGconn* m_dbCon;
	std::mutex m_dbMutex;
};
