// Copyright (C) 2019 The Xaya developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "signatures.hpp"

#include "xayagame/testutils.hpp"

#include <xayagame/rpc-stubs/xayarpcclient.h>
#include <xayautil/base64.hpp>
#include <xayautil/hash.hpp>

#include <jsonrpccpp/client/connectors/httpclient.h>
#include <jsonrpccpp/server/connectors/httpserver.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace xaya
{
namespace
{

using testing::Return;

class SignaturesTests : public testing::Test
{

private:

  jsonrpc::HttpServer httpServer;
  jsonrpc::HttpClient httpClient;

protected:

  MockXayaRpcServer mockXayaServer;
  XayaRpcClient rpcClient;

  SignaturesTests ()
    : httpServer(MockXayaRpcServer::HTTP_PORT),
      httpClient(MockXayaRpcServer::HTTP_URL),
      mockXayaServer(httpServer),
      rpcClient(httpClient)
  {
    mockXayaServer.StartListening ();
  }

  ~SignaturesTests ()
  {
    mockXayaServer.StopListening ();
  }

};

TEST_F (SignaturesTests, VerifyParticipantSignatures)
{
  ChannelMetadata meta;
  meta.add_participants ()->set_address ("address 1");
  meta.add_participants ()->set_address ("address 2");

  SignedData data;
  data.set_data ("foobar");
  const std::string msg = SHA256::Hash ("foobar").ToHex ();
  data.add_signatures ("signature 1");
  data.add_signatures ("signature 2");
  data.add_signatures ("signature 3");

  EXPECT_CALL (mockXayaServer,
               verifymessage ("", msg, EncodeBase64 ("signature 1")))
      .WillOnce (Return (ParseJson (R"({
        "valid": true,
        "address": "some other address"
      })")));
  EXPECT_CALL (mockXayaServer,
               verifymessage ("", msg, EncodeBase64 ("signature 2")))
      .WillOnce (Return (ParseJson (R"({
        "valid": true,
        "address": "address 2"
      })")));
  EXPECT_CALL (mockXayaServer,
               verifymessage ("", msg, EncodeBase64 ("signature 3")))
      .WillOnce (Return (ParseJson (R"({
        "valid": false
      })")));

  EXPECT_EQ (VerifyParticipantSignatures (rpcClient, meta, data),
             std::set<int> ({1}));
}

} // anonymous namespace
} // namespace xaya
