#pragma once

#include "UObject/Object.h"
#include "TwitchNativeTypes.h"
#include "TwitchLoginPresenter.generated.h"

class UUserWidget;
class UWidgetSwitcher;
class UTextBlock;
class UTwitchNativeSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FTwitchLoginSimpleEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTwitchLoginCodeEvent, const FString&, Uri, const FString&, UserCode);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FTwitchLoginUserEvent, const FTwitchUserInfoBP&, User);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FTwitchLoginErrorEvent, const FString&, Message);

/**
 * All of the login glue - subscription lifecycle, state caching, and optional widget driving -
 * in a form that does not occupy a widget's parent slot.
 *
 * Use this when your widget already needs to inherit some other base (a project menu framework,
 * for example). Create one, call Bind, optionally hand it your switcher and text blocks, and
 * bind its events. For the simple case where nothing else claims the parent slot,
 * UTwitchLoginWidget wraps this and needs no setup.
 */
UCLASS(BlueprintType)
class UETWITCHNATIVE_API UTwitchLoginPresenter : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Subscribe to the Twitch subsystem and start tracking state. Idempotent - calling twice
	 * will not double-subscribe. InOwner supplies the GameInstance used to resolve the subsystem.
	 */
	UFUNCTION(BlueprintCallable, Category="Twitch")
	void Bind(UUserWidget* InOwner);

	/** Unsubscribe. Safe to call when not bound; call from your widget's Destruct. */
	UFUNCTION(BlueprintCallable, Category="Twitch")
	void Unbind();

	/**
	 * Optional. Hand over widgets for the presenter to drive directly, so you don't have to
	 * wire them yourself. Any argument may be null.
	 *
	 * InSwitcher child order must match EUETwitchAuthStatus: 0 LoggedOut, 1 Loading,
	 * 2 WaitingForCode, 3 LoggedIn - with the error panel, if any, at ErrorPanelIndex.
	 * Wrapping each panel in a SizeBox or similar is fine; only the order of the switcher's
	 * direct children matters.
	 */
	UFUNCTION(BlueprintCallable, Category="Twitch")
	void SetDrivenWidgets(UWidgetSwitcher* InSwitcher, UTextBlock* InErrorText, UTextBlock* InDisplayNameText);

	/** Index of the error panel within the driven switcher. Negative leaves the panel alone on error. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Twitch|Layout")
	int32 ErrorPanelIndex = 4;

	UFUNCTION(BlueprintCallable, Category="Twitch")
	void BeginLogin();

	UFUNCTION(BlueprintCallable, Category="Twitch")
	void Logout();

	UFUNCTION(BlueprintCallable, Category="Twitch")
	void CopyCodeToClipboard();

	/** Move the driven switcher to the panel matching Status. No-op when no switcher is set. */
	UFUNCTION(BlueprintCallable, Category="Twitch")
	void ApplyStatusToSwitcher(EUETwitchAuthStatus Status);

	UFUNCTION(BlueprintPure, Category="Twitch")
	EUETwitchAuthStatus GetAuthStatus() const;

	UFUNCTION(BlueprintPure, Category="Twitch")
	bool IsLoggedIn() const { return GetAuthStatus() == EUETwitchAuthStatus::LoggedIn; }

	UFUNCTION(BlueprintPure, Category="Twitch")
	FTwitchAuthInfo GetLastAuthInfo() const { return LastAuthInfo; }

	UFUNCTION(BlueprintPure, Category="Twitch")
	FTwitchUserInfoBP GetLastUserInfo() const { return LastUserInfo; }

	UFUNCTION(BlueprintPure, Category="Twitch")
	FString GetLastErrorMessage() const { return LastErrorMessage; }

	UPROPERTY(BlueprintAssignable, Category="Twitch")
	FTwitchLoginSimpleEvent OnEnterLoggedOut;

	UPROPERTY(BlueprintAssignable, Category="Twitch")
	FTwitchLoginSimpleEvent OnEnterLoading;

	UPROPERTY(BlueprintAssignable, Category="Twitch")
	FTwitchLoginCodeEvent OnEnterWaitingForCode;

	UPROPERTY(BlueprintAssignable, Category="Twitch")
	FTwitchLoginUserEvent OnEnterLoggedIn;

	UPROPERTY(BlueprintAssignable, Category="Twitch")
	FTwitchLoginErrorEvent OnLoginError;

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

	/** Push LastUserInfo into the driven display name block, if one was supplied. */
	void RefreshUserDisplay();

	UPROPERTY(Transient)
	TWeakObjectPtr<UUserWidget> Owner;

	UPROPERTY(Transient)
	TObjectPtr<UWidgetSwitcher> Switcher;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> ErrorText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> DisplayNameText;

	UPROPERTY(Transient)
	FTwitchAuthInfo LastAuthInfo;

	UPROPERTY(Transient)
	FTwitchUserInfoBP LastUserInfo;

	UPROPERTY(Transient)
	FString LastErrorMessage;

	bool bBound = false;
};
