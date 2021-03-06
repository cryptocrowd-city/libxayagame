// Copyright (C) 2018-2020 The Xaya developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sqlitestorage.hpp"

#include <glog/logging.h>

#include <cstdio>

namespace xaya
{

/* ************************************************************************** */

namespace
{

/**
 * Error callback for SQLite, which prints logs using glog.
 */
void
SQLiteErrorLogger (void* arg, const int errCode, const char* msg)
{
  LOG (ERROR) << "SQLite error (code " << errCode << "): " << msg;
}

/**
 * Binds a BLOB corresponding to an uint256 value to a statement parameter.
 * The value is bound using SQLITE_STATIC, so the uint256's data must not be
 * changed until the statement execution has finished.
 */
void
BindUint256 (sqlite3_stmt* stmt, const int ind, const uint256& value)
{
  const int rc = sqlite3_bind_blob (stmt, ind,
                                    value.GetBlob (), uint256::NUM_BYTES,
                                    SQLITE_STATIC);
  if (rc != SQLITE_OK)
    LOG (FATAL) << "Failed to bind uint256 value to parameter: " << rc;
}

/**
 * Binds a BLOB parameter to a std::string value.  The value is bound using
 * SQLITE_STATIC, so the underlying string must remain valid until execution
 * of the prepared statement is done.
 */
void
BindStringBlob (sqlite3_stmt* stmt, const int ind, const std::string& value)
{
  const int rc = sqlite3_bind_blob (stmt, ind, &value[0], value.size (),
                                    SQLITE_STATIC);
  if (rc != SQLITE_OK)
    LOG (FATAL) << "Failed to bind string value to parameter: " << rc;
}

/**
 * Retrieves a column value from a BLOB field as std::string.
 */
std::string
GetStringBlob (sqlite3_stmt* stmt, const int ind)
{
  const void* blob = sqlite3_column_blob (stmt, ind);
  const size_t blobSize = sqlite3_column_bytes (stmt, ind);
  return std::string (static_cast<const char*> (blob), blobSize);
}

} // anonymous namespace

bool SQLiteDatabase::loggerInitialised = false;

SQLiteDatabase::SQLiteDatabase (const std::string& file, const int flags)
  : db(nullptr)
{
  if (!loggerInitialised)
    {
      LOG (INFO)
          << "Using SQLite version " << SQLITE_VERSION
          << " (library version: " << sqlite3_libversion () << ")";
      CHECK_EQ (SQLITE_VERSION_NUMBER, sqlite3_libversion_number ())
          << "Mismatch between header and library SQLite versions";

      const int rc
          = sqlite3_config (SQLITE_CONFIG_LOG, &SQLiteErrorLogger, nullptr);
      if (rc != SQLITE_OK)
        LOG (WARNING) << "Failed to set up SQLite error handler: " << rc;
      else
        LOG (INFO) << "Configured SQLite error handler";

      loggerInitialised = true;
    }

  const int rc = sqlite3_open_v2 (file.c_str (), &db, flags, nullptr);
  if (rc != SQLITE_OK)
    LOG (FATAL) << "Failed to open SQLite database: " << file;

  CHECK (db != nullptr);
  LOG (INFO) << "Opened SQLite database successfully: " << file;

  auto* stmt = Prepare ("PRAGMA `journal_mode` = WAL");
  CHECK_EQ (sqlite3_step (stmt), SQLITE_ROW);
  const auto mode = GetStringBlob (stmt, 0);
  CHECK_EQ (sqlite3_step (stmt), SQLITE_DONE);
  if (mode == "wal")
    {
      LOG (INFO) << "Set database to WAL mode";
      walMode = true;
    }
  else
    {
      LOG (WARNING) << "Failed to set WAL mode, journaling is " << mode;
      walMode = false;
    }
}

SQLiteDatabase::~SQLiteDatabase ()
{
  if (parent != nullptr)
    {
      LOG (INFO) << "Ending snapshot read transaction";
      CHECK_EQ (sqlite3_step (PrepareRo ("ROLLBACK")), SQLITE_DONE);
    }

  for (const auto& stmt : preparedStatements)
    {
      /* sqlite3_finalize returns the error code corresponding to the last
         evaluation of the statement, not an error code "about" finalising it.
         Thus we want to ignore it here.  */
      sqlite3_finalize (stmt.second);
    }

  CHECK (db != nullptr);
  const int rc = sqlite3_close (db);
  if (rc != SQLITE_OK)
    LOG (ERROR) << "Failed to close SQLite database";

  if (parent != nullptr)
    parent->UnrefSnapshot ();
}

void
SQLiteDatabase::SetReadonlySnapshot (const SQLiteStorage& p)
{
  CHECK (parent == nullptr);
  parent = &p;
  LOG (INFO) << "Starting read transaction for snapshot";

  /* There is no way to do an "immediate" read transaction.  Thus we have
     to start a default deferred one, and then issue some SELECT query
     that we don't really care about and that is guaranteed to work.  */

  auto* stmt = PrepareRo ("BEGIN");
  CHECK_EQ (sqlite3_step (stmt), SQLITE_DONE);

  stmt = PrepareRo ("SELECT COUNT(*) FROM `sqlite_master`");
  CHECK_EQ (sqlite3_step (stmt), SQLITE_ROW);
  CHECK_EQ (sqlite3_step (stmt), SQLITE_DONE);
}

sqlite3_stmt*
SQLiteDatabase::Prepare (const std::string& sql)
{
  return PrepareRo (sql);
}

sqlite3_stmt*
SQLiteDatabase::PrepareRo (const std::string& sql) const
{
  CHECK (db != nullptr);
  const auto mit = preparedStatements.find (sql);
  if (mit != preparedStatements.end ())
    {
      /* sqlite3_reset returns an error code if the last execution of the
         statement had an error.  We don't care about that here.  */
      sqlite3_reset (mit->second);

      const int rc = sqlite3_clear_bindings (mit->second);
      if (rc != SQLITE_OK)
        LOG (ERROR) << "Failed to reset bindings for statement: " << rc;

      return mit->second;
    }

  sqlite3_stmt* res = nullptr;
  const int rc = sqlite3_prepare_v2 (db, sql.c_str (), sql.size () + 1,
                                     &res, nullptr);
  if (rc != SQLITE_OK)
    LOG (FATAL) << "Failed to prepare SQL statement: " << rc;

  preparedStatements.emplace (sql, res);
  return res;
}

/* ************************************************************************** */

SQLiteStorage::~SQLiteStorage ()
{
  if (db != nullptr)
    CloseDatabase ();
}

void
SQLiteStorage::OpenDatabase ()
{
  CHECK (db == nullptr);
  db = std::make_unique<SQLiteDatabase> (filename,
          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);

  SetupSchema ();
}

void
SQLiteStorage::CloseDatabase ()
{
  CHECK (db != nullptr);

  std::unique_lock<std::mutex> lock(mutSnapshots);
  LOG_IF (INFO, snapshots > 0)
      << "Waiting for outstanding snapshots to be finished...";
  while (snapshots > 0)
    cvSnapshots.wait (lock);

  db.reset ();
}

SQLiteDatabase&
SQLiteStorage::GetDatabase ()
{
  CHECK (db != nullptr);
  return *db;
}

const SQLiteDatabase&
SQLiteStorage::GetDatabase () const
{
  CHECK (db != nullptr);
  return *db;
}

std::unique_ptr<SQLiteDatabase>
SQLiteStorage::GetSnapshot () const
{
  CHECK (db != nullptr);
  if (!db->IsWalMode ())
    {
      LOG (WARNING) << "Snapshot is not possible for non-WAL database";
      return nullptr;
    }

  std::lock_guard<std::mutex> lock(mutSnapshots);
  ++snapshots;

  auto res = std::make_unique<SQLiteDatabase> (filename, SQLITE_OPEN_READONLY);
  res->SetReadonlySnapshot (*this);

  return res;
}

void
SQLiteStorage::UnrefSnapshot () const
{
  std::lock_guard<std::mutex> lock(mutSnapshots);
  CHECK_GT (snapshots, 0);
  --snapshots;
  cvSnapshots.notify_all ();
}

/**
 * Steps a given statement and expects no results (i.e. for an update).
 * Can also be used for statements where we expect exactly one result to
 * verify that no more are there.
 */
void
SQLiteStorage::StepWithNoResult (sqlite3_stmt* stmt)
{
  const int rc = sqlite3_step (stmt);
  if (rc != SQLITE_DONE)
    LOG (FATAL) << "Expected SQLITE_DONE, got: " << rc;
}

void
SQLiteStorage::SetupSchema ()
{
  LOG (INFO) << "Setting up database schema if it does not exist yet";
  const int rc = sqlite3_exec (**db, R"(
    CREATE TABLE IF NOT EXISTS `xayagame_current`
        (`key` TEXT PRIMARY KEY,
         `value` BLOB);
    CREATE TABLE IF NOT EXISTS `xayagame_undo`
        (`hash` BLOB PRIMARY KEY,
         `data` BLOB,
         `height` INTEGER);
  )", nullptr, nullptr, nullptr);
  if (rc != SQLITE_OK)
    LOG (FATAL) << "Failed to set up database schema: " << rc;
}

void
SQLiteStorage::Initialise ()
{
  StorageInterface::Initialise ();
  if (db == nullptr)
    OpenDatabase ();
}

void
SQLiteStorage::Clear ()
{
  CloseDatabase ();

  if (filename == ":memory:")
    LOG (INFO)
        << "Database with filename '" << filename << "' is temporary,"
        << " so it does not need to be explicitly removed";
  else
    {
      LOG (INFO) << "Removing file to clear database: " << filename;
      const int rc = std::remove (filename.c_str ());
      if (rc != 0)
        LOG (FATAL) << "Failed to remove file: " << rc;
    }

  OpenDatabase ();
}

bool
SQLiteStorage::GetCurrentBlockHash (const SQLiteDatabase& db, uint256& hash)
{
  auto* stmt = db.PrepareRo (R"(
    SELECT `value` FROM `xayagame_current` WHERE `key` = 'blockhash'
  )");

  const int rc = sqlite3_step (stmt);
  if (rc == SQLITE_DONE)
    return false;
  if (rc != SQLITE_ROW)
    LOG (FATAL) << "Failed to fetch current block hash: " << rc;

  const void* blob = sqlite3_column_blob (stmt, 0);
  const size_t blobSize = sqlite3_column_bytes (stmt, 0);
  CHECK_EQ (blobSize, uint256::NUM_BYTES)
      << "Invalid uint256 value stored in database";
  hash.FromBlob (static_cast<const unsigned char*> (blob));

  StepWithNoResult (stmt);
  return true;
}

bool
SQLiteStorage::GetCurrentBlockHash (uint256& hash) const
{
  return GetCurrentBlockHash (*db, hash);
}

GameStateData
SQLiteStorage::GetCurrentGameState () const
{
  auto* stmt = db->Prepare (R"(
    SELECT `value` FROM `xayagame_current` WHERE `key` = 'gamestate'
  )");

  const int rc = sqlite3_step (stmt);
  if (rc != SQLITE_ROW)
    LOG (FATAL) << "Failed to fetch current game state: " << rc;

  const GameStateData res = GetStringBlob (stmt, 0);

  StepWithNoResult (stmt);
  return res;
}

void
SQLiteStorage::SetCurrentGameState (const uint256& hash,
                                    const GameStateData& data)
{
  CHECK (startedTransaction);

  StepWithNoResult (db->Prepare ("SAVEPOINT `xayagame-setcurrentstate`"));

  sqlite3_stmt* stmt = db->Prepare (R"(
    INSERT OR REPLACE INTO `xayagame_current` (`key`, `value`)
      VALUES ('blockhash', ?1)
  )");
  BindUint256 (stmt, 1, hash);
  StepWithNoResult (stmt);

  stmt = db->Prepare (R"(
    INSERT OR REPLACE INTO `xayagame_current` (`key`, `value`)
      VALUES ('gamestate', ?1)
  )");
  BindStringBlob (stmt, 1, data);
  StepWithNoResult (stmt);

  StepWithNoResult (db->Prepare (R"(
    RELEASE `xayagame-setcurrentstate`
  )"));
}

bool
SQLiteStorage::GetUndoData (const uint256& hash, UndoData& data) const
{
  auto* stmt = db->Prepare (R"(
    SELECT `data` FROM `xayagame_undo` WHERE `hash` = ?1
  )");
  BindUint256 (stmt, 1, hash);

  const int rc = sqlite3_step (stmt);
  if (rc == SQLITE_DONE)
    return false;
  if (rc != SQLITE_ROW)
    LOG (FATAL) << "Failed to fetch undo data: " << rc;

  data = GetStringBlob (stmt, 0);

  StepWithNoResult (stmt);
  return true;
}

void
SQLiteStorage::AddUndoData (const uint256& hash,
                            const unsigned height, const UndoData& data)
{
  CHECK (startedTransaction);

  auto* stmt = db->Prepare (R"(
    INSERT OR REPLACE INTO `xayagame_undo` (`hash`, `data`, `height`)
      VALUES (?1, ?2, ?3)
  )");

  BindUint256 (stmt, 1, hash);
  BindStringBlob (stmt, 2, data);

  const int rc = sqlite3_bind_int (stmt, 3, height);
  if (rc != SQLITE_OK)
    LOG (FATAL) << "Failed to bind block height value: " << rc;

  StepWithNoResult (stmt);
}

void
SQLiteStorage::ReleaseUndoData (const uint256& hash)
{
  CHECK (startedTransaction);

  auto* stmt = db->Prepare (R"(
    DELETE FROM `xayagame_undo` WHERE `hash` = ?1
  )");

  BindUint256 (stmt, 1, hash);
  StepWithNoResult (stmt);
}

void
SQLiteStorage::PruneUndoData (const unsigned height)
{
  CHECK (startedTransaction);

  auto* stmt = db->Prepare (R"(
    DELETE FROM `xayagame_undo` WHERE `height` <= ?1
  )");

  const int rc = sqlite3_bind_int (stmt, 1, height);
  if (rc != SQLITE_OK)
    LOG (FATAL) << "Failed to bind block height value: " << rc;

  StepWithNoResult (stmt);
}

void
SQLiteStorage::BeginTransaction ()
{
  CHECK (!startedTransaction);
  startedTransaction = true;
  StepWithNoResult (db->Prepare ("SAVEPOINT `xayagame-sqlitegame`"));
}

void
SQLiteStorage::CommitTransaction ()
{
  StepWithNoResult (db->Prepare ("RELEASE `xayagame-sqlitegame`"));
  CHECK (startedTransaction);
  startedTransaction = false;
}

void
SQLiteStorage::RollbackTransaction ()
{
  StepWithNoResult (db->Prepare ("ROLLBACK TO `xayagame-sqlitegame`"));
  CHECK (startedTransaction);
  startedTransaction = false;
}

/* ************************************************************************** */

} // namespace xaya
