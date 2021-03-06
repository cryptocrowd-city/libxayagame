// Copyright (C) 2018-2020 The Xaya developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sqlitegame.hpp"

#include <glog/logging.h>

#include <cstring>

namespace xaya
{

/* ************************************************************************** */

/**
 * Definition for ActiveAutoIds of SQLiteGame.  This class holds a set of
 * currently-active AutoId instances together with their string keys.  It
 * also manages the construction and destruction through RAII.
 *
 * The object also manages the activeIds reference in an owning
 * SQLiteGame instance:  It is set to "this" when constructed and
 * reset to null when destructed.
 */
class SQLiteGame::ActiveAutoIds
{

private:

  /** Reference to owning SQLiteGame instance.  */
  SQLiteGame& game;

  /** The map of AutoId values.  */
  std::map<std::string, std::unique_ptr<AutoId>> instances;

public:

  explicit ActiveAutoIds (SQLiteGame& g);
  ~ActiveAutoIds ();

  ActiveAutoIds () = delete;
  ActiveAutoIds (const ActiveAutoIds&) = delete;
  void operator= (const ActiveAutoIds&) = delete;

  AutoId& Get (const std::string& key);

};

SQLiteGame::ActiveAutoIds::ActiveAutoIds (SQLiteGame& g)
  : game(g)
{
  CHECK (g.activeIds == nullptr);
  g.activeIds = this;
}

SQLiteGame::ActiveAutoIds::~ActiveAutoIds ()
{
  CHECK (game.activeIds == this);
  game.activeIds = nullptr;

  for (auto& entry : instances)
    entry.second->Sync (game, entry.first);
}

SQLiteGame::AutoId&
SQLiteGame::ActiveAutoIds::Get (const std::string& key)
{
  const auto mit = instances.find (key);
  if (mit != instances.end ())
    return *mit->second;

  std::unique_ptr<AutoId> newId(new AutoId (game, key));
  const auto res = instances.emplace (key, std::move (newId));
  CHECK (res.second);
  return *res.first->second;
}

/* ************************************************************************** */

namespace
{

/** Keyword string for the initial game state.  */
constexpr const char* INITIAL_STATE = "initial";

/** Prefix for the block hash "game state" keywords.  */
constexpr const char* BLOCKHASH_STATE = "block ";

} // anonymous namespace

/**
 * Definition of SQLiteGame::Storage.  It is a basic subclass of SQLiteStorage
 * that has a reference to the SQLiteGame, so that it can call SetupSchema
 * there when the database is opened.
 */
class SQLiteGame::Storage : public SQLiteStorage
{

private:

  /** Reference to the SQLiteGame that is using this instance.  */
  SQLiteGame& game;

  /**
   * Checks whether the game-state is marked as "initialised" in the
   * internal bookkeeping table.
   */
  static bool IsGameInitialised (const SQLiteDatabase& db);

  /**
   * Initialises the game state in the database by calling InitialiseState
   * on the underlying game.
   */
  void InitialiseGame ();

  /**
   * Verifies that the database state corresponds to the given "current state"
   * from libxayagame.  Returns false if not.
   */
  bool CheckCurrentState (const SQLiteDatabase& db,
                          const GameStateData& state) const;

  friend class SQLiteGame;

protected:

  void
  SetupSchema () override
  {
    SQLiteStorage::SetupSchema ();

    auto& db = GetDatabase ();
    const int rc = sqlite3_exec (*db, R"(
      CREATE TABLE IF NOT EXISTS `xayagame_gamevars`
          (`onlyonerow` INTEGER PRIMARY KEY,
           `gamestate_initialised` INTEGER);
      INSERT OR IGNORE INTO `xayagame_gamevars`
          (`onlyonerow`, `gamestate_initialised`) VALUES (1, 0);

      CREATE TABLE IF NOT EXISTS `xayagame_autoids` (
          `key` TEXT PRIMARY KEY,
          `nextid` INTEGER
      );
    )", nullptr, nullptr, nullptr);
    CHECK_EQ (rc, SQLITE_OK) << "Failed to set up SQLiteGame's database schema";

    /* Since we use the session extension to handle rollbacks, only the main
       database should be used.  To enforce this (at least partially), disallow
       any attached databases.  */
    sqlite3_limit (*db, SQLITE_LIMIT_ATTACHED, 0);
    LOG (INFO) << "Set allowed number of attached databases to zero";

    if (game.messForDebug)
      {
        CHECK_EQ (sqlite3_exec (*db, R"(
          PRAGMA `reverse_unordered_selects` = 1;
        )", nullptr, nullptr, nullptr), SQLITE_OK);
        LOG (INFO) << "Enabled mess-for-debug in the database";
      }

    ActiveAutoIds ids(game);
    game.SetupSchema (db);
  }

public:

  explicit Storage (SQLiteGame& g, const std::string& f)
    : SQLiteStorage (f), game(g)
  {}

};

bool
SQLiteGame::Storage::IsGameInitialised (const SQLiteDatabase& db)
{
  auto* stmt = db.PrepareRo (R"(
    SELECT `gamestate_initialised` FROM `xayagame_gamevars`
  )");

  CHECK_EQ (sqlite3_step (stmt), SQLITE_ROW)
      << "Failed to fetch result for from xayagame_gamevars";
  const int initialised = sqlite3_column_int (stmt, 0);
  StepWithNoResult (stmt);

  return initialised;
}

void
SQLiteGame::Storage::InitialiseGame ()
{
  auto& db = GetDatabase ();

  if (IsGameInitialised (db))
    {
      VLOG (1) << "Game state is already initialised in the database";
      return;
    }

  LOG (INFO) << "Setting initial state in the DB";
  StepWithNoResult (db.Prepare ("SAVEPOINT `xayagame-stateinit`"));
  try
    {
      ActiveAutoIds ids(game);
      game.InitialiseState (db);
      StepWithNoResult (db.Prepare (R"(
        UPDATE `xayagame_gamevars` SET `gamestate_initialised` = 1
      )"));
      StepWithNoResult (db.Prepare ("RELEASE `xayagame-stateinit`"));
      LOG (INFO) << "Initialised the DB state successfully";
    }
  catch (...)
    {
      LOG (ERROR) << "Initialising state failed, rolling back the DB change";
      StepWithNoResult (db.Prepare ("ROLLBACK TO `xayagame-stateinit`"));
      throw;
    }
}

bool
SQLiteGame::Storage::CheckCurrentState (const SQLiteDatabase& db,
                                        const GameStateData& state) const
{
  VLOG (1) << "Checking if current database matches game state: " << state;

  /* In any case, state-based methods of GameLogic are only ever called when
     there is already a "current state" in the storage.  */
  uint256 hash;
  if (!GetCurrentBlockHash (db, hash))
    {
      VLOG (1) << "No current block hash in the database";
      return false;
    }
  const std::string hashHex = hash.ToHex ();

  /* Handle the case of a regular block hash (no initial state).  */
  const size_t prefixLen = std::strlen (BLOCKHASH_STATE);
  if (state.substr (0, prefixLen) == BLOCKHASH_STATE)
    {
      if (hashHex != state.substr (prefixLen))
        {
          VLOG (1)
              << "Current best block in the database (" << hashHex
              << ") does not match claimed current game state";
          return false;
        }
      CHECK (IsGameInitialised (db));
      return true;
    }

  /* Verify initial state.  */
  CHECK (state == INITIAL_STATE) << "Unexpected game state value: " << state;
  unsigned height;
  std::string initialHashHex;
  game.GetInitialStateBlock (height, initialHashHex);
  if (hashHex != initialHashHex)
    {
      VLOG (1)
          << "Current best block in the database (" << hashHex
          << ") does not match the game's initial block " << initialHashHex;
      return false;
    }
  CHECK (IsGameInitialised (db));
  return true;
}

/* ************************************************************************** */

/* These cannot be =default'ed in the header, since Storage is an incomplete
   type at that stage.  */
SQLiteGame::SQLiteGame () = default;
SQLiteGame::~SQLiteGame () = default;

void
SQLiteGame::EnsureCurrentState (const GameStateData& state)
{
  CHECK (database != nullptr) << "SQLiteGame has not bee initialised";
  CHECK (database->CheckCurrentState (database->GetDatabase (), state))
      << "Game state is inconsistent to database";
}

void
SQLiteGame::Initialise (const std::string& dbFile)
{
  database = std::make_unique<Storage> (*this, dbFile);
}

void
SQLiteGame::SetupSchema (SQLiteDatabase& db)
{
  /* Nothing needs to be set up here, but subclasses probably do some setup
     in an overridden method.  The set up of the schema we need for SQLiteGame
     is done in Storage::SetupSchema already before calling here.  */
}

StorageInterface&
SQLiteGame::GetStorage ()
{
  CHECK (database != nullptr) << "SQLiteGame has not bee initialised";
  return *database;
}

GameStateData
SQLiteGame::GetInitialStateInternal (unsigned& height, std::string& hashHex)
{
  GetInitialStateBlock (height, hashHex);

  CHECK (database != nullptr) << "SQLiteGame has not bee initialised";
  database->InitialiseGame ();

  return INITIAL_STATE;
}

void
SQLiteGame::SetMessForDebug (const bool val)
{
  CHECK (database == nullptr) << "SQLiteGame has already been initialised";
  messForDebug = val;
}

namespace
{

/**
 * Helper class wrapping aroung sqlite3_session and using RAII to ensure
 * proper management of its lifecycle.
 */
class SQLiteSession
{

private:

  /** The underlying sqlite3_session handle.  */
  sqlite3_session* session = nullptr;

public:

  /**
   * Construct a new session, monitoring the "main" database on the given
   * DB connection.
   */
  explicit SQLiteSession (sqlite3* db)
  {
    VLOG (1) << "Starting SQLite session to record undo data";

    CHECK_EQ (sqlite3session_create (db, "main", &session), SQLITE_OK)
        << "Failed to start SQLite session";
    CHECK (session != nullptr);
    CHECK_EQ (sqlite3session_attach (session, nullptr), SQLITE_OK)
        << "Failed to attach all tables to the SQLite session";
  }

  ~SQLiteSession ()
  {
    if (session != nullptr)
      sqlite3session_delete (session);
  }

  /**
   * Extracts the current changeset of the session as UndoData string.
   */
  UndoData
  ExtractChangeset ()
  {
    VLOG (1) << "Extracting recorded undo data from SQLite session";
    CHECK (session != nullptr);

    int changeSize;
    void* changeBytes;
    CHECK_EQ (sqlite3session_changeset (session, &changeSize, &changeBytes),
              SQLITE_OK)
        << "Failed to extract current session changeset";

    UndoData result(static_cast<const char*> (changeBytes), changeSize);
    sqlite3_free (changeBytes);

    return result;
  }

};

} // anonymous namespace

GameStateData
SQLiteGame::ProcessForwardInternal (const GameStateData& oldState,
                                    const Json::Value& blockData,
                                    UndoData& undo)
{
  EnsureCurrentState (oldState);

  auto& db = database->GetDatabase ();
  SQLiteSession session(*db);
  {
    ActiveAutoIds ids(*this);
    UpdateState (db, blockData);
  }
  undo = session.ExtractChangeset ();

  return BLOCKHASH_STATE + blockData["block"]["hash"].asString ();
}

namespace
{

/**
 * Conflict resolution function for sqlite3changeset_apply that simply
 * tells to abort the transaction.  (If all goes correct, then conflicts should
 * never happen as we simply rollback *the last* change and are not "merging"
 * changes in any way.)
 */
int
AbortOnConflict (void* ctx, const int conflict, sqlite3_changeset_iter* it)
{
  LOG (ERROR) << "Changeset application has a conflict of type " << conflict;
  return SQLITE_CHANGESET_ABORT;
}

/**
 * Utility class to manage an inverted changeset (based on undo data
 * representing an original one).  The main use of the class is to manage
 * the associated memory using RAII.
 */
class InvertedChangeset
{

private:

  /** Size of the inverted changeset.  */
  int size;
  /** Buffer holding the data for the inverted changeset.  */
  void* data = nullptr;

public:

  /**
   * Constructs the changeset by inverting the UndoData that represents
   * the original "forward" changeset.
   */
  explicit InvertedChangeset (const UndoData& undo)
  {
    CHECK_EQ (sqlite3changeset_invert (undo.size (), &undo[0], &size, &data),
              SQLITE_OK)
        << "Failed to invert SQLite changeset";
  }

  ~InvertedChangeset ()
  {
    sqlite3_free (data);
  }

  /**
   * Applies the inverted changeset to the database handle.  If conflicts
   * appear, the transaction is aborted and the function CHECK-fails.
   */
  void
  Apply (sqlite3* db)
  {
    CHECK_EQ (sqlite3changeset_apply (db, size, data, nullptr,
                                      &AbortOnConflict, nullptr),
              SQLITE_OK)
        << "Failed to apply undo changeset";
  }

};

} // anonymous namespace

GameStateData
SQLiteGame::ProcessBackwardsInternal (const GameStateData& newState,
                                      const Json::Value& blockData,
                                      const UndoData& undo)
{
  EnsureCurrentState (newState);

  /* Note that the undo data holds the *forward* changeset, not the inverted
     one.  Thus we have to invert it here before applying.  It might seem
     more intuitive for the undo data to already hold the inverted changeset,
     but as it is expected that most undo data values are never actually
     used to roll any changes back, it is more efficient to do the inversion
     only when actually needed.  */

  InvertedChangeset changeset(undo);
  changeset.Apply (*database->GetDatabase ());

  return BLOCKHASH_STATE + blockData["block"]["parent"].asString ();
}

SQLiteGame::AutoId&
SQLiteGame::Ids (const std::string& key)
{
  CHECK (activeIds != nullptr)
      << "Ids() can only be used while the game logic is active";
  return activeIds->Get (key);
}

Json::Value
SQLiteGame::GameStateToJson (const GameStateData& state)
{
  EnsureCurrentState (state);
  return GetStateAsJson (database->GetDatabase ());
}

Json::Value
SQLiteGame::GetCustomStateData (
    const Game& game, const std::string& jsonField,
    const ExtractJsonFromDbWithBlock& cb)
{
  return game.GetCustomStateData (jsonField,
      [this, &cb] (const GameStateData& state, const uint256& hash,
                   const unsigned height, std::unique_lock<std::mutex> lock)
        {
          CHECK (database != nullptr) << "SQLiteGame has not bee initialised";

          auto snapshot = database->GetSnapshot ();
          if (snapshot != nullptr
                && database->CheckCurrentState (*snapshot, state))
            {
              /* We have a valid snapshot matching the expected block hash,
                 so we can release the main lock and extract the custom state
                 data from the snapshot instead.  */
              lock.unlock ();
              return cb (*snapshot, hash, height);
            }

          /* Otherwise keep the lock and extract from the main database
             connection instead.  This may be needed e.g. if there are
             batched and uncommitted changes on the database during initial
             catching up.  */
          LOG (WARNING) << "Using main database for GetCustomStateData";
          EnsureCurrentState (state);
          return cb (database->GetDatabase (), hash, height);
        });
}

Json::Value
SQLiteGame::GetCustomStateData (
    const Game& game, const std::string& jsonField,
    const ExtractJsonFromDb& cb)
{
  return GetCustomStateData (game, jsonField,
    [&cb] (const SQLiteDatabase& db, const uint256& hash, const unsigned height)
    {
      return cb (db);
    });
}

SQLiteDatabase&
SQLiteGame::GetDatabaseForTesting ()
{
  CHECK (database != nullptr) << "SQLiteGame has not been initialised";
  return database->GetDatabase ();
}

/* ************************************************************************** */

namespace
{

/**
 * Binds a TEXT parameter to a std::string value.  The value is bound using
 * SQLITE_STATIC, so the underlying string must remain valid until execution
 * of the prepared statement is done.
 */
void
BindString (sqlite3_stmt* stmt, const int ind, const std::string& value)
{
  const int rc = sqlite3_bind_text (stmt, ind, &value[0], value.size (),
                                    SQLITE_STATIC);
  if (rc != SQLITE_OK)
    LOG (FATAL) << "Failed to bind string value to parameter: " << rc;
}

} // anonymous namespace

SQLiteGame::AutoId::AutoId (SQLiteGame& game, const std::string& key)
{
  auto* stmt = game.database->GetDatabase ().Prepare (R"(
    SELECT `nextid` FROM `xayagame_autoids` WHERE `key` = ?1
  )");
  BindString (stmt, 1, key);

  const int rc = sqlite3_step (stmt);
  if (rc == SQLITE_DONE)
    {
      LOG (INFO) << "No next value for AutoId " << key;
      nextValue = 1;
    }
  else if (rc == SQLITE_ROW)
    {
      nextValue = sqlite3_column_int (stmt, 0);
      dbValue = nextValue;
      LOG (INFO) << "Fetched next value " << nextValue << " for AutoId " << key;
      SQLiteStorage::StepWithNoResult (stmt);
    }
  else
    LOG (FATAL) << "Error initialising AutoId " << key;

  CHECK_NE (nextValue, EMPTY_ID);
}

SQLiteGame::AutoId::~AutoId ()
{
  CHECK_EQ (dbValue, nextValue) << "AutoId has not been synced";
}

void
SQLiteGame::AutoId::Sync (SQLiteGame& game, const std::string& key)
{
  if (nextValue == dbValue)
    {
      LOG (INFO) << "No need to sync AutoId " << key;
      return;
    }

  auto* stmt = game.database->GetDatabase ().Prepare (R"(
    INSERT OR REPLACE INTO `xayagame_autoids`
      (`key`, `nextid`) VALUES (?1, ?2)
  )");
  BindString (stmt, 1, key);
  CHECK_EQ (sqlite3_bind_int (stmt, 2, nextValue), SQLITE_OK);

  SQLiteStorage::StepWithNoResult (stmt);

  LOG (INFO) << "Synced AutoId " << key << " to database";
  dbValue = nextValue;
}

/* ************************************************************************** */

const SQLiteDatabase&
SQLiteGame::PendingMoves::AccessConfirmedState () const
{
  game.EnsureCurrentState (GetConfirmedState ());
  return game.database->GetDatabase ();
}

/* ************************************************************************** */

} // namespace xaya
