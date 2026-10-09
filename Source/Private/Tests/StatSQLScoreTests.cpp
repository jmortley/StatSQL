#include "StatSQLTypes.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStatSQLBlitzScoreTest, "StatSQL.Serialization.BlitzScore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStatSQLBlitzScoreTest::RunTest(const FString& Parameters)
{
	FPlayerMatchData Player;
	Player.Kills = 57;
	Player.DamageDone = 9196;
	// The WaitingPostMatch snapshot sees an old Score. The final FlagRun
	// highlight calculation will make it 57 kills + 45 kill assists = 102.
	Player.Score = StatSQLSnapshot::PlayerScore(true, 13.f, Player.Kills, 45);
	auto Json = StatSQLJson::BuildInsertMatchStats(TEXT("1"), Player);
	TestEqual(TEXT("Upload uses final Blitz score before highlights"), int32(Json->GetNumberField(TEXT("score"))), 102);
	TestEqual(TEXT("Kills remain separate"), int32(Json->GetNumberField(TEXT("kills"))), 57);
	TestEqual(TEXT("Damage is not used to invent a score"), int32(Json->GetNumberField(TEXT("damage"))), 9196);
	TestEqual(TEXT("Same result after highlights"), StatSQLSnapshot::PlayerScore(true, 102.f, 57, 45), 102);
	TestEqual(TEXT("Logout snapshot includes assists without needing highlights"), StatSQLSnapshot::PlayerScore(true, 2.f, 10, 7), 17);
	TestEqual(TEXT("Zero assists still include kills"), StatSQLSnapshot::PlayerScore(true, 2.f, 10, 0), 10);
	TestEqual(TEXT("Zero participation stays zero"), StatSQLSnapshot::PlayerScore(true, 0.f, 0, 0), 0);
	TestEqual(TEXT("CTF/iCTF objective score remains unchanged"), StatSQLSnapshot::PlayerScore(false, 230.f, 91, 45), 230);
	TestEqual(TEXT("Other modes keep integer truncation"), StatSQLSnapshot::PlayerScore(false, 42.9f, 10, 7), 42);
	TestEqual(TEXT("Negative scores in other modes remain negative"), StatSQLSnapshot::PlayerScore(false, -2.f, 10, 7), -2);
	return true;
}
#endif
