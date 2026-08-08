/*
 * FreeForAllSimultaneousTurnsTest.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 */

#include "StdInc.h"

#include "../game/GameStateTest.h"

#include "../../lib/CPlayerState.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapping/CMap.h"
#include "../../lib/networkPacks/PacksForClient.h"
#include "../../lib/networkPacks/PacksForServer.h"
#include "../../lib/serializer/CMemorySerializer.h"
#include "../../server/CGameHandler.h"
#include "../../server/IGameServer.h"
#include "../../server/battles/BattleProcessor.h"
#include "../../server/processors/TurnOrderProcessor.h"
#include "../../server/queries/QueriesProcessor.h"

namespace
{

constexpr PlayerColor RED_PLAYER{0};
constexpr PlayerColor BLUE_PLAYER{1};

class ApplyingGameServer final : public IGameServer
{
public:
	void setGameState(CGameState * value)
	{
		gameState = value;
	}

	void setState(EServerState value) override
	{
		state = value;
	}

	EServerState getState() const override
	{
		return state;
	}

	bool isPlayerHost(const PlayerColor & color) const override
	{
		return false;
	}

	bool hasPlayerAt(PlayerColor player, GameConnectionID connectionID) const override
	{
		return false;
	}

	bool hasBothPlayersAtSameConnection(PlayerColor left, PlayerColor right) const override
	{
		return false;
	}

	void clearPackageResult()
	{
		lastPackageResult.reset();
	}

	const std::optional<bool> & packageResult() const
	{
		return lastPackageResult;
	}

	void applyPack(CPackForClient & pack) override
	{
		gameState->apply(pack);
	}

	void sendPack(CPackForClient & pack, GameConnectionID connectionID) override
	{
		if(const auto * packageApplied = dynamic_cast<const PackageApplied *>(&pack))
			lastPackageResult = packageApplied->result;
	}

private:
	EServerState state = EServerState::GAMEPLAY;
	CGameState * gameState = nullptr;
	std::optional<bool> lastPackageResult;
};

class FreeForAllSimultaneousTurnsTest : public GameStateTest
{
protected:
	void SetUp() override
	{
		GameStateTest::SetUp();
		startTestGame();

		server.setGameState(gameState.get());
		gameHandler = std::make_unique<CGameHandler>(server, gameState);

		auto & simturnsInfo = gameState->getStartInfo()->simturnsInfo;
		simturnsInfo.requiredTurns = 0;
		simturnsInfo.optionalTurns = 999;
	}

	CGHeroInstance * hero(PlayerColor owner) const
	{
		for(const auto heroID : map->getHeroesOnMap())
		{
			auto * current = dynamic_cast<CGHeroInstance *>(map->getObject(heroID));
			if(current && current->getOwner() == owner)
				return current;
		}

		return nullptr;
	}

	void startTurns(bool ignorePlayerContacts)
	{
		gameState->getStartInfo()->simturnsInfo.ignorePlayerContacts = ignorePlayerContacts;
		gameHandler->turnOrder->addPlayer(RED_PLAYER);
		gameHandler->turnOrder->addPlayer(BLUE_PLAYER);
		gameHandler->turnOrder->onGameStarted();
	}

	bool moveOneTile(CGHeroInstance * movingHero, const int3 & destination)
	{
		return gameHandler->moveHero(
			movingHero->id,
			movingHero->convertFromVisitablePos(destination),
			EMovementMode::STANDARD,
			false,
			movingHero->getOwner());
	}

	bool startHeroBattle(PlayerColor attackerOwner, PlayerColor defenderOwner)
	{
		auto * attacker = hero(attackerOwner);
		auto * defender = hero(defenderOwner);
		if(!attacker || !defender)
			return false;

		while(!attacker->visitablePos().areNeighbours(defender->visitablePos()))
		{
			int3 destination = attacker->visitablePos();
			if(destination.x != defender->visitablePos().x)
				destination.x += destination.x < defender->visitablePos().x ? 1 : -1;
			else
				destination.y += destination.y < defender->visitablePos().y ? 1 : -1;

			if(!moveOneTile(attacker, destination))
				return false;
		}

		return moveOneTile(attacker, defender->visitablePos());
	}

	bool recruitReserveHero(PlayerColor owner)
	{
		const auto availableHeroes = map->getHeroesInPool();
		if(availableHeroes.empty())
			return false;

		const auto reserveHeroID = availableHeroes.front();
		const auto * reserveHero = map->tryGetFromHeroPool(reserveHeroID);
		if(!reserveHero)
			return false;

		HeroRecruited recruited;
		recruited.hid = reserveHeroID;
		recruited.tid = ObjectInstanceID::NONE;
		recruited.boatId = ObjectInstanceID::NONE;
		recruited.tile = reserveHero->convertFromVisitablePos(int3(1, 6, 0));
		recruited.player = owner;
		gameHandler->sendAndApply(recruited);

		return gameState->getPlayerState(owner)->getHeroes().size() == 2;
	}

	ApplyingGameServer server;
	std::unique_ptr<CGameHandler> gameHandler;
};

TEST_F(FreeForAllSimultaneousTurnsTest, enemyThreatRangesDoNotEndSimultaneousTurns)
{
	ASSERT_EQ(gameState->getPlayerRelations(RED_PLAYER, BLUE_PLAYER), PlayerRelations::ENEMIES);

	startTurns(true);

	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));
}

TEST_F(FreeForAllSimultaneousTurnsTest, contactDetectionCanStillBeEnabledForCompatibility)
{
	ASSERT_EQ(gameState->getPlayerRelations(RED_PLAYER, BLUE_PLAYER), PlayerRelations::ENEMIES);

	startTurns(false);

	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
	EXPECT_FALSE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));
}

TEST_F(FreeForAllSimultaneousTurnsTest, maximumDurationStillRestoresSequentialTurns)
{
	gameState->day = 1;
	gameState->getStartInfo()->simturnsInfo.optionalTurns = 0;

	startTurns(true);

	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
	EXPECT_FALSE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));
}

TEST_F(FreeForAllSimultaneousTurnsTest, enemiesCanAlternateNormalMovementDuringSharedTurn)
{
	startTurns(true);

	auto * redHero = hero(RED_PLAYER);
	auto * blueHero = hero(BLUE_PLAYER);
	ASSERT_NE(redHero, nullptr);
	ASSERT_NE(blueHero, nullptr);

	const int3 redStart = redHero->visitablePos();
	const int3 blueStart = blueHero->visitablePos();
	const int3 redDestination = redStart + int3(0, 1, 0);
	const int3 blueDestination = blueStart + int3(0, 1, 0);

	ASSERT_TRUE(gameHandler->moveHero(
		redHero->id,
		redHero->convertFromVisitablePos(redDestination),
		EMovementMode::STANDARD,
		false,
		RED_PLAYER));
	ASSERT_TRUE(gameHandler->moveHero(
		blueHero->id,
		blueHero->convertFromVisitablePos(blueDestination),
		EMovementMode::STANDARD,
		false,
		BLUE_PLAYER));

	EXPECT_EQ(redHero->visitablePos(), redDestination);
	EXPECT_EQ(blueHero->visitablePos(), blueDestination);
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));
}

TEST_F(FreeForAllSimultaneousTurnsTest, sharedTurnsRestartForEnemiesOnFollowingDay)
{
	startTurns(true);

	ASSERT_TRUE(gameHandler->turnOrder->onPlayerEndsTurn(RED_PLAYER));
	EXPECT_FALSE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));

	ASSERT_TRUE(gameHandler->turnOrder->onPlayerEndsTurn(BLUE_PLAYER));

	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));
}

TEST_F(FreeForAllSimultaneousTurnsTest, enemyHeroInteractionStartsBattleAndUsesExistingBattleLock)
{
	startTurns(true);

	auto * attacker = hero(RED_PLAYER);
	auto * defender = hero(BLUE_PLAYER);
	ASSERT_NE(attacker, nullptr);
	ASSERT_NE(defender, nullptr);
	ASSERT_EQ(gameState->getPlayerRelations(attacker->getOwner(), defender->getOwner()), PlayerRelations::ENEMIES);

	ASSERT_TRUE(startHeroBattle(RED_PLAYER, BLUE_PLAYER));

	const auto * battle = gameState->getBattle(RED_PLAYER);
	ASSERT_NE(battle, nullptr);
	EXPECT_EQ(gameState->getBattle(BLUE_PLAYER), battle);
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));

	MoveHero redMovement;
	MoveHero blueMovement;
	MakeAction battleAction;
	EXPECT_TRUE(gameHandler->isBlockedByQueries(&redMovement, RED_PLAYER));
	EXPECT_TRUE(gameHandler->isBlockedByQueries(&blueMovement, BLUE_PLAYER));
	EXPECT_FALSE(gameHandler->isBlockedByQueries(&battleAction, RED_PLAYER));
	EXPECT_FALSE(gameHandler->isBlockedByQueries(&battleAction, BLUE_PLAYER));
	EXPECT_FALSE(gameHandler->turnOrder->onPlayerEndsTurn(RED_PLAYER));
	EXPECT_FALSE(gameHandler->turnOrder->onPlayerEndsTurn(BLUE_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));
}

TEST_F(FreeForAllSimultaneousTurnsTest, adventurePacketsAreRejectedForBothPlayersDuringBattle)
{
	startTurns(true);
	ASSERT_TRUE(startHeroBattle(RED_PLAYER, BLUE_PLAYER));

	for(const auto player : { RED_PLAYER, BLUE_PLAYER })
	{
		MoveHero movement;
		movement.player = player;
		movement.hid = hero(player)->id;

		server.clearPackageResult();
		gameHandler->handleReceivedPack(GameConnectionID::FIRST_CONNECTION, movement);

		ASSERT_TRUE(server.packageResult().has_value());
		EXPECT_FALSE(*server.packageResult());
	}
}

TEST_F(FreeForAllSimultaneousTurnsTest, sameDestinationConflictStartsBattleInRequestOrder)
{
	startTurns(true);

	auto * redHero = hero(RED_PLAYER);
	auto * blueHero = hero(BLUE_PLAYER);
	ASSERT_NE(redHero, nullptr);
	ASSERT_NE(blueHero, nullptr);
	ASSERT_EQ(redHero->visitablePos().y, blueHero->visitablePos().y);
	ASSERT_EQ(std::abs(redHero->visitablePos().x - blueHero->visitablePos().x), 4);

	const int direction = redHero->visitablePos().x < blueHero->visitablePos().x ? 1 : -1;
	ASSERT_TRUE(moveOneTile(redHero, redHero->visitablePos() + int3(direction, 0, 0)));
	ASSERT_TRUE(moveOneTile(blueHero, blueHero->visitablePos() + int3(-direction, 0, 0)));

	const int3 contestedTile = redHero->visitablePos() + int3(direction, 0, 0);
	ASSERT_TRUE(contestedTile.areNeighbours(redHero->visitablePos()));
	ASSERT_TRUE(contestedTile.areNeighbours(blueHero->visitablePos()));

	ASSERT_TRUE(moveOneTile(redHero, contestedTile));
	const int3 bluePositionBeforeBattle = blueHero->visitablePos();
	ASSERT_TRUE(moveOneTile(blueHero, contestedTile));

	const auto * battle = gameState->getBattle(RED_PLAYER);
	ASSERT_NE(battle, nullptr);
	EXPECT_EQ(gameState->getBattle(BLUE_PLAYER), battle);
	EXPECT_EQ(redHero->visitablePos(), contestedTile);
	EXPECT_EQ(blueHero->visitablePos(), bluePositionBeforeBattle);
}

TEST_F(FreeForAllSimultaneousTurnsTest, battleResolutionClearsLocksAndPreservesSharedTurn)
{
	ASSERT_TRUE(recruitReserveHero(BLUE_PLAYER));
	startTurns(true);
	ASSERT_TRUE(startHeroBattle(RED_PLAYER, BLUE_PLAYER));

	ASSERT_NE(gameState->getBattle(RED_PLAYER), nullptr);
	ASSERT_NE(gameHandler->queries->topQuery(RED_PLAYER), nullptr);
	ASSERT_NE(gameHandler->queries->topQuery(BLUE_PLAYER), nullptr);

	gameHandler->battles->cheatBattleVictory(RED_PLAYER);

	EXPECT_EQ(gameState->getBattle(RED_PLAYER), nullptr);
	EXPECT_EQ(gameState->getBattle(BLUE_PLAYER), nullptr);
	EXPECT_EQ(gameHandler->queries->topQuery(RED_PLAYER), nullptr);
	EXPECT_EQ(gameHandler->queries->topQuery(BLUE_PLAYER), nullptr);
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));

	MoveHero movement;
	EXPECT_FALSE(gameHandler->isBlockedByQueries(&movement, RED_PLAYER));
	EXPECT_FALSE(gameHandler->isBlockedByQueries(&movement, BLUE_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->onPlayerEndsTurn(RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));
}

TEST(FreeForAllSimultaneousTurnsSerializationTest, preservesContactPolicyAcrossSaveCopy)
{
	SimturnsInfo source;
	source.requiredTurns = 3;
	source.optionalTurns = 999;
	source.allowHumanWithAI = true;
	source.ignorePlayerContacts = true;

	CMemorySerializer serializer;
	serializer.oser & source;

	SimturnsInfo restored;
	serializer.iser & restored;

	EXPECT_EQ(restored, source);
}

}
