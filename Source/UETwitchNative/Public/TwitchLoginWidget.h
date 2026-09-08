#pragma once

#include "Blueprint/UserWidget.h"
#include "TwitchNativeTypes.h"
#include "TwitchLoginWidget.generated.h"

class UTwitchLoginPresenter;
class UWidgetSwitcher;
class UTextBlock;

/**
 * Generic, un-styled login widget base for the simple case: subclass in Blueprint, lay out UI,
 * implement the OnEnter* events. All behaviour lives in a UTwitchLoginPresenter this widget owns.
 *
 * If your widget must inherit some other base instead (a project menu framework, for example),
 * don't use this class - create a UTwitchLoginPresenter directly on your own widget and call
 * Bind / SetDrivenWidgets. This class is only a convenience wrapper around that.
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
	FTwitchAuthInfo GetLastAuthInfo() const;

	UFUNCTION(BlueprintPure, Category="Twitch")
	FTwitchUserInfoBP GetLastUserInfo() const;

	UFUNCTION(BlueprintPure, Category="Twitch")
	FString GetLastErrorMessage() const;

	/** The object doing the actual work. Exposed so Blueprints can reach it if needed. */
	UFUNCTION(BlueprintPure, Category="Twitch")
	UTwitchLoginPresenter* GetPresenter() const { return Presenter; }

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

	/**
	 * Optional. Named widgets on the subclass are driven automatically - no Blueprint wiring.
	 * Switcher child order must match EUETwitchAuthStatus: 0 LoggedOut, 1 Loading,
	 * 2 WaitingForCode, 3 LoggedIn, with the error panel at ErrorPanelIndex. Wrapping each
	 * panel in a SizeBox is fine; only the order of the switcher's direct children matters.
	 */
	UPROPERTY(BlueprintReadOnly, Category="Twitch", meta=(BindWidgetOptional))
	TObjectPtr<UWidgetSwitcher> WS_State;

	/** Optional. Populated with the last error message whenever one arrives. */
	UPROPERTY(BlueprintReadOnly, Category="Twitch", meta=(BindWidgetOptional))
	TObjectPtr<UTextBlock> TXT_Error;

	/** Optional. Populated with the signed-in account's display name as soon as user info arrives. */
	UPROPERTY(BlueprintReadOnly, Category="Twitch", meta=(BindWidgetOptional))
	TObjectPtr<UTextBlock> CurrentUserDisplayName;

	/** Index of the error panel within WS_State. Set negative to leave the panel unchanged on error. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Twitch|Layout")
	int32 ErrorPanelIndex = 4;

private:
	UFUNCTION()
	void ForwardLoggedOut();

	UFUNCTION()
	void ForwardLoading();

	UFUNCTION()
	void ForwardWaitingForCode(const FString& Uri, const FString& UserCode);

	UFUNCTION()
	void ForwardLoggedIn(const FTwitchUserInfoBP& User);

	UFUNCTION()
	void ForwardError(const FString& Message);

	UPROPERTY(Transient)
	TObjectPtr<UTwitchLoginPresenter> Presenter;
};
