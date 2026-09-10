# UETwitchNative

A Twitch integration layer for Unreal Engine 5, built on Twitch's own `TwitchSDK`
plugin. It handles the parts the SDK leaves to you: an authentication state
machine, channel point rewards defined as data assets rather than code, stable
routing from a redemption back to the gameplay that should fire, and a
streamer-facing on/off surface for choosing which rewards go live.

Everything is exposed to Blueprint. Nothing downstream needs to include
`TwitchSDK` headers.

## Requirements

- Unreal Engine 5.6
- Twitch's `TwitchSDK` plugin, enabled (declared as a dependency in the `.uplugin`)
- A Twitch application Client Id

The plugin is `EnabledByDefault: false` and marked beta.

## Authentication

`UTwitchNativeSubsystem` is a `UGameInstanceSubsystem`. It wraps the SDK's device
code flow behind a four-state machine — `LoggedOut`, `Loading`, `WaitingForCode`,
`LoggedIn` — mirrored into this plugin's own `EUETwitchAuthStatus` so Blueprints
never take a dependency on SDK types.

```
ConnectUsingProjectSettings(bAutoLaunchBrowser)
  -> OnAuthInfoReceived (Uri + UserCode for the streamer to enter)
  -> polls while WaitingForCode
  -> OnAuthStatusChanged(LoggedIn)
  -> OnUserInfoReceived
```

`OnAuthStatusChanged` broadcasts only on an actual transition, so it is safe to
bind UI directly to it. While the flow is waiting on the user, a timer polls the
SDK rather than blocking. Setting `bAutoConnectOnStartup` attempts a silent
restore of persisted credentials at `Initialize`; if nothing is persisted, no UI
appears until something calls `ConnectUsingProjectSettings` explicitly.

`TwitchLoginPresenter` and `TwitchLoginWidget` provide a ready-made login surface
if you don't want to build one.

## Rewards are data, not code

A `UTwitchNativeRewardPack` is a `UPrimaryDataAsset` holding a list of
`FTwitchNativeRewardDefinition`. Each definition carries a `RewardKey` — a stable
`FName` your game code switches on — plus the Twitch-side presentation and limits:

| Field | Purpose |
| --- | --- |
| `RewardKey` | Stable identifier for game code. Never match on title text. |
| `Category` | Optional grouping for settings UI ("Flavor", "Hostile"). Purely organisational. |
| `Title`, `Prompt` | What the viewer sees. |
| `Cost` | Channel points. |
| `bUserInputRequired` | Whether the viewer types something with the redeem. |
| `MaxPerStream`, `MaxPerUserPerStream`, `GlobalCooldownSeconds` | Twitch-side rate limits. |
| `BackgroundColor` | Hex, e.g. `#9146FF`. |
| `bSkipQueue` | Redeems bypass the request queue, so fewer need manual resolution. |

The pack also carries a `RewardTitlePrefix` (e.g. `"OMP: "`) to avoid colliding
with rewards the streamer created by hand.

`SyncCustomRewardsFromPack` publishes a pack as the game's complete reward set.
Twitch scopes reward management to the creating Client Id, so a sync only ever
replaces rewards **this game** created — a streamer's own rewards are never
touched. `ClearCustomRewards` withdraws them, and runs automatically at shutdown
when `bClearRewardsOnShutdown` is set.

## Redemption routing

Bind `OnRewardTriggered` and switch on the `RewardKey`:

```cpp
void AMyGameMode::HandleReward(FName RewardKey, const FTwitchCustomRewardEvent& Event)
{
    if (RewardKey == TEXT("LightsOut"))
    {
        KillTheLights(Event.RedeemerName);
    }
}
```

The routing table is rebuilt on every sync. It maps prefixed title back to
`RewardKey`, because the SDK's `ReplaceCustomRewards` does not return reward ids —
and since the plugin writes those titles itself on each sync, they stay stable for
the session. Redemption ids are deduped, so a repeated event does not fire
gameplay twice.

`OnCustomRewardRedeemed` gives you the raw event if you want it unrouted.
`ResolveRedemption(RedemptionId, RewardId, bFulfill)` marks a pending redemption
fulfilled or cancelled — cancelling refunds the viewer's channel points.

## Reward selection

Streamers rarely want every reward a game ships with. `SetRewardKeyEnabled` and
`SetCategoryEnabled` drive an exclusion list — a key that has never been toggled
defaults to on — and `SaveRewardSelection` / `LoadRewardSelection` persist it.

One distinction matters and is easy to get wrong: **off** means the reward is
never published, which is the only thing that frees a channel slot.
`FTwitchNativeRewardDefinition::bEnabled` publishes the reward in a *disabled*
state and still consumes one. They are not interchangeable.

## Slot accounting — known limitation

Twitch caps custom rewards per channel (default 50, configurable via
`ChannelRewardCap`). `QueryChannelRewardUsage` is meant to report how much of that
cap is in use.

**It currently always reports unavailable.** Counting requires listing the
channel's rewards, which needs a Helix bearer token, and the SDK cannot provide
one: `RefreshOAuthToken` calls `RefreshAccessToken`, which invalidates the auth
result and POSTs `grant_type=refresh_token` with no `client_secret` — the SDK has
no way to supply one. Twitch answers 400, and the SDK then calls `Deauthenticate`.
Asking for a token logs the user out.

Callers degrade gracefully rather than breaking:

- `GetPublishableSlotBudget()` returns `-1` when the figures are unknown.
- `CanEnableAnotherReward()` **fails open** — it returns true while the budget is
  unknown, so a failed query never locks a streamer out of their own settings. A
  publish that was doomed anyway still surfaces through `OnTwitchError`.
- `HasSlotUsageInfo()` tells you whether the numbers mean anything.

Note that the publishable budget is deliberately *not*
`FTwitchRewardSlotUsage::Free`. `Free` subtracts this app's own published rewards
too, but a publish replaces those — gating on `Free` under-counts the real budget
by however many are already up.

**Regarding the unavailability on this one** This could be resolved if you used a middle-man server where you could keep a Helix bearer token secret,
but we can't package one alongside a game, so unless you plan to use a server somewhere as host, this feature cannot work as originally planned.

## Project settings

Under **Project Settings ▸ Game ▸ Twitch Native**:

| Setting | Default | Effect |
| --- | --- | --- |
| `DefaultScopes` | — | OAuth scopes requested at connect. |
| `bAutoConnectOnStartup` | `false` | Silently restore persisted credentials at `Initialize`. |
| `DefaultRewardPack` | — | Soft reference to the pack used when an API takes a null pack. |
| `bAutoSyncRewardsOnLogin` | `true` | Publish the default pack once login completes. |
| `bClearRewardsOnShutdown` | `true` | Withdraw this app's rewards on exit. |
| `ChannelRewardCap` | `50` | Twitch's channel-wide cap. Exposed rather than hardcoded so it can be corrected without a code change. |

## Events

| Delegate | Payload |
| --- | --- |
| `OnAuthStatusChanged` | `EUETwitchAuthStatus` — fires only on transition |
| `OnAuthInfoReceived` | `FTwitchAuthInfo` — device code `Uri` and `UserCode` |
| `OnUserInfoReceived` | `FTwitchUserInfoBP` — id, login, display name, avatar URL, broadcaster type |
| `OnRewardTriggered` | `FName RewardKey` + `FTwitchCustomRewardEvent` |
| `OnCustomRewardRedeemed` | Raw `FTwitchCustomRewardEvent` |
| `OnRewardSlotsUpdated` | `FTwitchRewardSlotUsage` |
| `OnTwitchError` | Message string |

`FTwitchUserInfoBP::ProfileImageUrl` is a URL — feed it to UMG's *Download Image*
async node to get a texture.

## Limitations

- Slot accounting is unavailable for the reason documented above.
- Channel point rewards are the implemented surface. Polls and chat are not.
- Beta; the API may still change.

## License

MIT. Copyright 2026 Patrick Barnhardt.
