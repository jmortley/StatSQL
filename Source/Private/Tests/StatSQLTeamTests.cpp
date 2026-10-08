#include "StatSQLTypes.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStatSQLTeamScoresTest, "StatSQL.Serialization.FourTeams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStatSQLTeamScoresTest::RunTest(const FString& Parameters)
{
	TMap<int32, int32> Scores;
	TMap<FString, FPlayerMatchData> Players;
	const TCHAR* Names[] = { TEXT("Red"), TEXT("Blue"), TEXT("Green"), TEXT("Yellow") };
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Scores.Add(Index, 10 + Index);
		FPlayerMatchData Player;
		Player.TeamIndex = Index;
		Player.Kills = 2 + Index;
		Player.Deaths = Index;
		Player.DamageDone = 100 * Index;
		Players.Add(FString::FromInt(Index), Player);
		TestEqual(TEXT("Player team label"), StatSQLJson::BuildInsertMatchStats(TEXT("1"), Player)->GetStringField(TEXT("team")), FString(Names[Index]));
	}
	auto Json = StatSQLJson::BuildUpdateMatch(TEXT("1"), 2, 3, 10, 11, 0, 1, 0, 100, TEXT(""), TEXT(""));
	StatSQLJson::AddTeamScores(Json, Scores, Players);
	const auto& Teams = Json->GetArrayField(TEXT("team_scores"));
	TestEqual(TEXT("All four teams serialized"), Teams.Num(), 4);
	for (int32 Index = 0; Index < Teams.Num(); ++Index)
	{
		const auto Team = Teams[Index]->AsObject();
		TestEqual(TEXT("Score survives"), int32(Team->GetNumberField(TEXT("score"))), 10 + Index);
		TestEqual(TEXT("Totals stay on their original team"), int32(Team->GetNumberField(TEXT("kills"))), 2 + Index);
	}
	Scores.Remove(3);
	Scores.Remove(0);
	StatSQLJson::AddTeamScores(Json, Scores, Players);
	TestTrue(TEXT("Missing Yellow score remains null"), Json->GetArrayField(TEXT("team_scores"))[3]->AsObject()->Values[TEXT("score")]->IsNull());
	TestTrue(TEXT("Missing legacy Red score remains null"), Json->Values[TEXT("redscore")]->IsNull());
	TestEqual(TEXT("No-team is never Blue"), StatSQLJson::TeamName(255), FString(TEXT("")));
	TestEqual(TEXT("Disconnected unknown is never Blue"), StatSQLJson::TeamName(-1), FString(TEXT("")));
	return true;
}
#endif
