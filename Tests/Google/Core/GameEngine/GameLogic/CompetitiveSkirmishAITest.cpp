/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <gtest/gtest.h>

#if !RTS_GENERALS
#include "GameLogic/CompetitiveSkirmishAI.h"
#include "Common/Xfer.h"
#include "Common/XferCRC.h"
#include <cstring>

class CompetitiveAITestAccess
{
public:
	static void populate(CompetitiveSkirmishAI& ai)
	{
		ai.m_baseSet = true; ai.m_base.x = 17; ai.m_base.y = 80;
		ai.m_goal.x = 750; ai.m_goal.y = 820; ai.m_hasGoal = true;
		ai.m_state = COMPETITIVE_RECOVERING; ai.m_changed = 123;
		ai.m_nextEconomy = 510; ai.m_nextStrategy = 590; ai.m_nextTactics = 504;
		ai.m_nextAbilities = 530; ai.m_nextScout = 920; ai.m_nextTunnelExit = 575;
		ai.m_lastProgress = 100; ai.m_goalHealth = 1500; ai.m_raidFailures = 2; ai.m_raidFailedFrame = 390;
		ai.m_raidFailedGoal = ai.m_goal;
		ai.m_scoutIndex = 3; ai.m_battlePlanCenter = ObjectID(25);
		ai.m_raidTarget = ObjectID(70); ai.m_raidPayload = 4;
		ai.m_collectorQuota = 7; ai.m_nextCollectorAdjustment = 1200;
		ai.m_supplyMarkers.push_back(ai.m_base); ai.m_supplyMarkers.push_back(ai.m_goal);
		CompetitiveSkirmishAI::RaidRoute route;
		route.position = ai.m_goal; route.failures = 2; route.failedFrame = 390;
		ai.m_raidRoutes.push_back(route);
		CompetitiveAIObservation seen;
		seen.id = ObjectID(70); seen.templateName = "GLATunnelNetwork";
		seen.position = ai.m_goal; seen.lastSeen = 222; seen.health = 870;
		seen.playerIndex = 4; seen.structure = true; seen.armed = true;
		seen.defense = true; seen.detector = true; seen.antiAir = false;
		seen.publicReadyFrame = 2000;
		ai.m_observations[seen.id] = seen;
		CompetitiveSkirmishAI::UnitOrder order;
		order.role = CompetitiveSkirmishAI::SIEGE; order.order = CompetitiveSkirmishAI::RETREAT;
		order.goal = ai.m_base; order.progressPosition = ai.m_goal; order.target = seen.id;
		order.issued = 330; order.progressFrame = 340; order.retreatUntil = 520;
		order.lastHealth = 75; order.damagePerSecond = 12;
		order.supplyBoxes = 3; order.delivered = 240; order.cycleFrames = 360; order.waitingFrames = 50;
		order.raidTarget = seen.id; order.raidGoal = ai.m_goal; order.raidCost = 1400;
		order.raidGain = 350; order.raidHealth = 600; order.raidMaxHealth = 1000; order.raidValue = 900;
		ai.m_orders[ObjectID(9)] = order;
	}
};

class CompetitiveAIMemoryXfer : public Xfer
{
public:
	CompetitiveAIMemoryXfer(XferMode mode, std::vector<UnsignedByte>& data) : m_data(data), m_offset(0)
	{
		m_xferMode = mode;
	}
	void open(AsciiString) override { m_offset = 0; }
	void close() override {}
	Int beginBlock() override { return 0; }
	void endBlock() override {}
	void skip(Int bytes) override { m_offset += bytes; }
	void xferSnapshot(Snapshot*) override { ADD_FAILURE() << "Unexpected snapshot wrapper"; }
	void xferAsciiString(AsciiString *value) override
	{
		UnsignedByte length = UnsignedByte(value->getLength());
		xferUnsignedByte(&length);
		if (m_xferMode == XFER_SAVE) xferImplementation((void*)value->str(), length);
		else
		{
			Char text[256];
			xferImplementation(text, length); text[length] = 0; *value = text;
		}
	}
protected:
	void xferImplementation(void *data, Int bytes) override
	{
		ASSERT_GE(bytes, 0);
		if (m_xferMode == XFER_SAVE)
		{
			const UnsignedByte *source = static_cast<const UnsignedByte*>(data);
			m_data.insert(m_data.end(), source, source + bytes);
		}
		else
		{
			ASSERT_LE(m_offset + bytes, m_data.size());
			std::memcpy(data, &m_data[m_offset], bytes);
			m_offset += bytes;
		}
	}
private:
	std::vector<UnsignedByte>& m_data;
	size_t m_offset;
};

TEST(CompetitiveAIObservation, InvisibleChangesDoNotAlterRememberedInformation)
{
	CompetitiveAIObservation memory;
	CompetitiveAIObservation visible;
	visible.id = ObjectID(17);
	visible.templateName = "AmericaVehicleHumvee";
	visible.position.x = 100;
	visible.position.y = 200;
	visible.health = 300;
	visible.lastSeen = 100;
	memory.observe(visible, true);

	CompetitiveAIObservation hidden = visible;
	hidden.id = ObjectID(99);
	hidden.templateName = "GLAVehicleBattleBus";
	hidden.position.x = 900;
	hidden.health = 1;
	hidden.lastSeen = 600;
	hidden.playerIndex = 7;
	memory.observe(hidden, false);

	EXPECT_EQ(memory.id, visible.id);
	EXPECT_EQ(memory.templateName, visible.templateName);
	EXPECT_EQ(memory.position.x, visible.position.x);
	EXPECT_EQ(memory.position.y, visible.position.y);
	EXPECT_EQ(memory.health, visible.health);
	EXPECT_EQ(memory.lastSeen, visible.lastSeen);
	EXPECT_EQ(memory.playerIndex, visible.playerIndex);
}

TEST(CompetitiveAIObservation, MobilePositionsBecomeUncertainAndEventuallyExpire)
{
	CompetitiveAIObservation memory;
	memory.lastSeen = 100;
	EXPECT_TRUE(memory.hasPrecisePosition(100 + 15 * LOGICFRAMES_PER_SECOND));
	EXPECT_FALSE(memory.hasPrecisePosition(101 + 15 * LOGICFRAMES_PER_SECOND));
	EXPECT_LT(memory.confidence(100 + 20 * LOGICFRAMES_PER_SECOND), 1.0f);
	EXPECT_GT(memory.confidence(100 + 45 * LOGICFRAMES_PER_SECOND), 0.0f);
	EXPECT_FALSE(memory.hasPrecisePosition(100 + 45 * LOGICFRAMES_PER_SECOND));
	EXPECT_EQ(memory.confidence(100 + 90 * LOGICFRAMES_PER_SECOND), 0.0f);
}

TEST(CompetitiveAIObservation, BuildingMemoryDoesNotRevealHiddenDestruction)
{
	CompetitiveAIObservation memory;
	memory.structure = true;
	memory.health = 2000;
	memory.lastSeen = 100;
	CompetitiveAIObservation destroyed = memory;
	destroyed.health = 0;
	memory.observe(destroyed, false);
	EXPECT_EQ(memory.health, 2000);
	EXPECT_TRUE(memory.hasPrecisePosition(10000));
	EXPECT_EQ(memory.confidence(10000), 1.0f);
	memory.observe(destroyed, true);
	EXPECT_EQ(memory.health, 0);
}

TEST(CompetitiveAIObservation, ConfidenceDoesNotIncreaseWhenVisionIsNotRenewed)
{
	CompetitiveAIObservation memory;
	memory.lastSeen = 100;
	Real previous = memory.confidence(100);
	for (UnsignedInt seconds = 1; seconds <= 100; ++seconds)
	{
		Real current = memory.confidence(100 + seconds * LOGICFRAMES_PER_SECOND);
		EXPECT_LE(current, previous);
		EXPECT_GE(current, 0.0f);
		previous = current;
	}
}

TEST(CompetitiveAIPolicy, ReserveUsesCurrentGameCostAndDoesNotBlockEmergencySpending)
{
	EXPECT_EQ(CompetitiveAIPolicy::reserve(0), 1000);
	EXPECT_EQ(CompetitiveAIPolicy::reserve(300), 1000);
	EXPECT_EQ(CompetitiveAIPolicy::reserve(700), 1400);
	EXPECT_EQ(CompetitiveAIPolicy::reserve(1100), 2200);
}

TEST(CompetitiveAIPolicy, EmptyFogIsNotAnUndefendedBase)
{
	EXPECT_FALSE(CompetitiveAIPolicy::attack(1000, 0, false, true));
	EXPECT_FALSE(CompetitiveAIPolicy::attack(1000, 100, true, false));
	EXPECT_FALSE(CompetitiveAIPolicy::attack(0, 0, true, true));
}

TEST(CompetitiveAIPolicy, AttackRetreatAndTradingBandsAreDistinct)
{
	EXPECT_FALSE(CompetitiveAIPolicy::attack(124, 100, true, true));
	EXPECT_TRUE(CompetitiveAIPolicy::attack(125, 100, true, true));
	EXPECT_TRUE(CompetitiveAIPolicy::retreat(84, 100));
	EXPECT_FALSE(CompetitiveAIPolicy::retreat(85, 100));
	EXPECT_FALSE(CompetitiveAIPolicy::retreat(100, 100));
	EXPECT_FALSE(CompetitiveAIPolicy::attack(100, 100, true, true));
}

TEST(CompetitiveAIPolicy, StrategicHysteresisAllowsImmediateEmergencyResponse)
{
	EXPECT_FALSE(CompetitiveAIPolicy::changeState(100 + 5 * LOGICFRAMES_PER_SECOND, 100, false));
	EXPECT_TRUE(CompetitiveAIPolicy::changeState(100 + 6 * LOGICFRAMES_PER_SECOND, 100, false));
	EXPECT_TRUE(CompetitiveAIPolicy::changeState(101, 100, true));
}

TEST(CompetitiveSkirmishAI, SpecializedGeneralsKeepTheirOriginalStrategies)
{
	EXPECT_TRUE(CompetitiveSkirmishAI::supportsSide("America"));
	EXPECT_TRUE(CompetitiveSkirmishAI::supportsSide("China"));
	EXPECT_TRUE(CompetitiveSkirmishAI::supportsSide("GLA"));
	EXPECT_FALSE(CompetitiveSkirmishAI::supportsSide("AmericaAirForceGeneral"));
	EXPECT_FALSE(CompetitiveSkirmishAI::supportsSide("ChinaInfantryGeneral"));
	EXPECT_FALSE(CompetitiveSkirmishAI::supportsSide("GLAStealthGeneral"));
	EXPECT_FALSE(CompetitiveSkirmishAI::supportsSide("Civilian"));
}

TEST(CompetitiveSkirmishAI, SaveLoadPreservesObservationsOrdersAndDecisionTimers)
{
	CompetitiveSkirmishAI original(nullptr), restored(nullptr);
	CompetitiveAITestAccess::populate(original);
	std::vector<UnsignedByte> saved, repeated;
	CompetitiveAIMemoryXfer writer(XFER_SAVE, saved);
	original.xfer(&writer);
	CompetitiveAIMemoryXfer reader(XFER_LOAD, saved);
	restored.xfer(&reader);
	EXPECT_EQ(restored.getState(), COMPETITIVE_RECOVERING);
	CompetitiveAIMemoryXfer again(XFER_SAVE, repeated);
	restored.xfer(&again);
	EXPECT_EQ(saved, repeated);
}

TEST(CompetitiveSkirmishAI, SaveLoadPreservesReplayCRC)
{
	CompetitiveSkirmishAI original(nullptr), restored(nullptr);
	CompetitiveAITestAccess::populate(original);
	std::vector<UnsignedByte> saved;
	CompetitiveAIMemoryXfer writer(XFER_SAVE, saved);
	original.xfer(&writer);
	CompetitiveAIMemoryXfer reader(XFER_LOAD, saved);
	restored.xfer(&reader);
	XferCRC before, after;
	before.open("competitive-before"); after.open("competitive-after");
	original.xfer(&before); restored.xfer(&after);
	EXPECT_EQ(before.getCRC(), after.getCRC());
}
#endif
