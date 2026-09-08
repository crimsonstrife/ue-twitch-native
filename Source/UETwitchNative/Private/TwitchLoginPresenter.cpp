#include "TwitchLoginPresenter.h"

#include "TwitchNativeSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Components/WidgetSwitcher.h"
#include "Components/TextBlock.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformApplicationMisc.h"

void UTwitchLoginPresenter::Bind(UUserWidget* InOwner)
{
	if (bBound)
	{
		return;
	}

	Owner = InOwner;

	UTwitchNativeSubsystem* Subsystem = ResolveSubsystem();
	if (!Subsystem)
	{
		return;
	}

	Subsystem->OnAuthStatusChanged.AddDynamic(this, &UTwitchLoginPresenter::HandleStatusChanged);
	Subsystem->OnAuthInfoReceived.AddDynamic(this, &UTwitchLoginPresenter::HandleAuthInfo);
	Subsystem->OnUserInfoReceived.AddDynamic(this, &UTwitchLoginPresenter::HandleUserInfo);
	Subsystem->OnTwitchError.AddDynamic(this, &UTwitchLoginPresenter::HandleError);

	bBound = true;

	// Prime the view with whatever state the subsystem already holds - it may have restored a
	// session before this widget ever existed.
	HandleStatusChanged(Subsystem->GetAuthStatus());
}

void UTwitchLoginPresenter::Unbind()
{
	if (!bBound)
	{
		return;
	}

	if (UTwitchNativeSubsystem* Subsystem = ResolveSubsystem())
	{
		Subsystem->OnAuthStatusChanged.RemoveDynamic(this, &UTwitchLoginPresenter::HandleStatusChanged);
		Subsystem->OnAuthInfoReceived.RemoveDynamic(this, &UTwitchLoginPresenter::HandleAuthInfo);
		Subsystem->OnUserInfoReceived.RemoveDynamic(this, &UTwitchLoginPresenter::HandleUserInfo);
		Subsystem->OnTwitchError.RemoveDynamic(this, &UTwitchLoginPresenter::HandleError);
	}

	bBound = false;
}

void UTwitchLoginPresenter::SetDrivenWidgets(UWidgetSwitcher* InSwitcher, UTextBlock* InErrorText, UTextBlock* InDisplayNameText)
{
	Switcher = InSwitcher;
	ErrorText = InErrorText;
	DisplayNameText = InDisplayNameText;

	// Catch the widgets up to current state rather than waiting for the next transition.
	ApplyStatusToSwitcher(GetAuthStatus());
	RefreshUserDisplay();
}

void UTwitchLoginPresenter::BeginLogin()
{
	if (UTwitchNativeSubsystem* Subsystem = ResolveSubsystem())
	{
		Subsystem->ConnectUsingProjectSettings(/*bAutoLaunchBrowser=*/true);
	}
}

void UTwitchLoginPresenter::Logout()
{
	if (UTwitchNativeSubsystem* Subsystem = ResolveSubsystem())
	{
		Subsystem->LogOut();
	}
}

void UTwitchLoginPresenter::CopyCodeToClipboard()
{
	if (!LastAuthInfo.UserCode.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*LastAuthInfo.UserCode);
	}
}

void UTwitchLoginPresenter::ApplyStatusToSwitcher(EUETwitchAuthStatus Status)
{
	if (!Switcher)
	{
		return;
	}

	// EUETwitchAuthStatus values are declared to match switcher child order 1:1.
	const int32 Index = static_cast<int32>(Status);
	if (Index >= 0 && Index < Switcher->GetNumWidgets())
	{
		Switcher->SetActiveWidgetIndex(Index);
	}
}

EUETwitchAuthStatus UTwitchLoginPresenter::GetAuthStatus() const
{
	if (const UTwitchNativeSubsystem* Subsystem = ResolveSubsystem())
	{
		return Subsystem->GetAuthStatus();
	}
	return EUETwitchAuthStatus::LoggedOut;
}

void UTwitchLoginPresenter::RefreshUserDisplay()
{
	if (DisplayNameText)
	{
		DisplayNameText->SetText(FText::FromString(LastUserInfo.DisplayName));
	}
}

void UTwitchLoginPresenter::HandleStatusChanged(EUETwitchAuthStatus NewStatus)
{
	// Drive the panel first so listeners observe the switcher already settled.
	ApplyStatusToSwitcher(NewStatus);

	// User info and the status flip race each other depending on which path got us here,
	// so refresh on both rather than assuming an order.
	if (NewStatus == EUETwitchAuthStatus::LoggedIn)
	{
		RefreshUserDisplay();
	}

	switch (NewStatus)
	{
	case EUETwitchAuthStatus::LoggedOut:
		OnEnterLoggedOut.Broadcast();
		break;
	case EUETwitchAuthStatus::Loading:
		OnEnterLoading.Broadcast();
		break;
	case EUETwitchAuthStatus::WaitingForCode:
		OnEnterWaitingForCode.Broadcast(LastAuthInfo.Uri, LastAuthInfo.UserCode);
		break;
	case EUETwitchAuthStatus::LoggedIn:
		OnEnterLoggedIn.Broadcast(LastUserInfo);
		break;
	}
}

void UTwitchLoginPresenter::HandleAuthInfo(const FTwitchAuthInfo& Info)
{
	LastAuthInfo = Info;

	// If we're already in WaitingForCode the event above already fired with empty info - re-emit
	// now that the real Uri/UserCode have arrived.
	if (GetAuthStatus() == EUETwitchAuthStatus::WaitingForCode)
	{
		OnEnterWaitingForCode.Broadcast(Info.Uri, Info.UserCode);
	}
}

void UTwitchLoginPresenter::HandleUserInfo(const FTwitchUserInfoBP& Info)
{
	LastUserInfo = Info;
	RefreshUserDisplay();

	if (GetAuthStatus() == EUETwitchAuthStatus::LoggedIn)
	{
		OnEnterLoggedIn.Broadcast(Info);
	}
}

void UTwitchLoginPresenter::HandleError(const FString& Message)
{
	LastErrorMessage = Message;

	if (ErrorText)
	{
		ErrorText->SetText(FText::FromString(Message));
	}

	if (Switcher && ErrorPanelIndex >= 0 && ErrorPanelIndex < Switcher->GetNumWidgets())
	{
		Switcher->SetActiveWidgetIndex(ErrorPanelIndex);
	}

	OnLoginError.Broadcast(Message);
}

UTwitchNativeSubsystem* UTwitchLoginPresenter::ResolveSubsystem() const
{
	if (Owner.IsValid())
	{
		if (UGameInstance* GI = Owner->GetGameInstance())
		{
			return GI->GetSubsystem<UTwitchNativeSubsystem>();
		}
	}
	return nullptr;
}
