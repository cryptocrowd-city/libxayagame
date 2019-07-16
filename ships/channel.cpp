// Copyright (C) 2019 The Xaya developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "channel.hpp"

#include "proto/winnerstatement.pb.h"

#include <gamechannel/proto/signatures.pb.h>
#include <gamechannel/protoutils.hpp>
#include <gamechannel/signatures.hpp>
#include <xayautil/hash.hpp>

namespace ships
{

bool
ShipsChannel::IsPositionSet () const
{
  return position.GetBits () != 0;
}

void
ShipsChannel::SetPosition (const Grid& g)
{
  CHECK (!IsPositionSet ());

  if (!VerifyPositionOfShips (g))
    {
      LOG (ERROR)
          << "Cannot set " << g.GetBits () << " as position, that is invalid";
      return;
    }

  position = g;
  positionSalt = rnd.Get<xaya::uint256> ();
  LOG (INFO)
      << "Stored player position " << position.GetBits ()
      << " and generated salt: " << positionSalt.ToHex ();

  CHECK (IsPositionSet ());
}

proto::BoardMove
ShipsChannel::GetShotMove (const Coord& c) const
{
  CHECK (c.IsOnBoard ());

  proto::BoardMove res;
  res.mutable_shot ()->set_location (c.GetIndex ());

  return res;
}

proto::BoardMove
ShipsChannel::GetPositionRevealMove () const
{
  CHECK (IsPositionSet ());

  proto::BoardMove res;
  auto* reveal = res.mutable_position_reveal ();
  reveal->set_position (position.GetBits ());
  reveal->set_salt (positionSalt.GetBinaryString ());

  return res;
}

int
ShipsChannel::GetPlayerIndex (const ShipsBoardState& state) const
{
  const auto& meta = state.GetMetadata ();

  int res = -1;
  for (int i = 0; i < meta.participants_size (); ++i)
    if (meta.participants (i).name () == playerName)
      {
        CHECK_EQ (res, -1);
        res = i;
      }

  CHECK_GE (res, 0);
  CHECK_LE (res, 1);
  return res;
}

bool
ShipsChannel::AutoPositionCommitment (proto::BoardMove& mv)
{
  if (!IsPositionSet ())
    return false;

  xaya::SHA256 hasher;
  hasher << position.Blob () << positionSalt;
  const std::string hashStr = hasher.Finalise ().GetBinaryString ();

  mv.mutable_position_commitment ()->set_position_hash (hashStr);
  return true;
}

bool
ShipsChannel::InternalAutoMove (const ShipsBoardState& state,
                                proto::BoardMove& mv)
{
  const auto& id = state.GetChannelId ();
  const auto& meta = state.GetMetadata ();
  const auto& pb = state.GetState ();

  const int index = GetPlayerIndex (state);
  CHECK_EQ (index, pb.turn ());

  const auto phase = state.GetPhase ();
  switch (phase)
    {
    case ShipsBoardState::Phase::FIRST_COMMITMENT:
      {
        CHECK_EQ (index, 0);

        if (!AutoPositionCommitment (mv))
          return false;

        seed0 = rnd.Get<xaya::uint256> ();
        LOG (INFO) << "Random seed for first player: " << seed0.ToHex ();

        xaya::SHA256 seedHasher;
        seedHasher << seed0;
        const std::string seedHash = seedHasher.Finalise ().GetBinaryString ();

        mv.mutable_position_commitment ()->set_seed_hash (seedHash);
        return true;
      }

    case ShipsBoardState::Phase::SECOND_COMMITMENT:
      {
        CHECK_EQ (index, 1);

        if (!AutoPositionCommitment (mv))
          return false;

        const auto seed1 = rnd.Get<xaya::uint256> ();
        LOG (INFO) << "Random seed for second player: " << seed1.ToHex ();

        mv.mutable_position_commitment ()->set_seed (seed1.GetBinaryString ());
        return true;
      }

    case ShipsBoardState::Phase::FIRST_REVEAL_SEED:
      {
        CHECK_EQ (index, 0);
        mv.mutable_seed_reveal ()->set_seed (seed0.GetBinaryString ());
        return true;
      }

    case ShipsBoardState::Phase::SHOOT:
      {
        /* If we already hit all ships of the opponent, then we go on
           to reveal our position to ensure that we win.  */

        const int other = 1 - index;
        CHECK_GE (other, 0);
        CHECK_LE (other, 1);

        const Grid hits(pb.known_ships (other).hits ());
        if (hits.CountOnes () >= Grid::TotalShipCells ())
          {
            LOG (INFO) << "We hit all opponent ships, revealing";
            mv = GetPositionRevealMove ();
            return true;
          }

        return false;
      }

    case ShipsBoardState::Phase::ANSWER:
      {
        CHECK (IsPositionSet ());
        const Coord target(pb.current_shot ());
        CHECK (target.IsOnBoard ());

        const bool hit = position.Get (target);
        mv.mutable_reply ()->set_reply (hit ? proto::ReplyMove::HIT
                                            : proto::ReplyMove::MISS);
        return true;
      }

    case ShipsBoardState::Phase::SECOND_REVEAL_POSITION:
      {
        mv = GetPositionRevealMove ();
        return true;
      }

    case ShipsBoardState::Phase::WINNER_DETERMINED:
      {
        CHECK (pb.has_winner ());

        proto::WinnerStatement stmt;
        stmt.set_winner (pb.winner ());

        CHECK_NE (index, stmt.winner ());

        auto* data = mv.mutable_winner_statement ()->mutable_statement ();
        CHECK (stmt.SerializeToString (data->mutable_data ()));

        if (!xaya::SignDataForParticipant (wallet, id, meta, "winnerstatement",
                                           index, *data))
          {
            LOG (ERROR)
                << "Tried to send winner statement, but signature failed";
            return false;
          }

        return true;
      }

    case ShipsBoardState::Phase::FINISHED:
    default:
      LOG (FATAL)
          << "Invalid phase for auto move: " << static_cast<int> (phase);
    }
}

namespace
{

Json::Value
DisputeResolutionMove (const std::string& type,
                       const xaya::uint256& channelId,
                       const xaya::proto::StateProof& p)
{
  Json::Value data(Json::objectValue);
  data["id"] = channelId.ToHex ();
  data["state"] = xaya::ProtoToBase64 (p);

  Json::Value res(Json::objectValue);
  res[type] = data;

  return res;
}

} // anonymous namespace

Json::Value
ShipsChannel::ResolutionMove (const xaya::uint256& channelId,
                              const xaya::proto::StateProof& p) const
{
  return DisputeResolutionMove ("r", channelId, p);
}

Json::Value
ShipsChannel::DisputeMove (const xaya::uint256& channelId,
                           const xaya::proto::StateProof& p) const
{
  return DisputeResolutionMove ("d", channelId, p);
}

bool
ShipsChannel::MaybeAutoMove (const xaya::ParsedBoardState& state,
                             xaya::BoardMove& mv)
{
  const auto& shipsState = dynamic_cast<const ShipsBoardState&> (state);

  proto::BoardMove mvPb;
  if (!InternalAutoMove (shipsState, mvPb))
    return false;

  CHECK (mvPb.SerializeToString (&mv));
  return true;
}

void
ShipsChannel::MaybeOnChainMove (const xaya::ParsedBoardState& state,
                                xaya::MoveSender& sender)
{
  const auto& shipsState = dynamic_cast<const ShipsBoardState&> (state);
  const auto& id = shipsState.GetChannelId ();
  const auto& meta = shipsState.GetMetadata ();

  if (shipsState.GetPhase () != ShipsBoardState::Phase::FINISHED)
    return;

  const auto& statePb = shipsState.GetState ();
  CHECK (statePb.has_winner_statement ());
  const auto& signedStmt = statePb.winner_statement ();

  proto::WinnerStatement stmt;
  CHECK (stmt.ParseFromString (signedStmt.data ()));
  CHECK (stmt.has_winner ());
  CHECK_GE (stmt.winner (), 0);
  CHECK_LT (stmt.winner (), meta.participants_size ());
  if (meta.participants (stmt.winner ()).name () != playerName)
    return;

  LOG (INFO) << "Channel has a winner statement and we won, closing on-chain";

  Json::Value data(Json::objectValue);
  data["id"] = id.ToHex ();
  data["stmt"] = xaya::ProtoToBase64 (signedStmt);

  Json::Value mv(Json::objectValue);
  mv["w"] = data;
  sender.SendMove (mv);
}

} // namespace ships
