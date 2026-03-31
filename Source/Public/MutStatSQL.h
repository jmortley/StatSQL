// MutStatSQL.h
// C++ stats tracking mutator
// Hooks into game events, collects per-player stats, POSTs to ut4stats.com

#pragma once

#include "CoreMinimal.h"
#include "UnrealTournament.h"
#include "UTMutator.h"
#include "StatSQLTypes.h"
#include "Http.h"
#include "MutStatSQL.generated.h"

class AUTPlayerState;
class AUTPlayerController;
class AUTGameState;
class AUTCTFGameState;
class AUTCarriedObject;

DECLARE_LOG_CATEGORY_EXTERN(LogStatSQL, Log, All);

UCLASS(Blueprintable, Meta = (ChildCanTick))
class STATSQL_API AMutStatSQL : public AUTMutator
{
	GENERATED_BODY()

public:
	AMutStatSQL(const FObjectInitializer& ObjectInitializer);

	// ============================================================
	// AUTMutator overrides
	// ============================================================

	virtual void Init_Implementation(const FString& Options) override;
	virtual void PostPlayerInit_Implementation(AController* C) override;
	virtual void NotifyLogout_Implementation(AController* C) override;
	virtual void ScoreKill_Implementation(AController* Killer, AController* Other,
		TSubclassOf<UDamageType> DamageType) override;
	virtual void ScoreDamage_Implementation(int32 DamageAmount, AUTPlayerState* Victim,
		AUTPlayerState* Attacker) override;
	virtual bool ModifyDamage_Implementation(int32& Damage, FVector& Momentum, APawn* Injured,
		AController* InstigatedBy, const FHitResult& HitInfo,
		AActor* DamageCauser, TSubclassOf<UDamageType> DamageType) override;
	virtual void ScoreObject_Implementation(AUTCarriedObject* GameObject, AUTCharacter* HolderPawn,
		AUTPlayerState* Holder, FName Reason) override;
	virtual void NotifyMatchStateChange_Implementation(FName NewState) override;
	virtual void Mutate_Implementation(const FString& MutateString, APlayerController* Sender) override;

	// ============================================================
	// AActor overrides
	// ============================================================

	virtual void BeginPlay() override;

protected:

	// ============================================================
	// Configuration
	// ============================================================

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StatSQL")
	FString ApiBaseUrl;

	/** Auth token sent as "Authorization: Token <key>" header */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StatSQL")
	FString ApiAuthKey;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StatSQL")
	bool bEnabled;

	/** Debug logging */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StatSQL")
	bool bDebug;

	/** Allow players to use "mutate setname <name>" */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StatSQL")
	bool bAllowNameChange;

	/** Rate limit: minimum seconds between name changes per player */
	float NameChangeCooldown;

	/** Tracks last name-change time per player (keyed by StatsID) */
	TMap<FString, float> LastNameChangeTime;

	/** Approved server keys (from Mod.ini) — if non-empty, only these keys can submit */
	TArray<FString> ApprovedKeys;

	// ============================================================
	// Match data
	// ============================================================

	/** Per-player stats keyed by StatsID */
	TMap<FString, FPlayerMatchData> PlayerData;

	/** Timestamped match events */
	TArray<FTimelineEvent> Timeline;

	/** Per-damage-event log for the damage feed */
	TArray<FDamageLogEntry> DamageLog;

	/** Match ID returned by server on insert_match */
	FString RemoteMatchId;

	/** Cached match info */
	FString CachedMapName;
	FString CachedGameMode;
	FString CachedServerName;
	int32 CachedTimeLimit;
	float MatchStartWorldTime;
	float AccumulatedRoundTime;  // Sum of elapsed time from completed rounds (Elim)
	float LastRoundStartWorldTime;  // World time when current round started
	bool bMatchInProgress;
	bool bFirstRoundStarted;

	/** Active flag carries (keyed by carrier StatsID) for per-grab duration tracking */
	TMap<FString, FFlagCarryInstance> ActiveFlagCarries;

	/** Timer for sampling flag carrier positions */
	FTimerHandle FlagRouteSampleTimer;

	/** Sample all active flag carriers' positions (called on timer) */
	void SampleFlagCarrierPositions();

	// ============================================================
	// Time helpers
	// ============================================================

	/** Get elapsed match seconds (handles countdown timers and no-timelimit modes) */
	float GetMatchSeconds() const;

	/** Get current period: 0=first half/regulation, 1=second half, 2+=overtime */
	uint8 GetCurrentPeriod() const;

	// ============================================================
	// Player data helpers
	// ============================================================

	/** Get or create player data entry. Returns nullptr if bot or invalid. */
	FPlayerMatchData* GetOrCreatePlayerData(AUTPlayerState* PS);

	/** Get StatsID string from player state */
	static FString GetStatsID(AUTPlayerState* PS);

	/** Check if controller is a valid human player */
	static bool IsHumanPlayer(AController* C);

	/** Get UTPlayerState from a controller */
	static AUTPlayerState* GetUTPS(AController* C);

	// ============================================================
	// Stat collection
	// ============================================================

	/** Snapshot all stats from a player's StatsData TMap into our FPlayerMatchData.
	 *  Called at match end for connected players, or at logout for early leavers. */
	void SnapshotPlayerStats(AUTPlayerState* PS, FPlayerMatchData& OutData);

	/** Collect end-of-match stats for all connected players */
	void CollectEndOfMatchStats();

	/** Add a timeline event (generic: flags, overtime, etc.) */
	void AddTimelineEvent(const FString& EventType, const FString& ActorID,
		const FString& TargetID = FString(), const FString& Detail = FString());

	/** Add a detailed kill timeline event with full kill log data */
	void AddKillTimelineEvent(AController* Killer, AController* Other,
		TSubclassOf<UDamageType> DamageType);

	// ============================================================
	// TeamArena delegate handlers
	// ============================================================

	UFUNCTION()
	void OnTeamArenaACE(AUTPlayerState* PlayerState);

	UFUNCTION()
	void OnTeamArenaDarkHorse(AUTPlayerState* PlayerState, int32 EnemiesKilled);

	UFUNCTION()
	void OnTeamArenaClutch(AUTPlayerState* ClutchPlayer, int32 EnemiesAlive);

	/** Try to bind to TeamArena delegates if the game mode supports them */
	void TryBindTeamArenaEvents();

	// ============================================================
	// HTTP submission
	// ============================================================

	/** Kick off the full submission chain */
	void SubmitMatchData();

	/** Send a POST request with retry logic */
	void SendPost(const FString& Endpoint, const FString& JsonBody,
		TFunction<void(bool bSuccess, const FString& Response)> OnComplete,
		int32 RetryCount = 0);

	/** Max retries per request */
	static const int32 MaxRetries = 3;

	// Submission chain steps - each calls the next on success
	void PostInsertMatch();
	void PostInsertPlayers();
	void PostInsertMatchStats();
	void PostInsertItems();
	void PostInsertWeapons();
	void PostInsertAccuracy();
	void PostInsertMovement();
	void PostInsertFlagStats();
	void PostKillFeed();
	void PostDamageFeed();
	void PostTimeline();
	void PostUpdateMatch();

	/** Map a UE4 damage type class name to the short string Django expects */
	static FString MapDamageTypeToFeedName(const FString& ClassName);

	/** Build game options string for update_match */
	FString BuildGameOptions() const;

	/** Get replay ID if available */
	FString GetReplayId() const;

	/** Get the minimap world bounds (NavMesh bounding box) for coordinate mapping.
	 *  Returns false if unavailable. */
	bool GetMinimapWorldBounds(FBox& OutBounds) const;

	/** Check if the server already has a map image for this map, and upload if not.
	 *  Calls GET /map_image_check?map=<mapname>, uploads PNG if 404. */
	void CheckAndUploadMapImage();

	/** Export the minimap render target to a PNG byte array.
	 *  Returns false if minimap texture is unavailable. */
	bool ExportMinimapToPNG(TArray<uint8>& OutPNGData) const;

	/** Compute team totals for update_match */
	void ComputeTeamTotals(int32& RedKills, int32& BlueKills, int32& RedDeaths, int32& BlueDeaths,
		float& RedDamage, float& BlueDamage) const;

	// ============================================================
	// Mutate commands (setname)
	// ============================================================

	/** Handle "mutate setname <newname>" */
	void HandleSetName(APlayerController* Sender, const FString& NewName);

	/** Normalize leet speak substitutions (n1gg3r -> nigger, f4g -> fag, etc.) */
	static FString NormalizeLeetSpeak(const FString& Input);

	/** Check if name contains banned words (checks both raw and leet-normalized) */
	static bool ContainsBadWord(const FString& Name);

	/** Bad word list for name filtering */
	static const TArray<FString>& GetBadWords();

	// ============================================================
	// Config loading
	// ============================================================

	/** Load configuration from Mod.ini in Saved/Config */
	void LoadModIni();
};
