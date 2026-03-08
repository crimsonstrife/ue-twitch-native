#pragma once

#include "GameFramework/SaveGame.h"
#include "TwitchNativeSaveGame.generated.h"

UCLASS()
class UTwitchNativeSaveGame : public USaveGame
{
	GENERATED_BODY()
public:
	UPROPERTY() FString UserId;
	UPROPERTY() TMap<FName, FString> RewardKeyToId;
};