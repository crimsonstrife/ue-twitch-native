#include "TwitchNativeSubsystem.h"

#include "TwitchNativeRewardPack.h"
#include "TwitchNativeSaveGame.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Containers/StringConv.h"
#include "Async/Async.h"
#include "Modules/ModuleManager.h"
#include "Engine/GameInstance.h"
#include "TimerManager.h"
#include "Kismet/GameplayStatics.h"

#include <type_traits>

#include "TwitchNativeSettings.h"
#include "TwitchSDKStructs.h"

DEFINE_LOG_CATEGORY_STATIC(LogTwitchNative, Log, All);

// FString -> TwitchSDK::string (owned)
static TwitchSDK::string ToTwitchString(const FString& In)
{
	using TwitchChar = typename TwitchSDK::string::value_type;

	// StringCast works for wchar_t / char16_t / char in UE builds
	const auto Cast = StringCast<TwitchChar>(*In);
	return TwitchSDK::string(Cast.Get(), static_cast<size_t>(Cast.Length()));
}

// FString -> TwitchSDK::string_holder (owned; safe after function returns)
static TwitchSDK::string_holder ToTwitchHolder(const FString& In)
{
	// StringHolder has an overload that takes string&&, so this stores by-value. :contentReference[oaicite:1]{index=1}
	return TwitchSDK::string_holder(ToTwitchString(In));
}

static FString TwitchScopeToString(const FTwitchSDKOAuthScope Scope)
{
	switch (Scope)
	{
	case FTwitchSDKOAuthScope::ChannelManagePolls:        return TEXT("channel:manage:polls");
	case FTwitchSDKOAuthScope::ChannelManagePredictions:  return TEXT("channel:manage:predictions");
	case FTwitchSDKOAuthScope::ChannelManageBroadcast:    return TEXT("channel:manage:broadcast");
	case FTwitchSDKOAuthScope::ChannelManageRedemptions:  return TEXT("channel:manage:redemptions");
	case FTwitchSDKOAuthScope::ChannelReadHype_Train:     return TEXT("channel:read:hype_train");
	case FTwitchSDKOAuthScope::ClipsEdit:                 return TEXT("clips:edit");
	case FTwitchSDKOAuthScope::UserReadSubscriptions:     return TEXT("user:read:subscriptions");
	case FTwitchSDKOAuthScope::BitsRead:                  return TEXT("bits:read");
	default:                                              return TEXT("");
	}
}

static std::wstring BuildOAuthScopesW(const TArray<FTwitchSDKOAuthScope>& Scopes)
{
	TArray<FString> Parts;
	Parts.Reserve(Scopes.Num());

	for (const FTwitchSDKOAuthScope Scope : Scopes)
	{
		const FString S = TwitchScopeToString(Scope);
		if (!S.IsEmpty())
		{
			Parts.Add(S);
		}
	}

	const FString Joined = FString::Join(Parts, TEXT(" "));
	const auto Wide = StringCast<wchar_t>(*Joined);
	return std::wstring(Wide.Get(), Wide.Length());
}

void UTwitchNativeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Query persisted credentials. If LoggedIn, surface state immediately and pull user info so consumers
	// (and bAutoSyncRewardsOnLogin) react. Otherwise, optionally kick off a silent auto-connect.
	QueryAuthState([this](EUETwitchAuthStatus Status)
	{
		SetStatus(Status);

		if (Status == EUETwitchAuthStatus::LoggedIn)
		{
			WaitForLoginAndFetchUserInfo();
			return;
		}

		const UTwitchNativeSettings* Settings = UTwitchNativeSettings::Get();
		if (Settings && Settings->bAutoConnectOnStartup)
		{
			ConnectUsingProjectSettings(/*bAutoLaunchBrowser=*/false);
		}
	});
}

void UTwitchNativeSubsystem::Deinitialize()
{
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(AuthPollHandle);
	}

	// The SDK asks that a game withdraw any rewards it enabled when it terminates, otherwise they
	// linger on the channel while nothing is running to service redemptions.
	const UTwitchNativeSettings* Settings = UTwitchNativeSettings::Get();
	if (bHasPublishedRewards && Settings && Settings->bClearRewardsOnShutdown)
	{
		ClearCustomRewards();
	}

	StopListeningForCustomRewards();
	Super::Deinitialize();
}

void UTwitchNativeSubsystem::SetStatus(EUETwitchAuthStatus NewStatus)
{
	if (NewStatus == CurrentStatus)
	{
		return;
	}
	CurrentStatus = NewStatus;
	OnAuthStatusChanged.Broadcast(NewStatus);
}

void UTwitchNativeSubsystem::QueryAuthState(TFunction<void(EUETwitchAuthStatus)> OnDone)
{
	if (!IsTwitchSdkAvailable())
	{
		if (OnDone) OnDone(EUETwitchAuthStatus::LoggedOut);
		return;
	}

	auto Core = FModuleManager::GetModuleChecked<FTwitchSDKModule>("TwitchSDK").Core;
	TWeakObjectPtr<UTwitchNativeSubsystem> WeakThis(this);

	Core->GetAuthState(
		[WeakThis, OnDone](const TwitchSDK::AuthState& State)
		{
			const EUETwitchAuthStatus Mapped = static_cast<EUETwitchAuthStatus>(static_cast<uint8>(State.Status));
			AsyncTask(ENamedThreads::GameThread, [WeakThis, OnDone, Mapped]()
			{
				if (!WeakThis.IsValid()) return;
				if (OnDone) OnDone(Mapped);
			});
		},
		[WeakThis, OnDone](const std::exception& E)
		{
			const FString Msg = FString::Printf(TEXT("GetAuthState failed: %s"), UTF8_TO_TCHAR(E.what()));
			AsyncTask(ENamedThreads::GameThread, [WeakThis, OnDone, Msg]()
			{
				if (!WeakThis.IsValid()) return;
				WeakThis->OnTwitchError.Broadcast(Msg);
				if (OnDone) OnDone(EUETwitchAuthStatus::LoggedOut);
			});
		}
	);
}

void UTwitchNativeSubsystem::RefreshAuthStatus()
{
	QueryAuthState([this](EUETwitchAuthStatus Status) { SetStatus(Status); });
}

void UTwitchNativeSubsystem::PollAuthState()
{
	QueryAuthState([this](EUETwitchAuthStatus Status)
	{
		switch (Status)
		{
		case EUETwitchAuthStatus::LoggedIn:
			if (UGameInstance* GI = GetGameInstance())
			{
				GI->GetTimerManager().ClearTimer(AuthPollHandle);
			}
			SetStatus(EUETwitchAuthStatus::LoggedIn);
			WaitForLoginAndFetchUserInfo();
			break;

		case EUETwitchAuthStatus::LoggedOut:
			// Twitch device codes expire after ~15 min; we cap at 10 to give the user a buffer to retry.
			if (FPlatformTime::Seconds() - DeviceCodeFlowStartedSeconds > 600.0)
			{
				if (UGameInstance* GI = GetGameInstance())
				{
					GI->GetTimerManager().ClearTimer(AuthPollHandle);
				}
				SetStatus(EUETwitchAuthStatus::LoggedOut);
				OnTwitchError.Broadcast(TEXT("Device code expired before authorization."));
			}
			break;

		default:
			break;
		}
	});
}

bool UTwitchNativeSubsystem::IsTwitchSdkAvailable() const
{
	if (!FModuleManager::Get().IsModuleLoaded("TwitchSDK"))
	{
		return false;
	}

	const FTwitchSDKModule& TwitchModule = FModuleManager::GetModuleChecked<FTwitchSDKModule>("TwitchSDK");
	return TwitchModule.Core != nullptr;
}

void UTwitchNativeSubsystem::RequestAuthenticationInfo(const TArray<FTwitchSDKOAuthScope>& Scopes)
{
	if (!IsTwitchSdkAvailable())
	{
		OnTwitchError.Broadcast(TEXT("TwitchSDK module/Core not available."));
		return;
	}

	// Re-entry guard: a second click while we're still negotiating shouldn't start a parallel poll.
	if (CurrentStatus == EUETwitchAuthStatus::Loading || CurrentStatus == EUETwitchAuthStatus::WaitingForCode)
	{
		return;
	}

	SetStatus(EUETwitchAuthStatus::Loading);

	auto Core = FModuleManager::GetModuleChecked<FTwitchSDKModule>("TwitchSDK").Core;
	TWeakObjectPtr<UTwitchNativeSubsystem> WeakThis(this);

	Core->GetAuthenticationInfo(
		BuildOAuthScopesW(Scopes),
		[WeakThis](const TwitchSDK::AuthenticationInfo& Info)
		{
			if (!WeakThis.IsValid()) return;

			FTwitchAuthInfo Out;
			Out.bAlreadyAuthenticated = (Info.UserCode.size() == 0);
			// Must use ToFString, not UTF8_TO_TCHAR. On Win64 the SDK is compiled with
			// R66_STR_WSTRING, so these are wchar_t (UTF-16) buffers - decoding them as UTF-8
			// stops at the first embedded null byte and yields a single character. Also,
			// StringHolder::data() is explicitly NOT null-terminated; ToFString pairs it with
			// size() through the string_view conversion.
			Out.Uri      = TwitchSDK::ToFString(Info.Uri);
			Out.UserCode = TwitchSDK::ToFString(Info.UserCode);

			AsyncTask(ENamedThreads::GameThread, [WeakThis, Out]()
			{
				if (!WeakThis.IsValid()) return;

				// Auto open the verification page (only once per ConnectUsingProjectSettings call)
				if (WeakThis->bPendingAutoLaunchBrowser
					&& !Out.bAlreadyAuthenticated
					&& !WeakThis->bLaunchedBrowserForThisAuth
					&& !Out.Uri.IsEmpty())
				{
					WeakThis->bLaunchedBrowserForThisAuth = true;
					FPlatformProcess::LaunchURL(*Out.Uri, nullptr, nullptr);
				}

				// Publish the code/URI before flipping status so listeners that read cached info
				// on the status-change handler see the real values rather than a brief empty state.
				WeakThis->OnAuthInfoReceived.Broadcast(Out);

				if (Out.bAlreadyAuthenticated)
				{
					WeakThis->SetStatus(EUETwitchAuthStatus::LoggedIn);
					WeakThis->WaitForLoginAndFetchUserInfo();
				}
				else
				{
					WeakThis->DeviceCodeFlowStartedSeconds = FPlatformTime::Seconds();
					if (UGameInstance* GI = WeakThis->GetGameInstance())
					{
						GI->GetTimerManager().SetTimer(
							WeakThis->AuthPollHandle,
							FTimerDelegate::CreateUObject(WeakThis.Get(), &UTwitchNativeSubsystem::PollAuthState),
							3.0f,
							/*bLoop=*/true,
							/*FirstDelay=*/3.0f);
					}
					WeakThis->SetStatus(EUETwitchAuthStatus::WaitingForCode);
				}
			});
		},
		[WeakThis](const std::exception& E)
		{
			if (!WeakThis.IsValid()) return;

			const FString Msg = FString::Printf(TEXT("GetAuthenticationInfo failed: %s"), UTF8_TO_TCHAR(E.what()));
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Msg]()
			{
				if (!WeakThis.IsValid()) return;
				WeakThis->SetStatus(EUETwitchAuthStatus::LoggedOut);
				WeakThis->OnTwitchError.Broadcast(Msg);
			});
		}
	);
}

void UTwitchNativeSubsystem::StartListeningForCustomRewards()
{
	if (!IsTwitchSdkAvailable())
	{
		OnTwitchError.Broadcast(TEXT("TwitchSDK module/Core not available."));
		return;
	}

	// restart cleanly
	StopListeningForCustomRewards();
	SeenRedemptionIds.Reset();

	auto Core = FModuleManager::GetModuleChecked<FTwitchSDKModule>("TwitchSDK").Core;
	CustomRewardSubscription = MakeUnique<FCustomRewardSubscription>(Core);
	WaitForNextCustomReward();
}

void UTwitchNativeSubsystem::StopListeningForCustomRewards()
{
	CustomRewardSubscription.Reset();
}

void UTwitchNativeSubsystem::WaitForNextCustomReward()
{
	if (!CustomRewardSubscription)
	{
		return;
	}

	TWeakObjectPtr<UTwitchNativeSubsystem> WeakThis(this);

	CustomRewardSubscription->Stream.WaitForEvent([WeakThis](const TwitchSDK::CustomRewardEvent& E)
	{
		if (!WeakThis.IsValid()) return;

		FTwitchCustomRewardEvent Out;
		Out.RedemptionId = TwitchSDK::ToFString(E.RedemptionId);
		Out.RewardId     = TwitchSDK::ToFString(E.CustomRewardId);
		Out.RedeemerName = TwitchSDK::ToFString(E.RedeemerName);
		Out.RewardTitle  = TwitchSDK::ToFString(E.CustomRewardTitle);
		Out.UserInput    = TwitchSDK::ToFString(E.UserInput);
		Out.Cost         = (int64)E.CustomRewardCost;

		AsyncTask(ENamedThreads::GameThread, [WeakThis, Out]()
{
	if (!WeakThis.IsValid()) return;

	// If listening was stopped while an event was in-flight
	if (!WeakThis->CustomRewardSubscription)
	{
		return;
	}

	// Dedupe (prevents double-firing if SDK delivers update/repeat)
	if (WeakThis->SeenRedemptionIds.Contains(Out.RedemptionId))
	{
		WeakThis->WaitForNextCustomReward();
		return;
	}
	WeakThis->SeenRedemptionIds.Add(Out.RedemptionId);

	// Optional: prevent unbounded growth over very long streams
	if (WeakThis->SeenRedemptionIds.Num() > 4096)
	{
		WeakThis->SeenRedemptionIds.Reset();
		WeakThis->SeenRedemptionIds.Add(Out.RedemptionId);
	}

	// Route to the pack's stable key by title. Anything we didn't publish (a reward the
		// streamer created by hand) has no key, so it falls through as a raw redemption.
			if (const FName* RewardKey = WeakThis->RewardTitleToKey.Find(Out.RewardTitle))
			{
				WeakThis->OnRewardTriggered.Broadcast(*RewardKey, Out);
			}
			else
			{
				WeakThis->OnCustomRewardRedeemed.Broadcast(Out);
			}

	WeakThis->WaitForNextCustomReward();
});
	});
}

void UTwitchNativeSubsystem::WaitForLoginAndFetchUserInfo()
{
	if (!IsTwitchSdkAvailable())
	{
		OnTwitchError.Broadcast(TEXT("TwitchSDK module/Core not available."));
		return;
	}

	auto Core = FModuleManager::GetModuleChecked<FTwitchSDKModule>("TwitchSDK").Core;
	TWeakObjectPtr<UTwitchNativeSubsystem> WeakThis(this);

	Core->GetMyUserInfo(
		[WeakThis](const TwitchSDK::UserInfo& Info)
		{
			if (!WeakThis.IsValid()) return;

			FTwitchUserInfoBP Out;
			Out.UserId          = TwitchSDK::ToFString(Info.ChannelId);
			Out.Login           = TwitchSDK::ToFString(Info.LoginName);
			Out.DisplayName     = TwitchSDK::ToFString(Info.DisplayName);
			Out.ProfileImageUrl = TwitchSDK::ToFString(Info.ProfileImageUrl);
			Out.BroadcasterType = TwitchSDK::ToFString(Info.BroadcasterType);

			WeakThis->BroadcasterId = Out.UserId; 

			AsyncTask(ENamedThreads::GameThread, [WeakThis, Out]()
			{
				if (!WeakThis.IsValid()) return;

				WeakThis->OnUserInfoReceived.Broadcast(Out);

				const UTwitchNativeSettings* Settings = UTwitchNativeSettings::Get();
				if (Settings && Settings->bAutoSyncRewardsOnLogin)
				{
					WeakThis->SyncDefaultRewardsFromSettings();
				}

				// Ensure only one active subscription
				WeakThis->StartListeningForCustomRewards();
			});

		},
		[WeakThis](const std::exception& E)
		{
			if (!WeakThis.IsValid()) return;
			const FString Msg = FString::Printf(TEXT("GetMyUserInfo failed: %s"), UTF8_TO_TCHAR(E.what()));
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Msg]()
			{
				if (WeakThis.IsValid()) WeakThis->OnTwitchError.Broadcast(Msg);
			});
		}
	);
}

void UTwitchNativeSubsystem::LogOut()
{
	if (!IsTwitchSdkAvailable()) return;

	// Cancel any in-flight device-code poll up front so a logout during WaitingForCode is honored immediately.
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(AuthPollHandle);
	}

	auto Core = FModuleManager::GetModuleChecked<FTwitchSDKModule>("TwitchSDK").Core;
	TWeakObjectPtr<UTwitchNativeSubsystem> WeakThis(this);

	Core->LogOut(
		[WeakThis]()
		{
			AsyncTask(ENamedThreads::GameThread, [WeakThis]()
			{
				if (WeakThis.IsValid()) WeakThis->SetStatus(EUETwitchAuthStatus::LoggedOut);
			});
		},
		[WeakThis](const std::exception& E)
		{
			if (!WeakThis.IsValid()) return;
			const FString Msg = FString::Printf(TEXT("LogOut failed: %s"), UTF8_TO_TCHAR(E.what()));
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Msg]()
			{
				if (WeakThis.IsValid()) WeakThis->OnTwitchError.Broadcast(Msg);
			});
		}
	);
}

void UTwitchNativeSubsystem::ConnectUsingProjectSettings(bool bAutoLaunchBrowser)
{
	const UTwitchNativeSettings* Settings = UTwitchNativeSettings::Get();
	if (!Settings)
	{
		OnTwitchError.Broadcast(TEXT("TwitchNativeSettings not available."));
		return;
	}

	bPendingAutoLaunchBrowser = bAutoLaunchBrowser;
	bLaunchedBrowserForThisAuth = false;

	RequestAuthenticationInfo(Settings->DefaultScopes);
}

void UTwitchNativeSubsystem::SyncDefaultRewardsFromSettings()
{
	const UTwitchNativeSettings* Settings = UTwitchNativeSettings::Get();
	if (!Settings || Settings->DefaultRewardPack.IsNull())
	{
		OnTwitchError.Broadcast(TEXT("DefaultRewardPack not configured."));
		return;
	}

	UTwitchNativeRewardPack* Pack = Settings->DefaultRewardPack.LoadSynchronous();
	if (!Pack)
	{
		OnTwitchError.Broadcast(TEXT("Failed to load DefaultRewardPack."));
		return;
	}

	SyncCustomRewardsFromPack(Pack);
}

static FString SanitizeHexColor(const FString& In)
{
	FString C = In.TrimStartAndEnd();
	C.ReplaceInline(TEXT("\""), TEXT(""));
	C.ReplaceInline(TEXT("'"), TEXT(""));
	// Twitch examples show colors with a leading '#', so keep it if provided. 
	if (!C.IsEmpty() && C[0] != TCHAR('#'))
	{
		// If your data uses "00E5CB" etc, prepend '#'
		C = FString::Printf(TEXT("#%s"), *C);
	}
	return C;
}

/**
 * Map one pack entry onto the SDK's reward struct.
 *
 * Every string is copied into an OWNED holder via ToTwitchHolder. Do not reach for
 * R66::FromFString here - it returns a non-owning string_view into the FString, which
 * dangles the moment the caller's locals go out of scope.
 */
static TwitchSDK::CustomRewardDefinition BuildSdkRewardDefinition(
	const FTwitchNativeRewardDefinition& Def,
	const FString& FullTitle)
{
	TwitchSDK::CustomRewardDefinition Out;

	Out.Title  = ToTwitchHolder(FullTitle);
	Out.Cost   = static_cast<int64_t>(Def.Cost);
	Out.Prompt = ToTwitchHolder(Def.Prompt);

	Out.IsEnabled           = Def.bEnabled;
	Out.IsUserInputRequired = Def.bUserInputRequired;

	if (!Def.BackgroundColor.IsEmpty())
	{
		Out.BackgroundColor = ToTwitchHolder(SanitizeHexColor(Def.BackgroundColor));
	}

	// Twitch pairs each limit with its own enable flag; we treat 0 as "no limit".
	Out.IsMaxPerStreamEnabled = Def.MaxPerStream > 0;
	Out.MaxPerStream          = Def.MaxPerStream;

	Out.IsMaxPerUserPerStreamEnabled = Def.MaxPerUserPerStream > 0;
	Out.MaxPerUserPerStream          = Def.MaxPerUserPerStream;

	Out.IsGlobalCooldownEnabled = Def.GlobalCooldownSeconds > 0;
	Out.GlobalCooldownSeconds   = Def.GlobalCooldownSeconds;

	Out.ShouldRedemptionsSkipRequestQueue = Def.bSkipQueue;

	return Out;
}

void UTwitchNativeSubsystem::SyncCustomRewardsFromPack(UTwitchNativeRewardPack* Pack)
{
	if (!Pack)
	{
		OnTwitchError.Broadcast(TEXT("SyncCustomRewardsFromPack called with no RewardPack."));
		return;
	}

	if (!IsTwitchSdkAvailable())
	{
		OnTwitchError.Broadcast(TEXT("TwitchSDK module/Core not available."));
		return;
	}

	// Rebuild routing before publishing, so a redemption landing immediately after the
	// call completes already has somewhere to go.
	RewardTitleToKey.Reset();

	TwitchSDK::CustomRewardList List;
	List.Rewards.reserve(Pack->Rewards.Num());

	for (const FTwitchNativeRewardDefinition& Def : Pack->Rewards)
	{
		// Switched-off rewards are omitted entirely rather than published disabled - a disabled
		// reward still occupies one of the channel's slots.
		if (!IsRewardKeyEnabled(Def.RewardKey))
		{
			continue;
		}

		const FString FullTitle = Pack->RewardTitlePrefix + Def.Title;

		if (!Def.RewardKey.IsNone())
		{
			RewardTitleToKey.Add(FullTitle, Def.RewardKey);
		}

		List.Rewards.push_back(BuildSdkRewardDefinition(Def, FullTitle));
	}

	const int32 Count = static_cast<int32>(List.Rewards.size());
	auto Core = FModuleManager::GetModuleChecked<FTwitchSDKModule>("TwitchSDK").Core;
	TWeakObjectPtr<UTwitchNativeSubsystem> WeakThis(this);

	Core->ReplaceCustomRewards(
		List,
		[WeakThis, Count]()
		{
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Count]()
			{
				if (!WeakThis.IsValid()) return;
				WeakThis->bHasPublishedRewards = (Count > 0);
				UE_LOG(LogTwitchNative, Log, TEXT("Published %d custom reward(s)."), Count);
			});
		},
		[WeakThis](const std::exception& E)
		{
			const FString Msg = FString::Printf(TEXT("ReplaceCustomRewards failed: %s"), UTF8_TO_TCHAR(E.what()));
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Msg]()
			{
				if (WeakThis.IsValid()) WeakThis->OnTwitchError.Broadcast(Msg);
			});
		}
	);
}

void UTwitchNativeSubsystem::ClearCustomRewards()
{
	if (!IsTwitchSdkAvailable())
	{
		return;
	}

	RewardTitleToKey.Reset();

	auto Core = FModuleManager::GetModuleChecked<FTwitchSDKModule>("TwitchSDK").Core;
	TWeakObjectPtr<UTwitchNativeSubsystem> WeakThis(this);

	// An empty list is how the SDK expects a game to withdraw its rewards.
	TwitchSDK::CustomRewardList Empty;

	Core->ReplaceCustomRewards(
		Empty,
		[WeakThis]()
		{
			AsyncTask(ENamedThreads::GameThread, [WeakThis]()
			{
				if (WeakThis.IsValid()) WeakThis->bHasPublishedRewards = false;
			});
		},
		[](const std::exception& E)
		{
			// Deliberately does not touch the subsystem or broadcast: this commonly runs from
			// Deinitialize, by which point neither is safe to reach for.
			UE_LOG(LogTwitchNative, Warning, TEXT("ClearCustomRewards failed: %s"), UTF8_TO_TCHAR(E.what()));
		}
	);
}

void UTwitchNativeSubsystem::ResolveRedemption(const FString& RedemptionId, const FString& RewardId, bool bFulfill)
{
	if (!IsTwitchSdkAvailable())
	{
		OnTwitchError.Broadcast(TEXT("TwitchSDK module/Core not available."));
		return;
	}

	if (BroadcasterId.IsEmpty())
	{
		OnTwitchError.Broadcast(TEXT("Cannot resolve a redemption before user info has been fetched."));
		return;
	}

	TwitchSDK::CustomRewardResolveRequest Req;
	Req.RedemptionId   = ToTwitchHolder(RedemptionId);
	Req.CustomRewardId = ToTwitchHolder(RewardId);
	Req.BroadcasterId  = ToTwitchHolder(BroadcasterId);
	Req.Resolution     = bFulfill
		? TwitchSDK::CustomRewardRedemptionState::Fulfilled
		: TwitchSDK::CustomRewardRedemptionState::Canceled;

	auto Core = FModuleManager::GetModuleChecked<FTwitchSDKModule>("TwitchSDK").Core;
	TWeakObjectPtr<UTwitchNativeSubsystem> WeakThis(this);

	Core->ResolveCustomReward(
		Req,
		[]() {},
		[WeakThis](const std::exception& E)
		{
			const FString Msg = FString::Printf(TEXT("ResolveCustomReward failed: %s"), UTF8_TO_TCHAR(E.what()));
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Msg]()
			{
				if (WeakThis.IsValid()) WeakThis->OnTwitchError.Broadcast(Msg);
			});
		}
	);
}

// ---------------------------------------------------------------------------
// Reward selection
// ---------------------------------------------------------------------------

static const TCHAR* GRewardSelectionSlot = TEXT("TwitchNativeRewardSelection");

UTwitchNativeRewardPack* UTwitchNativeSubsystem::ResolveRewardPack(UTwitchNativeRewardPack* InPack) const
{
	if (InPack)
	{
		return InPack;
	}
	return GetDefaultRewardPack();
}

UTwitchNativeRewardPack* UTwitchNativeSubsystem::GetDefaultRewardPack() const
{
	const UTwitchNativeSettings* Settings = UTwitchNativeSettings::Get();
	if (!Settings || Settings->DefaultRewardPack.IsNull())
	{
		return nullptr;
	}
	return Settings->DefaultRewardPack.LoadSynchronous();
}

TArray<FTwitchNativeRewardDefinition> UTwitchNativeSubsystem::GetRewardsInCategory(UTwitchNativeRewardPack* Pack, FName Category) const
{
	TArray<FTwitchNativeRewardDefinition> Out;

	Pack = ResolveRewardPack(Pack);
	if (!Pack)
	{
		return Out;
	}

	for (const FTwitchNativeRewardDefinition& Def : Pack->Rewards)
	{
		if (Def.Category == Category)
		{
			Out.Add(Def);
		}
	}
	return Out;
}

bool UTwitchNativeSubsystem::IsRewardKeyEnabled(FName RewardKey) const
{
	// Exclusion list: anything not explicitly switched off is on, so rewards added to a pack
	// later default to enabled instead of silently vanishing.
	return !DisabledRewardKeys.Contains(RewardKey);
}

void UTwitchNativeSubsystem::SetRewardKeyEnabled(FName RewardKey, bool bEnabled)
{
	if (RewardKey.IsNone())
	{
		return;
	}

	if (bEnabled)
	{
		DisabledRewardKeys.Remove(RewardKey);
	}
	else
	{
		DisabledRewardKeys.Add(RewardKey);
	}
}

void UTwitchNativeSubsystem::SetCategoryEnabled(UTwitchNativeRewardPack* Pack, FName Category, bool bEnabled)
{
	Pack = ResolveRewardPack(Pack);
	if (!Pack)
	{
		return;
	}

	for (const FTwitchNativeRewardDefinition& Def : Pack->Rewards)
	{
		if (Def.Category == Category)
		{
			SetRewardKeyEnabled(Def.RewardKey, bEnabled);
		}
	}
}

TArray<FName> UTwitchNativeSubsystem::GetRewardCategories(UTwitchNativeRewardPack* Pack) const
{
	TArray<FName> Out;

	Pack = ResolveRewardPack(Pack);
	if (!Pack)
	{
		return Out;
	}

	for (const FTwitchNativeRewardDefinition& Def : Pack->Rewards)
	{
		Out.AddUnique(Def.Category);
	}
	return Out;
}

int32 UTwitchNativeSubsystem::GetEnabledRewardCount(UTwitchNativeRewardPack* Pack) const
{
	Pack = ResolveRewardPack(Pack);
	if (!Pack)
	{
		return 0;
	}

	int32 Count = 0;
	for (const FTwitchNativeRewardDefinition& Def : Pack->Rewards)
	{
		if (IsRewardKeyEnabled(Def.RewardKey))
		{
			++Count;
		}
	}
	return Count;
}

int32 UTwitchNativeSubsystem::GetPublishableSlotBudget() const
{
	if (!LastSlotUsage.bValid)
	{
		return -1;
	}
	return FMath::Max(0, LastSlotUsage.Cap - LastSlotUsage.UsedByOthers);
}

bool UTwitchNativeSubsystem::IsSelectionOverBudget(UTwitchNativeRewardPack* Pack) const
{
	const int32 Budget = GetPublishableSlotBudget();
	if (Budget < 0)
	{
		return false;
	}
	return GetEnabledRewardCount(Pack) > Budget;
}

bool UTwitchNativeSubsystem::CanEnableAnotherReward(UTwitchNativeRewardPack* Pack) const
{
	const int32 Budget = GetPublishableSlotBudget();
	if (Budget < 0)
	{
		return true;
	}
	return GetEnabledRewardCount(Pack) < Budget;
}

void UTwitchNativeSubsystem::SaveRewardSelection()
{
	UTwitchNativeSaveGame* Save = Cast<UTwitchNativeSaveGame>(
		UGameplayStatics::CreateSaveGameObject(UTwitchNativeSaveGame::StaticClass()));
	if (!Save)
	{
		return;
	}

	Save->UserId = BroadcasterId;
	Save->DisabledRewardKeys = DisabledRewardKeys;

	UGameplayStatics::SaveGameToSlot(Save, GRewardSelectionSlot, 0);
}

void UTwitchNativeSubsystem::LoadRewardSelection()
{
	if (!UGameplayStatics::DoesSaveGameExist(GRewardSelectionSlot, 0))
	{
		return;
	}

	UTwitchNativeSaveGame* Save = Cast<UTwitchNativeSaveGame>(
		UGameplayStatics::LoadGameFromSlot(GRewardSelectionSlot, 0));
	if (!Save)
	{
		return;
	}

	// Don't leak one streamer's choices onto another account sharing the install.
	if (!Save->UserId.IsEmpty() && !BroadcasterId.IsEmpty() && Save->UserId != BroadcasterId)
	{
		DisabledRewardKeys.Reset();
		return;
	}

	DisabledRewardKeys = Save->DisabledRewardKeys;
}

// ---------------------------------------------------------------------------
// Slot accounting (read-only Helix)
// ---------------------------------------------------------------------------

void UTwitchNativeSubsystem::QueryChannelRewardUsage()
{
	// Deliberately does not attempt to fetch a token. See the header comment: the only route to
	// one (RefreshOAuthToken) invalidates the session and deauthenticates the user, because the
	// SDK cannot send a client_secret with a refresh. Report "unknown" and let callers fail open.
	LastSlotUsage = FTwitchRewardSlotUsage();
	OnRewardSlotsUpdated.Broadcast(LastSlotUsage);

	UE_LOG(LogTwitchNative, Verbose,
		TEXT("Reward slot usage is unavailable: listing channel rewards needs a Helix token the SDK cannot issue."));
}
