#include "../../../include/utils/db/DbConnection.h"
#include "../../../include/utils/db/DbException.h"
#include <muduo/base/Logging.h>

namespace http 
{
namespace db 
{

namespace
{

std::shared_ptr<sql::Connection> openConnection(const std::string& host,
                                                const std::string& user,
                                                const std::string& password,
                                                const std::string& database,
                                                unsigned int timeoutSeconds,
                                                bool setIoTimeouts)
{
    sql::ConnectOptionsMap options;
    options["hostName"] = host;
    options["userName"] = user;
    options["password"] = password;
    options["schema"] = database;
    const int timeout = static_cast<int>(timeoutSeconds);
    options["OPT_CONNECT_TIMEOUT"] = timeout;
    options["OPT_RECONNECT"] = true;
    if (setIoTimeouts)
    {
        options["OPT_READ_TIMEOUT"] = timeout;
        options["OPT_WRITE_TIMEOUT"] = timeout;
    }

    sql::mysql::MySQL_Driver* driver = sql::mysql::get_mysql_driver_instance();
    return std::shared_ptr<sql::Connection>(driver->connect(options));
}

} // namespace

DbConnection::DbConnection(const std::string& host,
                         const std::string& user,
                         const std::string& password,
                         const std::string& database)
    : host_(host)
    , user_(user)
    , password_(password)
    , database_(database)
{
    try 
    {
        connect();
        if (conn_) 
        {
            conn_->setClientOption("multi_statements", "false");
            
            // 设置字符集
            std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
            stmt->execute("SET NAMES utf8mb4");
            
            LOG_INFO << "Database connection established";
        }
    } 
    catch (const sql::SQLException& e) 
    {
        LOG_ERROR << "Failed to create database connection: " << e.what();
        throw DbException(e.what());
    }
}

void DbConnection::connect()
{
    conn_ = openConnection(host_, user_, password_, database_, 10, false);
}

DbConnection::~DbConnection() 
{
    try 
    {
        cleanup();
    } 
    catch (...) 
    {
        // 析构函数中不抛出异常
    }
    LOG_INFO << "Database connection closed";
}

bool DbConnection::ping() 
{
    std::lock_guard<std::mutex> lock(mutex_);
    try 
    {
        std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
        std::unique_ptr<sql::ResultSet> rs(stmt->executeQuery("SELECT 1"));
        return rs && rs->next();
    } 
    catch (const sql::SQLException& e) 
    {
        LOG_ERROR << "Ping failed: " << e.what();
        return false;
    }
}

bool DbConnection::probe(const std::string& host,
                         const std::string& user,
                         const std::string& password,
                         const std::string& database,
                         unsigned int timeoutSeconds)
{
    try
    {
        std::shared_ptr<sql::Connection> connection =
            openConnection(host, user, password, database, timeoutSeconds, true);
        std::unique_ptr<sql::Statement> statement(connection->createStatement());
        std::unique_ptr<sql::ResultSet> result(statement->executeQuery("SELECT 1"));
        return result && result->next();
    }
    catch (const sql::SQLException& e)
    {
        LOG_ERROR << "Database probe failed: " << e.what();
        return false;
    }
}

int DbConnection::executeRawUpdate(const std::string& sql)
{
    std::lock_guard<std::mutex> lock(mutex_);
    try
    {
        std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
        return stmt->executeUpdate(sql);
    }
    catch (const sql::SQLException& e)
    {
        LOG_ERROR << "Raw update failed: " << e.what() << ", SQL: " << sql;
        throw DbException(e.what());
    }
}

bool DbConnection::isValid() 
{
    try 
    {
        if (!conn_) return false;
        std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
        stmt->execute("SELECT 1");
        return true;
    } 
    catch (const sql::SQLException&) 
    {
        return false;
    }
}

void DbConnection::reconnect() 
{
    try 
    {
        if (conn_) 
        {
            conn_->reconnect();
        } 
        else 
        {
            connect();
        }
    } 
    catch (const sql::SQLException& e) 
    {
        LOG_ERROR << "Reconnect failed: " << e.what();
        throw DbException(e.what());
    }
}

void DbConnection::cleanup() 
{
    std::lock_guard<std::mutex> lock(mutex_);
    try 
    {
        if (conn_) 
        {
            // 确保所有事务都已完成
            if (!conn_->getAutoCommit()) 
            {
                conn_->rollback();
                conn_->setAutoCommit(true);
            }
            
            // 清理所有未处理的结果集
            std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
            while (stmt->getMoreResults()) 
            {
                auto result = stmt->getResultSet();
                while (result && result->next()) 
                {
                    // 消费所有结果
                }
            }
        }
    } 
    catch (const std::exception& e) 
    {
        LOG_WARN << "Error cleaning up connection: " << e.what();
        try 
        {
            reconnect();
        } 
        catch (...) 
        {
            // 忽略重连错误
        }
    }
}

} // namespace db
} // namespace http
