#pragma once

#include "Engine/DeveloperSettings.h"
#include "TwitchSDK.h"
#include "TwitchNativeSettings.generated.h"

class UTwitchNativeRewardPack;

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Twitch Native"))
class UETWITCHNATIVE_API UTwitchNativeSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	static const UTwitchNativeSettings* Get() { return GetDefault<UTwitchNativeSettings>(); }

	UPROPERTY(Config, EditAnywhere, Category="Auth")
	TArray<FTwitchSDKOAuthScope> DefaultScopes;

	/** If true, the subsystem will attempt to silently restore persisted credentials at Initialize. If no credentials are persisted, no UI is shown until something explicitly calls ConnectUsingProjectSettings. */
	UPROPERTY(Config, EditAnywhere, Category="Auth")
	bool bAutoConnectOnStartup = false;

	UPROPERTY(Config, EditAnywhere, Category="Rewards")
	TSoftObjectPtr<UTwitchNativeRewardPack> DefaultRewardPack;

	UPROPERTY(Config, EditAnywhere, Category="Rewards")
	bool bAutoSyncRewardsOnLogin = true;

	UPROPERTY(Config, EditAnywhere, Category="Rewards")
	bool bClearRewardsOnShutdown = true;

	/**
	 * Twitch's channel-wide cap on custom rewards. Counts enabled AND disabled rewards from
	 * every source, not just ones this app created. Exposed rather than hardcoded so it can be
	 * corrected if Twitch changes it, without a code change.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Rewards", meta=(ClampMin=1, UIMin=1))
	int32 ChannelRewardCap = 50;
};