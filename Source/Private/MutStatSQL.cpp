// MutStatSQL.cpp
// Core implementation of the StatSQL mutator

#include "MutStatSQL.h"
#include "StatSQLTypes.h"
#include "UnrealTournament.h"
#include "UTGameMode.h"
#include "UTGameState.h"
#include "UTCTFGameState.h"
#include "UTPlayerState.h"
#include "UTPlayerController.h"
#include "UTCarriedObject.h"
#include "UTTeamInfo.h"
#include "UTBaseGameMode.h"
#include "UTBasePlayerController.h"
#include "UTDamageType.h"
#include "UTGameVolume.h"
#include "UTCharacter.h"
#include "UTRecastNavMesh.h"
#include "ImageUtils.h"
#include "StatNames.h"
#include "UTATypes.h"
#include "Http.h"
#include "Json.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY(LogStatSQL);

// ============================================================
// Constructor
// ============================================================

AMutStatSQL::AMutStatSQL(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DisplayName = NSLOCTEXT("StatSQL", "DisplayName", "StatSQL");
	Author = NSLOCTEXT("StatSQL", "Author", "phantaci");
	Description = NSLOCTEXT("StatSQL", "Description", "C++ stats tracking - POSTs match data to ut4stats.com");

	// Required for ModifyDamage_Implementation to be called
	bModifyDamage = true;

	ApiBaseUrl = TEXT("https://ut4stats.com");
	bEnabled = true;
	bDebug = false;
	bAllowNameChange = true;
	bMatchInProgress = false;
	bFirstRoundStarted = false;
	CachedTimeLimit = 0;
	MatchStartWorldTime = 0.f;
	AccumulatedRoundTime = 0.f;
	LastRoundStartWorldTime = 0.f;
}

// ============================================================
// Init & BeginPlay
// ============================================================

void AMutStatSQL::Init_Implementation(const FString& Options)
{
	Super::Init_Implementation(Options);

	// Load Mod.ini first (primary config source)
	LoadModIni();

	// URL options can override Mod.ini values
	FString UrlVal = ParseOption(Options, TEXT("StatSQLUrl"));
	if (!UrlVal.IsEmpty())
	{
		ApiBaseUrl = UrlVal;
	}

	FString AuthVal = ParseOption(Options, TEXT("StatSQLKey"));
	if (!AuthVal.IsEmpty())
	{
		ApiAuthKey = AuthVal;
	}

	FString EnabledVal = ParseOption(Options, TEXT("StatSQLEnabled"));
	if (!EnabledVal.IsEmpty())
	{
		bEnabled = EnabledVal.ToBool();
	}

	UE_LOG(LogStatSQL, Log, TEXT("Init - URL: %s, Enabled: %s, Key: %s, AllowNameChange: %s"),
		*ApiBaseUrl, bEnabled ? TEXT("true") : TEXT("false"),
		ApiAuthKey.IsEmpty() ? TEXT("NOT SET") : TEXT("SET"),
		bAllowNameChange ? TEXT("true") : TEXT("false"));
}

void AMutStatSQL::LoadModIni()
{
	// Look for Mod.ini in Saved/Config directory
	FString ModIniPath = FPaths::GameSavedDir() / TEXT("Config") / TEXT("Mod.ini");

	if (!FPaths::FileExists(ModIniPath))
	{
		UE_LOG(LogStatSQL, Warning, TEXT("Mod.ini not found at: %s"), *ModIniPath);
		return;
	}

	FConfigFile ModIni;
	ModIni.Read(ModIniPath);

	const FConfigSection* Section = ModIni.Find(TEXT("UTPUGS_STATS"));
	if (!Section)
	{
		UE_LOG(LogStatSQL, Warning, TEXT("Mod.ini missing [UTPUGS_STATS] section"));
		return;
	}

	// Key (auth token)
	const FConfigValue* KeyVal = Section->Find(FName(TEXT("Key")));
	if (KeyVal && !KeyVal->GetValue().IsEmpty())
	{
		ApiAuthKey = KeyVal->GetValue();
	}

	// ServerName override
	const FConfigValue* ServerVal = Section->Find(FName(TEXT("ServerName")));
	if (ServerVal && !ServerVal->GetValue().IsEmpty())
	{
		CachedServerName = ServerVal->GetValue();
	}

	// SendStats (enables/disables submission)
	const FConfigValue* SendVal = Section->Find(FName(TEXT("SendStats")));
	if (SendVal)
	{
		bEnabled = SendVal->GetValue().ToBool();
	}

	// Debug
	const FConfigValue* DebugVal = Section->Find(FName(TEXT("Debug")));
	if (DebugVal)
	{
		bDebug = DebugVal->GetValue().ToBool();
	}

	// AllowNameChange
	const FConfigValue* NameChangeVal = Section->Find(FName(TEXT("AllowNameChange")));
	if (NameChangeVal)
	{
		bAllowNameChange = NameChangeVal->GetValue().ToBool();
	}

	UE_LOG(LogStatSQL, Log, TEXT("Mod.ini loaded - ServerName: %s, SendStats: %s, Debug: %s"),
		*CachedServerName, bEnabled ? TEXT("true") : TEXT("false"),
		bDebug ? TEXT("true") : TEXT("false"));
}

void AMutStatSQL::BeginPlay()
{
	Super::BeginPlay();

	// Cache match info
	AUTGameMode* GM = Cast<AUTGameMode>(GetWorld()->GetAuthGameMode());
	if (GM)
	{
		CachedMapName = GetWorld()->GetMapName();
		// Strip PIE prefix (e.g. "UEDPIE_0_CTF-Pistola" -> "CTF-Pistola")
		FString Stripped = GetWorld()->GetMapName();
		Stripped.RemoveFromStart(GetWorld()->StreamingLevelsPrefix);
		if (!Stripped.IsEmpty())
		{
			CachedMapName = Stripped;
		}

		CachedGameMode = GM->GetClass()->GetName();
	}

	// Get ServerName from Game.ini [/Script/UnrealTournament.UTGameState] if not set in Mod.ini
	if (CachedServerName.IsEmpty())
	{
		AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
		if (GS && !GS->ServerName.IsEmpty())
		{
			CachedServerName = GS->ServerName;
		}
	}
}

// ============================================================
// Time helpers
// ============================================================

float AMutStatSQL::GetMatchSeconds() const
{
	float RoundElapsed = 0.f;

	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (GS && GS->TimeLimit > 0)
	{
		// Timed round: elapsed = TimeLimit - RemainingTime (TimeLimit is already in seconds)
		RoundElapsed = (float)(GS->TimeLimit) - (float)GS->GetRemainingTime();
		RoundElapsed = FMath::Max(0.f, RoundElapsed);
	}
	else if (MatchStartWorldTime > 0.f)
	{
		// No time limit: use world time since round started
		RoundElapsed = GetWorld()->GetTimeSeconds() - MatchStartWorldTime;
	}

	// Add accumulated time from all previous rounds (Elim/round-based modes)
	return AccumulatedRoundTime + RoundElapsed;
}

uint8 AMutStatSQL::GetCurrentPeriod() const
{
	// Check CTF-specific state first
	AUTCTFGameState* CTFGS = GetWorld()->GetGameState<AUTCTFGameState>();
	if (CTFGS)
	{
		if (CTFGS->IsMatchInOvertime()) return 2;
		if (CTFGS->bSecondHalf) return 1;
		return 0;
	}

	// Non-CTF: check for overtime
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (GS && GS->IsMatchInOvertime()) return 1;
	return 0;
}

// ============================================================
// Player data helpers
// ============================================================

FString AMutStatSQL::GetStatsID(AUTPlayerState* PS)
{
	if (!PS) return FString();
	return PS->StatsID;
}

bool AMutStatSQL::IsHumanPlayer(AController* C)
{
	if (!C) return false;
	AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState);
	if (!PS) return false;
	if (PS->bIsABot) return false;
	if (PS->bOnlySpectator) return false;
	if (PS->StatsID.IsEmpty()) return false;
	return true;
}

AUTPlayerState* AMutStatSQL::GetUTPS(AController* C)
{
	if (!C) return nullptr;
	return Cast<AUTPlayerState>(C->PlayerState);
}

FPlayerMatchData* AMutStatSQL::GetOrCreatePlayerData(AUTPlayerState* PS)
{
	if (!PS || PS->bIsABot || PS->bOnlySpectator || PS->StatsID.IsEmpty())
	{
		return nullptr;
	}

	FString ID = PS->StatsID;
	FPlayerMatchData* Existing = PlayerData.Find(ID);
	if (Existing)
	{
		// Update name/team in case they changed
		Existing->PlayerName = PS->PlayerName;
		if (PS->Team)
		{
			Existing->TeamIndex = PS->Team->TeamIndex;
		}
		return Existing;
	}

	// Create new entry
	FPlayerMatchData NewData;
	NewData.StatsID = ID;
	NewData.PlayerName = PS->PlayerName;
	NewData.TeamIndex = PS->Team ? PS->Team->TeamIndex : -1;
	PlayerData.Add(ID, NewData);
	return PlayerData.Find(ID);
}

// ============================================================
// Timeline
// ============================================================

void AMutStatSQL::AddTimelineEvent(const FString& EventType, const FString& ActorID,
	const FString& TargetID, const FString& Detail)
{
	FTimelineEvent Event;
	Event.MatchSeconds = GetMatchSeconds();
	Event.RealTimeSeconds = GetWorld()->GetTimeSeconds();
	Event.Period = GetCurrentPeriod();
	Event.EventType = EventType;
	Event.ActorID = ActorID;
	Event.TargetID = TargetID;
	Event.Detail = Detail;
	Timeline.Add(Event);
}

void AMutStatSQL::AddKillTimelineEvent(AController* Killer, AController* Other,
	TSubclassOf<UDamageType> DamageType)
{
	AUTPlayerState* KillerPS = GetUTPS(Killer);
	AUTPlayerState* VictimPS = GetUTPS(Other);

	// Skip if both are bots or invalid
	FString KillerID = KillerPS ? GetStatsID(KillerPS) : FString();
	FString VictimID = VictimPS ? GetStatsID(VictimPS) : FString();
	if (KillerID.IsEmpty() && VictimID.IsEmpty()) return;

	FTimelineEvent Event;
	Event.MatchSeconds = GetMatchSeconds();
	Event.RealTimeSeconds = GetWorld()->GetTimeSeconds();
	Event.Period = GetCurrentPeriod();
	Event.EventType = (Killer == Other) ? TEXT("suicide") : TEXT("kill");
	Event.ActorID = KillerID;
	Event.TargetID = VictimID;

	// Damage type name
	if (DamageType)
	{
		Event.Detail = DamageType->GetName();

		// Console death message from the damage type CDO
		UUTDamageType* UTDT = Cast<UUTDamageType>(DamageType.GetDefaultObject());
		if (UTDT)
		{
			Event.ConsoleDeathMessage = UTDT->ConsoleDeathMessage.ToString();
		}
	}

	// Player names
	if (KillerPS) Event.KillerName = KillerPS->PlayerName;
	if (VictimPS) Event.KilledName = VictimPS->PlayerName;

	// Team names
	if (KillerPS && KillerPS->Team)
	{
		Event.KillerTeam = (KillerPS->Team->TeamIndex == 0) ? TEXT("Red") : TEXT("Blue");
	}
	if (VictimPS && VictimPS->Team)
	{
		Event.KilledTeam = (VictimPS->Team->TeamIndex == 0) ? TEXT("Red") : TEXT("Blue");
	}

	// Victim stats
	if (VictimPS)
	{
		Event.VictimDamageDone = VictimPS->DamageDone;

		// Victim health at time of death (typically 0, but useful for overkill tracking)
		APawn* VictimPawn = Other ? Other->GetPawn() : nullptr;
		AUTCharacter* VictimChar = Cast<AUTCharacter>(VictimPawn);
		if (VictimChar)
		{
			Event.VictimHealth = VictimChar->Health;
		}
	}

	// Locations and distance
	APawn* KillerPawn = Killer ? Killer->GetPawn() : nullptr;
	APawn* VictimPawn = Other ? Other->GetPawn() : nullptr;

	if (KillerPawn)
	{
		FVector KLoc = KillerPawn->GetActorLocation();
		Event.KillerLocation = FString::Printf(TEXT("%.0f,%.0f,%.0f"), KLoc.X, KLoc.Y, KLoc.Z);
	}
	if (VictimPawn)
	{
		FVector VLoc = VictimPawn->GetActorLocation();
		Event.KilledLocation = FString::Printf(TEXT("%.0f,%.0f,%.0f"), VLoc.X, VLoc.Y, VLoc.Z);
	}

	// Kill distance
	if (KillerPawn && VictimPawn)
	{
		Event.KillDistance = FVector::Dist(KillerPawn->GetActorLocation(), VictimPawn->GetActorLocation());
	}

	// Volume names (map zones)
	if (KillerPS && KillerPS->LastKnownLocation)
	{
		Event.KillerLastVolume = KillerPS->LastKnownLocation->VolumeName.ToString();
	}
	if (VictimPS && VictimPS->LastKnownLocation)
	{
		Event.KilledLastVolume = VictimPS->LastKnownLocation->VolumeName.ToString();
	}

	Timeline.Add(Event);
}

// ============================================================
// Mutator hooks
// ============================================================

void AMutStatSQL::PostPlayerInit_Implementation(AController* C)
{
	Super::PostPlayerInit_Implementation(C);

	if (!bEnabled) return;

	AUTPlayerState* PS = GetUTPS(C);
	if (PS && !PS->bIsABot && !PS->bOnlySpectator)
	{
		GetOrCreatePlayerData(PS);
		UE_LOG(LogStatSQL, Verbose, TEXT("Player registered: %s [%s]"), *PS->PlayerName, *PS->StatsID);
	}
}

void AMutStatSQL::NotifyLogout_Implementation(AController* C)
{
	if (bEnabled && bMatchInProgress)
	{
		AUTPlayerState* PS = GetUTPS(C);
		if (PS && !PS->bIsABot)
		{
			FPlayerMatchData* Data = GetOrCreatePlayerData(PS);
			if (Data && !Data->bStatsSnapshotted)
			{
				// Snapshot all stats NOW before the PlayerState is destroyed
				SnapshotPlayerStats(PS, *Data);
				Data->bDisconnected = true;
				UE_LOG(LogStatSQL, Log, TEXT("Player disconnected, stats snapshotted: %s"), *PS->PlayerName);
			}
		}
	}

	Super::NotifyLogout_Implementation(C);
}

void AMutStatSQL::ScoreKill_Implementation(AController* Killer, AController* Other,
	TSubclassOf<UDamageType> DamageType)
{
	Super::ScoreKill_Implementation(Killer, Other, DamageType);

	if (!bEnabled || !bMatchInProgress) return;

	AUTPlayerState* KillerPS = GetUTPS(Killer);
	AUTPlayerState* VictimPS = GetUTPS(Other);

	// Track killer stats
	if (KillerPS && Killer != Other) // Not a suicide
	{
		FPlayerMatchData* KillerData = GetOrCreatePlayerData(KillerPS);
		if (KillerData)
		{
			KillerData->Kills++;
		}
	}

	// Track victim stats
	if (VictimPS)
	{
		FPlayerMatchData* VictimData = GetOrCreatePlayerData(VictimPS);
		if (VictimData)
		{
			VictimData->Deaths++;
		}

		// If victim was carrying a flag, close out their carry instance
		FString VictimID = GetStatsID(VictimPS);
		if (ActiveFlagCarries.Contains(VictimID))
		{
			FFlagCarryInstance& Carry = ActiveFlagCarries[VictimID];
			Carry.DropOrCapTime = GetMatchSeconds();
			Carry.Duration = Carry.DropOrCapTime - Carry.GrabTime;
			Carry.Result = TEXT("killed");

			// Record death location as final route point
			APawn* VPawn = Other ? Other->GetPawn() : nullptr;
			if (VPawn)
			{
				FFlagRoutePoint DeathPoint;
				DeathPoint.MatchSeconds = Carry.DropOrCapTime;
				FVector Loc = VPawn->GetActorLocation();
				DeathPoint.Location = FString::Printf(TEXT("%.0f,%.0f,%.0f"), Loc.X, Loc.Y, Loc.Z);
				Carry.Route.Add(DeathPoint);
			}

			if (FPlayerMatchData* VData = PlayerData.Find(VictimID))
			{
				VData->FlagStats.CarryInstances.Add(Carry);
			}
			ActiveFlagCarries.Remove(VictimID);

			if (ActiveFlagCarries.Num() == 0 && FlagRouteSampleTimer.IsValid())
			{
				GetWorldTimerManager().ClearTimer(FlagRouteSampleTimer);
			}
		}
	}

	// Detailed kill timeline event
	AddKillTimelineEvent(Killer, Other, DamageType);
}

void AMutStatSQL::ScoreDamage_Implementation(int32 DamageAmount, AUTPlayerState* Victim,
	AUTPlayerState* Attacker)
{
	Super::ScoreDamage_Implementation(DamageAmount, Victim, Attacker);

	if (!bEnabled || !bMatchInProgress) return;

	if (Attacker && Attacker != Victim)
	{
		FPlayerMatchData* AttackerData = GetOrCreatePlayerData(Attacker);
		if (AttackerData)
		{
			AttackerData->DamageDone += DamageAmount;
		}
	}
}

bool AMutStatSQL::ModifyDamage_Implementation(int32& Damage, FVector& Momentum, APawn* Injured,
	AController* InstigatedBy, const FHitResult& HitInfo,
	AActor* DamageCauser, TSubclassOf<UDamageType> DamageType)
{
	bool bResult = Super::ModifyDamage_Implementation(Damage, Momentum, Injured, InstigatedBy, HitInfo, DamageCauser, DamageType);

	if (!bEnabled || !bMatchInProgress || Damage <= 0) return bResult;

	AUTPlayerState* AttackerPS = GetUTPS(InstigatedBy);
	AUTCharacter* InjuredChar = Cast<AUTCharacter>(Injured);
	AUTPlayerState* VictimPS = InjuredChar ? Cast<AUTPlayerState>(InjuredChar->PlayerState) : nullptr;

	if (AttackerPS && VictimPS && AttackerPS != VictimPS)
	{
		FString AttackerID = GetStatsID(AttackerPS);
		FString VictimID = GetStatsID(VictimPS);

		if (!AttackerID.IsEmpty() && !VictimID.IsEmpty())
		{
			FDamageLogEntry Entry;
			Entry.AttackerID = AttackerID;
			Entry.VictimID = VictimID;
			Entry.DamageAmount = Damage;

			if (DamageType)
			{
				FString RawName = DamageType->GetName();
				Entry.DamageType = MapDamageTypeToFeedName(RawName);
				UE_LOG(LogStatSQL, Log, TEXT("DamageType raw=%s mapped=%s dmg=%d"), *RawName, *Entry.DamageType, Damage);
			}

			DamageLog.Add(Entry);
		}
	}

	return bResult;
}

FString AMutStatSQL::MapDamageTypeToFeedName(const FString& ClassName)
{
	// Map UE4 damage type class names to the short strings Django's damage_feed expects
	// These match what the Blueprint mutator sent
	if (ClassName.Contains(TEXT("SniperHeadShot"))) return TEXT("SniperHeadshot");
	if (ClassName.Contains(TEXT("Sniper"))) return TEXT("Sniper");
	if (ClassName.Contains(TEXT("ShockCombo"))) return TEXT("ShockCombo");
	if (ClassName.Contains(TEXT("ShockPrimary")) || ClassName.Contains(TEXT("ShockCore"))) return TEXT("ShockCore");
	if (ClassName.Contains(TEXT("ShockBeam")) || ClassName.Contains(TEXT("ShockSecondary"))) return TEXT("ShockBeam");
	if (ClassName.Contains(TEXT("ImpactHammer"))) return TEXT("ImpactHammer");
	if (ClassName.Contains(TEXT("Enforcer"))) return TEXT("Enforcer");
	if (ClassName.Contains(TEXT("Bio"))) return TEXT("BioRifle");
	if (ClassName.Contains(TEXT("LinkBeam"))) return TEXT("LinkBeam");
	if (ClassName.Contains(TEXT("Link"))) return TEXT("Link");
	if (ClassName.Contains(TEXT("MinigunShard"))) return TEXT("MinigunShard");
	if (ClassName.Contains(TEXT("Minigun"))) return TEXT("Minigun");
	if (ClassName.Contains(TEXT("FlakShard")) || ClassName.Contains(TEXT("FlakShred"))) return TEXT("FlakShard");
	if (ClassName.Contains(TEXT("FlakShell"))) return TEXT("FlakShell");
	if (ClassName.Contains(TEXT("Rocket"))) return TEXT("Rocket");
	// LG Blueprint reskin: UT+LG_Headshot_C, UT+LG_C, etc.
	if (ClassName.Contains(TEXT("LG_Headshot")) || ClassName.Contains(TEXT("LightningHeadshot")) || ClassName.Contains(TEXT("LightningRifleHeadshot"))) return TEXT("LightningRifleHeadshot");
	if (ClassName.Contains(TEXT("LG_Secondary")) || ClassName.Contains(TEXT("LightningSecondary")) || ClassName.Contains(TEXT("LightningSec"))) return TEXT("LightningRifleSecondary");
	if (ClassName.Contains(TEXT("UT+LG")) || ClassName.Contains(TEXT("Lightning"))) return TEXT("LightningRiflePrimary");
	if (ClassName.Contains(TEXT("Redeemer"))) return TEXT("Redeemer");
	if (ClassName.Contains(TEXT("Telefrag"))) return TEXT("Telefrag");
	if (ClassName.Contains(TEXT("Translocator"))) return TEXT("Translocator");

	// Unknown — log it so we can add the mapping, then return raw class name
	UE_LOG(LogStatSQL, Warning, TEXT("Unknown damage type: %s"), *ClassName);
	return ClassName;
}

void AMutStatSQL::ScoreObject_Implementation(AUTCarriedObject* GameObject, AUTCharacter* HolderPawn,
	AUTPlayerState* Holder, FName Reason)
{
	Super::ScoreObject_Implementation(GameObject, HolderPawn, Holder, Reason);

	if (!bEnabled || !bMatchInProgress || !Holder) return;

	FString HolderID = GetStatsID(Holder);
	if (HolderID.IsEmpty()) return;

	FString ReasonStr = Reason.ToString();

	// Helper to close out a flag carry and stop timer if no more active carries
	auto CloseCarry = [&](const FString& PlayerID, const FString& CarryResult)
	{
		if (ActiveFlagCarries.Contains(PlayerID))
		{
			FFlagCarryInstance& Carry = ActiveFlagCarries[PlayerID];
			Carry.DropOrCapTime = GetMatchSeconds();
			Carry.Duration = Carry.DropOrCapTime - Carry.GrabTime;
			Carry.Result = CarryResult;

			// Record final position
			AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
			if (GS)
			{
				for (APlayerState* BasePS : GS->PlayerArray)
				{
					AUTPlayerState* PS = Cast<AUTPlayerState>(BasePS);
					if (PS && PS->StatsID == PlayerID)
					{
						APawn* Pawn = PS->GetOwner() ? Cast<AController>(PS->GetOwner())->GetPawn() : nullptr;
						if (Pawn)
						{
							FFlagRoutePoint EndPoint;
							EndPoint.MatchSeconds = Carry.DropOrCapTime;
							FVector Loc = Pawn->GetActorLocation();
							EndPoint.Location = FString::Printf(TEXT("%.0f,%.0f,%.0f"), Loc.X, Loc.Y, Loc.Z);
							Carry.Route.Add(EndPoint);
						}
						break;
					}
				}
			}

			if (FPlayerMatchData* Data = PlayerData.Find(PlayerID))
			{
				Data->FlagStats.CarryInstances.Add(Carry);
			}
			ActiveFlagCarries.Remove(PlayerID);

			// Stop sampling timer if no more active carries
			if (ActiveFlagCarries.Num() == 0 && FlagRouteSampleTimer.IsValid())
			{
				GetWorldTimerManager().ClearTimer(FlagRouteSampleTimer);
			}
		}
	};

	// Flag capture
	if (Reason == FName(TEXT("FlagCapture")))
	{
		CloseCarry(HolderID, TEXT("capped"));
		AddTimelineEvent(TEXT("flag_cap"), HolderID);
	}
	// Flag return
	else if (Reason == FName(TEXT("SentHome")))
	{
		AddTimelineEvent(TEXT("flag_return"), HolderID);
	}
	// Flag deny
	else if (Reason == FName(TEXT("FlagDeny")))
	{
		AddTimelineEvent(TEXT("flag_deny"), HolderID);
	}
	// Flag grab - start carry tracking with route sampling
	else if (Reason == FName(TEXT("FlagGrab")) || Reason == FName(TEXT("FlagFirstGrab")))
	{
		FFlagCarryInstance NewCarry;
		NewCarry.GrabTime = GetMatchSeconds();
		NewCarry.Period = GetCurrentPeriod();
		NewCarry.CarrierID = HolderID;
		NewCarry.CarrierName = Holder->PlayerName;
		NewCarry.Team = (Holder->Team && Holder->Team->TeamIndex == 0) ? TEXT("Red") : TEXT("Blue");

		// Record grab location as first route point
		if (HolderPawn)
		{
			FFlagRoutePoint GrabPoint;
			GrabPoint.MatchSeconds = NewCarry.GrabTime;
			FVector Loc = HolderPawn->GetActorLocation();
			GrabPoint.Location = FString::Printf(TEXT("%.0f,%.0f,%.0f"), Loc.X, Loc.Y, Loc.Z);
			NewCarry.Route.Add(GrabPoint);
		}

		ActiveFlagCarries.Add(HolderID, NewCarry);
		AddTimelineEvent(TEXT("flag_grab"), HolderID);

		// Start route sampling timer if not already running
		if (!FlagRouteSampleTimer.IsValid())
		{
			GetWorldTimerManager().SetTimer(FlagRouteSampleTimer, this,
				&AMutStatSQL::SampleFlagCarrierPositions, 0.5f, true);
		}
	}
	// Flag drop
	else if (Reason == FName(TEXT("FlagDrop")))
	{
		CloseCarry(HolderID, TEXT("dropped"));
		AddTimelineEvent(TEXT("flag_drop"), HolderID);
	}
}

void AMutStatSQL::SampleFlagCarrierPositions()
{
	if (ActiveFlagCarries.Num() == 0) return;

	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS) return;

	float Now = GetMatchSeconds();

	for (auto& Pair : ActiveFlagCarries)
	{
		FFlagCarryInstance& Carry = Pair.Value;

		// Find the carrier's pawn
		for (APlayerState* BasePS : GS->PlayerArray)
		{
			AUTPlayerState* PS = Cast<AUTPlayerState>(BasePS);
			if (PS && PS->StatsID == Pair.Key)
			{
				AController* C = Cast<AController>(PS->GetOwner());
				APawn* Pawn = C ? C->GetPawn() : nullptr;
				if (Pawn)
				{
					FFlagRoutePoint Point;
					Point.MatchSeconds = Now;
					FVector Loc = Pawn->GetActorLocation();
					Point.Location = FString::Printf(TEXT("%.0f,%.0f,%.0f"), Loc.X, Loc.Y, Loc.Z);
					Carry.Route.Add(Point);
				}
				break;
			}
		}
	}
}

void AMutStatSQL::NotifyMatchStateChange_Implementation(FName NewState)
{
	Super::NotifyMatchStateChange_Implementation(NewState);

	if (!bEnabled) return;

	if (NewState == MatchState::InProgress)
	{
		bMatchInProgress = true;
		float Now = GetWorld()->GetTimeSeconds();

		if (!bFirstRoundStarted)
		{
			// First round — clear everything
			bFirstRoundStarted = true;
			DamageLog.Empty();
			Timeline.Empty();
			AccumulatedRoundTime = 0.f;
		}
		else if (LastRoundStartWorldTime > 0.f)
		{
			// Subsequent round — accumulate elapsed time from the previous round
			AccumulatedRoundTime += (Now - LastRoundStartWorldTime);
		}

		LastRoundStartWorldTime = Now;
		MatchStartWorldTime = Now;

		AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
		if (GS)
		{
			CachedTimeLimit = GS->TimeLimit;
		}

		// Try binding to TeamArena delegates
		TryBindTeamArenaEvents();

		UE_LOG(LogStatSQL, Log, TEXT("Match started - Map: %s, Mode: %s, TimeLimit: %d"),
			*CachedMapName, *CachedGameMode, CachedTimeLimit);
	}
	else if (NewState == MatchState::WaitingPostMatch || NewState == MatchState::MapVoteHappening)
	{
		if (bMatchInProgress)
		{
			bMatchInProgress = false;
			UE_LOG(LogStatSQL, Log, TEXT("Match ended - collecting stats and submitting"));
			CollectEndOfMatchStats();
			SubmitMatchData();
		}
	}
	else if (NewState == MatchState::MatchIsInOvertime)
	{
		AddTimelineEvent(TEXT("overtime"), FString());
		UE_LOG(LogStatSQL, Log, TEXT("Match entered overtime"));
	}
}

// ============================================================
// TeamArena delegate binding
// ============================================================

void AMutStatSQL::TryBindTeamArenaEvents()
{
	// Check if the game mode is AUTeamArenaGame by looking for the delegates
	// We use FindObject rather than a hard dependency to avoid linking issues
	AGameModeBase* GM = GetWorld()->GetAuthGameMode();
	if (!GM) return;

	// Check class name to avoid hard dependency on TeamArena module
	FString ClassName = GM->GetClass()->GetName();
	if (!ClassName.Contains(TEXT("TeamArena"))) return;

	// Use UFunction-based delegate binding via reflection
	UFunction* ACEFunc = GM->FindFunction(FName(TEXT("OnPlayerACE")));
	UFunction* DarkHorseFunc = GM->FindFunction(FName(TEXT("OnPlayerDarkHorse")));
	UFunction* ClutchFunc = GM->FindFunction(FName(TEXT("OnClutchSituationStarted")));

	// Bind via the multicast delegate properties
	FMulticastScriptDelegate* ACEDelegate = nullptr;
	FMulticastScriptDelegate* DarkHorseDelegate = nullptr;
	FMulticastScriptDelegate* ClutchDelegate = nullptr;

	for (TFieldIterator<UProperty> PropIt(GM->GetClass()); PropIt; ++PropIt)
	{
		UProperty* Prop = *PropIt;
		if (UMulticastDelegateProperty* DelProp = Cast<UMulticastDelegateProperty>(Prop))
		{
			if (Prop->GetName() == TEXT("OnPlayerACE"))
			{
				ACEDelegate = DelProp->GetPropertyValuePtr_InContainer(GM);
			}
			else if (Prop->GetName() == TEXT("OnPlayerDarkHorse"))
			{
				DarkHorseDelegate = DelProp->GetPropertyValuePtr_InContainer(GM);
			}
			else if (Prop->GetName() == TEXT("OnClutchSituationStarted"))
			{
				ClutchDelegate = DelProp->GetPropertyValuePtr_InContainer(GM);
			}
		}
	}

	if (ACEDelegate)
	{
		FScriptDelegate Del;
		Del.BindUFunction(this, FName(TEXT("OnTeamArenaACE")));
		ACEDelegate->AddUnique(Del);
		UE_LOG(LogStatSQL, Log, TEXT("Bound to TeamArena OnPlayerACE delegate"));
	}
	if (DarkHorseDelegate)
	{
		FScriptDelegate Del;
		Del.BindUFunction(this, FName(TEXT("OnTeamArenaDarkHorse")));
		DarkHorseDelegate->AddUnique(Del);
		UE_LOG(LogStatSQL, Log, TEXT("Bound to TeamArena OnPlayerDarkHorse delegate"));
	}
	if (ClutchDelegate)
	{
		FScriptDelegate Del;
		Del.BindUFunction(this, FName(TEXT("OnTeamArenaClutch")));
		ClutchDelegate->AddUnique(Del);
		UE_LOG(LogStatSQL, Log, TEXT("Bound to TeamArena OnClutchSituationStarted delegate"));
	}
}

void AMutStatSQL::OnTeamArenaACE(AUTPlayerState* PlayerState)
{
	if (PlayerState)
	{
		AddTimelineEvent(TEXT("ace"), GetStatsID(PlayerState));
	}
}

void AMutStatSQL::OnTeamArenaDarkHorse(AUTPlayerState* PlayerState, int32 EnemiesKilled)
{
	if (PlayerState)
	{
		AddTimelineEvent(TEXT("dark_horse"), GetStatsID(PlayerState), FString(),
			FString::Printf(TEXT("1v%d"), EnemiesKilled));
	}
}

void AMutStatSQL::OnTeamArenaClutch(AUTPlayerState* ClutchPlayer, int32 EnemiesAlive)
{
	if (ClutchPlayer)
	{
		AddTimelineEvent(TEXT("clutch"), GetStatsID(ClutchPlayer), FString(),
			FString::Printf(TEXT("1v%d"), EnemiesAlive));
	}
}

// ============================================================
// Stat snapshot (from UTPlayerState::StatsData)
// ============================================================

void AMutStatSQL::SnapshotPlayerStats(AUTPlayerState* PS, FPlayerMatchData& OutData)
{
	if (!PS || OutData.bStatsSnapshotted) return;

	// Basic stats from properties
	OutData.Score = PS->Score;
	OutData.Kills = PS->Kills;
	OutData.Deaths = PS->Deaths;
	OutData.Ping = PS->Ping;
	if (PS->Team)
	{
		OutData.TeamIndex = PS->Team->TeamIndex;
	}

	// Multi-kills from StatsData
	OutData.MultiKills[0] = (int32)PS->GetStatsValue(NAME_MultiKillLevel0);
	OutData.MultiKills[1] = (int32)PS->GetStatsValue(NAME_MultiKillLevel1);
	OutData.MultiKills[2] = (int32)PS->GetStatsValue(NAME_MultiKillLevel2);
	OutData.MultiKills[3] = (int32)PS->GetStatsValue(NAME_MultiKillLevel3);

	// Kill sprees
	OutData.Sprees[0] = (int32)PS->GetStatsValue(NAME_SpreeKillLevel0);
	OutData.Sprees[1] = (int32)PS->GetStatsValue(NAME_SpreeKillLevel1);
	OutData.Sprees[2] = (int32)PS->GetStatsValue(NAME_SpreeKillLevel2);
	OutData.Sprees[3] = (int32)PS->GetStatsValue(NAME_SpreeKillLevel3);
	OutData.Sprees[4] = (int32)PS->GetStatsValue(NAME_SpreeKillLevel4);

	// Special
	OutData.AirRox = (int32)PS->GetStatsValue(NAME_AirRox);
	OutData.FlakShreds = (int32)PS->GetStatsValue(NAME_FlakShreds);

	// Damage (use engine-tracked value as authoritative)
	OutData.DamageDone = FMath::Max(OutData.DamageDone, (int32)PS->DamageDone);

	// ---- Weapon kills/deaths ----
	auto SetWeapon = [&](FName StatKills, FName StatDeaths, FName Key)
	{
		int32 K = (int32)PS->GetStatsValue(StatKills);
		int32 D = (int32)PS->GetStatsValue(StatDeaths);
		if (K > 0 || D > 0)
		{
			FStatSQLWeaponData WD;
			WD.Kills = K;
			WD.Deaths = D;
			OutData.WeaponStats.Add(Key, WD);
		}
	};

	SetWeapon(NAME_ImpactHammerKills, NAME_ImpactHammerDeaths, FName(TEXT("ImpactHammerKills")));
	SetWeapon(NAME_EnforcerKills, NAME_EnforcerDeaths, FName(TEXT("EnforcerKills")));
	SetWeapon(NAME_BioRifleKills, NAME_BioRifleDeaths, FName(TEXT("BioRifleKills")));
	SetWeapon(NAME_ShockBeamKills, NAME_ShockBeamDeaths, FName(TEXT("ShockBeamKills")));
	SetWeapon(NAME_ShockCoreKills, NAME_ShockCoreDeaths, FName(TEXT("ShockCoreKills")));
	SetWeapon(NAME_ShockComboKills, NAME_ShockComboDeaths, FName(TEXT("ShockComboKills")));
	SetWeapon(NAME_LinkKills, NAME_LinkDeaths, FName(TEXT("LinkKills")));
	SetWeapon(NAME_LinkBeamKills, NAME_LinkBeamDeaths, FName(TEXT("LinkBeamKills")));
	SetWeapon(NAME_MinigunKills, NAME_MinigunDeaths, FName(TEXT("MinigunKills")));
	SetWeapon(NAME_MinigunShardKills, NAME_MinigunShardDeaths, FName(TEXT("MinigunShardKills")));
	SetWeapon(NAME_FlakShardKills, NAME_FlakShardDeaths, FName(TEXT("FlakShardKills")));
	SetWeapon(NAME_FlakShellKills, NAME_FlakShellDeaths, FName(TEXT("FlakShellKills")));
	SetWeapon(NAME_RocketKills, NAME_RocketDeaths, FName(TEXT("RocketKills")));
	SetWeapon(NAME_SniperKills, NAME_SniperDeaths, FName(TEXT("SniperKills")));
	SetWeapon(NAME_SniperHeadshotKills, NAME_SniperHeadshotDeaths, FName(TEXT("SniperHeadshotKills")));
	SetWeapon(NAME_LightningRiflePrimaryKills, NAME_LightningRiflePrimaryDeaths, FName(TEXT("LightningRiflePrimaryKills")));
	SetWeapon(NAME_LightningRifleSecondaryKills, NAME_LightningRifleSecondaryDeaths, FName(TEXT("LightningRifleSecondaryKills")));
	SetWeapon(NAME_LightningRifleTertiaryKills, NAME_LightningRifleTertiaryDeaths, FName(TEXT("LightningRifleTertiaryKills")));
	SetWeapon(NAME_RedeemerKills, NAME_RedeemerDeaths, FName(TEXT("RedeemerKills")));
	SetWeapon(NAME_TelefragKills, NAME_TelefragDeaths, FName(TEXT("TelefragKills")));

	// ---- Accuracy ----
	auto SetAccuracy = [&](FName StatShots, FName StatHits, FName Key)
	{
		float S = PS->GetStatsValue(StatShots);
		float H = PS->GetStatsValue(StatHits);
		if (S > 0.f || H > 0.f)
		{
			FStatSQLAccuracyData AD;
			AD.Shots = S;
			AD.Hits = H;
			OutData.AccuracyStats.Add(Key, AD);
		}
	};

	SetAccuracy(NAME_EnforcerShots, NAME_EnforcerHits, FName(TEXT("EnforcerShots")));
	SetAccuracy(NAME_BioRifleShots, NAME_BioRifleHits, FName(TEXT("BioRifleShots")));
	SetAccuracy(NAME_ShockRifleShots, NAME_ShockRifleHits, FName(TEXT("ShockRifleShots")));
	SetAccuracy(NAME_LinkShots, NAME_LinkHits, FName(TEXT("LinkShots")));
	SetAccuracy(NAME_MinigunShots, NAME_MinigunHits, FName(TEXT("MinigunShots")));
	SetAccuracy(NAME_FlakShots, NAME_FlakHits, FName(TEXT("FlakShots")));
	SetAccuracy(NAME_RocketShots, NAME_RocketHits, FName(TEXT("RocketShots")));
	SetAccuracy(NAME_SniperShots, NAME_SniperHits, FName(TEXT("SniperShots")));
	SetAccuracy(NAME_LightningRifleShots, NAME_LightningRifleHits, FName(TEXT("LightningRifleShots")));
	SetAccuracy(NAME_InstagibShots, NAME_InstagibHits, FName(TEXT("InstagibShots")));

	// ---- Movement ----
	OutData.MovementStats.RunDist = PS->GetStatsValue(NAME_RunDist);
	OutData.MovementStats.InAirDist = PS->GetStatsValue(NAME_InAirDist);
	OutData.MovementStats.TranslocDist = PS->GetStatsValue(NAME_TranslocDist);
	OutData.MovementStats.SlideDist = PS->GetStatsValue(NAME_SlideDist);
	OutData.MovementStats.WallRunDist = PS->GetStatsValue(NAME_WallRunDist);
	OutData.MovementStats.NumDodges = PS->GetStatsValue(NAME_NumDodges);
	OutData.MovementStats.NumWallDodges = PS->GetStatsValue(NAME_NumWallDodges);
	OutData.MovementStats.NumJumps = PS->GetStatsValue(NAME_NumJumps);
	OutData.MovementStats.NumFloorSlides = PS->GetStatsValue(NAME_NumFloorSlides);
	OutData.MovementStats.NumWallRuns = PS->GetStatsValue(NAME_NumWallRuns);
	OutData.MovementStats.NumImpactJumps = PS->GetStatsValue(NAME_NumImpactJumps);
	OutData.MovementStats.NumLiftJumps = PS->GetStatsValue(NAME_NumLiftJumps);

	// ---- Items ----
	OutData.ItemStats.ShieldBeltCount = (int32)PS->GetStatsValue(NAME_ShieldBeltCount);
	OutData.ItemStats.ArmorVestCount = (int32)PS->GetStatsValue(NAME_ArmorVestCount);
	OutData.ItemStats.ArmorPadsCount = (int32)PS->GetStatsValue(NAME_ArmorPadsCount);
	OutData.ItemStats.HelmetCount = (int32)PS->GetStatsValue(NAME_HelmetCount);
	OutData.ItemStats.UDamageCount = (int32)PS->GetStatsValue(NAME_UDamageCount);
	OutData.ItemStats.UDamageTime = (int32)PS->GetStatsValue(NAME_UDamageTime);

	// ---- Flag stats ----
	OutData.FlagStats.FlagCaptures = (int32)PS->GetStatsValue(NAME_FlagCaptures);
	OutData.FlagStats.FlagReturns = (int32)PS->GetStatsValue(NAME_FlagReturns);
	OutData.FlagStats.FlagAssists = (int32)PS->GetStatsValue(NAME_FlagAssists);
	OutData.FlagStats.FlagGrabs = (int32)PS->GetStatsValue(NAME_FlagGrabs);
	OutData.FlagStats.FlagHeldDeny = (int32)PS->GetStatsValue(NAME_FlagHeldDeny);
	OutData.FlagStats.FlagHeldDenyTime = (int32)PS->GetStatsValue(NAME_FlagHeldDenyTime);
	OutData.FlagStats.FlagReturnPoints = (int32)PS->GetStatsValue(NAME_FlagReturnPoints);
	OutData.FlagStats.CarryAssists = (int32)PS->GetStatsValue(NAME_CarryAssist);
	OutData.FlagStats.CarryAssistPoints = (int32)PS->GetStatsValue(NAME_CarryAssistPoints);
	OutData.FlagStats.FlagCapPoints = (int32)PS->GetStatsValue(NAME_FlagCapPoints);
	OutData.FlagStats.DefendAssist = (int32)PS->GetStatsValue(NAME_DefendAssist);
	OutData.FlagStats.DefendAssistPoints = (int32)PS->GetStatsValue(NAME_DefendAssistPoints);
	OutData.FlagStats.ReturnAssist = (int32)PS->GetStatsValue(NAME_ReturnAssist);
	OutData.FlagStats.ReturnAssistPoints = (int32)PS->GetStatsValue(NAME_ReturnAssistPoints);
	OutData.FlagStats.EnemyFCDamage = (int32)PS->GetStatsValue(NAME_EnemyFCDamage);
	OutData.FlagStats.FCKills = (int32)PS->GetStatsValue(NAME_FCKills);
	OutData.FlagStats.FCKillPoints = (int32)PS->GetStatsValue(NAME_FCKillPoints);
	OutData.FlagStats.FlagSupportKills = (int32)PS->GetStatsValue(NAME_FlagSupportKills);
	OutData.FlagStats.RegularKillPoints = (int32)PS->GetStatsValue(NAME_RegularKillPoints);
	OutData.FlagStats.AttackerScore = (int32)PS->GetStatsValue(NAME_AttackerScore);
	OutData.FlagStats.DefenderScore = (int32)PS->GetStatsValue(NAME_DefenderScore);

	OutData.bStatsSnapshotted = true;
}

void AMutStatSQL::CollectEndOfMatchStats()
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS) return;

	// Snapshot stats for all connected players (disconnected ones already done)
	for (APlayerState* BasePS : GS->PlayerArray)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(BasePS);
		if (!PS || PS->bIsABot || PS->bOnlySpectator) continue;

		FPlayerMatchData* Data = GetOrCreatePlayerData(PS);
		if (Data && !Data->bStatsSnapshotted)
		{
			SnapshotPlayerStats(PS, *Data);
		}
	}

	UE_LOG(LogStatSQL, Log, TEXT("End-of-match stats collected for %d players"), PlayerData.Num());
}

// ============================================================
// HTTP submission
// ============================================================

void AMutStatSQL::SendPost(const FString& Endpoint, const FString& JsonBody,
	TFunction<void(bool, const FString&)> OnComplete, int32 RetryCount)
{
	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(ApiBaseUrl + Endpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	// Auth format: "Authorization: Token <key>" (matching Django TokenAuthentication)
	if (!ApiAuthKey.IsEmpty())
	{
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Token %s"), *ApiAuthKey));
	}

	Request->SetContentAsString(JsonBody);

	// Capture what we need for retry (copy strings, not 'this')
	FString CapturedUrl = ApiBaseUrl;
	FString CapturedEndpoint = Endpoint;
	FString CapturedBody = JsonBody;
	FString CapturedAuth = ApiAuthKey;

	Request->OnProcessRequestComplete().BindLambda(
		[this, OnComplete, RetryCount, CapturedEndpoint, CapturedBody](
			FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnected)
	{
		bool bSuccess = bConnected && Resp.IsValid() &&
			EHttpResponseCodes::IsOk(Resp->GetResponseCode());

		if (!bSuccess && RetryCount < MaxRetries)
		{
			// Retry with exponential backoff
			float Delay = FMath::Pow(2.f, (float)RetryCount); // 1s, 2s, 4s
			UE_LOG(LogStatSQL, Warning, TEXT("POST %s failed (attempt %d/%d), retrying in %.0fs"),
				*CapturedEndpoint, RetryCount + 1, MaxRetries, Delay);

			FTimerHandle RetryHandle;
			GetWorldTimerManager().SetTimer(RetryHandle, [this, CapturedEndpoint, CapturedBody, OnComplete, RetryCount]()
			{
				SendPost(CapturedEndpoint, CapturedBody, OnComplete, RetryCount + 1);
			}, Delay, false);
			return;
		}

		if (!bSuccess)
		{
			int32 Code = Resp.IsValid() ? Resp->GetResponseCode() : 0;
			UE_LOG(LogStatSQL, Error, TEXT("POST %s FAILED after %d attempts (HTTP %d)"),
				*CapturedEndpoint, MaxRetries, Code);
		}

		FString ResponseBody = (bSuccess && Resp.IsValid()) ? Resp->GetContentAsString() : FString();
		OnComplete(bSuccess, ResponseBody);
	});

	Request->ProcessRequest();
}

void AMutStatSQL::SubmitMatchData()
{
	if (!bEnabled || ApiBaseUrl.IsEmpty())
	{
		UE_LOG(LogStatSQL, Warning, TEXT("Submission skipped - disabled or no URL configured"));
		return;
	}

	if (PlayerData.Num() == 0)
	{
		UE_LOG(LogStatSQL, Warning, TEXT("Submission skipped - no player data"));
		return;
	}

	UE_LOG(LogStatSQL, Log, TEXT("Starting match submission chain (%d players, %d timeline events, %d damage events)"),
		PlayerData.Num(), Timeline.Num(), DamageLog.Num());

	PostInsertMatch();
}

void AMutStatSQL::PostInsertMatch()
{
	auto Json = StatSQLJson::BuildInsertMatch(CachedGameMode, CachedMapName, CachedServerName);
	FString Body = StatSQLJson::Serialize(Json);

	SendPost(TEXT("/json_entry/"), Body, [this](bool bOK, const FString& Response)
	{
		if (!bOK)
		{
			UE_LOG(LogStatSQL, Error, TEXT("insert_match failed - aborting submission"));
			return;
		}

		// Parse matchid from response
		TSharedPtr<FJsonObject> JsonObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response);
		if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
		{
			// matchid may come as number or string
			if (JsonObj->HasField(TEXT("matchid")))
			{
				double MatchIdNum;
				if (JsonObj->TryGetNumberField(TEXT("matchid"), MatchIdNum))
				{
					RemoteMatchId = FString::Printf(TEXT("%d"), (int32)MatchIdNum);
				}
				else
				{
					RemoteMatchId = JsonObj->GetStringField(TEXT("matchid"));
				}
			}
		}

		UE_LOG(LogStatSQL, Log, TEXT("insert_match OK - MatchId: %s"), *RemoteMatchId);

		// Check if server needs the map image (fire-and-forget, doesn't block the chain)
		CheckAndUploadMapImage();

		PostInsertPlayers();
	});
}

void AMutStatSQL::PostInsertPlayers()
{
	// Build array of all player submissions
	TArray<TPair<FString, FString>> PlayerSubmissions;
	for (const auto& Pair : PlayerData)
	{
		const FPlayerMatchData& Data = Pair.Value;
		auto Json = StatSQLJson::BuildInsertPlayer(Data.StatsID, Data.PlayerName);
		TPair<FString, FString> Entry;
		Entry.Key = Data.StatsID;
		Entry.Value = StatSQLJson::Serialize(Json);
		PlayerSubmissions.Add(Entry);
	}

	// Submit players sequentially using a shared index
	TSharedPtr<int32> Index = MakeShareable(new int32(0));
	TSharedPtr<TArray<TPair<FString, FString>>> Submissions = MakeShareable(
		new TArray<TPair<FString, FString>>(MoveTemp(PlayerSubmissions)));

	auto SubmitNext = [this, Index, Submissions]()
	{
		// Forward declare for recursive lambda
	};

	// Use a helper to process sequentially
	struct FPlayerSubmitter
	{
		static void SubmitNext(AMutStatSQL* Self, TSharedPtr<int32> Idx,
			TSharedPtr<TArray<TPair<FString, FString>>> Subs)
		{
			if (*Idx >= Subs->Num())
			{
				// All players submitted, move to next step
				Self->PostInsertMatchStats();
				return;
			}

			const FString& Body = (*Subs)[*Idx].Value;
			Self->SendPost(TEXT("/json_entry/"), Body, [Self, Idx, Subs](bool bOK, const FString& Response)
			{
				if (!bOK)
				{
					UE_LOG(LogStatSQL, Warning, TEXT("insert_player failed for player %d"), *Idx);
				}
				(*Idx)++;
				SubmitNext(Self, Idx, Subs);
			});
		}
	};

	FPlayerSubmitter::SubmitNext(this, Index, Submissions);
}

void AMutStatSQL::PostInsertMatchStats()
{
	TArray<FString> Bodies;
	for (const auto& Pair : PlayerData)
	{
		auto Json = StatSQLJson::BuildInsertMatchStats(RemoteMatchId, Pair.Value);
		Bodies.Add(StatSQLJson::Serialize(Json));
	}

	TSharedPtr<int32> Index = MakeShareable(new int32(0));
	TSharedPtr<TArray<FString>> Subs = MakeShareable(new TArray<FString>(MoveTemp(Bodies)));

	struct FSubmitter
	{
		static void Next(AMutStatSQL* Self, TSharedPtr<int32> Idx, TSharedPtr<TArray<FString>> S)
		{
			if (*Idx >= S->Num()) { Self->PostInsertItems(); return; }
			Self->SendPost(TEXT("/json_entry/"), (*S)[*Idx], [Self, Idx, S](bool bOK, const FString&)
			{
				if (!bOK) UE_LOG(LogStatSQL, Warning, TEXT("insert_matchstats failed for player %d"), *Idx);
				(*Idx)++;
				Next(Self, Idx, S);
			});
		}
	};
	FSubmitter::Next(this, Index, Subs);
}

void AMutStatSQL::PostInsertItems()
{
	TArray<FString> Bodies;
	for (const auto& Pair : PlayerData)
	{
		auto Json = StatSQLJson::BuildInsertItem(RemoteMatchId, Pair.Value);
		Bodies.Add(StatSQLJson::Serialize(Json));
	}

	TSharedPtr<int32> Index = MakeShareable(new int32(0));
	TSharedPtr<TArray<FString>> Subs = MakeShareable(new TArray<FString>(MoveTemp(Bodies)));

	struct FSubmitter
	{
		static void Next(AMutStatSQL* Self, TSharedPtr<int32> Idx, TSharedPtr<TArray<FString>> S)
		{
			if (*Idx >= S->Num()) { Self->PostInsertWeapons(); return; }
			Self->SendPost(TEXT("/json_entry/"), (*S)[*Idx], [Self, Idx, S](bool bOK, const FString&)
			{
				if (!bOK) UE_LOG(LogStatSQL, Warning, TEXT("insert_item failed for player %d"), *Idx);
				(*Idx)++;
				Next(Self, Idx, S);
			});
		}
	};
	FSubmitter::Next(this, Index, Subs);
}

void AMutStatSQL::PostInsertWeapons()
{
	TArray<FString> Bodies;
	for (const auto& Pair : PlayerData)
	{
		auto Json = StatSQLJson::BuildInsertWeapon(RemoteMatchId, Pair.Value);
		Bodies.Add(StatSQLJson::Serialize(Json));
	}

	TSharedPtr<int32> Index = MakeShareable(new int32(0));
	TSharedPtr<TArray<FString>> Subs = MakeShareable(new TArray<FString>(MoveTemp(Bodies)));

	struct FSubmitter
	{
		static void Next(AMutStatSQL* Self, TSharedPtr<int32> Idx, TSharedPtr<TArray<FString>> S)
		{
			if (*Idx >= S->Num()) { Self->PostInsertAccuracy(); return; }
			Self->SendPost(TEXT("/weapon_entry/"), (*S)[*Idx], [Self, Idx, S](bool bOK, const FString&)
			{
				if (!bOK) UE_LOG(LogStatSQL, Warning, TEXT("insert_weapon failed for player %d"), *Idx);
				(*Idx)++;
				Next(Self, Idx, S);
			});
		}
	};
	FSubmitter::Next(this, Index, Subs);
}

void AMutStatSQL::PostInsertAccuracy()
{
	TArray<FString> Bodies;
	for (const auto& Pair : PlayerData)
	{
		auto Json = StatSQLJson::BuildInsertAccuracy(RemoteMatchId, Pair.Value);
		Bodies.Add(StatSQLJson::Serialize(Json));
	}

	TSharedPtr<int32> Index = MakeShareable(new int32(0));
	TSharedPtr<TArray<FString>> Subs = MakeShareable(new TArray<FString>(MoveTemp(Bodies)));

	struct FSubmitter
	{
		static void Next(AMutStatSQL* Self, TSharedPtr<int32> Idx, TSharedPtr<TArray<FString>> S)
		{
			if (*Idx >= S->Num()) { Self->PostInsertMovement(); return; }
			Self->SendPost(TEXT("/weapon_entry/"), (*S)[*Idx], [Self, Idx, S](bool bOK, const FString&)
			{
				if (!bOK) UE_LOG(LogStatSQL, Warning, TEXT("insert_accuracy failed for player %d"), *Idx);
				(*Idx)++;
				Next(Self, Idx, S);
			});
		}
	};
	FSubmitter::Next(this, Index, Subs);
}

void AMutStatSQL::PostInsertMovement()
{
	TArray<FString> Bodies;
	for (const auto& Pair : PlayerData)
	{
		auto Json = StatSQLJson::BuildInsertMovement(RemoteMatchId, Pair.Value);
		Bodies.Add(StatSQLJson::Serialize(Json));
	}

	TSharedPtr<int32> Index = MakeShareable(new int32(0));
	TSharedPtr<TArray<FString>> Subs = MakeShareable(new TArray<FString>(MoveTemp(Bodies)));

	struct FSubmitter
	{
		static void Next(AMutStatSQL* Self, TSharedPtr<int32> Idx, TSharedPtr<TArray<FString>> S)
		{
			if (*Idx >= S->Num()) { Self->PostInsertFlagStats(); return; }
			Self->SendPost(TEXT("/movement_entry/"), (*S)[*Idx], [Self, Idx, S](bool bOK, const FString&)
			{
				if (!bOK) UE_LOG(LogStatSQL, Warning, TEXT("insert_movement failed for player %d"), *Idx);
				(*Idx)++;
				Next(Self, Idx, S);
			});
		}
	};
	FSubmitter::Next(this, Index, Subs);
}

void AMutStatSQL::PostInsertFlagStats()
{
	// Only submit flag stats for CTF/FlagRun game modes
	bool bIsCTF = CachedGameMode.Contains(TEXT("CTF")) || CachedGameMode.Contains(TEXT("FlagRun"));
	if (!bIsCTF)
	{
		// Skip flag stats, go to kill feed
		PostKillFeed();
		return;
	}

	TArray<FString> Bodies;
	for (const auto& Pair : PlayerData)
	{
		auto Json = StatSQLJson::BuildInsertFlagStats(RemoteMatchId, Pair.Value);
		Bodies.Add(StatSQLJson::Serialize(Json));
	}

	TSharedPtr<int32> Index = MakeShareable(new int32(0));
	TSharedPtr<TArray<FString>> Subs = MakeShareable(new TArray<FString>(MoveTemp(Bodies)));

	struct FSubmitter
	{
		static void Next(AMutStatSQL* Self, TSharedPtr<int32> Idx, TSharedPtr<TArray<FString>> S)
		{
			if (*Idx >= S->Num()) { Self->PostKillFeed(); return; }
			Self->SendPost(TEXT("/flag_entry/"), (*S)[*Idx], [Self, Idx, S](bool bOK, const FString&)
			{
				if (!bOK) UE_LOG(LogStatSQL, Warning, TEXT("insert_flag_stats failed for player %d"), *Idx);
				(*Idx)++;
				Next(Self, Idx, S);
			});
		}
	};
	FSubmitter::Next(this, Index, Subs);
}

void AMutStatSQL::PostKillFeed()
{
	// Build kill feed from timeline events
	bool bHasKills = false;
	for (const FTimelineEvent& Event : Timeline)
	{
		if (Event.EventType == TEXT("kill") || Event.EventType == TEXT("suicide"))
		{
			bHasKills = true;
			break;
		}
	}

	if (!bHasKills)
	{
		PostDamageFeed();
		return;
	}

	FString Body = StatSQLJson::BuildKillFeed(Timeline);
	FString Endpoint = FString::Printf(TEXT("/kill_feed_utpugs/%s/"), *RemoteMatchId);

	SendPost(Endpoint, Body, [this](bool bOK, const FString&)
	{
		if (!bOK) UE_LOG(LogStatSQL, Warning, TEXT("kill_feed submission failed"));
		PostDamageFeed();
	});
}

void AMutStatSQL::PostDamageFeed()
{
	if (DamageLog.Num() == 0)
	{
		PostTimeline();
		return;
	}

	UE_LOG(LogStatSQL, Log, TEXT("Submitting damage feed: %d events"), DamageLog.Num());

	FString Body = StatSQLJson::BuildDamageFeed(DamageLog);
	FString Endpoint = FString::Printf(TEXT("/damage_feed_utpugs/%s/"), *RemoteMatchId);

	SendPost(Endpoint, Body, [this](bool bOK, const FString&)
	{
		if (!bOK) UE_LOG(LogStatSQL, Warning, TEXT("damage_feed submission failed"));
		PostTimeline();
	});
}

void AMutStatSQL::PostTimeline()
{
	// Inject flag carry routes into the timeline as flag_carry events
	// so the JS heatmap can render them alongside kills
	for (const auto& Pair : PlayerData)
	{
		const FPlayerMatchData& Data = Pair.Value;
		for (const FFlagCarryInstance& Carry : Data.FlagStats.CarryInstances)
		{
			if (Carry.Route.Num() < 2) continue;

			FTimelineEvent CarryEvent;
			CarryEvent.EventType = TEXT("flag_carry");
			CarryEvent.MatchSeconds = Carry.GrabTime;
			CarryEvent.Period = Carry.Period;
			CarryEvent.ActorID = Carry.CarrierID;
			CarryEvent.Detail = Carry.Result;

			// Store route, team, and carrier name as extra fields
			CarryEvent.CarrierName = Carry.CarrierName;
			CarryEvent.Team = Carry.Team;
			CarryEvent.Result = Carry.Result;
			CarryEvent.Route = Carry.Route;

			Timeline.Add(CarryEvent);
		}
	}

	if (Timeline.Num() == 0)
	{
		PostUpdateMatch();
		return;
	}

	auto Json = StatSQLJson::BuildTimeline(RemoteMatchId, Timeline);
	FString Body = StatSQLJson::Serialize(Json);

	FString Endpoint = FString::Printf(TEXT("/timeline_entry/%s/"), *RemoteMatchId);
	SendPost(Endpoint, Body, [this](bool bOK, const FString&)
	{
		if (!bOK)
		{
			UE_LOG(LogStatSQL, Warning, TEXT("Timeline submission failed"));
		}
		PostUpdateMatch();
	});
}

void AMutStatSQL::PostUpdateMatch()
{
	int32 RedKills, BlueKills, RedDeaths, BlueDeaths;
	float RedDamage, BlueDamage;
	ComputeTeamTotals(RedKills, BlueKills, RedDeaths, BlueDeaths, RedDamage, BlueDamage);

	// Get team scores from game state
	int32 RedScore = 0, BlueScore = 0;
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (GS)
	{
		for (AUTTeamInfo* Team : GS->Teams)
		{
			if (Team)
			{
				if (Team->TeamIndex == 0) RedScore = Team->Score;
				else if (Team->TeamIndex == 1) BlueScore = Team->Score;
			}
		}
	}

	auto Json = StatSQLJson::BuildUpdateMatch(RemoteMatchId, RedKills, BlueKills,
		RedScore, BlueScore, RedDeaths, BlueDeaths, RedDamage, BlueDamage,
		BuildGameOptions(), GetReplayId());

	// Add map bounds for kill heatmap overlay on the website
	FBox MapBounds(ForceInit);
	if (GetMinimapWorldBounds(MapBounds))
	{
		TSharedRef<FJsonObject> BoundsObj = MakeShareable(new FJsonObject());
		BoundsObj->SetNumberField(TEXT("min_x"), MapBounds.Min.X);
		BoundsObj->SetNumberField(TEXT("min_y"), MapBounds.Min.Y);
		BoundsObj->SetNumberField(TEXT("min_z"), MapBounds.Min.Z);
		BoundsObj->SetNumberField(TEXT("max_x"), MapBounds.Max.X);
		BoundsObj->SetNumberField(TEXT("max_y"), MapBounds.Max.Y);
		BoundsObj->SetNumberField(TEXT("max_z"), MapBounds.Max.Z);
		BoundsObj->SetNumberField(TEXT("map_size"), 1024); // matches UTHUD minimap texture size
		Json->SetObjectField(TEXT("map_bounds"), BoundsObj);
	}

	FString Body = StatSQLJson::Serialize(Json);

	SendPost(TEXT("/json_entry/"), Body, [this](bool bOK, const FString&)
	{
		if (bOK)
		{
			UE_LOG(LogStatSQL, Log, TEXT("Match submission complete - MatchId: %s"), *RemoteMatchId);
		}
		else
		{
			UE_LOG(LogStatSQL, Error, TEXT("update_match failed - MatchId: %s"), *RemoteMatchId);
		}
	});
}

// ============================================================
// Helpers
// ============================================================

FString AMutStatSQL::BuildGameOptions() const
{
	AUTGameMode* GM = Cast<AUTGameMode>(GetWorld()->GetAuthGameMode());
	if (!GM) return FString();

	FString Options;
	Options += FString::Printf(TEXT("TimeLimit=%d"), CachedTimeLimit);

	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (GS)
	{
		Options += FString::Printf(TEXT("?GoalScore=%d"), GS->GoalScore);
	}

	Options += FString::Printf(TEXT("?MaxPlayers=%d"), GM->GetNumPlayers());

	// Collect active mutator class names (matches command line format)
	FString MutatorNames;
	AUTMutator* Mut = GM->BaseMutator;
	while (Mut)
	{
		if (!MutatorNames.IsEmpty()) MutatorNames += TEXT(",");
		MutatorNames += Mut->GetClass()->GetName();
		Mut = Mut->NextMutator;
	}
	if (!MutatorNames.IsEmpty())
	{
		Options += FString::Printf(TEXT("?mutator=%s"), *MutatorNames);
	}

	return Options;
}

FString AMutStatSQL::GetReplayId() const
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (GS)
	{
		return GS->ReplayID;
	}
	return FString();
}

void AMutStatSQL::CheckAndUploadMapImage()
{
	// GET /map_image_check?map=<mapname> to see if the server already has this map's image
	FString CheckUrl = FString::Printf(TEXT("%s/map_image_check/?map=%s"), *ApiBaseUrl, *CachedMapName);

	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(CheckUrl);
	Request->SetVerb(TEXT("GET"));
	if (!ApiAuthKey.IsEmpty())
	{
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Token %s"), *ApiAuthKey));
	}

	Request->OnProcessRequestComplete().BindLambda(
		[this](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnected)
	{
		if (bConnected && Resp.IsValid() && Resp->GetResponseCode() == 200)
		{
			// Server already has the map image, skip upload
			UE_LOG(LogStatSQL, Verbose, TEXT("Map image already exists for %s"), *CachedMapName);
			return;
		}

		// Server doesn't have it (404 or error) — generate and upload
		UE_LOG(LogStatSQL, Log, TEXT("Map image not found on server, generating for %s"), *CachedMapName);

		TArray<uint8> PNGData;
		if (!ExportMinimapToPNG(PNGData))
		{
			UE_LOG(LogStatSQL, Warning, TEXT("Failed to export minimap PNG for %s"), *CachedMapName);
			return;
		}

		// POST the PNG as multipart/form-data
		FString Boundary = FString::Printf(TEXT("----StatSQLBoundary%d"), FMath::Rand());
		TArray<uint8> PostData;

		// Map name field
		FString FieldHeader = FString::Printf(TEXT("\r\n--%s\r\nContent-Disposition: form-data; name=\"map\"\r\n\r\n%s"),
			*Boundary, *CachedMapName);
		PostData.Append((uint8*)TCHAR_TO_UTF8(*FieldHeader), FieldHeader.Len());

		// Also send the map bounds as a field
		FBox MapBounds(ForceInit);
		if (GetMinimapWorldBounds(MapBounds))
		{
			FString BoundsStr = FString::Printf(
				TEXT("\r\n--%s\r\nContent-Disposition: form-data; name=\"map_bounds\"\r\n\r\n%.1f,%.1f,%.1f,%.1f,%.1f,%.1f"),
				*Boundary, MapBounds.Min.X, MapBounds.Min.Y, MapBounds.Min.Z,
				MapBounds.Max.X, MapBounds.Max.Y, MapBounds.Max.Z);
			PostData.Append((uint8*)TCHAR_TO_UTF8(*BoundsStr), BoundsStr.Len());
		}

		// PNG file field
		FString FileHeader = FString::Printf(
			TEXT("\r\n--%s\r\nContent-Disposition: form-data; name=\"image\"; filename=\"%s.png\"\r\nContent-Type: image/png\r\n\r\n"),
			*Boundary, *CachedMapName);
		PostData.Append((uint8*)TCHAR_TO_UTF8(*FileHeader), FileHeader.Len());
		PostData.Append(PNGData);

		// Closing boundary
		FString Closing = FString::Printf(TEXT("\r\n--%s--\r\n"), *Boundary);
		PostData.Append((uint8*)TCHAR_TO_UTF8(*Closing), Closing.Len());

		TSharedRef<IHttpRequest> UploadReq = FHttpModule::Get().CreateRequest();
		UploadReq->SetURL(FString::Printf(TEXT("%s/map_image_upload/"), *ApiBaseUrl));
		UploadReq->SetVerb(TEXT("POST"));
		UploadReq->SetHeader(TEXT("Content-Type"), FString::Printf(TEXT("multipart/form-data; boundary=%s"), *Boundary));
		if (!ApiAuthKey.IsEmpty())
		{
			UploadReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Token %s"), *ApiAuthKey));
		}
		UploadReq->SetContent(PostData);

		UploadReq->OnProcessRequestComplete().BindLambda(
			[this](FHttpRequestPtr Req2, FHttpResponsePtr Resp2, bool bOK)
		{
			if (bOK && Resp2.IsValid() && EHttpResponseCodes::IsOk(Resp2->GetResponseCode()))
			{
				UE_LOG(LogStatSQL, Log, TEXT("Map image uploaded successfully for %s"), *CachedMapName);
			}
			else
			{
				int32 Code = Resp2.IsValid() ? Resp2->GetResponseCode() : 0;
				FString Body = Resp2.IsValid() ? Resp2->GetContentAsString() : TEXT("no response");
				UE_LOG(LogStatSQL, Warning, TEXT("Map image upload failed for %s (HTTP %d): %s"), *CachedMapName, Code, *Body);
			}
		});

		UploadReq->ProcessRequest();
	});

	Request->ProcessRequest();
}

bool AMutStatSQL::ExportMinimapToPNG(TArray<uint8>& OutPNGData) const
{
	// We need to generate our own minimap since the HUD's render target is client-side only.
	// On a dedicated server there's no HUD, so we render the NavMesh triangles to an image ourselves.

	FBox LevelBox(ForceInit);
	if (!GetMinimapWorldBounds(LevelBox))
	{
		UE_LOG(LogStatSQL, Warning, TEXT("ExportMinimapToPNG: GetMinimapWorldBounds failed"));
		return false;
	}
	UE_LOG(LogStatSQL, Log, TEXT("ExportMinimapToPNG: Bounds Min=(%.1f,%.1f,%.1f) Max=(%.1f,%.1f,%.1f)"),
		LevelBox.Min.X, LevelBox.Min.Y, LevelBox.Min.Z, LevelBox.Max.X, LevelBox.Max.Y, LevelBox.Max.Z);

	AUTRecastNavMesh* NavMesh = GetUTNavData(GetWorld());
	if (!NavMesh)
	{
		UE_LOG(LogStatSQL, Warning, TEXT("ExportMinimapToPNG: No NavMesh found"));
		return false;
	}

	TMap<const UUTPathNode*, FNavMeshTriangleList> TriangleMap;
	NavMesh->GetNodeTriangleMap(TriangleMap);
	UE_LOG(LogStatSQL, Log, TEXT("ExportMinimapToPNG: Got %d path nodes from NavMesh"), TriangleMap.Num());

	const int32 MapSize = 1024;

	// Calculate transform (same as CalcMinimapTransform)
	const float ExtentX = LevelBox.GetExtent().X;
	const float ExtentY = LevelBox.GetExtent().Y;
	const bool bLargerX = ExtentX > ExtentY;
	const float LevelRadius = bLargerX ? ExtentX : ExtentY;
	const float Scale = (float)MapSize / (LevelRadius * 2.0f);
	const float CenteringX = bLargerX ? 0.f : (ExtentY - ExtentX);
	const float CenteringY = bLargerX ? (ExtentX - ExtentY) : 0.f;

	// Create pixel buffer (RGBA)
	TArray<FColor> Pixels;
	Pixels.SetNumZeroed(MapSize * MapSize);

	// Rasterize NavMesh triangles
	for (auto& Pair : TriangleMap)
	{
		const FNavMeshTriangleList& TriList = Pair.Value;
		for (const FNavMeshTriangleList::FTriangle& Tri : TriList.Triangles)
		{
			// Transform triangle vertices to pixel coords
			FVector2D V[3];
			for (int32 i = 0; i < 3; i++)
			{
				const FVector& WorldVert = TriList.Verts[Tri.Indices[i]];
				V[i].X = (WorldVert.X - LevelBox.Min.X + CenteringX) * Scale;
				V[i].Y = (WorldVert.Y - LevelBox.Min.Y + CenteringY) * Scale;
			}

			// Simple scanline triangle rasterization
			// Sort vertices by Y
			if (V[0].Y > V[1].Y) Swap(V[0], V[1]);
			if (V[0].Y > V[2].Y) Swap(V[0], V[2]);
			if (V[1].Y > V[2].Y) Swap(V[1], V[2]);

			auto DrawScanline = [&](int32 Y, float X1, float X2)
			{
				if (Y < 0 || Y >= MapSize) return;
				int32 MinX = FMath::Clamp(FMath::FloorToInt(FMath::Min(X1, X2)), 0, MapSize - 1);
				int32 MaxX = FMath::Clamp(FMath::CeilToInt(FMath::Max(X1, X2)), 0, MapSize - 1);
				for (int32 X = MinX; X <= MaxX; X++)
				{
					Pixels[Y * MapSize + X] = FColor(100, 120, 140, 255); // NavMesh walkable area color
				}
			};

			// Rasterize top half
			float TotalHeight = V[2].Y - V[0].Y;
			if (TotalHeight > 0.f)
			{
				float TopHeight = V[1].Y - V[0].Y;
				float BottomHeight = V[2].Y - V[1].Y;

				int32 YStart = FMath::Max(0, FMath::FloorToInt(V[0].Y));
				int32 YMid = FMath::Min(MapSize - 1, FMath::FloorToInt(V[1].Y));
				int32 YEnd = FMath::Min(MapSize - 1, FMath::FloorToInt(V[2].Y));

				for (int32 Y = YStart; Y <= YMid; Y++)
				{
					float T1 = (float)(Y - V[0].Y) / TotalHeight;
					float T2 = (TopHeight > 0.f) ? (float)(Y - V[0].Y) / TopHeight : 0.f;
					float XA = V[0].X + (V[2].X - V[0].X) * T1;
					float XB = V[0].X + (V[1].X - V[0].X) * T2;
					DrawScanline(Y, XA, XB);
				}

				for (int32 Y = YMid + 1; Y <= YEnd; Y++)
				{
					float T1 = (float)(Y - V[0].Y) / TotalHeight;
					float T2 = (BottomHeight > 0.f) ? (float)(Y - V[1].Y) / BottomHeight : 0.f;
					float XA = V[0].X + (V[2].X - V[0].X) * T1;
					float XB = V[1].X + (V[2].X - V[1].X) * T2;
					DrawScanline(Y, XA, XB);
				}
			}
		}
	}

	// Count filled pixels
	int32 FilledPixels = 0;
	for (const FColor& P : Pixels) { if (P.A > 0) FilledPixels++; }
	UE_LOG(LogStatSQL, Log, TEXT("ExportMinimapToPNG: Rasterized %d filled pixels out of %d"), FilledPixels, MapSize * MapSize);

	if (FilledPixels == 0)
	{
		UE_LOG(LogStatSQL, Warning, TEXT("ExportMinimapToPNG: No pixels were rasterized - empty image"));
		return false;
	}

	// Compress to PNG
	FImageUtils::CompressImageArray(MapSize, MapSize, Pixels, OutPNGData);
	UE_LOG(LogStatSQL, Log, TEXT("ExportMinimapToPNG: PNG compressed to %d bytes"), OutPNGData.Num());
	return OutPNGData.Num() > 0;
}

bool AMutStatSQL::GetMinimapWorldBounds(FBox& OutBounds) const
{
	// Replicate the same bounding box calculation that UTHUD::UpdateMinimapTexture uses
	AUTRecastNavMesh* NavMesh = GetUTNavData(GetWorld());
	if (!NavMesh) return false;

	TMap<const UUTPathNode*, FNavMeshTriangleList> TriangleMap;
	NavMesh->GetNodeTriangleMap(TriangleMap);

	FBox LevelBox(ForceInit);
	for (TMap<const UUTPathNode*, FNavMeshTriangleList>::TConstIterator It(TriangleMap); It; ++It)
	{
		const FNavMeshTriangleList& TriList = It.Value();
		for (const FVector& Vert : TriList.Verts)
		{
			LevelBox += Vert;
		}
	}

	if (!LevelBox.IsValid)
	{
		return false;
	}

	// Match UTHUD: expand by 1%
	OutBounds = LevelBox.ExpandBy(LevelBox.GetSize() * 0.01f);
	return true;
}

void AMutStatSQL::ComputeTeamTotals(int32& RedKills, int32& BlueKills,
	int32& RedDeaths, int32& BlueDeaths, float& RedDamage, float& BlueDamage) const
{
	RedKills = BlueKills = RedDeaths = BlueDeaths = 0;
	RedDamage = BlueDamage = 0.f;

	for (const auto& Pair : PlayerData)
	{
		const FPlayerMatchData& Data = Pair.Value;
		if (Data.TeamIndex == 0) // Red
		{
			RedKills += Data.Kills;
			RedDeaths += Data.Deaths;
			RedDamage += (float)Data.DamageDone;
		}
		else if (Data.TeamIndex == 1) // Blue
		{
			BlueKills += Data.Kills;
			BlueDeaths += Data.Deaths;
			BlueDamage += (float)Data.DamageDone;
		}
	}
}

// ============================================================
// Mutate command handler
// ============================================================

void AMutStatSQL::Mutate_Implementation(const FString& MutateString, APlayerController* Sender)
{
	Super::Mutate_Implementation(MutateString, Sender);

	if (!HasAuthority()) return;

	// "mutate setname NewPlayerName"
	if (MutateString.StartsWith(TEXT("setname ")) && bAllowNameChange)
	{
		FString NewName = MutateString.Mid(8).Trim();
		HandleSetName(Sender, NewName);
	}
}

void AMutStatSQL::HandleSetName(APlayerController* Sender, const FString& NewName)
{
	if (!Sender) return;

	AUTPlayerState* PS = Cast<AUTPlayerState>(Sender->PlayerState);
	if (!PS) return;

	// Validate length
	if (NewName.Len() < 2 || NewName.Len() > 24)
	{
		AUTPlayerController* UTPC = Cast<AUTPlayerController>(Sender);
		if (UTPC)
		{
			UTPC->ClientSay(nullptr, TEXT("Name must be 2-24 characters."), ChatDestinations::System);
		}
		return;
	}

	// Bad word filter
	if (ContainsBadWord(NewName))
	{
		AUTPlayerController* UTPC = Cast<AUTPlayerController>(Sender);
		if (UTPC)
		{
			UTPC->ClientSay(nullptr, TEXT("That name is not allowed."), ChatDestinations::System);
		}
		return;
	}

	FString OldName = PS->PlayerName;

	// Use the game mode's ChangeName to properly update everything
	AUTBaseGameMode* GM = Cast<AUTBaseGameMode>(GetWorld()->GetAuthGameMode());
	if (GM)
	{
		GM->ChangeName(Sender, NewName, true);
		UE_LOG(LogStatSQL, Log, TEXT("Player renamed: %s -> %s"), *OldName, *NewName);

		// Update our cached data
		FPlayerMatchData* Data = PlayerData.Find(GetStatsID(PS));
		if (Data)
		{
			Data->PlayerName = NewName;
		}
	}
}

FString AMutStatSQL::NormalizeLeetSpeak(const FString& Input)
{
	FString Result = Input;
	Result = Result.Replace(TEXT("0"), TEXT("o"));
	Result = Result.Replace(TEXT("1"), TEXT("i"));
	Result = Result.Replace(TEXT("3"), TEXT("e"));
	Result = Result.Replace(TEXT("4"), TEXT("a"));
	Result = Result.Replace(TEXT("5"), TEXT("s"));
	Result = Result.Replace(TEXT("7"), TEXT("t"));
	Result = Result.Replace(TEXT("8"), TEXT("b"));
	Result = Result.Replace(TEXT("@"), TEXT("a"));
	Result = Result.Replace(TEXT("$"), TEXT("s"));
	Result = Result.Replace(TEXT("!"), TEXT("i"));
	Result = Result.Replace(TEXT("+"), TEXT("t"));
	Result = Result.Replace(TEXT("("), TEXT("c"));
	Result = Result.Replace(TEXT("}{"), TEXT("h"));
	Result = Result.Replace(TEXT("|<"), TEXT("k"));
	return Result;
}

bool AMutStatSQL::ContainsBadWord(const FString& Name)
{
	FString LowerName = Name.ToLower();
	FString NormalizedName = NormalizeLeetSpeak(LowerName);

	for (const FString& BadWord : GetBadWords())
	{
		if (LowerName.Contains(BadWord) || NormalizedName.Contains(BadWord))
		{
			return true;
		}
	}
	return false;
}

const TArray<FString>& AMutStatSQL::GetBadWords()
{
	static TArray<FString> BadWords;
	if (BadWords.Num() == 0)
	{
		BadWords.Add(TEXT("nigger"));
		BadWords.Add(TEXT("nigga"));
		BadWords.Add(TEXT("faggot"));
		BadWords.Add(TEXT("kike"));
		BadWords.Add(TEXT("spic"));
		BadWords.Add(TEXT("chink"));
		BadWords.Add(TEXT("wetback"));
		BadWords.Add(TEXT("tranny"));
		BadWords.Add(TEXT("retard"));
		BadWords.Add(TEXT("coon"));
		BadWords.Add(TEXT("beaner"));
		BadWords.Add(TEXT("gook"));
		BadWords.Add(TEXT("towelhead"));
		BadWords.Add(TEXT("raghead"));
	}
	return BadWords;
}
