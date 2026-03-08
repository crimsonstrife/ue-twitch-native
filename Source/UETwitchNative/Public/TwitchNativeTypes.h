#pragma once

#include "CoreMinimal.h"
#include "TwitchNativeTypes.generated.h"

USTRUCT(BlueprintType)
struct FTwitchAuthInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) bool bAlreadyAuthenticated = false;
	UPROPERTY(BlueprintReadOnly) FString Uri;
	UPROPERTY(BlueprintReadOnly) FString UserCode;
};

USTRUCT(BlueprintType)
struct FTwitchCustomRewardEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) FString RedemptionId;
	UPROPERTY(BlueprintReadOnly) FString RewardId; 
	UPROPERTY(BlueprintReadOnly) FString RedeemerName;
	UPROPERTY(BlueprintReadOnly) FString RewardTitle;
	UPROPERTY(BlueprintReadOnly) FString UserInput;
	UPROPERTY(BlueprintReadOnly) int64 Cost = 0;
};

USTRUCT(BlueprintType)
struct FTwitchUserInfoBP
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) FString UserId;
	UPROPERTY(BlueprintReadOnly) FString Login;
	UPROPERTY(BlueprintReadOnly) FString DisplayName;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FTwitchUserInfoReceived, const FTwitchUserInfoBP&, Info);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FTwitchAuthInfoReceived, const FTwitchAuthInfo&, Info);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FTwitchError, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FTwitchCustomRewardRedeemed, const FTwitchCustomRewardEvent&, Event);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FTwitchRewardTriggered,
	FName, RewardKey,
	const FTwitchCustomRewardEvent&, Event
);