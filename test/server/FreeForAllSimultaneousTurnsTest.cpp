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
#include "../../server/processors/TurnOrderProcessor.h"

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

	void applyPack(CPackForClient & pack) override
	{
		gameState->apply(pack);
	}

	void sendPack(CPackForClient & pack, GameConnectionID connectionID) override
	{
	}

private:
	EServerState state = EServerState::GAMEPLAY;
	CGameState * gameState = nullptr;
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

	while(!attacker->visitablePos().areNeighbours(defender->visitablePos()))
	{
		int3 destination = attacker->visitablePos();
		destination.x += destination.x < defender->visitablePos().x ? 1 : -1;

		ASSERT_TRUE(gameHandler->moveHero(
			attacker->id,
			attacker->convertFromVisitablePos(destination),
			EMovementMode::STANDARD,
			false,
			RED_PLAYER));
	}

	ASSERT_TRUE(gameHandler->moveHero(
		attacker->id,
		attacker->convertFromVisitablePos(defender->visitablePos()),
		EMovementMode::STANDARD,
		false,
		RED_PLAYER));

	const auto * battle = gameState->getBattle(RED_PLAYER);
	ASSERT_NE(battle, nullptr);
	EXPECT_EQ(gameState->getBattle(BLUE_PLAYER), battle);
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(RED_PLAYER));
	EXPECT_TRUE(gameHandler->turnOrder->isPlayerMakingTurn(BLUE_PLAYER));

	MoveHero redMovement;
	MoveHero blueMovement;
	EXPECT_TRUE(gameHandler->isBlockedByQueries(&redMovement, RED_PLAYER));
	EXPECT_TRUE(gameHandler->isBlockedByQueries(&blueMovement, BLUE_PLAYER));
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
