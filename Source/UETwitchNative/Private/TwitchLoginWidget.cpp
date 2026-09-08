#include "TwitchLoginWidget.h"

#include "TwitchLoginPresenter.h"

void UTwitchLoginWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	Presenter = NewObject<UTwitchLoginPresenter>(this);
	Presenter->ErrorPanelIndex = ErrorPanelIndex;

	// Widgets first, so the priming pass inside Bind lands on a switcher that already exists.
	Presenter->SetDrivenWidgets(WS_State, TXT_Error, CurrentUserDisplayName);

	Presenter->OnEnterLoggedOut.AddDynamic(this, &UTwitchLoginWidget::ForwardLoggedOut);
	Presenter->OnEnterLoading.AddDynamic(this, &UTwitchLoginWidget::ForwardLoading);
	Presenter->OnEnterWaitingForCode.AddDynamic(this, &UTwitchLoginWidget::ForwardWaitingForCode);
	Presenter->OnEnterLoggedIn.AddDynamic(this, &UTwitchLoginWidget::ForwardLoggedIn);
	Presenter->OnLoginError.AddDynamic(this, &UTwitchLoginWidget::ForwardError);

	Presenter->Bind(this);
}

void UTwitchLoginWidget::NativeDestruct()
{
	if (Presenter)
	{
		Presenter->Unbind();
	}

	Super::NativeDestruct();
}

void UTwitchLoginWidget::BeginLogin()
{
	if (Presenter) Presenter->BeginLogin();
}

void UTwitchLoginWidget::Logout()
{
	if (Presenter) Presenter->Logout();
}

void UTwitchLoginWidget::CopyCodeToClipboard()
{
	if (Presenter) Presenter->CopyCodeToClipboard();
}

EUETwitchAuthStatus UTwitchLoginWidget::GetAuthStatus() const
{
	return Presenter ? Presenter->GetAuthStatus() : EUETwitchAuthStatus::LoggedOut;
}

FTwitchAuthInfo UTwitchLoginWidget::GetLastAuthInfo() const
{
	return Presenter ? Presenter->GetLastAuthInfo() : FTwitchAuthInfo();
}

FTwitchUserInfoBP UTwitchLoginWidget::GetLastUserInfo() const
{
	return Presenter ? Presenter->GetLastUserInfo() : FTwitchUserInfoBP();
}

FString UTwitchLoginWidget::GetLastErrorMessage() const
{
	return Presenter ? Presenter->GetLastErrorMessage() : FString();
}

void UTwitchLoginWidget::ForwardLoggedOut()
{
	OnEnterLoggedOut();
}

void UTwitchLoginWidget::ForwardLoading()
{
	OnEnterLoading();
}

void UTwitchLoginWidget::ForwardWaitingForCode(const FString& Uri, const FString& UserCode)
{
	OnEnterWaitingForCode(Uri, UserCode);
}

void UTwitchLoginWidget::ForwardLoggedIn(const FTwitchUserInfoBP& User)
{
	OnEnterLoggedIn(User);
}

void UTwitchLoginWidget::ForwardError(const FString& Message)
{
	OnLoginError(Message);
}
