#include "TwitchNativeSubsystem.h"

#include "TwitchNativeRewardPack.h"
#include "TwitchNativeSaveGame.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Containers/StringConv.h" 
#include "Async/Async.h"
#include "Modules/ModuleManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <type_traits>

#include "TwitchNativeSettings.h"
#include "TwitchSDKStructs.h"

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
}

void UTwitchNativeSubsystem::Deinitialize()
{
	StopListeningForCustomRewards();
	Super::Deinitialize();
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

	auto Core = FModuleManager::GetModuleChecked<FTwitchSDKModule>("TwitchSDK").Core;
	TWeakObjectPtr<UTwitchNativeSubsystem> WeakThis(this);

	Core->GetAuthenticationInfo(
		BuildOAuthScopesW(Scopes),
		[WeakThis](const TwitchSDK::AuthenticationInfo& Info)
		{
			if (!WeakThis.IsValid()) return;

			FTwitchAuthInfo Out;
			Out.bAlreadyAuthenticated = (Info.UserCode.size() == 0);
			Out.Uri      = FString(UTF8_TO_TCHAR(Info.Uri.data()));
			Out.UserCode = FString(UTF8_TO_TCHAR(Info.UserCode.data()));

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

				if (Out.bAlreadyAuthenticated)
				{
					WeakThis->WaitForLoginAndFetchUserInfo();
				}

				WeakThis->OnAuthInfoReceived.Broadcast(Out);
			});
		},
		[WeakThis](const std::exception& E)
		{
			if (!WeakThis.IsValid()) return;

			const FString Msg = FString::Printf(TEXT("GetAuthenticationInfo failed: %s"), UTF8_TO_TCHAR(E.what()));
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Msg]()
			{
				if (WeakThis.IsValid())
				{
					WeakThis->OnTwitchError.Broadcast(Msg);
				}
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

	// Route to stable key if possible
			if (const FName* Key = WeakThis->RewardIdToKey.Find(Out.RewardId))
			{
				WeakThis->OnRewardTriggered.Broadcast(*Key, Out);
			}
			else if (const FName* rewardKey = WeakThis->RewardTitleToKey.Find(Out.RewardTitle))
			{
				WeakThis->OnRewardTriggered.Broadcast(*rewardKey, Out);
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
			Out.UserId      = TwitchSDK::ToFString(Info.ChannelId);
			Out.Login       = TwitchSDK::ToFString(Info.LoginName);
			Out.DisplayName = TwitchSDK::ToFString(Info.DisplayName);

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

	auto Core = FModuleManager::GetModuleChecked<FTwitchSDKModule>("TwitchSDK").Core;
	TWeakObjectPtr<UTwitchNativeSubsystem> WeakThis(this);

	Core->LogOut(
		[](){},
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

void UTwitchNativeSubsystem::SyncCustomRewardsFromPack(UTwitchNativeRewardPack* Pack)
{
	if (!Pack || !IsTwitchSdkAvailable())
	{
		OnTwitchError.Broadcast(TEXT("No RewardPack or Twitch unavailable."));
		return;
	}

	// Build routing table for RewardTitle -> RewardKey (still used as fallback)
	RewardTitleToKey.Reset();

	for (const FTwitchNativeRewardDefinition& Def : Pack->Rewards)
	{
		const FString Title = Pack->RewardTitlePrefix + Def.Title;

		if (!Def.RewardKey.IsNone())
		{
			RewardTitleToKey.Add(Title, Def.RewardKey);
		}
	}

	// Use Helix CRUD so we only manage our prefixed rewards
	SyncManagedRewardsFromPack_Helix(Pack);
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

static FString JsonStringify(const TSharedRef<FJsonObject>& Obj)
{
	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);
	return Out;
}

void UTwitchNativeSubsystem::SetHelixCredentials(const FString& InClientId, const FString& InUserAccessToken)
{
	HelixClientId = InClientId;
	HelixUserAccessToken = InUserAccessToken;
}

bool UTwitchNativeSubsystem::TryBuildHelixAuth(FString& OutClientId, FString& OutToken, FString& OutBroadcasterId) const
{
	OutBroadcasterId = BroadcasterId;

	OutClientId = HelixClientId;
	OutToken    = HelixUserAccessToken;

	if (OutBroadcasterId.IsEmpty())
	{
		return false;
	}
	if (OutClientId.IsEmpty() || OutToken.IsEmpty())
	{
		return false;
	}
	return true;
}

void UTwitchNativeSubsystem::HelixRequest(
	const FString& Verb,
	const FString& Url,
	const FString& BodyJson,
	TFunction<void(bool bOk, int32 Code, const FString& ResponseBody, const FString& Error)> Callback)
{
	FString ClientId, Token, Broadcaster;
	if (!TryBuildHelixAuth(ClientId, Token, Broadcaster))
	{
		Callback(false, -1, TEXT(""), TEXT("Helix auth not configured (ClientId/Token/BroadcasterId missing)."));
		return;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(Verb);

	Req->SetHeader(TEXT("Client-Id"), ClientId);
	Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Token));
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	if (!BodyJson.IsEmpty())
	{
		Req->SetContentAsString(BodyJson);
	}

	TWeakObjectPtr<UTwitchNativeSubsystem> WeakThis(this);

	Req->OnProcessRequestComplete().BindLambda(
		[WeakThis, Callback](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
		{
			const int32 Code = Response.IsValid() ? Response->GetResponseCode() : -1;
			const FString Body = Response.IsValid() ? Response->GetContentAsString() : TEXT("");

			if (!bSucceeded || !Response.IsValid())
			{
				AsyncTask(ENamedThreads::GameThread, [Callback]()
				{
					Callback(false, -1, TEXT(""), TEXT("HTTP request failed (no response)."));
				});
				return;
			}

			const bool bOk = (Code >= 200 && Code < 300);

			AsyncTask(ENamedThreads::GameThread, [Callback, bOk, Code, Body]()
			{
				Callback(bOk, Code, Body, bOk ? TEXT("") : Body);
			});
		});

	Req->ProcessRequest();
}

void UTwitchNativeSubsystem::HelixGetCustomRewards(TFunction<void(bool bOk, const TArray<FHelixRewardLite>& Rewards, const FString& Error)> Callback)
{
	FString ClientId, Token, Broadcaster;
	if (!TryBuildHelixAuth(ClientId, Token, Broadcaster))
	{
		Callback(false, {}, TEXT("Helix auth not configured."));
		return;
	}

	// only_manageable_rewards=true keeps the list focused on rewards you can manage. :contentReference[oaicite:7]{index=7}
	const FString Url = FString::Printf(
		TEXT("https://api.twitch.tv/helix/channel_points/custom_rewards?broadcaster_id=%s&only_manageable_rewards=true"),
		*Broadcaster);

	HelixRequest(TEXT("GET"), Url, TEXT(""),
		[Callback](bool bOk, int32 Code, const FString& Body, const FString& Error)
		{
			if (!bOk)
			{
				Callback(false, {}, FString::Printf(TEXT("GetCustomRewards failed (%d): %s"), Code, *Error));
				return;
			}

			TArray<FHelixRewardLite> Out;

			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				Callback(false, {}, TEXT("Failed to parse GetCustomRewards JSON."));
				return;
			}

			const TArray<TSharedPtr<FJsonValue>>* Data = nullptr;
			if (Root->TryGetArrayField(TEXT("data"), Data) && Data)
			{
				for (const TSharedPtr<FJsonValue>& V : *Data)
				{
					const TSharedPtr<FJsonObject> O = V->AsObject();
					if (!O.IsValid()) continue;

					FHelixRewardLite R;
					R.Id = O->GetStringField(TEXT("id"));
					R.Title = O->GetStringField(TEXT("title"));
					Out.Add(MoveTemp(R));
				}
			}

			Callback(true, Out, TEXT(""));
		});
}

void UTwitchNativeSubsystem::HelixCreateCustomReward(const FString& BodyJson, TFunction<void(bool bOk, const FString& Error)> Callback)
{
	FString ClientId, Token, Broadcaster;
	if (!TryBuildHelixAuth(ClientId, Token, Broadcaster))
	{
		Callback(false, TEXT("Helix auth not configured."));
		return;
	}

	// POST ...custom_rewards?broadcaster_id=... 
	const FString Url = FString::Printf(
		TEXT("https://api.twitch.tv/helix/channel_points/custom_rewards?broadcaster_id=%s"),
		*Broadcaster);

	HelixRequest(TEXT("POST"), Url, BodyJson,
		[Callback](bool bOk, int32 Code, const FString& Body, const FString& Error)
		{
			Callback(bOk, bOk ? TEXT("") : FString::Printf(TEXT("Create failed (%d): %s"), Code, *Error));
		});
}

void UTwitchNativeSubsystem::HelixUpdateCustomReward(const FString& RewardId, const FString& BodyJson, TFunction<void(bool bOk, const FString& Error)> Callback)
{
	FString ClientId, Token, Broadcaster;
	if (!TryBuildHelixAuth(ClientId, Token, Broadcaster))
	{
		Callback(false, TEXT("Helix auth not configured."));
		return;
	}

	// PATCH ...custom_rewards?broadcaster_id=...&id=... :contentReference[oaicite:9]{index=9}
	const FString Url = FString::Printf(
		TEXT("https://api.twitch.tv/helix/channel_points/custom_rewards?broadcaster_id=%s&id=%s"),
		*Broadcaster, *RewardId);

	HelixRequest(TEXT("PATCH"), Url, BodyJson,
		[Callback](bool bOk, int32 Code, const FString& Body, const FString& Error)
		{
			Callback(bOk, bOk ? TEXT("") : FString::Printf(TEXT("Update failed (%d): %s"), Code, *Error));
		});
}

void UTwitchNativeSubsystem::HelixDeleteCustomReward(const FString& RewardId, TFunction<void(bool bOk, const FString& Error)> Callback)
{
	FString ClientId, Token, Broadcaster;
	if (!TryBuildHelixAuth(ClientId, Token, Broadcaster))
	{
		Callback(false, TEXT("Helix auth not configured."));
		return;
	}

	// DELETE ...custom_rewards?broadcaster_id=...&id=... :contentReference[oaicite:10]{index=10}
	const FString Url = FString::Printf(
		TEXT("https://api.twitch.tv/helix/channel_points/custom_rewards?broadcaster_id=%s&id=%s"),
		*Broadcaster, *RewardId);

	HelixRequest(TEXT("DELETE"), Url, TEXT(""),
		[Callback](bool bOk, int32 Code, const FString& Body, const FString& Error)
		{
			Callback(bOk, bOk ? TEXT("") : FString::Printf(TEXT("Delete failed (%d): %s"), Code, *Error));
		});
}

static FString BuildHelixRewardBody(const FTwitchNativeRewardDefinition& Def, const FString& FullTitle)
{
	// Request body params for Create/Update Custom Reward. :contentReference[oaicite:11]{index=11}
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();

	O->SetStringField(TEXT("title"), FullTitle);
	O->SetNumberField(TEXT("cost"), (int32)Def.Cost);

	if (!Def.Prompt.IsEmpty())
	{
		O->SetStringField(TEXT("prompt"), Def.Prompt);
	}

	O->SetBoolField(TEXT("is_enabled"), Def.bEnabled);
	O->SetBoolField(TEXT("is_user_input_required"), Def.bUserInputRequired);

	// Color field is shown with leading '#'. 
	if (!Def.BackgroundColor.IsEmpty())
	{
		O->SetStringField(TEXT("background_color"), SanitizeHexColor(Def.BackgroundColor));
	}

	O->SetBoolField(TEXT("should_redemptions_skip_request_queue"), Def.bSkipQueue);

	// Twitch wants the *_enabled flags paired with their values. :contentReference[oaicite:13]{index=13}
	const bool bMaxPerStream = Def.MaxPerStream > 0;
	O->SetBoolField(TEXT("is_max_per_stream_enabled"), bMaxPerStream);
	if (bMaxPerStream) O->SetNumberField(TEXT("max_per_stream"), Def.MaxPerStream);

	const bool bMaxPerUser = Def.MaxPerUserPerStream > 0;
	O->SetBoolField(TEXT("is_max_per_user_per_stream_enabled"), bMaxPerUser);
	if (bMaxPerUser) O->SetNumberField(TEXT("max_per_user_per_stream"), Def.MaxPerUserPerStream);

	const bool bCooldown = Def.GlobalCooldownSeconds > 0;
	O->SetBoolField(TEXT("is_global_cooldown_enabled"), bCooldown);
	if (bCooldown) O->SetNumberField(TEXT("global_cooldown_seconds"), Def.GlobalCooldownSeconds);

	return JsonStringify(O);
}

void UTwitchNativeSubsystem::SyncManagedRewardsFromPack_Helix(UTwitchNativeRewardPack* Pack)
{
	FString ClientId, Token, Broadcaster;
	if (!TryBuildHelixAuth(ClientId, Token, Broadcaster))
	{
		OnTwitchError.Broadcast(TEXT("Helix sync requires ClientId + UserAccessToken + BroadcasterId."));
		return;
	}

	const FString Prefix = Pack->RewardTitlePrefix;

	HelixGetCustomRewards(
		[this, Pack, Prefix](bool bOk, const TArray<FHelixRewardLite>& Existing, const FString& Error)
		{
			if (!bOk)
			{
				OnTwitchError.Broadcast(Error);
				return;
			}

			// Filter to "ours" by prefix
			TMap<FString, FHelixRewardLite> ExistingByTitle;
			TSet<FString> ExistingManagedIds;

			for (const FHelixRewardLite& R : Existing)
			{
				if (!Prefix.IsEmpty() && R.Title.StartsWith(Prefix))
				{
					ExistingByTitle.Add(R.Title, R);
					ExistingManagedIds.Add(R.Id);
				}
			}

			// Build desired titles from pack
			TSet<FString> DesiredTitles;
			DesiredTitles.Reserve(Pack->Rewards.Num());

			// We'll run requests sequentially (avoids rate spikes).
			TArray<TFunction<void(TFunction<void()>)>> Steps;

			for (const FTwitchNativeRewardDefinition& Def : Pack->Rewards)
			{
				const FString FullTitle = Prefix + Def.Title;
				DesiredTitles.Add(FullTitle);

				const FString BodyJson = BuildHelixRewardBody(Def, FullTitle);

				if (const FHelixRewardLite* Found = ExistingByTitle.Find(FullTitle))
				{
					const FString RewardId = Found->Id;
					Steps.Add([this, RewardId, BodyJson](TFunction<void()> Next)
					{
						HelixUpdateCustomReward(RewardId, BodyJson, [this, Next](bool bOk2, const FString& Err2)
						{
							if (!bOk2) OnTwitchError.Broadcast(Err2);
							Next();
						});
					});
				}
				else
				{
					Steps.Add([this, BodyJson](TFunction<void()> Next)
					{
						HelixCreateCustomReward(BodyJson, [this, Next](bool bOk2, const FString& Err2)
						{
							if (!bOk2) OnTwitchError.Broadcast(Err2);
							Next();
						});
					});
				}
			}

			// After upserts, refresh list, rebuild RewardIdToKey/ManagedRewardIds, then delete obsolete managed rewards.
			Steps.Add([this, Pack, Prefix, DesiredTitles](TFunction<void()> Next)
			{
				HelixGetCustomRewards([this, Pack, Prefix, DesiredTitles, Next](bool bOk3, const TArray<FHelixRewardLite>& Now, const FString& Err3)
				{
					if (!bOk3)
					{
						OnTwitchError.Broadcast(Err3);
						Next();
						return;
					}

					RewardIdToKey.Reset();
					ManagedRewardIds.Reset();

					// Rebuild ID mapping from the refreshed list
					for (const FHelixRewardLite& R : Now)
					{
						if (!Prefix.IsEmpty() && !R.Title.StartsWith(Prefix)) continue;

						if (const FName* Key = RewardTitleToKey.Find(R.Title))
						{
							RewardIdToKey.Add(R.Id, *Key);
							ManagedRewardIds.Add(*Key, R.Id);
						}
					}

					// Delete managed rewards no longer present in pack (prefix + missing title)
					TArray<FString> ObsoleteIds;
					for (const FHelixRewardLite& R : Now)
					{
						if (!Prefix.IsEmpty() && R.Title.StartsWith(Prefix) && !DesiredTitles.Contains(R.Title))
						{
							ObsoleteIds.Add(R.Id);
						}
					}

					if (ObsoleteIds.Num() == 0)
					{
						Next();
						return;
					}

					// Delete sequentially
					int32* Index = new int32(0);
					TFunction<void()> DeleteNext = [this, ObsoleteIds, Index, Next, &DeleteNext]()
					{
						if (*Index >= ObsoleteIds.Num())
						{
							delete Index;
							Next();
							return;
						}

						const FString IdToDelete = ObsoleteIds[*Index];
						(*Index)++;

						HelixDeleteCustomReward(IdToDelete, [this, Next, &DeleteNext](bool bOkDel, const FString& ErrDel)
						{
							if (!bOkDel) OnTwitchError.Broadcast(ErrDel);
							DeleteNext();
						});
					};

					DeleteNext();
				});
			});

			// Run the chain
			int32* StepIndex = new int32(0);
			TFunction<void()> RunNext = [this, Steps, StepIndex, &RunNext]()
			{
				if (*StepIndex >= Steps.Num())
				{
					delete StepIndex;
					return;
				}

				const int32 Local = (*StepIndex)++;
				Steps[Local](RunNext);
			};

			RunNext();
		});
}

void UTwitchNativeSubsystem::ResolveRedemption_Helix(const FString& RedemptionId, const FString& RewardId, bool bFulfill)
{
	FString ClientId, Token, Broadcaster;
	if (!TryBuildHelixAuth(ClientId, Token, Broadcaster))
	{
		OnTwitchError.Broadcast(TEXT("Helix resolve requires ClientId + UserAccessToken + BroadcasterId."));
		return;
	}

	// PATCH .../redemptions?broadcaster_id=...&reward_id=...&id=... { "status": "CANCELED" } :contentReference[oaicite:14]{index=14}
	const FString Url = FString::Printf(
		TEXT("https://api.twitch.tv/helix/channel_points/custom_rewards/redemptions?broadcaster_id=%s&reward_id=%s&id=%s"),
		*Broadcaster, *RewardId, *RedemptionId);

	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("status"), bFulfill ? TEXT("FULFILLED") : TEXT("CANCELED"));

	HelixRequest(TEXT("PATCH"), Url, JsonStringify(Body),
		[this](bool bOk, int32 Code, const FString& Resp, const FString& Err)
		{
			if (!bOk)
			{
				OnTwitchError.Broadcast(FString::Printf(TEXT("Resolve failed (%d): %s"), Code, *Err));
			}
		});
}

