#pragma once

#include "TwitchNativeRewardPack.h"

#include "Subsystems/GameInstanceSubsystem.h"
#include "Engine/EngineTypes.h"
#include "TwitchNativeTypes.h"
#include "TwitchSDK.h"
#include "TwitchNativeSubsystem.generated.h"

UCLASS()
class UETWITCHNATIVE_API UTwitchNativeSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category="Twitch")
	bool IsTwitchSdkAvailable() const;

	UFUNCTION(BlueprintPure, Category="Twitch")
	EUETwitchAuthStatus GetAuthStatus() const { return CurrentStatus; }

	UFUNCTION(BlueprintPure, Category="Twitch")
	bool IsLoggedIn() const { return CurrentStatus == EUETwitchAuthStatus::LoggedIn; }

	/** Query the SDK once for the current auth state and broadcast OnAuthStatusChanged on transition. */
	UFUNCTION(BlueprintCallable, Category="Twitch")
	void RefreshAuthStatus();

	UFUNCTION(BlueprintCallable, Category="Twitch")
	void RequestAuthenticationInfo(const TArray<FTwitchSDKOAuthScope>& Scopes);

	UFUNCTION(BlueprintCallable, Category="Twitch")
	void StartListeningForCustomRewards();

	UFUNCTION(BlueprintCallable, Category="Twitch")
	void StopListeningForCustomRewards();
	
	UFUNCTION(BlueprintCallable, Category="Twitch")
	void WaitForLoginAndFetchUserInfo();

	UFUNCTION(BlueprintCallable, Category="Twitch")
	void LogOut();

	UFUNCTION(BlueprintCallable, Category="Twitch")
	void ConnectUsingProjectSettings(bool bAutoLaunchBrowser = true);

	UFUNCTION(BlueprintCallable, Category="Twitch")
	void SyncDefaultRewardsFromSettings();

	/**
	 * Publish this pack as the game's complete set of channel point rewards.
	 * Twitch scopes reward management to the creating Client Id, so this only ever replaces
	 * rewards this game created - rewards the streamer made by hand are never touched.
	 */
	UFUNCTION(BlueprintCallable, Category="Twitch")
	void SyncCustomRewardsFromPack(UTwitchNativeRewardPack* Pack);

	/** Withdraw every reward this game created. Runs automatically on shutdown when bClearRewardsOnShutdown is set. */
	UFUNCTION(BlueprintCallable, Category="Twitch")
	void ClearCustomRewards();

	/** Resolve a pending redemption. Canceling refunds the viewer their channel points. */
	UFUNCTION(BlueprintCallable, Category="Twitch")
	void ResolveRedemption(const FString& RedemptionId, const FString& RewardId, bool bFulfill);

	// ---- Reward selection ---------------------------------------------------
	// "Off" means the reward is never published, which is the only thing that frees a channel
	// slot. FTwitchNativeRewardDefinition::bEnabled publishes it in a disabled state and still
	// consumes one - the two are not interchangeable.

	/**
	 * The reward pack configured in Project Settings, loaded. Returns null if none is set.
	 * Blueprint cannot read UTwitchNativeSettings directly, so this is the way in.
	 */
	UFUNCTION(BlueprintPure, Category="Twitch|Rewards")
	UTwitchNativeRewardPack* GetDefaultRewardPack() const;

	/** Rewards in the pack carrying the given Category. Leave Pack empty to use the default pack. */
	UFUNCTION(BlueprintPure, Category="Twitch|Rewards")
	TArray<FTwitchNativeRewardDefinition> GetRewardsInCategory(UTwitchNativeRewardPack* Pack, FName Category) const;

	/** True if this key will be published on the next sync. Keys never toggled default to true. */
	UFUNCTION(BlueprintPure, Category="Twitch|Rewards")
	bool IsRewardKeyEnabled(FName RewardKey) const;

	UFUNCTION(BlueprintCallable, Category="Twitch|Rewards")
	void SetRewardKeyEnabled(FName RewardKey, bool bEnabled);

	/** Apply one on/off state to every reward carrying the given Category. Empty Pack means the default pack. */
	UFUNCTION(BlueprintCallable, Category="Twitch|Rewards")
	void SetCategoryEnabled(UTwitchNativeRewardPack* Pack, FName Category, bool bEnabled);

	/** Distinct Category values in first-seen order; unset categories report as None. Empty Pack means the default pack. */
	UFUNCTION(BlueprintPure, Category="Twitch|Rewards")
	TArray<FName> GetRewardCategories(UTwitchNativeRewardPack* Pack) const;

	/** How many rewards are currently switched on. Empty Pack means the default pack. */
	UFUNCTION(BlueprintPure, Category="Twitch|Rewards")
	int32 GetEnabledRewardCount(UTwitchNativeRewardPack* Pack) const;

	UFUNCTION(BlueprintCallable, Category="Twitch|Rewards")
	void SaveRewardSelection();

	UFUNCTION(BlueprintCallable, Category="Twitch|Rewards")
	void LoadRewardSelection();

	// ---- Slot accounting ----------------------------------------------------

	/**
	 * Reports channel reward-slot usage via OnRewardSlotsUpdated.
	 *
	 * CURRENTLY ALWAYS REPORTS UNAVAILABLE (bValid=false), and callers degrade gracefully:
	 * GetPublishableSlotBudget returns -1 and CanEnableAnotherReward fails open.
	 *
	 * Counting requires listing the channel's rewards, which needs a Helix bearer token.
	 * The SDK cannot provide one: RefreshOAuthToken calls RefreshAccessToken, which invalidates
	 * the auth result and POSTs grant_type=refresh_token with no client_secret (the SDK has no
	 * way to supply one). Twitch answers 400, and the SDK then calls Deauthenticate - i.e.
	 * asking for a token logs the user out. Do not reintroduce that call.
	 */
	UFUNCTION(BlueprintCallable, Category="Twitch|Rewards")
	void QueryChannelRewardUsage();

	/** True once QueryChannelRewardUsage has returned usable figures. */
	UFUNCTION(BlueprintPure, Category="Twitch|Rewards")
	bool HasSlotUsageInfo() const { return LastSlotUsage.bValid; }

	/**
	 * How many rewards this app may publish: the channel cap minus rewards it did not create.
	 *
	 * This is deliberately NOT FTwitchRewardSlotUsage::Free. Free subtracts our own published
	 * rewards too, but a publish replaces those, so gating on Free under-counts the real budget
	 * by however many we already have up.
	 *
	 * Returns -1 when the figures have not been fetched yet.
	 */
	UFUNCTION(BlueprintPure, Category="Twitch|Rewards")
	int32 GetPublishableSlotBudget() const;

	/** True if more rewards are switched on than can actually be published. Always false while the budget is unknown. */
	UFUNCTION(BlueprintPure, Category="Twitch|Rewards")
	bool IsSelectionOverBudget(UTwitchNativeRewardPack* Pack) const;

	/**
	 * Whether one more reward can be switched on right now.
	 * Fails open: returns true while the budget is unknown, so a failed query never locks the
	 * streamer out of their own settings. A doomed publish still surfaces via OnTwitchError.
	 */
	UFUNCTION(BlueprintPure, Category="Twitch|Rewards")
	bool CanEnableAnotherReward(UTwitchNativeRewardPack* Pack) const;

	/** Last figures fetched by QueryChannelRewardUsage. bValid is false until one succeeds. */
	UFUNCTION(BlueprintPure, Category="Twitch|Rewards")
	FTwitchRewardSlotUsage GetLastKnownSlotUsage() const { return LastSlotUsage; }

	UPROPERTY(BlueprintAssignable, Category="Twitch|Rewards")
	FTwitchRewardSlotsUpdated OnRewardSlotsUpdated;

	UPROPERTY(BlueprintAssignable, Category="Twitch")
	FTwitchRewardTriggered OnRewardTriggered;

	UPROPERTY(BlueprintAssignable, Category="Twitch")
	FTwitchUserInfoReceived OnUserInfoReceived;

	UPROPERTY(BlueprintAssignable, Category="Twitch")
	FTwitchAuthInfoReceived OnAuthInfoReceived;

	UPROPERTY(BlueprintAssignable, Category="Twitch")
	FTwitchAuthStatusChanged OnAuthStatusChanged;

	UPROPERTY(BlueprintAssignable, Category="Twitch")
	FTwitchError OnTwitchError;

	UPROPERTY(BlueprintAssignable, Category="Twitch")
	FTwitchCustomRewardRedeemed OnCustomRewardRedeemed;

private:
	// Auth state machine
	EUETwitchAuthStatus CurrentStatus = EUETwitchAuthStatus::LoggedOut;
	FTimerHandle AuthPollHandle;
	double DeviceCodeFlowStartedSeconds = 0.0;

	/** Assign + broadcast only on actual transition. */
	void SetStatus(EUETwitchAuthStatus NewStatus);

	/** Wraps Core->GetAuthState, marshals result to game thread, invokes OnDone with mapped enum (or LoggedOut on error). */
	void QueryAuthState(TFunction<void(EUETwitchAuthStatus)> OnDone);

	/** Timer-driven poll body used while WaitingForCode. */
	void PollAuthState();

	/**
	 * Reward title -> RewardKey routing table, rebuilt on every sync.
	 * ReplaceCustomRewards returns no reward ids, so the prefixed title is the routing key.
	 * We rewrite those titles ourselves on each sync, so they stay stable for the session.
	 */
	TMap<FString, FName> RewardTitleToKey;

	// RedemptionId dedupe
	TSet<FString> SeenRedemptionIds;

	// Auto-launch browser support
	bool bPendingAutoLaunchBrowser = false;
	bool bLaunchedBrowserForThisAuth = false;

	/** Cached from GetMyUserInfo; required to build a CustomRewardResolveRequest. */
	FString BroadcasterId;

	/** Set once a sync has published rewards, so shutdown knows whether it has anything to withdraw. */
	bool bHasPublishedRewards = false;

	/** Returns InPack, or the project's configured default pack when InPack is null. */
	UTwitchNativeRewardPack* ResolveRewardPack(UTwitchNativeRewardPack* InPack) const;

	/** Reward keys switched off by the streamer. Exclusion list: absent means enabled. */
	TSet<FName> DisabledRewardKeys;

	FTwitchRewardSlotUsage LastSlotUsage;


	void WaitForNextCustomReward();

	struct FCustomRewardSubscription
	{
		TwitchSDK::TwitchEventStream<TwitchSDK::CustomRewardEvent> Stream;

		explicit FCustomRewardSubscription(const decltype(FTwitchSDKModule::Get().Core)& InCore)
			: Stream(InCore->SubscribeToCustomRewardEvents())
		{}
	};

	TUniquePtr<FCustomRewardSubscription> CustomRewardSubscription;
};
