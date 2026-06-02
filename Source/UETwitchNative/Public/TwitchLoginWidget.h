#pragma once

#include "Blueprint/UserWidget.h"
#include "TwitchNativeTypes.h"
#include "TwitchLoginWidget.generated.h"

class UTwitchNativeSubsystem;

/**
 * Generic, un-styled login widget base. Subclass in Blueprint to lay out UI; implement the OnEnter* events
 * to switch panels. Calls into UTwitchNativeSubsystem for all auth actions.
 */
UCLASS(Abstract, Blueprintable)
class UETWITCHNATIVE_API UTwitchLoginWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeOnInitialized() override;
	virtual void NativeDestruct() override;

	UFUNCTION(BlueprintCallable, Category="Twitch")
	void BeginLogin();

	UFUNCTION(BlueprintCallable, Category="Twitch")
	void Logout();

	UFUNCTION(BlueprintCallable, Category="Twitch")
	void CopyCodeToClipboard();

	UFUNCTION(BlueprintPure, Category="Twitch")
	EUETwitchAuthStatus GetAuthStatus() const;

	UFUNCTION(BlueprintPure, Category="Twitch")
	FTwitchAuthInfo GetLastAuthInfo() const { return LastAuthInfo; }

	UFUNCTION(BlueprintPure, Category="Twitch")
	FTwitchUserInfoBP GetLastUserInfo() const { return LastUserInfo; }

protected:
	/** Implement to show the LoggedOut panel (e.g. a Connect button). */
	UFUNCTION(BlueprintImplementableEvent, Category="Twitch")
	void OnEnterLoggedOut();

	UFUNCTION(BlueprintImplementableEvent, Category="Twitch")
	void OnEnterLoading();

	/** Implement to display the verification URL and user code to the player. */
	UFUNCTION(BlueprintImplementableEvent, Category="Twitch")
	void OnEnterWaitingForCode(const FString& Uri, const FString& UserCode);

	UFUNCTION(BlueprintImplementableEvent, Category="Twitch")
	void OnEnterLoggedIn(const FTwitchUserInfoBP& User);

	UFUNCTION(BlueprintImplementableEvent, Category="Twitch")
	void OnLoginError(const FString& Message);

private:
	UFUNCTION()
	void HandleStatusChanged(EUETwitchAuthStatus NewStatus);

	UFUNCTION()
	void HandleAuthInfo(const FTwitchAuthInfo& Info);

	UFUNCTION()
	void HandleUserInfo(const FTwitchUserInfoBP& Info);

	UFUNCTION()
	void HandleError(const FString& Message);

	UTwitchNativeSubsystem* ResolveSubsystem() const;

	UPROPERTY(Transient)
	FTwitchAuthInfo LastAuthInfo;

	UPROPERTY(Transient)
	FTwitchUserInfoBP LastUserInfo;
};
