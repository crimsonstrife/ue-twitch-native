#pragma once

#include "GameFramework/SaveGame.h"
#include "TwitchNativeSaveGame.generated.h"

UCLASS()
class UETWITCHNATIVE_API UTwitchNativeSaveGame : public USaveGame
{
	GENERATED_BODY()
public:
	UPROPERTY() FString UserId;
	UPROPERTY() TMap<FName, FString> RewardKeyToId;

	/**
	 * Reward keys the streamer has switched OFF, stored as an exclusion list so that an empty
	 * save means "everything on" and rewards added to the pack later default to on.
	 *
	 * "Off" here means the reward is not published to Twitch at all. That is deliberately NOT
	 * the same as FTwitchNativeRewardDefinition::bEnabled, which publishes the reward in a
	 * disabled state - and Twitch counts disabled rewards against the channel cap, so that
	 * flag frees no space.
	 */
	UPROPERTY() TSet<FName> DisabledRewardKeys;
};