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
constexpr PlayerColor TAN_PLAYER{2};

GameConnectionID connectionFor(PlayerColor player)
{
	return static_cast<GameConnectionID>(player.getNum() + 1);
}

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
		return connectionID == connectionFor(player);
	}

	bool hasBothPlayersAtSameConnection(PlayerColor left, PlayerColor right) const override
	{
		return connectionFor(left) == connectionFor(right);
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
public:
	explicit FreeForAllSimultaneousTurnsTest(const std::string & mapPath = "test/MiniTest/")
		: GameStateTest(mapPath)
	{
	}

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

class ThreePlayerFreeForAllSimultaneousTurnsTest : public FreeForAllSimultaneousTurnsTest
{
public:
	ThreePlayerFreeForAllSimultaneousTurnsTest()
		: FreeForAllSimultaneousTurnsTest("test/ThreePlayerSimturns/")
	{
	}

protected:
	void startThreePlayerTurns()
	{
		gameState->getStartInfo()->simturnsInfo.ignorePlayerContacts = true;
		gameHandler->turnOrder->addPlayer(RED_PLAYER);
		gameHandler->turnOrder->addPlayer(BLUE_PLAYER);
		gameHandler->turnOrder->addPlayer(TAN_PLAYER);
		gameHandler->turnOrder->onGameStarted();
		gameHandler->onAdvInterfaceReady(RED_PLAYER);
		gameHandler->onAdvInterfaceReady(BLUE_PLAYER);
		gameHandler->onAdvInterfaceReady(TAN_PLAYER);
	}
};

TEST_F(ThreePlayerFreeForAllSimultaneousTurnsTest, allEnemiesStartTheirTurnTogether)
{
	ASSERT_EQ(gameState->getPlayerRelations(RED_PLAYER, BLUE_PLAYER), PlayerRelations::ENEMIES);
	ASSERT_EQ(gameState->getPlayerRelations(RED_PLAYER, TAN_PLAYER), PlayerRelations::ENEMIES);
	ASSERT_EQ(gameState->getPlayerRelations(BLUE_PLAYER, TAN_PLAYER), PlayerRelations::ENEMIES);

	startThreePlayerTurns();

	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(TAN_PLAYER));
}

TEST_F(ThreePlayerFreeForAllSimultaneousTurnsTest, battleLocksOnlyItsParticipants)
{
	startThreePlayerTurns();
	ASSERT_TRUE(startHeroBattle(RED_PLAYER, BLUE_PLAYER));

	const auto * battle = gameState->getBattle(RED_PLAYER);
	ASSERT_NE(battle, nullptr);
	EXPECT_EQ(gameState->getBattle(BLUE_PLAYER), battle);
	EXPECT_EQ(gameState->getBattle(TAN_PLAYER), nullptr);

	MoveHero probeMovement;
	EXPECT_TRUE(gameHandler->isBlockedByQueries(&probeMovement, RED_PLAYER));
	EXPECT_TRUE(gameHandler->isBlockedByQueries(&probeMovement, BLUE_PLAYER));
	EXPECT_FALSE(gameHandler->isBlockedByQueries(&probeMovement, TAN_PLAYER));

	auto * tanHero = hero(TAN_PLAYER);
	ASSERT_NE(tanHero, nullptr);
	const int3 tanDestination = tanHero->visitablePos() + int3(0, -1, 0);
	MoveHero networkMovement(
		{tanHero->convertFromVisitablePos(tanDestination)},
		EPathfindingLayer::LAND,
		tanHero->id,
		false);
	networkMovement.player = TAN_PLAYER;
	server.clearPackageResult();
	gameHandler->handleReceivedPack(connectionFor(TAN_PLAYER), networkMovement);
	ASSERT_TRUE(server.packageResult().has_value());
	EXPECT_TRUE(*server.packageResult());
	EXPECT_EQ(tanHero->visitablePos(), tanDestination);
	EXPECT_TRUE(gameHandler->turnOrder->onPlayerEndsTurn(TAN_PLAYER));
	EXPECT_FALSE(gameHandler->turnOrder->isPlayerMakingTurn(TAN_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));
}

TEST_F(ThreePlayerFreeForAllSimultaneousTurnsTest, laterColorBattleAlsoLocksOnlyItsParticipants)
{
	startThreePlayerTurns();
	ASSERT_TRUE(startHeroBattle(BLUE_PLAYER, TAN_PLAYER));

	const auto * battle = gameState->getBattle(BLUE_PLAYER);
	ASSERT_NE(battle, nullptr);
	EXPECT_EQ(gameState->getBattle(TAN_PLAYER), battle);
	EXPECT_EQ(gameState->getBattle(RED_PLAYER), nullptr);

	MoveHero movement;
	EXPECT_TRUE(gameHandler->isBlockedByQueries(&movement, BLUE_PLAYER));
	EXPECT_TRUE(gameHandler->isBlockedByQueries(&movement, TAN_PLAYER));
	EXPECT_FALSE(gameHandler->isBlockedByQueries(&movement, RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
}

TEST_F(ThreePlayerFreeForAllSimultaneousTurnsTest, sharedTurnsRestartAfterPlayersFinishOutOfOrder)
{
	startThreePlayerTurns();
	const int startingDay = gameState->day;

	ASSERT_TRUE(gameHandler->turnOrder->onPlayerEndsTurn(TAN_PLAYER));
	ASSERT_TRUE(gameHandler->turnOrder->onPlayerEndsTurn(RED_PLAYER));
	ASSERT_TRUE(gameHandler->turnOrder->onPlayerEndsTurn(BLUE_PLAYER));

	EXPECT_EQ(gameState->day, startingDay + 1);
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(TAN_PLAYER));
}

TEST_F(ThreePlayerFreeForAllSimultaneousTurnsTest, thirdPlayerCannotJoinOccupiedBattleButCanMoveElsewhere)
{
	startThreePlayerTurns();
	ASSERT_TRUE(startHeroBattle(RED_PLAYER, BLUE_PLAYER));

	auto * redHero = hero(RED_PLAYER);
	auto * tanHero = hero(TAN_PLAYER);
	ASSERT_NE(redHero, nullptr);
	ASSERT_NE(tanHero, nullptr);

	while(!tanHero->visitablePos().areNeighbours(redHero->visitablePos()))
	{
		int3 destination = tanHero->visitablePos();
		if(destination.y != redHero->visitablePos().y)
			destination.y += destination.y < redHero->visitablePos().y ? 1 : -1;
		else
			destination.x += destination.x < redHero->visitablePos().x ? 1 : -1;

		ASSERT_TRUE(moveOneTile(tanHero, destination));
	}

	const int3 positionBeforeRejectedInteraction = tanHero->visitablePos();
	EXPECT_FALSE(moveOneTile(tanHero, redHero->visitablePos()));
	EXPECT_EQ(tanHero->visitablePos(), positionBeforeRejectedInteraction);
	EXPECT_EQ(gameState->getBattle(TAN_PLAYER), nullptr);

	const int horizontalDirection = tanHero->visitablePos().x > 0 ? -1 : 1;
	const int3 freeDestination = tanHero->visitablePos() + int3(horizontalDirection, 0, 0);
	ASSERT_TRUE(moveOneTile(tanHero, freeDestination));
	EXPECT_EQ(tanHero->visitablePos(), freeDestination);
}

TEST_F(ThreePlayerFreeForAllSimultaneousTurnsTest, battleRecoveryPreservesAllSharedTurnParticipants)
{
	ASSERT_TRUE(recruitReserveHero(BLUE_PLAYER));
	startThreePlayerTurns();
	ASSERT_TRUE(startHeroBattle(RED_PLAYER, BLUE_PLAYER));

	auto * tanHero = hero(TAN_PLAYER);
	ASSERT_NE(tanHero, nullptr);
	const int3 tanDestination = tanHero->visitablePos() + int3(0, -1, 0);
	ASSERT_TRUE(moveOneTile(tanHero, tanDestination));

	gameHandler->battles->cheatBattleVictory(RED_PLAYER);
	for(int remainingDialogLimit = 100; ; --remainingDialogLimit)
	{
		auto query = gameHandler->queries->topQuery(RED_PLAYER);
		if(!query)
			break;

		ASSERT_GT(remainingDialogLimit, 0) << "Too many post-battle dialogs";
		ASSERT_TRUE(query->endsByPlayerAnswer()) << query;
		ASSERT_TRUE(gameHandler->queryReply(query->queryID, 0, RED_PLAYER));
	}

	EXPECT_EQ(gameState->getBattle(RED_PLAYER), nullptr);
	EXPECT_EQ(gameState->getBattle(BLUE_PLAYER), nullptr);
	EXPECT_EQ(gameState->getBattle(TAN_PLAYER), nullptr);
	EXPECT_EQ(gameHandler->queries->topQuery(RED_PLAYER), nullptr);
	EXPECT_EQ(gameHandler->queries->topQuery(BLUE_PLAYER), nullptr);
	EXPECT_EQ(gameHandler->queries->topQuery(TAN_PLAYER), nullptr);
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(TAN_PLAYER));
	EXPECT_EQ(tanHero->visitablePos(), tanDestination);

	auto * redHero = hero(RED_PLAYER);
	auto * blueReserveHero = hero(BLUE_PLAYER);
	ASSERT_NE(redHero, nullptr);
	ASSERT_NE(blueReserveHero, nullptr);
	const int3 redDestination = redHero->visitablePos() + int3(0, 1, 0);
	const int3 blueDestination = blueReserveHero->visitablePos() + int3(0, -1, 0);
	ASSERT_TRUE(moveOneTile(redHero, redDestination));
	ASSERT_TRUE(moveOneTile(blueReserveHero, blueDestination));
	EXPECT_EQ(redHero->visitablePos(), redDestination);
	EXPECT_EQ(blueReserveHero->visitablePos(), blueDestination);

	const int battleDay = gameState->day;
	EXPECT_TRUE(gameHandler->turnOrder->onPlayerEndsTurn(RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(TAN_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->onPlayerEndsTurn(BLUE_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->onPlayerEndsTurn(TAN_PLAYER));
	EXPECT_EQ(gameState->day, battleDay + 1);
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(TAN_PLAYER));
}

TEST_F(ThreePlayerFreeForAllSimultaneousTurnsTest, currentTurnOrderRoundTripPreservesProgress)
{
	startThreePlayerTurns();
	ASSERT_TRUE(gameHandler->turnOrder->onPlayerEndsTurn(TAN_PLAYER));

	CMemorySerializer serializer;
	serializer.oser & *gameHandler->turnOrder;

	TurnOrderProcessor restored(gameHandler.get());
	serializer.iser & restored;

	EXPECT_TRUE(restored.isPlayerMakingTurn(RED_PLAYER));
	EXPECT_TRUE(restored.isPlayerMakingTurn(BLUE_PLAYER));
	EXPECT_FALSE(restored.isPlayerMakingTurn(TAN_PLAYER));
	EXPECT_EQ(
		restored.isContactAllowed(RED_PLAYER, BLUE_PLAYER),
		gameHandler->turnOrder->isContactAllowed(RED_PLAYER, BLUE_PLAYER));
	EXPECT_EQ(
		restored.isContactAllowed(RED_PLAYER, TAN_PLAYER),
		gameHandler->turnOrder->isContactAllowed(RED_PLAYER, TAN_PLAYER));
	EXPECT_EQ(
		restored.isContactAllowed(BLUE_PLAYER, TAN_PLAYER),
		gameHandler->turnOrder->isContactAllowed(BLUE_PLAYER, TAN_PLAYER));
}

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
		gameHandler->handleReceivedPack(connectionFor(player), movement);

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
