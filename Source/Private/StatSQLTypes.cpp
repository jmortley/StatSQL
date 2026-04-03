// StatSQLTypes.cpp
// JSON builder implementations for all API endpoints

#include "StatSQLTypes.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace StatSQLJson
{

FString Serialize(const TSharedRef<FJsonObject>& JsonObj)
{
	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(JsonObj, Writer);
	return Output;
}

TSharedRef<FJsonObject> BuildInsertMatch(const FString& GameMode, const FString& MapName, const FString& ServerName)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("request"), TEXT("insert_match"));
	Json->SetStringField(TEXT("gamemode"), GameMode);
	Json->SetStringField(TEXT("gamemap"), MapName);
	Json->SetStringField(TEXT("servername"), ServerName);
	return Json;
}

TSharedRef<FJsonObject> BuildUpdateMatch(const FString& MatchId, int32 RedKills, int32 BlueKills,
	int32 RedScore, int32 BlueScore, int32 RedDeaths, int32 BlueDeaths,
	float RedDamage, float BlueDamage, const FString& GameOptions, const FString& ReplayId)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("request"), TEXT("update_match"));
	Json->SetStringField(TEXT("matchid"), MatchId);
	Json->SetNumberField(TEXT("redkills"), RedKills);
	Json->SetNumberField(TEXT("bluekills"), BlueKills);
	Json->SetNumberField(TEXT("redscore"), RedScore);
	Json->SetNumberField(TEXT("bluescore"), BlueScore);
	Json->SetNumberField(TEXT("reddeaths"), RedDeaths);
	Json->SetNumberField(TEXT("bluedeaths"), BlueDeaths);
	Json->SetNumberField(TEXT("reddamage"), RedDamage);
	Json->SetNumberField(TEXT("bluedamage"), BlueDamage);
	if (!GameOptions.IsEmpty())
	{
		Json->SetStringField(TEXT("gameoptions"), GameOptions);
	}
	if (!ReplayId.IsEmpty())
	{
		Json->SetStringField(TEXT("replayid"), ReplayId);
	}
	return Json;
}

TSharedRef<FJsonObject> BuildInsertPlayer(const FString& PlayerID, const FString& PlayerName)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("request"), TEXT("insert_player"));
	Json->SetStringField(TEXT("playerid"), PlayerID);
	Json->SetStringField(TEXT("playername"), PlayerName);
	return Json;
}

TSharedRef<FJsonObject> BuildInsertMatchStats(const FString& MatchId, const FPlayerMatchData& Data)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("request"), TEXT("insert_matchstats"));
	Json->SetStringField(TEXT("matchid"), MatchId);
	Json->SetStringField(TEXT("playerid"), Data.StatsID);
	Json->SetNumberField(TEXT("score"), Data.Score);
	Json->SetNumberField(TEXT("kills"), Data.Kills);
	Json->SetNumberField(TEXT("deaths"), Data.Deaths);
	Json->SetNumberField(TEXT("ping"), Data.Ping);
	Json->SetStringField(TEXT("team"), Data.TeamIndex == 0 ? TEXT("Red") : TEXT("Blue"));
	Json->SetNumberField(TEXT("damage"), Data.DamageDone);
	Json->SetNumberField(TEXT("multi0"), Data.MultiKills[0]);
	Json->SetNumberField(TEXT("multi1"), Data.MultiKills[1]);
	Json->SetNumberField(TEXT("multi2"), Data.MultiKills[2]);
	Json->SetNumberField(TEXT("multi3"), Data.MultiKills[3]);
	// Blueprint sends multi4 but StatNames only has 0-3; send 0 for compat
	Json->SetNumberField(TEXT("multi4"), 0);
	Json->SetNumberField(TEXT("spree0"), Data.Sprees[0]);
	Json->SetNumberField(TEXT("spree1"), Data.Sprees[1]);
	Json->SetNumberField(TEXT("spree2"), Data.Sprees[2]);
	Json->SetNumberField(TEXT("spree3"), Data.Sprees[3]);
	Json->SetNumberField(TEXT("spree4"), Data.Sprees[4]);
	return Json;
}

TSharedRef<FJsonObject> BuildInsertItem(const FString& MatchId, const FPlayerMatchData& Data)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("request"), TEXT("insert_item"));
	Json->SetStringField(TEXT("matchid"), MatchId);
	Json->SetStringField(TEXT("playerid"), Data.StatsID);
	Json->SetNumberField(TEXT("vest"), Data.ItemStats.ArmorVestCount);
	Json->SetNumberField(TEXT("pads"), Data.ItemStats.ArmorPadsCount);
	Json->SetNumberField(TEXT("helmet"), Data.ItemStats.HelmetCount);
	Json->SetNumberField(TEXT("belt"), Data.ItemStats.ShieldBeltCount);
	Json->SetNumberField(TEXT("udmgcount"), Data.ItemStats.UDamageCount);
	Json->SetNumberField(TEXT("udmgtime"), Data.ItemStats.UDamageTime);
	return Json;
}

// Helper to get weapon stat value with fallback
static int32 GetWeaponStat(const TMap<FName, FStatSQLWeaponData>& Stats, FName Key, bool bKills)
{
	const FStatSQLWeaponData* Found = Stats.Find(Key);
	if (Found)
	{
		return bKills ? Found->Kills : Found->Deaths;
	}
	return 0;
}

TSharedRef<FJsonObject> BuildInsertWeapon(const FString& MatchId, const FPlayerMatchData& Data)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("request"), TEXT("insert_weapon"));
	Json->SetStringField(TEXT("matchid"), MatchId);
	Json->SetStringField(TEXT("playerid"), Data.StatsID);

	const auto& W = Data.WeaponStats;

	// Each weapon stat key stores both kills AND deaths in the FStatSQLWeaponData struct
	// true = get kills, false = get deaths

	// Headshots (sniper headshot)
	Json->SetNumberField(TEXT("HeadshotKills"), GetWeaponStat(W, FName(TEXT("SniperHeadshotKills")), true));
	Json->SetNumberField(TEXT("HeadshotDeaths"), GetWeaponStat(W, FName(TEXT("SniperHeadshotKills")), false));

	// Sniper
	Json->SetNumberField(TEXT("SniperKills"), GetWeaponStat(W, FName(TEXT("SniperKills")), true));
	Json->SetNumberField(TEXT("SniperDeaths"), GetWeaponStat(W, FName(TEXT("SniperKills")), false));

	// Shock combo
	Json->SetNumberField(TEXT("ComboKills"), GetWeaponStat(W, FName(TEXT("ShockComboKills")), true));
	Json->SetNumberField(TEXT("ComboDeaths"), GetWeaponStat(W, FName(TEXT("ShockComboKills")), false));

	// Shock core
	Json->SetNumberField(TEXT("CoreKills"), GetWeaponStat(W, FName(TEXT("ShockCoreKills")), true));
	Json->SetNumberField(TEXT("CoreDeaths"), GetWeaponStat(W, FName(TEXT("ShockCoreKills")), false));

	// Shock beam
	Json->SetNumberField(TEXT("BeamKills"), GetWeaponStat(W, FName(TEXT("ShockBeamKills")), true));
	Json->SetNumberField(TEXT("BeamDeaths"), GetWeaponStat(W, FName(TEXT("ShockBeamKills")), false));

	// Hammer
	Json->SetNumberField(TEXT("HammerKills"), GetWeaponStat(W, FName(TEXT("ImpactHammerKills")), true));
	Json->SetNumberField(TEXT("HammerDeaths"), GetWeaponStat(W, FName(TEXT("ImpactHammerKills")), false));

	// Enforcer
	Json->SetNumberField(TEXT("EnforcerKills"), GetWeaponStat(W, FName(TEXT("EnforcerKills")), true));
	Json->SetNumberField(TEXT("EnforcerDeaths"), GetWeaponStat(W, FName(TEXT("EnforcerKills")), false));

	// Bio
	Json->SetNumberField(TEXT("BioKills"), GetWeaponStat(W, FName(TEXT("BioRifleKills")), true));
	Json->SetNumberField(TEXT("BioDeaths"), GetWeaponStat(W, FName(TEXT("BioRifleKills")), false));

	// Link primary
	Json->SetNumberField(TEXT("LinkKills"), GetWeaponStat(W, FName(TEXT("LinkKills")), true));
	Json->SetNumberField(TEXT("LinkDeaths"), GetWeaponStat(W, FName(TEXT("LinkKills")), false));

	// Link beam
	Json->SetNumberField(TEXT("LinkBeamKills"), GetWeaponStat(W, FName(TEXT("LinkBeamKills")), true));
	Json->SetNumberField(TEXT("LinkBeamDeaths"), GetWeaponStat(W, FName(TEXT("LinkBeamKills")), false));

	// Minigun primary
	Json->SetNumberField(TEXT("MinigunKills"), GetWeaponStat(W, FName(TEXT("MinigunKills")), true));
	Json->SetNumberField(TEXT("MinigunDeaths"), GetWeaponStat(W, FName(TEXT("MinigunKills")), false));

	// Minigun shard
	Json->SetNumberField(TEXT("MinigunShardKills"), GetWeaponStat(W, FName(TEXT("MinigunShardKills")), true));
	Json->SetNumberField(TEXT("MinigunShardDeaths"), GetWeaponStat(W, FName(TEXT("MinigunShardKills")), false));

	// Flak shard
	Json->SetNumberField(TEXT("FlakShardKills"), GetWeaponStat(W, FName(TEXT("FlakShardKills")), true));
	Json->SetNumberField(TEXT("FlakShardDeaths"), GetWeaponStat(W, FName(TEXT("FlakShardKills")), false));

	// Flak shell
	Json->SetNumberField(TEXT("FlakShellKills"), GetWeaponStat(W, FName(TEXT("FlakShellKills")), true));
	Json->SetNumberField(TEXT("FlakShellDeaths"), GetWeaponStat(W, FName(TEXT("FlakShellKills")), false));

	// Flak shreds (special)
	Json->SetNumberField(TEXT("FlakShreds"), Data.FlakShreds);

	// Rocket
	Json->SetNumberField(TEXT("RocketKills"), GetWeaponStat(W, FName(TEXT("RocketKills")), true));
	Json->SetNumberField(TEXT("RocketDeaths"), GetWeaponStat(W, FName(TEXT("RocketKills")), false));

	// Air rox (special)
	Json->SetNumberField(TEXT("AirRox"), Data.AirRox);

	// Lightning rifle secondary
	Json->SetNumberField(TEXT("LightningSKills"), GetWeaponStat(W, FName(TEXT("LightningRifleSecondaryKills")), true));
	Json->SetNumberField(TEXT("LightningSDeaths"), GetWeaponStat(W, FName(TEXT("LightningRifleSecondaryKills")), false));

	// Lightning rifle primary
	Json->SetNumberField(TEXT("LightningPKills"), GetWeaponStat(W, FName(TEXT("LightningRiflePrimaryKills")), true));
	Json->SetNumberField(TEXT("LightningPDeaths"), GetWeaponStat(W, FName(TEXT("LightningRiflePrimaryKills")), false));

	// Lightning rifle headshot (tertiary)
	Json->SetNumberField(TEXT("LightningRifleHeadshotKills"), GetWeaponStat(W, FName(TEXT("LightningRifleTertiaryKills")), true));
	Json->SetNumberField(TEXT("LightningRifleHeadshotDeaths"), GetWeaponStat(W, FName(TEXT("LightningRifleTertiaryKills")), false));

	// Redeemer
	Json->SetNumberField(TEXT("RedeemerKills"), GetWeaponStat(W, FName(TEXT("RedeemerKills")), true));
	Json->SetNumberField(TEXT("RedeemerDeaths"), GetWeaponStat(W, FName(TEXT("RedeemerKills")), false));

	// Telefrag
	Json->SetNumberField(TEXT("TelefragKills"), GetWeaponStat(W, FName(TEXT("TelefragKills")), true));
	Json->SetNumberField(TEXT("TeleFragDeaths"), GetWeaponStat(W, FName(TEXT("TelefragKills")), false));

	return Json;
}

// Helper to get accuracy stat value
static float GetAccuracyStat(const TMap<FName, FStatSQLAccuracyData>& Stats, FName Key, bool bShots)
{
	const FStatSQLAccuracyData* Found = Stats.Find(Key);
	if (Found)
	{
		return bShots ? Found->Shots : Found->Hits;
	}
	return 0.f;
}

TSharedRef<FJsonObject> BuildInsertAccuracy(const FString& MatchId, const FPlayerMatchData& Data)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("request"), TEXT("insert_accuracy"));
	Json->SetStringField(TEXT("matchid"), MatchId);
	Json->SetStringField(TEXT("playerid"), Data.StatsID);

	const auto& A = Data.AccuracyStats;

	// Key is always the "Shots" FName — both Shots and Hits live in the same struct
	Json->SetNumberField(TEXT("SniperShots"), GetAccuracyStat(A, FName(TEXT("SniperShots")), true));
	Json->SetNumberField(TEXT("SniperHits"), GetAccuracyStat(A, FName(TEXT("SniperShots")), false));
	Json->SetNumberField(TEXT("ShockShots"), GetAccuracyStat(A, FName(TEXT("ShockRifleShots")), true));
	Json->SetNumberField(TEXT("ShockHits"), GetAccuracyStat(A, FName(TEXT("ShockRifleShots")), false));
	Json->SetNumberField(TEXT("InstagibShots"), GetAccuracyStat(A, FName(TEXT("InstagibShots")), true));
	Json->SetNumberField(TEXT("InstagibHits"), GetAccuracyStat(A, FName(TEXT("InstagibShots")), false));
	Json->SetNumberField(TEXT("EnforcerShots"), GetAccuracyStat(A, FName(TEXT("EnforcerShots")), true));
	Json->SetNumberField(TEXT("EnforcerHits"), GetAccuracyStat(A, FName(TEXT("EnforcerShots")), false));
	Json->SetNumberField(TEXT("BioRifleShots"), GetAccuracyStat(A, FName(TEXT("BioRifleShots")), true));
	Json->SetNumberField(TEXT("BioRifleHits"), GetAccuracyStat(A, FName(TEXT("BioRifleShots")), false));
	Json->SetNumberField(TEXT("LinkShots"), GetAccuracyStat(A, FName(TEXT("LinkShots")), true));
	Json->SetNumberField(TEXT("LinkHits"), GetAccuracyStat(A, FName(TEXT("LinkShots")), false));
	Json->SetNumberField(TEXT("MinigunShots"), GetAccuracyStat(A, FName(TEXT("MinigunShots")), true));
	Json->SetNumberField(TEXT("MinigunHits"), GetAccuracyStat(A, FName(TEXT("MinigunShots")), false));
	Json->SetNumberField(TEXT("FlakShots"), GetAccuracyStat(A, FName(TEXT("FlakShots")), true));
	Json->SetNumberField(TEXT("FlakHits"), GetAccuracyStat(A, FName(TEXT("FlakShots")), false));
	Json->SetNumberField(TEXT("RocketShots"), GetAccuracyStat(A, FName(TEXT("RocketShots")), true));
	Json->SetNumberField(TEXT("RocketHits"), GetAccuracyStat(A, FName(TEXT("RocketShots")), false));
	Json->SetNumberField(TEXT("LightningRifleShots"), GetAccuracyStat(A, FName(TEXT("LightningRifleShots")), true));
	Json->SetNumberField(TEXT("LightningRifleHits"), GetAccuracyStat(A, FName(TEXT("LightningRifleShots")), false));

	return Json;
}

TSharedRef<FJsonObject> BuildInsertMovement(const FString& MatchId, const FPlayerMatchData& Data)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("request"), TEXT("insert_movement"));
	Json->SetStringField(TEXT("matchid"), MatchId);
	Json->SetStringField(TEXT("playerid"), Data.StatsID);

	const auto& M = Data.MovementStats;

	Json->SetNumberField(TEXT("RunDist"), M.RunDist);
	Json->SetNumberField(TEXT("InAirDist"), M.InAirDist);
	Json->SetNumberField(TEXT("TranslocDist"), M.TranslocDist);
	Json->SetNumberField(TEXT("NumDodges"), M.NumDodges);
	Json->SetNumberField(TEXT("NumWallDodges"), M.NumWallDodges);
	Json->SetNumberField(TEXT("NumJumps"), M.NumJumps);
	Json->SetNumberField(TEXT("NumFloorSlides"), M.NumFloorSlides);
	Json->SetNumberField(TEXT("NumWallRuns"), M.NumWallRuns);
	Json->SetNumberField(TEXT("SlideDist"), M.SlideDist);
	Json->SetNumberField(TEXT("NumImpactJumps"), M.NumImpactJumps);
	Json->SetNumberField(TEXT("WallRunDist"), M.WallRunDist);

	return Json;
}

TSharedRef<FJsonObject> BuildInsertFlagStats(const FString& MatchId, const FPlayerMatchData& Data)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("request"), TEXT("insert_flag_stats"));
	Json->SetStringField(TEXT("matchid"), MatchId);
	Json->SetStringField(TEXT("playerid"), Data.StatsID);

	const auto& F = Data.FlagStats;

	Json->SetNumberField(TEXT("FlagCaps"), F.FlagCaptures);
	Json->SetNumberField(TEXT("FlagReturns"), F.FlagReturns);
	Json->SetNumberField(TEXT("FlagAssists"), F.FlagAssists);
	Json->SetNumberField(TEXT("FlagHeldDeny"), F.FlagHeldDeny);
	Json->SetNumberField(TEXT("FlagHeldDenyTime"), F.FlagHeldDenyTime);
	Json->SetNumberField(TEXT("FlagReturnPoints"), F.FlagReturnPoints);
	Json->SetNumberField(TEXT("CarryAssists"), F.CarryAssists);
	Json->SetNumberField(TEXT("CarryAssistPoints"), F.CarryAssistPoints);
	Json->SetNumberField(TEXT("FlagCapPoints"), F.FlagCapPoints);
	Json->SetNumberField(TEXT("DefendAssist"), F.DefendAssist);
	Json->SetNumberField(TEXT("DefendAssistPoints"), F.DefendAssistPoints);
	Json->SetNumberField(TEXT("ReturnAssist"), F.ReturnAssist);
	Json->SetNumberField(TEXT("ReturnAssistPoints"), F.ReturnAssistPoints);
	Json->SetNumberField(TEXT("EnemyFCDamage"), F.EnemyFCDamage);
	Json->SetNumberField(TEXT("FCKills"), F.FCKills);
	Json->SetNumberField(TEXT("FCKillsPoints"), F.FCKillPoints);
	Json->SetNumberField(TEXT("FlagSupportKills"), F.FlagSupportKills);
	Json->SetNumberField(TEXT("RegularKillPoints"), F.RegularKillPoints);
	Json->SetNumberField(TEXT("FlagGrabs"), F.FlagGrabs);
	Json->SetNumberField(TEXT("AttackerScore"), F.AttackerScore);
	Json->SetNumberField(TEXT("DefenderScore"), F.DefenderScore);

	return Json;
}

TSharedRef<FJsonObject> BuildTimeline(const FString& MatchId, const TArray<FTimelineEvent>& Timeline)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("request"), TEXT("insert_timeline"));
	Json->SetStringField(TEXT("matchid"), MatchId);

	TArray<TSharedPtr<FJsonValue>> EventsArray;
	for (const auto& Event : Timeline)
	{
		TSharedRef<FJsonObject> EventObj = MakeShareable(new FJsonObject());
		EventObj->SetNumberField(TEXT("match_seconds"), Event.MatchSeconds);
		EventObj->SetNumberField(TEXT("real_time_seconds"), Event.RealTimeSeconds);
		EventObj->SetNumberField(TEXT("period"), Event.Period);
		EventObj->SetStringField(TEXT("event_type"), Event.EventType);

		if (!Event.ActorID.IsEmpty()) EventObj->SetStringField(TEXT("actor_id"), Event.ActorID);
		if (!Event.TargetID.IsEmpty()) EventObj->SetStringField(TEXT("target_id"), Event.TargetID);
		if (!Event.Detail.IsEmpty()) EventObj->SetStringField(TEXT("detail"), Event.Detail);
		if (!Event.ActorLocation.IsEmpty()) EventObj->SetStringField(TEXT("actor_location"), Event.ActorLocation);

		// Extended kill data (only present for kill/suicide events)
		if (Event.EventType == TEXT("kill") || Event.EventType == TEXT("suicide"))
		{
			if (!Event.KillerName.IsEmpty()) EventObj->SetStringField(TEXT("killer_name"), Event.KillerName);
			if (!Event.KilledName.IsEmpty()) EventObj->SetStringField(TEXT("killed_name"), Event.KilledName);
			EventObj->SetNumberField(TEXT("kill_distance"), Event.KillDistance);
			if (!Event.KillerLocation.IsEmpty()) EventObj->SetStringField(TEXT("killer_location"), Event.KillerLocation);
			if (!Event.KilledLocation.IsEmpty()) EventObj->SetStringField(TEXT("killed_location"), Event.KilledLocation);
			if (!Event.KilledTeam.IsEmpty()) EventObj->SetStringField(TEXT("killed_team"), Event.KilledTeam);
			if (!Event.KillerTeam.IsEmpty()) EventObj->SetStringField(TEXT("killer_team"), Event.KillerTeam);
			EventObj->SetNumberField(TEXT("victim_damage_done"), Event.VictimDamageDone);
			EventObj->SetNumberField(TEXT("victim_health"), Event.VictimHealth);
			if (!Event.KilledLastVolume.IsEmpty()) EventObj->SetStringField(TEXT("killed_last_volume"), Event.KilledLastVolume);
			if (!Event.KillerLastVolume.IsEmpty()) EventObj->SetStringField(TEXT("killer_last_volume"), Event.KillerLastVolume);
			if (!Event.ConsoleDeathMessage.IsEmpty()) EventObj->SetStringField(TEXT("console_death_message"), Event.ConsoleDeathMessage);
		}

		// Flag carry route data
		if (Event.EventType == TEXT("flag_carry") && Event.Route.Num() > 0)
		{
			if (!Event.CarrierName.IsEmpty()) EventObj->SetStringField(TEXT("carrier_name"), Event.CarrierName);
			if (!Event.Team.IsEmpty()) EventObj->SetStringField(TEXT("team"), Event.Team);
			if (!Event.Result.IsEmpty()) EventObj->SetStringField(TEXT("result"), Event.Result);

			TArray<TSharedPtr<FJsonValue>> RouteArray;
			for (const FFlagRoutePoint& Pt : Event.Route)
			{
				TSharedRef<FJsonObject> PtObj = MakeShareable(new FJsonObject());
				PtObj->SetNumberField(TEXT("t"), Pt.MatchSeconds);
				PtObj->SetStringField(TEXT("pos"), Pt.Location);
				RouteArray.Add(MakeShareable(new FJsonValueObject(PtObj)));
			}
			EventObj->SetArrayField(TEXT("route"), RouteArray);
		}

		EventsArray.Add(MakeShareable(new FJsonValueObject(EventObj)));
	}
	Json->SetArrayField(TEXT("events"), EventsArray);

	return Json;
}

TSharedRef<FJsonObject> BuildFlagRoutes(const FString& MatchId, const TMap<FString, FPlayerMatchData>& PlayerData)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("request"), TEXT("insert_flag_routes"));
	Json->SetStringField(TEXT("matchid"), MatchId);

	TArray<TSharedPtr<FJsonValue>> RoutesArray;
	for (const auto& Pair : PlayerData)
	{
		const FPlayerMatchData& Data = Pair.Value;
		for (const FFlagCarryInstance& Carry : Data.FlagStats.CarryInstances)
		{
			if (Carry.Route.Num() < 2) continue; // Need at least grab + end point

			TSharedRef<FJsonObject> RouteObj = MakeShareable(new FJsonObject());
			RouteObj->SetStringField(TEXT("carrier_id"), Carry.CarrierID);
			RouteObj->SetStringField(TEXT("carrier_name"), Carry.CarrierName);
			RouteObj->SetStringField(TEXT("team"), Carry.Team);
			RouteObj->SetNumberField(TEXT("grab_time"), Carry.GrabTime);
			RouteObj->SetNumberField(TEXT("end_time"), Carry.DropOrCapTime);
			RouteObj->SetNumberField(TEXT("duration"), Carry.Duration);
			RouteObj->SetNumberField(TEXT("period"), Carry.Period);
			RouteObj->SetStringField(TEXT("result"), Carry.Result);

			TArray<TSharedPtr<FJsonValue>> PointsArray;
			for (const FFlagRoutePoint& Pt : Carry.Route)
			{
				TSharedRef<FJsonObject> PtObj = MakeShareable(new FJsonObject());
				PtObj->SetNumberField(TEXT("t"), Pt.MatchSeconds);
				PtObj->SetStringField(TEXT("pos"), Pt.Location);
				PointsArray.Add(MakeShareable(new FJsonValueObject(PtObj)));
			}
			RouteObj->SetArrayField(TEXT("route"), PointsArray);

			RoutesArray.Add(MakeShareable(new FJsonValueObject(RouteObj)));
		}
	}
	Json->SetArrayField(TEXT("carries"), RoutesArray);

	return Json;
}

FString BuildKillFeed(const TArray<FTimelineEvent>& Timeline)
{
	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> KillArray;

	for (const FTimelineEvent& Event : Timeline)
	{
		if (Event.EventType != TEXT("kill") && Event.EventType != TEXT("suicide")) continue;

		TSharedRef<FJsonObject> Kill = MakeShareable(new FJsonObject());
		Kill->SetStringField(TEXT("killer_id"), Event.ActorID);
		Kill->SetStringField(TEXT("killed_id"), Event.TargetID);
		Kill->SetStringField(TEXT("damage_type"), Event.Detail);
		Kill->SetNumberField(TEXT("time_stamp"), Event.MatchSeconds);
		Kill->SetStringField(TEXT("killer_name"), Event.KillerName);
		Kill->SetStringField(TEXT("killed_name"), Event.KilledName);
		Kill->SetNumberField(TEXT("kill_distance"), Event.KillDistance);
		Kill->SetStringField(TEXT("killer_team"), Event.KillerTeam);
		Kill->SetStringField(TEXT("killed_team"), Event.KilledTeam);

		KillArray.Add(MakeShareable(new FJsonValueObject(Kill)));
	}

	Root->SetArrayField(TEXT("kill_feed_utpugs"), KillArray);
	return Serialize(Root);
}

FString BuildDamageFeed(const TArray<FDamageLogEntry>& DamageLog)
{
	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> DamageArray;

	for (const FDamageLogEntry& Entry : DamageLog)
	{
		TSharedRef<FJsonObject> Dmg = MakeShareable(new FJsonObject());
		Dmg->SetStringField(TEXT("attacker_id"), Entry.AttackerID);
		Dmg->SetStringField(TEXT("victim_id"), Entry.VictimID);
		Dmg->SetStringField(TEXT("damage_type"), Entry.DamageType);
		Dmg->SetNumberField(TEXT("damage_amount"), Entry.DamageAmount);

		DamageArray.Add(MakeShareable(new FJsonValueObject(Dmg)));
	}

	Root->SetArrayField(TEXT("damage_feed_utpugs"), DamageArray);
	return Serialize(Root);
}

} // namespace StatSQLJson
