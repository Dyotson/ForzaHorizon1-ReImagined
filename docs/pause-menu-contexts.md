# Stock pause contexts and custom menu coverage

The custom pause menu reads FH1 TU4's current action model, including native
localized labels and enabled flags. The normal model builder (`827DE1B0`) and
the Rally model builder (`827D65A0`) retain responsibility for event, party,
progression, hardware and DLC availability. A row that the stock builder does
not publish is not added by the custom menu. Disabled rows remain disabled.
Activation reads the model again; cached tile availability cannot authorize an
action after the context changes.

Home, Cars, Social and Settings retain their IDs and order. Activity and Horizon
pages reuse the existing six tile donors and scrolling tab strip; additional
Horizon rows use a second page. Opening pause with an available Quit Race,
Quit PR Stunt or Finish action selects Activity. Returning from a child retains
the originating page and tile. Native labels preserve distinctions such as
Quit Race versus Retire rather than interpreting both as Leave Session.

## Action checklist

These IDs are from the original TU4 pause builders and dispatcher `827EBEE0`.
The dispatcher takes the action in **r6**, with event **r4=2**; r5 is not the ID.

| Native ID | Stock action/context | Custom destination and routing |
|---|---|---|
| 0 | Quit PR Stunt, when the current mission supplies it | Activity; original confirmation 0 and result handler |
| 1 | Restart PR Stunt, when that mission supports restart | Activity; original confirmation 1 and result handler |
| 2 | Map, when allowed by the current activity | Existing Home map |
| 3 | Resume; also present in the special Rally model | Existing Back/Start resume callback |
| 4 | Co-op Challenges in the applicable multiplayer modes | Horizon; original child route |
| 5 | Message Center when the native gate permits it | Horizon; original message/profile checks and child route |
| 6 | Horizon Rally when offered by the current event/DLC state | Horizon; original Rally eligibility checks and flow |
| 7 | Multiplayer in supported contexts | Horizon; original entry/confirmation, alongside the custom Social entry points |
| 8 | Sponsor Challenges when unlocked | Horizon; original child route |
| 9 | Dispatcher branch exists, but neither pause model builder emits it | Not synthesized or exposed |
| 10 | Restart; native confirmation varies for race, Rivals and Rally | Activity; original confirmation 5, including its native difficulty choice |
| 11 | Photo Mode when allowed | Existing Home tile |
| 12 | My Profile | Existing Home tile and general Settings difficulty availability |
| 13 | Head Tracking when the native hardware/state gate offers it | Horizon; original child route |
| 14 | Quit Race or Retire, according to the current native event/party state | Activity; original label, confirmation 17 and result handler |
| 15 | End Free Roam for an eligible host | Activity and existing Social session page; original confirmation 18 |
| 16 | Finish current event/challenge when offered | Activity; original confirmation 19 and result handler |
| 17 | Quit, with the stock single-player/party context | Y footer; original confirmation 2 outside the desktop-exit context below |
| 18 | Difficulty in the special Rally pause model | Settings; original dispatcher allocates context **22**, preserving its apply/return behavior |
| 19 | 1000 Club when the native expansion gate offers it | Horizon; original content/license checks and challenge flow |

The special Rally builder emits only 3, 18 and 10. The custom menu does not add
a race quit/retire action to that model unless the original builder supplies
one. Normal event, Rivals, PR Stunt, co-op and online host/guest differences come
from their actual rows, not from a host-side recreation of every game mode.
Expansion package detection alone never enables Rally or 1000 Club.

The existing desktop-exit extension is selected only with an available Map row,
known single-player Colorado free-roam presence/region, no race/stunt/Rally-only
rows, and its existing offline,
save-manager and callback gates. Other Quit contexts use the original handler.
This preserves party choices and race-specific confirmations. The existing
save/flush/title completion checks still control desktop closure.

## Confirmation and return contracts

The custom adapter forwards the common native result handler `827618F8` exactly
once. That handler owns the high-numbered content/marketplace results and
delegates the pause-specific results to `827E5CA8`. The adapter records accepted
native child requests (`82F240C8`: pending flag +264, owned parameters +160),
reads the ID through the same virtual getter as `82F28AD0`, and matches both the
returning child's parent and retained parameters. Confirmation choices require
the native 16-byte Result type. Information/difficulty returns do not read a
choice. **0 is a valid PR Stunt confirmation ID.**

No accepted child on a confirmation-only route restores the custom page. A
scoped observer forwards the original Club guard `827540F8` once and detects
its no-child rejection paths. The adapter does not rerun content checks or
infer success from dispatch alone. Follow-up native children retain the hidden
parent, and the native result handler's keep-open flag is respected.

| Confirmation | Choice | Custom composition handling |
|---|---|---|
| 0, 1, 3, 17, 18, 19, 23 | 0 | Release composition before original event transition |
| 0, 1, 3, 17, 18, 19, 23 | Cancel | Restore page after forwarding original handler |
| 2 (Quit) | 0 or 6 (leave with party) | Release before original quit delegate |
| 2 (Quit) | Cancel | Restore page |
| 5 (Restart) | 0 | Release only after the original restart gate accepts (+468); otherwise restore page |
| 5 (Restart) | 3 (B) | Restore page |
| 5 (Restart) | 6 (difficulty) | Keep parent hidden; preserve the native delegate's full-screen 170 route and return bookmark |
| 16, 20, 21 | Any | Restore after native information/eligibility prompt dismissal |
| 22 (Rally difficulty) | Return | Preserve native apply handler, then restore page |
| 100005, 100007, 100011–100014 | Return | Preserve common native content/marketplace handling; follow any replacement child before restoring |

No custom action directly resets an event, changes rewards, writes a save or
calls a quit/restart delegate in place of the native confirmation.

## Validation and outstanding runtime checks

Focused source tests exercise the actual native dispatch adapter for all 20
native IDs; rejected missing, disabled, duplicate and malformed models; preserved
guest bytes and PPC caller registers; denied Club guard with/without a prompt;
accepted child request IDs/parameter identity and wrong-parent/stale-result
rejection; contextual labels; absent/disabled DLC rows; Rally difficulty;
modal choice distinctions; spatial navigation and tab
overflow. Existing car access/reload tests remain covered. Source-disabled TU0
and TU4 builds and the enabled TU4 scene translation unit compile locally.

**No in-game execution is claimed for these additions.** Before accepting the
feature, run these against isolated profile copies, recording the source and
runtime revisions:

- Career race: Quit Race Cancel/Confirm, Restart Cancel/Confirm, difficulty
  child and Back, then race resume and reward/progression behavior.
- PR Stunt and Rivals: context-specific labels, quit/restart/finish/retire
  confirmations and native destination on each accepted result.
- Rally: its three-row pause model, native difficulty apply/cancel and restart;
  Colorado-to-Rally entry only when the native flow offers it.
- 1000 Club and Sponsor Challenges: available and unavailable content/activity
  states, child Back, native challenge data, and no-child eligibility rejection.
- Message Center, native Multiplayer and Rally entry: Cancel/OK on eligibility
  errors, confirmation cancellation, and native marketplace follow-up returns.
- Multiplayer host/guest: co-op entry, retire versus leave, End Free Roam and
  party-leader choice 6; no offline desktop extension in these contexts.
- Free roam: original four tabs, new Horizon pages, resume, native title quit,
  and existing save-confirmed desktop exit.

Camera movement and studio fade remain separate work. No Bucket List action or
second game's resources are included.
