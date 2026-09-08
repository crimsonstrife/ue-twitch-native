#pragma once

#include "TwitchNativeRewardPack.generated.h"

USTRUCT(BlueprintType)
struct FTwitchNativeRewardDefinition
{
	GENERATED_BODY()

	/** Stable key your game code uses (don’t rely on title text). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Twitch")
	FName RewardKey;

	/**
	 * Optional grouping key for settings UI - e.g. "Flavor", "Helpful", "Hostile".
	 * Purely organisational; the plugin never behaves differently based on it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Twitch")
	FName Category;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Twitch")
	FString Title;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Twitch", meta=(MultiLine=true))
	FString Prompt;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Twitch", meta=(ClampMin=1, UIMin=1))
	int32 Cost = 1000;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Twitch")
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Twitch")
	bool bUserInputRequired = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Twitch", meta=(ClampMin=0, UIMin=0))
	int32 MaxPerStream = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Twitch", meta=(ClampMin=0, UIMin=0))
	int32 MaxPerUserPerStream = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Twitch", meta=(ClampMin=0, UIMin=0))
	int32 GlobalCooldownSeconds = 0;

	/** Hex string like "#9146FF" */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Twitch")
	FString BackgroundColor = TEXT("#9146FF");

	/** If true, redeems don’t go into the request queue (less need to resolve manually). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Twitch")
	bool bSkipQueue = true;
};

UCLASS(BlueprintType)
class UETWITCHNATIVE_API UTwitchNativeRewardPack : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Optional prefix to avoid title collisions, e.g. "OMP: " */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Twitch")
	FString RewardTitlePrefix;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Twitch")
	TArray<FTwitchNativeRewardDefinition> Rewards;
};