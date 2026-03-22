// StatSQLTypes.h
// Data structures for match stat collection and JSON serialization

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Per-weapon kill/death tracking
struct FStatSQLWeaponData
{
	int32 Kills = 0;
	int32 Deaths = 0;
};

// Per-weapon accuracy tracking
struct FStatSQLAccuracyData
{
	float Shots = 0.f;
	float Hits = 0.f;
};

// Movement stats
struct FStatSQLMovementData
{
	float RunDist = 0.f;
	float InAirDist = 0.f;
	float TranslocDist = 0.f;
	float SlideDist = 0.f;
	float WallRunDist = 0.f;
	float NumDodges = 0.f;
	float NumWallDodges = 0.f;
	float NumJumps = 0.f;
	float NumFloorSlides = 0.f;
	float NumWallRuns = 0.f;
	float NumImpactJumps = 0.f;
	float NumLiftJumps = 0.f;
};

// Item pickup stats
struct FStatSQLItemData
{
	int32 ShieldBeltCount = 0;
	int32 ArmorVestCount = 0;
	int32 ArmorPadsCount = 0;
	int32 HelmetCount = 0;
	int32 UDamageCount = 0;
	int32 UDamageTime = 0;
};

// A single position sample along a flag carry route
struct FFlagRoutePoint
{
	float MatchSeconds = 0.f;
	FString Location;           // "X,Y,Z"
};

// Individual flag carry instance
struct FFlagCarryInstance
{
	float GrabTime = 0.f;       // Match seconds when grabbed
	float DropOrCapTime = 0.f;  // Match seconds when dropped/capped/returned
	float Duration = 0.f;       // How long held
	uint8 Period = 0;           // Period when grabbed
	FString Result;             // "capped", "returned", "dropped", "killed"
	FString CarrierID;          // StatsID of the carrier
	FString CarrierName;
	FString Team;               // Team of the carrier

	// Route tracking — sampled positions while carrying
	TArray<FFlagRoutePoint> Route;
};

// Per-player flag stats
struct FStatSQLFlagData
{
	int32 FlagCaptures = 0;
	int32 FlagReturns = 0;
	int32 FlagAssists = 0;
	int32 FlagGrabs = 0;
	int32 FlagHeldDeny = 0;
	int32 FlagHeldDenyTime = 0;
	int32 FlagReturnPoints = 0;
	int32 CarryAssists = 0;
	int32 CarryAssistPoints = 0;
	int32 FlagCapPoints = 0;
	int32 DefendAssist = 0;
	int32 DefendAssistPoints = 0;
	int32 ReturnAssist = 0;
	int32 ReturnAssistPoints = 0;
	int32 EnemyFCDamage = 0;
	int32 FCKills = 0;
	int32 FCKillPoints = 0;
	int32 FlagSupportKills = 0;
	int32 RegularKillPoints = 0;
	int32 AttackerScore = 0;
	int32 DefenderScore = 0;

	// Per-grab carry tracking
	TArray<FFlagCarryInstance> CarryInstances;
};

// Timeline event (kills, flag events, overtime, etc.)
struct FTimelineEvent
{
	float MatchSeconds = 0.f;
	float RealTimeSeconds = 0.f; // World seconds (absolute)
	uint8 Period = 0;          // 0=first half/regulation, 1=second half, 2+=overtime
	FString EventType;         // "kill", "suicide", "flag_cap", "flag_return", "flag_grab", "flag_deny", "overtime", "ace", "dark_horse", "clutch"
	FString ActorID;           // StatsID of player who did it
	FString TargetID;          // StatsID of victim (kills) or empty
	FString Detail;            // Weapon name for kills, reason for flags

	// Extended kill data (populated only for kill/suicide events)
	FString KillerName;
	FString KilledName;
	float KillDistance = 0.f;
	FString KillerLocation;    // World location as "X,Y,Z"
	FString KilledLocation;
	FString KilledTeam;
	FString KillerTeam;
	int32 VictimDamageDone = 0;
	int32 VictimHealth = 0;
	FString KilledLastVolume;  // Map zone name
	FString KillerLastVolume;
	FString ConsoleDeathMessage;
};

// Aggregated per-player match data
struct FPlayerMatchData
{
	FString StatsID;
	FString PlayerName;
	int32 TeamIndex = -1;
	int32 Score = 0;
	int32 Kills = 0;
	int32 Deaths = 0;
	int32 DamageDone = 0;
	float Ping = 0.f;

	// Multi-kills: [0]=double, [1]=triple, [2]=mega, [3]=ultra
	int32 MultiKills[4] = { 0, 0, 0, 0 };

	// Kill sprees: [0]=killing spree, [1]=rampage, [2]=dominating, [3]=unstoppable, [4]=godlike
	int32 Sprees[5] = { 0, 0, 0, 0, 0 };

	// Special
	int32 AirRox = 0;
	int32 FlakShreds = 0;

	// Sub-stat categories
	TMap<FName, FStatSQLWeaponData> WeaponStats;
	TMap<FName, FStatSQLAccuracyData> AccuracyStats;
	FStatSQLMovementData MovementStats;
	FStatSQLItemData ItemStats;
	FStatSQLFlagData FlagStats;

	// State tracking
	bool bDisconnected = false;     // Left before match end (stats already snapshotted)
	bool bStatsSnapshotted = false; // True once we've pulled from StatsData
};

// ============================================================
// JSON builder functions
// ============================================================

namespace StatSQLJson
{
	// Build JSON for insert_match
	TSharedRef<FJsonObject> BuildInsertMatch(const FString& GameMode, const FString& MapName, const FString& ServerName);

	// Build JSON for update_match
	TSharedRef<FJsonObject> BuildUpdateMatch(const FString& MatchId, int32 RedKills, int32 BlueKills,
		int32 RedScore, int32 BlueScore, int32 RedDeaths, int32 BlueDeaths,
		float RedDamage, float BlueDamage, const FString& GameOptions, const FString& ReplayId);

	// Build JSON for insert_player
	TSharedRef<FJsonObject> BuildInsertPlayer(const FString& PlayerID, const FString& PlayerName);

	// Build JSON for insert_matchstats
	TSharedRef<FJsonObject> BuildInsertMatchStats(const FString& MatchId, const FPlayerMatchData& Data);

	// Build JSON for insert_item
	TSharedRef<FJsonObject> BuildInsertItem(const FString& MatchId, const FPlayerMatchData& Data);

	// Build JSON for insert_weapon
	TSharedRef<FJsonObject> BuildInsertWeapon(const FString& MatchId, const FPlayerMatchData& Data);

	// Build JSON for insert_accuracy
	TSharedRef<FJsonObject> BuildInsertAccuracy(const FString& MatchId, const FPlayerMatchData& Data);

	// Build JSON for insert_movement
	TSharedRef<FJsonObject> BuildInsertMovement(const FString& MatchId, const FPlayerMatchData& Data);

	// Build JSON for insert_flag_stats
	TSharedRef<FJsonObject> BuildInsertFlagStats(const FString& MatchId, const FPlayerMatchData& Data);

	// Build JSON for timeline events
	TSharedRef<FJsonObject> BuildTimeline(const FString& MatchId, const TArray<FTimelineEvent>& Timeline);

	// Build JSON for flag carry routes (all players' carry instances)
	TSharedRef<FJsonObject> BuildFlagRoutes(const FString& MatchId, const TMap<FString, FPlayerMatchData>& PlayerData);

	// Serialize FJsonObject to string
	FString Serialize(const TSharedRef<FJsonObject>& JsonObj);
}
