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

	UFUNCTION(BlueprintCallable, Category="Twitch")
	void SyncCustomRewardsFromPack(UTwitchNativeRewardPack* Pack);
	
	UFUNCTION(BlueprintCallable, Category="Twitch|Helix")
	void SetHelixCredentials(const FString& InClientId, const FString& InUserAccessToken);

	/** resolve a redemption via Helix (FULFILLED/CANCELED). */
	UFUNCTION(BlueprintCallable, Category="Twitch|Helix")
	void ResolveRedemption_Helix(const FString& RedemptionId, const FString& RewardId, bool bFulfill);

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

	// Title -> RewardKey routing table (built from the last synced pack)
	TMap<FString, FName> RewardIdToKey;
	TMap<FString, FName> RewardTitleToKey;

	// RewardKey -> RewardId (managed by our sync)
	TMap<FName, FString> ManagedRewardIds;

	// RedemptionId dedupe
	TSet<FString> SeenRedemptionIds;

	// Auto-launch browser support
	bool bPendingAutoLaunchBrowser = false;
	bool bLaunchedBrowserForThisAuth = false;

	// NEW: cached broadcaster + helix auth
	FString BroadcasterId;
	FString HelixClientId;
	FString HelixUserAccessToken;

	void WaitForNextCustomReward();

	struct FHelixRewardLite
	{
		FString Id;
		FString Title;
	};

	bool TryBuildHelixAuth(FString& OutClientId, FString& OutToken, FString& OutBroadcasterId) const;

	void HelixRequest(
		const FString& Verb,
		const FString& Url,
		const FString& BodyJson,
		TFunction<void(bool bOk, int32 Code, const FString& ResponseBody, const FString& Error)> Callback);

	void HelixGetCustomRewards(TFunction<void(bool bOk, const TArray<FHelixRewardLite>& Rewards, const FString& Error)> Callback);
	void HelixCreateCustomReward(const FString& BodyJson, TFunction<void(bool bOk, const FString& Error)> Callback);
	void HelixUpdateCustomReward(const FString& RewardId, const FString& BodyJson, TFunction<void(bool bOk, const FString& Error)> Callback);
	void HelixDeleteCustomReward(const FString& RewardId, TFunction<void(bool bOk, const FString& Error)> Callback);

	void SyncManagedRewardsFromPack_Helix(UTwitchNativeRewardPack* Pack);

	struct FCustomRewardSubscription
	{
		TwitchSDK::TwitchEventStream<TwitchSDK::CustomRewardEvent> Stream;

		explicit FCustomRewardSubscription(const decltype(FTwitchSDKModule::Get().Core)& InCore)
			: Stream(InCore->SubscribeToCustomRewardEvents())
		{}
	};

	TUniquePtr<FCustomRewardSubscription> CustomRewardSubscription;
};
