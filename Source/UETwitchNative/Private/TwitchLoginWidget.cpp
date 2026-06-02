#include "TwitchLoginWidget.h"

#include "TwitchNativeSubsystem.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformApplicationMisc.h"

void UTwitchLoginWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	UTwitchNativeSubsystem* Subsystem = ResolveSubsystem();
	if (!Subsystem)
	{
		return;
	}

	Subsystem->OnAuthStatusChanged.AddDynamic(this, &UTwitchLoginWidget::HandleStatusChanged);
	Subsystem->OnAuthInfoReceived.AddDynamic(this, &UTwitchLoginWidget::HandleAuthInfo);
	Subsystem->OnUserInfoReceived.AddDynamic(this, &UTwitchLoginWidget::HandleUserInfo);
	Subsystem->OnTwitchError.AddDynamic(this, &UTwitchLoginWidget::HandleError);

	// Prime the view with whatever state the subsystem is already in.
	HandleStatusChanged(Subsystem->GetAuthStatus());
}

void UTwitchLoginWidget::NativeDestruct()
{
	if (UTwitchNativeSubsystem* Subsystem = ResolveSubsystem())
	{
		Subsystem->OnAuthStatusChanged.RemoveDynamic(this, &UTwitchLoginWidget::HandleStatusChanged);
		Subsystem->OnAuthInfoReceived.RemoveDynamic(this, &UTwitchLoginWidget::HandleAuthInfo);
		Subsystem->OnUserInfoReceived.RemoveDynamic(this, &UTwitchLoginWidget::HandleUserInfo);
		Subsystem->OnTwitchError.RemoveDynamic(this, &UTwitchLoginWidget::HandleError);
	}

	Super::NativeDestruct();
}

void UTwitchLoginWidget::BeginLogin()
{
	if (UTwitchNativeSubsystem* Subsystem = ResolveSubsystem())
	{
		Subsystem->ConnectUsingProjectSettings(/*bAutoLaunchBrowser=*/true);
	}
}

void UTwitchLoginWidget::Logout()
{
	if (UTwitchNativeSubsystem* Subsystem = ResolveSubsystem())
	{
		Subsystem->LogOut();
	}
}

void UTwitchLoginWidget::CopyCodeToClipboard()
{
	if (!LastAuthInfo.UserCode.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*LastAuthInfo.UserCode);
	}
}

EUETwitchAuthStatus UTwitchLoginWidget::GetAuthStatus() const
{
	if (const UTwitchNativeSubsystem* Subsystem = ResolveSubsystem())
	{
		return Subsystem->GetAuthStatus();
	}
	return EUETwitchAuthStatus::LoggedOut;
}

void UTwitchLoginWidget::HandleStatusChanged(EUETwitchAuthStatus NewStatus)
{
	switch (NewStatus)
	{
	case EUETwitchAuthStatus::LoggedOut:
		OnEnterLoggedOut();
		break;
	case EUETwitchAuthStatus::Loading:
		OnEnterLoading();
		break;
	case EUETwitchAuthStatus::WaitingForCode:
		OnEnterWaitingForCode(LastAuthInfo.Uri, LastAuthInfo.UserCode);
		break;
	case EUETwitchAuthStatus::LoggedIn:
		OnEnterLoggedIn(LastUserInfo);
		break;
	}
}

void UTwitchLoginWidget::HandleAuthInfo(const FTwitchAuthInfo& Info)
{
	LastAuthInfo = Info;
	// If we're already in WaitingForCode the OnEnter event already fired with stale (empty) info — re-emit
	// now that we have the real Uri/UserCode.
	if (GetAuthStatus() == EUETwitchAuthStatus::WaitingForCode)
	{
		OnEnterWaitingForCode(Info.Uri, Info.UserCode);
	}
}

void UTwitchLoginWidget::HandleUserInfo(const FTwitchUserInfoBP& Info)
{
	LastUserInfo = Info;
	if (GetAuthStatus() == EUETwitchAuthStatus::LoggedIn)
	{
		OnEnterLoggedIn(Info);
	}
}

void UTwitchLoginWidget::HandleError(const FString& Message)
{
	OnLoginError(Message);
}

UTwitchNativeSubsystem* UTwitchLoginWidget::ResolveSubsystem() const
{
	if (UGameInstance* GI = GetGameInstance())
	{
		return GI->GetSubsystem<UTwitchNativeSubsystem>();
	}
	return nullptr;
}
