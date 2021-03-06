// Copyright (C) 2018 The Xaya developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "defaultmain.hpp"

#include "game.hpp"
#include "gamerpcserver.hpp"
#include "lmdbstorage.hpp"
#include "sqlitestorage.hpp"

#include <jsonrpccpp/client/connectors/httpclient.h>
#include <jsonrpccpp/server/connectors/tcpsocketserver.h>
#include <jsonrpccpp/server/connectors/httpserver.h>

#include <glog/logging.h>

#include <experimental/filesystem>

#include <cstdlib>
#include <exception>
#include <memory>

namespace xaya
{

namespace
{

namespace fs = std::experimental::filesystem;

/**
 * Sets up a StorageInterface instance according to the configuration.
 */
std::unique_ptr<StorageInterface>
CreateStorage (const GameDaemonConfiguration& config,
               const std::string& gameId, const Chain chain)
{
  if (config.StorageType == "memory")
    return std::make_unique<MemoryStorage> ();

  CHECK (!config.DataDirectory.empty ())
      << "DataDirectory must be set if non-memory storage is used";
  const fs::path gameDir
      = fs::path (config.DataDirectory)
          / fs::path (gameId)
          / fs::path (ChainToString (chain));

  if (fs::is_directory (gameDir))
    LOG (INFO) << "Using existing data directory: " << gameDir;
  else
    {
      LOG (INFO) << "Creating data directory: " << gameDir;
      CHECK (fs::create_directories (gameDir));
    }

  if (config.StorageType == "lmdb")
    {
      const fs::path lmdbDir = gameDir / fs::path ("lmdb");
      if (!fs::is_directory (lmdbDir))
        {
          LOG (INFO) << "Creating directory for LMDB environment: " << lmdbDir;
          CHECK (fs::create_directories (lmdbDir));
        }
      return std::make_unique<LMDBStorage> (lmdbDir.string ());
    }

  if (config.StorageType == "sqlite")
    {
      const fs::path dbFile = gameDir / fs::path ("storage.sqlite");
      return std::make_unique<SQLiteStorage> (dbFile.string ());
    }

  LOG (FATAL) << "Invalid storage type selected: " << config.StorageType;
}

/**
 * Constructs the server connector for the JSON-RPC server (if any) based
 * on the configuration.
 */
std::unique_ptr<jsonrpc::AbstractServerConnector>
CreateRpcServerConnector (const GameDaemonConfiguration& config)
{
  switch (config.GameRpcServer)
    {
    case RpcServerType::NONE:
      return nullptr;

    case RpcServerType::HTTP:
      CHECK (config.GameRpcPort != 0)
          << "GameRpcPort must be specified for HTTP server type";
      LOG (INFO)
          << "Starting JSON-RPC HTTP server at port " << config.GameRpcPort;
      return std::make_unique<jsonrpc::HttpServer> (config.GameRpcPort);

    case RpcServerType::TCP:
      CHECK (config.GameRpcPort != 0)
          << "GameRpcPort must be specified for TCP server type";
      LOG (INFO)
          << "Starting JSON-RPC TCP server at port " << config.GameRpcPort;
      return std::make_unique<jsonrpc::TcpSocketServer> ("127.0.0.1",
                                                         config.GameRpcPort);
    }

  LOG (FATAL)
      << "Invalid GameRpcServer value: "
      << static_cast<int> (config.GameRpcServer);
}

} // anonymous namespace

int
DefaultMain (const GameDaemonConfiguration& config, const std::string& gameId,
             GameLogic& rules)
{
  try
    {
      CHECK (!config.XayaRpcUrl.empty ()) << "XayaRpcUrl must be configured";
      const std::string jsonRpcUrl(config.XayaRpcUrl);
      jsonrpc::HttpClient httpConnector(jsonRpcUrl);

      auto game = std::make_unique<Game> (gameId);
      game->ConnectRpcClient (httpConnector);
      CHECK (game->DetectZmqEndpoint ());

      std::unique_ptr<StorageInterface> storage
          = CreateStorage (config, gameId, game->GetChain ());
      game->SetStorage (storage.get ());

      game->SetGameLogic (&rules);

      if (config.EnablePruning >= 0)
        game->EnablePruning (config.EnablePruning);

      auto serverConnector = CreateRpcServerConnector (config);
      std::unique_ptr<GameRpcServer> rpcServer;
      if (serverConnector == nullptr)
          LOG (WARNING)
              << "No connector has been set up for the game RPC server,"
                 " no RPC interface will be available";
      else
          rpcServer = std::make_unique<GameRpcServer> (*game, *serverConnector);

      if (rpcServer != nullptr)
        rpcServer->StartListening ();
      game->Run ();
      if (rpcServer != nullptr)
        rpcServer->StopListening ();

      /* We need to make sure that the Game instance is destructed before the
         storage is.  That is necessary, since destructing the Game instance
         may still cause some batched transactions to be flushed, and this
         needs the storage intact.  */
      game.reset ();
    }
  catch (const std::exception& exc)
    {
      LOG (FATAL) << "Exception caught: " << exc.what ();
    }
  catch (...)
    {
      LOG (FATAL) << "Unknown exception caught";
    }

  return EXIT_SUCCESS;
}

namespace
{

class CallbackGameLogic : public GameLogic
{

private:

  /** The callback pointers.  */
  const GameLogicCallbacks& callbacks;

public:

  explicit CallbackGameLogic (const GameLogicCallbacks& cb)
    : callbacks(cb)
  {}

  GameStateData
  GetInitialState (unsigned& height, std::string& hashHex)
  {
    CHECK (callbacks.GetInitialState != nullptr);
    return callbacks.GetInitialState (GetChain (), height, hashHex);
  }

  GameStateData
  ProcessForward (const GameStateData& oldState, const Json::Value& blockData,
                  UndoData& undoData) override
  {
    CHECK (callbacks.ProcessForward != nullptr);
    return callbacks.ProcessForward (GetChain (), oldState,
                                     blockData, undoData);
  }

  GameStateData
  ProcessBackwards (const GameStateData& oldState, const Json::Value& blockData,
                    const UndoData& undoData) override
  {
    CHECK (callbacks.ProcessBackwards != nullptr);
    return callbacks.ProcessBackwards (GetChain (), oldState,
                                       blockData, undoData);
  }

  Json::Value
  GameStateToJson (const GameStateData& state) override
  {
    if (callbacks.GameStateToJson != nullptr)
      return callbacks.GameStateToJson (state);
    return GameLogic::GameStateToJson (state);
  }

};

} // anonymous namespace

int
DefaultMain (const GameDaemonConfiguration& config, const std::string& gameId,
             const GameLogicCallbacks& callbacks)
{
  CallbackGameLogic rules(callbacks);
  return DefaultMain (config, gameId, rules);
}

} // namespace xaya
