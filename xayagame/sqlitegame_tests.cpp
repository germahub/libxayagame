// Copyright (C) 2018 The Xaya developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sqlitegame.hpp"

#include "game.hpp"

#include "testutils.hpp"

#include <json/json.h>

#include <sqlite3.h>

#include <gtest/gtest.h>

#include <glog/logging.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace xaya
{
namespace
{

/* ************************************************************************** */

/** Game ID of the test game.  */
constexpr const char* GAME_ID = "chat";

/** The block height at which the initial state is defined.  */
constexpr unsigned GENESIS_HEIGHT = 10;

/**
 * Returns the block hash for the game's initial state.
 */
uint256
GenesisHash ()
{
  return BlockHash (GENESIS_HEIGHT);
}

/**
 * Exception thrown if an SQL operation is meant to fail for testing
 * error recovery.
 */
class Failure : public std::runtime_error
{

public:

  Failure ()
    : std::runtime_error ("failed SQL operation")
  {}

};

/**
 * Callback for sqlite3_exec that expects not to be called.
 */
static int
ExpectNoResult (void* data, int columns, char** strs, char** names)
{
  LOG (FATAL) << "Expected no result from DB query";
}

/**
 * Executes the given SQL statement on the database, expecting no results.
 * If there are results or there is any error, CHECK-fails.
 */
static void
ExecuteWithNoResult (sqlite3* db, const std::string& sql)
{
  const int rc = sqlite3_exec (db, sql.c_str (),
                               &ExpectNoResult, nullptr, nullptr);
  CHECK_EQ (rc, SQLITE_OK) << "Failed to query database";
}

/**
 * Basic SQLite game template for the test games that we use.  It implements
 * common functionality, like the initial hash/height (which can be the same
 * for all test games) and the "request to fail" flag.
 */
class TestGame : public SQLiteGame
{

protected:

  /**
   * Whether or not SQL-routines (initialisation and update of the DB-based
   * game state) should throw an exception.
   */
  bool shouldFail = false;

  void
  GetInitialStateBlock (unsigned& height, std::string& hashHex) const override
  {
    height = GENESIS_HEIGHT;
    hashHex = GenesisHash ().ToHex ();
  }

  explicit TestGame (const std::string& f)
    : SQLiteGame (f)
  {}

public:

  TestGame () = delete;
  TestGame (const TestGame&) = delete;
  void operator= (const TestGame&) = delete;

  /**
   * Sets a flag that makes SQL methods of the game (initialisation and
   * update of the DB-based state) throw an exception if true.  This can be
   * used to test error handling and atomicity of updates.
   */
  void
  SetShouldFail (const bool v)
  {
    shouldFail = v;
  }

};

/* ************************************************************************** */

/**
 * Example game using SQLite:  A simple chat "game".  The state is simply a
 * table in the database mapping the user's account name in Xaya to a string,
 * and moves are JSON-arrays of strings that update the state sequentially.
 * (This is somewhat pointless as always the last entry will prevail, but
 * it verifies that the rollback mechanism handles multiple changes to
 * a single row correctly.)
 */
class ChatGame : public TestGame
{

private:

  /**
   * Callback that builds up a State object with data from the `chat` table.
   */
  static int
  SaveToMap (void* ptr, int columns, char** strs, char** names)
  {
    State* s = static_cast<State*> (ptr);
    CHECK_EQ (columns, 2);
    CHECK_EQ (std::string (names[0]), "user");
    CHECK_EQ (std::string (names[1]), "msg");
    CHECK (s->count (strs[0]) == 0);
    s->emplace (strs[0], strs[1]);
    return 0;
  }

protected:

  void
  SetupSchema (sqlite3* db) override
  {
    ExecuteWithNoResult (db, R"(
      CREATE TABLE IF NOT EXISTS `chat`
          (`user` TEXT PRIMARY KEY,
           `msg` TEXT);
    )");
  }

  void
  InitialiseState (sqlite3* db) override
  {
    /* To verify proper intialisation, the initial state of the chat game is
       not empty but has predefined starting messages.  */

    ExecuteWithNoResult (db, R"(
      INSERT INTO `chat` (`user`, `msg`) VALUES ('domob', 'hello world')
    )");

    if (shouldFail)
      throw Failure ();

    ExecuteWithNoResult (db, R"(
      INSERT INTO `chat` (`user`, `msg`) VALUES ('foo', 'bar')
    )");
  }

  void
  UpdateState (sqlite3* db, const Json::Value& blockData) override
  {
    for (const auto& m : blockData["moves"])
      {
        const std::string name = m["name"].asString ();
        for (const auto& v : m["move"])
          {
            const std::string value = v.asString ();
            ExecuteWithNoResult (db, R"(
              INSERT OR REPLACE INTO `chat` (`user`, `msg`) VALUES
            (')" + name + "', '" + value + "')");
          }
      }

    if (shouldFail)
      throw Failure ();
  }

  Json::Value
  GetStateAsJson (sqlite3* db) override
  {
    State data;
    const int rc = sqlite3_exec (db, R"(
      SELECT `user`, `msg` FROM `chat`
    )", &SaveToMap, &data, nullptr);
    CHECK_EQ (rc, SQLITE_OK) << "Failed to retrieve current state from DB";

    Json::Value res(Json::objectValue);
    for (const auto& entry : data)
      res[entry.first] = entry.second;
    return res;
  }

public:

  /** Type holding a game state as in-memory map (for easy handling).  */
  using State = std::map<std::string, std::string>;

  /**
   * Convenience type to hold moves before they are converted to JSON using
   * the Move() function.  This is a vector instead of a map, as each player
   * can send more than one message (and it will get put into a JSON array).
   */
  using MoveSet = std::vector<std::pair<std::string, std::string>>;

  explicit ChatGame (const std::string& f)
    : TestGame (f)
  {}

  /**
   * Expects that the current game state (corresponding to the given
   * GameStateData) matches the Map object.
   */
  void
  ExpectState (const GameStateData& state, const State& s)
  {
    const Json::Value jsonState = GameStateToJson (state);
    ASSERT_TRUE (jsonState.isObject ());
    ASSERT_EQ (jsonState.size (), s.size ());
    for (const auto& entry : s)
      {
        ASSERT_TRUE (jsonState.isMember (entry.first));
        ASSERT_EQ (jsonState[entry.first].asString (), entry.second);
      }
  }

  /**
   * Builds a JSON object holding the moves represented by the MoveSet.
   */
  static Json::Value
  Moves (const MoveSet& moves)
  {
    std::map<std::string, Json::Value> perPlayer;
    for (const auto& m : moves)
      {
        if (perPlayer.count (m.first) == 0)
          perPlayer.emplace (m.first, Json::arrayValue);
        perPlayer[m.first].append (m.second);
      }

    Json::Value res(Json::arrayValue);
    for (const auto& p : perPlayer)
      {
        Json::Value obj(Json::objectValue);
        obj["name"] = p.first;
        obj["move"] = p.second;
        res.append (obj);
      }

    return res;
  }

};

/* ************************************************************************** */

/**
 * Queries the game rules for the initial state (and block hash), and
 * stores those into the storage so that we have an initialised state
 * from Game's point of view.
 */
void
InitialiseState (Game& game, SQLiteGame& rules)
{
  unsigned height;
  std::string hashHex;
  const GameStateData state = rules.GetInitialState (height, hashHex);

  uint256 hash;
  ASSERT_TRUE (hash.FromHex (hashHex));

  rules.GetStorage ()->BeginTransaction ();
  rules.GetStorage ()->SetCurrentGameState (hash, state);
  rules.GetStorage ()->CommitTransaction ();
}

template <typename G>
  class SQLiteGameTests : public GameTestWithBlockchain
{

protected:

  Game game;
  G rules;

  SQLiteGameTests ()
    : GameTestWithBlockchain(GAME_ID),
      game(GAME_ID), rules(":memory:")
  {
    SetStartingBlock (GenesisHash ());

    game.SetStorage (rules.GetStorage ());
    game.SetGameLogic (&rules);

    InitialiseState (game, rules);

    /* We don't want to use a mock Xaya server, so reinitialising the state
       won't work.  Just set it to up-to-date, which is fine after we set the
       initial state already in the storage.  */
    ForceState (game, State::UP_TO_DATE);
  }

  /**
   * Expects that the current game state (as in the Game's storage) matches
   * the given Map object.
   */
  void
  ExpectState (const typename G::State& s)
  {
    const GameStateData state = rules.GetStorage ()->GetCurrentGameState ();
    rules.ExpectState (state, s);
  }

};

/* ************************************************************************** */

using StateInitialisationTests = SQLiteGameTests<ChatGame>;

TEST_F (StateInitialisationTests, HeightAndHash)
{
  unsigned height;
  std::string hashHex;
  const GameStateData state = rules.GetInitialState (height, hashHex);
  EXPECT_EQ (height, GENESIS_HEIGHT);
  EXPECT_EQ (hashHex, GenesisHash ().ToHex ());
}

TEST_F (StateInitialisationTests, DatabaseInitialised)
{
  ExpectState ({{"domob", "hello world"}, {"foo", "bar"}});
}

TEST_F (StateInitialisationTests, MultipleRequests)
{
  ExpectState ({{"domob", "hello world"}, {"foo", "bar"}});
  ExpectState ({{"domob", "hello world"}, {"foo", "bar"}});
}

TEST_F (StateInitialisationTests, ErrorHandling)
{
  rules.SetShouldFail (true);
  try
    {
      ExpectState ({{"domob", "hello world"}, {"foo", "bar"}});
      FAIL () << "No exception was thrown";
    }
  catch (const Failure& exc)
    {}

  rules.SetShouldFail (false);
  ExpectState ({{"domob", "hello world"}, {"foo", "bar"}});
}

/* ************************************************************************** */

using GameStateStringTests = SQLiteGameTests<ChatGame>;

TEST_F (GameStateStringTests, Initial)
{
  rules.ExpectState ("initial", {{"domob", "hello world"}, {"foo", "bar"}});
}

TEST_F (GameStateStringTests, BlockHash)
{
  /* We need to call with "initial" first, so that the state gets actually
     initialised in the database.  */
  rules.ExpectState ("initial", {{"domob", "hello world"}, {"foo", "bar"}});

  rules.ExpectState ("block " + GenesisHash ().ToHex (),
                     {{"domob", "hello world"}, {"foo", "bar"}});
}

TEST_F (GameStateStringTests, InitialWrongHash)
{
  rules.GetStorage ()->BeginTransaction ();
  rules.GetStorage ()->SetCurrentGameState (BlockHash (42), "");
  rules.GetStorage ()->CommitTransaction ();
  EXPECT_DEATH (
      rules.GameStateToJson ("initial"),
      "does not match the game's initial block");
}

TEST_F (GameStateStringTests, WrongBlockHash)
{
  EXPECT_DEATH (
      rules.GameStateToJson ("block " + BlockHash (42).ToHex ()),
      "does not match claimed current game state");
}

TEST_F (GameStateStringTests, InvalidString)
{
  EXPECT_DEATH (
      rules.GameStateToJson ("foo"),
      "Unexpected game state value");
}

/* ************************************************************************** */

using MovingTests = SQLiteGameTests<ChatGame>;

TEST_F (MovingTests, ForwardAndBackward)
{
  ExpectState ({{"domob", "hello world"}, {"foo", "bar"}});

  AttachBlock (game, BlockHash (11), ChatGame::Moves ({
    {"domob", "new"},
    {"a", "x"},
    {"a", "y"},
  }));
  ExpectState ({
    {"a", "y"},
    {"domob", "new"},
    {"foo", "bar"},
  });

  AttachBlock (game, BlockHash (12), ChatGame::Moves ({{"a", "z"}}));
  ExpectState ({
    {"a", "z"},
    {"domob", "new"},
    {"foo", "bar"},
  });

  DetachBlock (game);
  ExpectState ({
    {"a", "y"},
    {"domob", "new"},
    {"foo", "bar"},
  });

  DetachBlock (game);
  ExpectState ({{"domob", "hello world"}, {"foo", "bar"}});
}

TEST_F (MovingTests, ErrorHandling)
{
  ExpectState ({{"domob", "hello world"}, {"foo", "bar"}});

  rules.SetShouldFail (true);
  try
    {
      AttachBlock (game, BlockHash (11), ChatGame::Moves ({
        {"domob", "failed"}
      }));
      FAIL () << "No exception was thrown";
    }
  catch (const Failure& exc)
    {}
  ExpectState ({{"domob", "hello world"}, {"foo", "bar"}});

  rules.SetShouldFail (false);
  AttachBlock (game, BlockHash (11), ChatGame::Moves ({
    {"domob", "new"},
    {"a", "x"},
    {"a", "y"},
  }));
  ExpectState ({
    {"a", "y"},
    {"domob", "new"},
    {"foo", "bar"},
  });
}

/* ************************************************************************** */

class PersistenceTests : public GameTestWithBlockchain
{

private:

  /** The filename of the temporary on-disk database.  */
  std::string filename;

  /** Resettable game logic.  */
  std::unique_ptr<ChatGame> rules;

protected:

  /**
   * The game instance we use.  Since we can change the storage and game logic
   * on it, this doesn't need to be recreated.
   */
  Game game;

  PersistenceTests ()
    : GameTestWithBlockchain(GAME_ID),
      game(GAME_ID)
  {
    filename = std::tmpnam (nullptr);
    LOG (INFO) << "Using temporary database file: " << filename;

    CreateChatGame ();

    SetStartingBlock (GenesisHash ());
    InitialiseState (game, *rules);
    ForceState (game, State::UP_TO_DATE);
  }

  ~PersistenceTests ()
  {
    rules.reset ();

    LOG (INFO) << "Cleaning up temporary file: " << filename;
    std::remove (filename.c_str ());
  }

  /**
   * Creates a fresh ChatGame instance and attaches it to the game instance.
   */
  void
  CreateChatGame ()
  {
    rules = std::make_unique<ChatGame> (filename);
    game.SetStorage (rules->GetStorage ());
    game.SetGameLogic (rules.get ());
  }

  void
  ExpectState (const ChatGame::State& s)
  {
    const GameStateData state = rules->GetStorage ()->GetCurrentGameState ();
    rules->ExpectState (state, s);
  }

};

TEST_F (PersistenceTests, KeepsData)
{
  ExpectState ({{"domob", "hello world"}, {"foo", "bar"}});

  AttachBlock (game, BlockHash (11), ChatGame::Moves ({{"domob", "new"}}));
  ExpectState ({
    {"domob", "new"},
    {"foo", "bar"},
  });

  CreateChatGame ();
  ExpectState ({
    {"domob", "new"},
    {"foo", "bar"},
  });
}

/* ************************************************************************** */

/**
 * Example game where each name that sends a move is simply inserted into
 * two database tables with a generated integer ID.  This is used to verify
 * that database rollbacks and transaction atomicity with exceptions work fine
 * for auto-generated IDs.
 */
class InsertGame : public TestGame
{

private:

  /** Type used to represent data of one of the two map tables.  */
  using Map = std::map<std::string, int>;

  /**
   * Callback that builds up a map of (name, id) pairs from the SELECT query
   * on one of the two tables.
   */
  static int
  SaveToMap (void* ptr, int columns, char** strs, char** names)
  {
    Map* m = static_cast<Map*> (ptr);
    CHECK_EQ (columns, 2);
    CHECK_EQ (std::string (names[0]), "id");
    CHECK_EQ (std::string (names[1]), "name");
    CHECK (m->count (strs[1]) == 0);
    m->emplace (strs[1], std::atoi (strs[0]));
    return 0;
  }

protected:

  void
  SetupSchema (sqlite3* db) override
  {
    ExecuteWithNoResult (db, R"(
      CREATE TABLE IF NOT EXISTS `first` (
          `id` INTEGER PRIMARY KEY,
          `name` TEXT
      );
      CREATE TABLE IF NOT EXISTS `second` (
          `id` INTEGER PRIMARY KEY,
          `name` TEXT
      );
    )");

    /* Just make sure that we can access the IDs also here.  */
    CHECK_EQ (Ids ("test").GetNext (), 1);
  }

  void
  InitialiseState (sqlite3* db) override
  {
    /* To verify proper intialisation, the initial state is not empty but
       has some pre-existing data and IDs.  */

    ExecuteWithNoResult (db, R"(
      INSERT INTO `first` (`id`, `name`) VALUES (2, 'domob');
      INSERT INTO `second` (`id`, `name`) VALUES (5, 'domob');
    )");

    Ids ("first").ReserveUpTo (2);
    Ids ("second").ReserveUpTo (9);

    /* A second call with a smaller value should still be fine and not
       change anything.  */
    Ids ("second").ReserveUpTo (4);

    /* Verify also the "test" ID range.  */
    CHECK_EQ (Ids ("test").GetNext (), 2);
  }

  void
  UpdateState (sqlite3* db, const Json::Value& blockData) override
  {
    for (const auto& m : blockData["moves"])
      {
        const std::string name = m["name"].asString ();

        std::ostringstream firstId;
        firstId << Ids ("first").GetNext ();
        std::ostringstream secondId;
        secondId << Ids ("second").GetNext ();

        ExecuteWithNoResult (db, R"(
          INSERT INTO `first` (`id`, `name`) VALUES
        ()" + firstId.str () + ", '" + name + "')");
        ExecuteWithNoResult (db, R"(
          INSERT INTO `second` (`id`, `name`) VALUES
        ()" + secondId.str () + ", '" + name + "')");
      }

    if (shouldFail)
      throw Failure ();
  }

  Json::Value
  GetStateAsJson (sqlite3* db) override
  {
    Map first;
    int rc = sqlite3_exec (db, R"(
      SELECT `id`, `name` FROM `first`
    )", &SaveToMap, &first, nullptr);
    CHECK_EQ (rc, SQLITE_OK) << "Failed to retrieve first table";

    Map second;
    rc = sqlite3_exec (db, R"(
      SELECT `id`, `name` FROM `second`
    )", &SaveToMap, &second, nullptr);
    CHECK_EQ (rc, SQLITE_OK) << "Failed to retrieve second table";
    CHECK_EQ (first.size (), second.size ());

    Json::Value res(Json::objectValue);
    for (const auto& entry : first)
      {
        const auto mitSecond = second.find (entry.first);
        CHECK (mitSecond != second.end ());

        Json::Value pair(Json::arrayValue);
        pair.append (entry.second);
        pair.append (mitSecond->second);

        res[entry.first] = pair;
      }
    return res;
  }

public:

  /**
   * Type holding a game state as in-memory map (for easy handling).  The state
   * is characterised by a map from names to the IDs in the two tables.
   */
  using State = std::map<std::string, std::pair<int, int>>;

  /**
   * Convenience type to hold moves before they are converted to JSON using
   * the Move() function.  This is just a list of player names that are
   * to be inserted.
   */
  using MoveSet = std::vector<std::string>;

  explicit InsertGame (const std::string& f)
    : TestGame (f)
  {}

  /**
   * Expects that the current game state (corresponding to the given
   * GameStateData) matches the Map object.
   */
  void
  ExpectState (const GameStateData& state, const State& s)
  {
    const Json::Value jsonState = GameStateToJson (state);
    ASSERT_TRUE (jsonState.isObject ());
    ASSERT_EQ (jsonState.size (), s.size ());
    for (const auto& entry : s)
      {
        ASSERT_TRUE (jsonState.isMember (entry.first));

        const auto& pair = jsonState[entry.first];
        ASSERT_TRUE (pair.isArray ());
        ASSERT_EQ (pair.size (), 2);
        EXPECT_EQ (pair[0].asInt (), entry.second.first);
        EXPECT_EQ (pair[1].asInt (), entry.second.second);;
      }
  }

  /**
   * Builds a JSON object holding the moves represented by the MoveSet.
   */
  static Json::Value
  Moves (const MoveSet& moves)
  {
    Json::Value res(Json::arrayValue);
    for (const auto& m : moves)
      {
        Json::Value obj(Json::objectValue);
        obj["name"] = m;
        obj["move"] = true;
        res.append (obj);
      }

    return res;
  }

};

using GeneratedIdTests = SQLiteGameTests<InsertGame>;

TEST_F (GeneratedIdTests, ForwardAndBackward)
{
  ExpectState ({{"domob", {2, 5}}});

  AttachBlock (game, BlockHash (11), InsertGame::Moves ({"foo", "bar"}));
  ExpectState ({
    {"domob", {2, 5}},
    {"foo", {3, 10}},
    {"bar", {4, 11}},
  });

  DetachBlock (game);
  ExpectState ({{"domob", {2, 5}}});

  AttachBlock (game, BlockHash (11), InsertGame::Moves ({"foo", "baz"}));
  ExpectState ({
    {"domob", {2, 5}},
    {"foo", {3, 10}},
    {"baz", {4, 11}},
  });

  AttachBlock (game, BlockHash (11), InsertGame::Moves ({"abc"}));
  ExpectState ({
    {"domob", {2, 5}},
    {"foo", {3, 10}},
    {"baz", {4, 11}},
    {"abc", {5, 12}},
  });
}

TEST_F (GeneratedIdTests, ErrorHandling)
{
  ExpectState ({{"domob", {2, 5}}});

  rules.SetShouldFail (true);
  try
    {
      AttachBlock (game, BlockHash (11), InsertGame::Moves ({"foo", "bar"}));
      FAIL () << "No exception was thrown";
    }
  catch (const Failure& exc)
    {}
  ExpectState ({{"domob", {2, 5}}});

  rules.SetShouldFail (false);
  AttachBlock (game, BlockHash (11), InsertGame::Moves ({"foo", "bar"}));
  ExpectState ({
    {"domob", {2, 5}},
    {"foo", {3, 10}},
    {"bar", {4, 11}},
  });
}

/* ************************************************************************** */

} // anonymous namespace
} // namespace xaya
