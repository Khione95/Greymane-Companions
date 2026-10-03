# Greymane Companions - research notes

## 2026-10-02: static research (feasibility_2026-10-02.txt, reports_2026-10-02.json, scripts/)
- The game has a full companion system, the "mercenary clan": mercenaryinfo (Mercenary_Main 0x40 = Damiane / Oongka /
  playable, Mercenary_Melee 0x46, Mercenary_Range 0x47 in group Mercenary_Combat 0x48 - the only one with
  is_hyosi_usable, "Follow into Combat" via the Whistling Arrow), mercenarygroupinfo.
- characterinfo._mercenaryInfo (u8, 0xFF none) + _mercenaryHireMessage ("Comrade Found") decide what a hired
  character becomes. Story Greymanes (Naira 0x19C, Yann 0x192, Duane 0x195, Shane 0x19A, Jian 0x19B, Roy 0xF51D4,
  Torian 0xF45F8, ...) are 0xFF. 38 camp comrades (24 melee, 14 range) are hireable.
- Unused, complete companion records exist: Nairah 0xF52CC, Yann 0xF5235 (+ Yahn 0x2, Witch 0xF52CF) - Mercenary_Main
  like Damian, ally "player", Follow/Standby/Heal prompts, 1_pc assets, never hired by anything.
- Hiring = a drop set "Mercenary(<character>)" (dropsetinfo; e.g. Mercenary(Damian) 0xF7B1A: "01 <key u32>" at +44,
  "ff ff <key u32>" at +103). Interactions can grant drop sets (Npc_Dialog1 0xAC: DropSet_Friendly_Talk at +239).
- E-prompts (interactioninfo, names in eng/interaction.paloc): Npc_Employ "Recruit" 0xF4292, Npc_Order_Follow
  0xF4251, Npc_Order_Wait "Standby" 0xF4253, Npc_Order_Disband 0xF425B, Npc_Hyosi_Add/Remove 0xF428F/0xF4290,
  Mercenary_Heal 0xF4254. On an NPC: characterinfo.interaction_info_list. Follow/Standby gated by CheckAccompanyType.
- Runtime (for an ASI later): accompany types 0 None, 1 Wait, 2 FollowOffensive, 3 FollowDefensive, 4 Follow;
  setter 0x21523A0, registration 0x2150930 (clan [[actor+0x68]+0x110]); server side unprotected.
- Restrictions: stageinfo allow_accompany, regioninfo _isAccompanyAllowed (DMM's "Companions Everywhere" lifts them).

## Plan
A (data, DMM JSON): Naira / Yann as full companions - new drop set Mercenary(Nairah) cloned from 0xF7B1A, a cloned
"Recruit" interaction granting it, added to story Naira's interaction list.
B (data): the other named Greymanes as camp comrades (type 0x46/0x47, Follow into Combat, own Mercenary(...) drop set).
C (ASI): make the world NPC itself follow (accompany path) - only if A/B are not enough.
First test: A for Naira + B for Duane + Npc_Employ on Naira as a control, on a test save.

## Test 2 (2026-10-02): the game did not load
"CharacterInfo의 _characterInfoOverridableFlags_Mercenary를 읽어들이는데 실패했다" - an empty LocalizableString is 13 bytes
(kind 0, field u32, owner u32, text length u32 = 0), not 1; the story records were 12 bytes off. Test 3 replaces all
13 bytes (kind 3, field 0x32, owner, length 13, the text id) - the rest of the record lines up as in Ryan's.

## Test 3 (2026-10-02): applied by DMM with 0 errors (2 drop sets, 2 interactions, 43 characters); the game loads.
Morrow (camp comrade, hired) shows only his normal greeting - none of the added options (Recruit, Follow, Standby,
Disband) appear: their conditions hide them (Recruit: IsHiredMercenary / IsInParty; Follow / Standby: CheckAccompanyType
0x197 - only for a comrade already accompanying). Data alone does not start Follow mode -> the plugin sets it.

## Experiment 1 (2026-10-03): F9 in game
AccompanyMercenary -> 0 (ok): the nearest comrade (Ashton/Astin 0x23E, 3.8 m - not Morrow) was added to the
accompany list as type 2 FollowOffensive (actor B0100012), next to the player (1) and 0xF5092 (type 2, mercenary FC,
probably the horse). The user saw "nothing happen" (was watching for Morrow) - next: check whether Ashton walks after
the player, or stands (a camp AI chart without the follow branch). Parked by the user for now.

## Experiment 1, second session (2026-10-03): it works with the vanilla orders
- F9 picked the pet dog first (Animal_Jindodog_Domestic_03 0xF5092, 3.1 m; pets are clan records, mercenary FC). It
  already followed by itself. By the next F9 (4 min later) its entry was gone and its actor was 0 (despawned).
  -> F9 / the roster must only take people (Greymanes), never animals.
- Aldric (NHM_Unique_Aldric_660 0x294, 7.0 m): err 0, type 2, but he did not follow and the user lost sight of him.
  4 min later he was still in the list, 6.3 m from the player on the server.
- Matilda (NHW_Unique_Matilda_634_1 0xF4C70, 1.0 m): err 0, type 2. Talking to her showed vanilla Standby; after
  Standby, vanilla Follow showed; after Follow SHE FOLLOWS. So AccompanyMercenary only registers; the follow
  behaviour starts with the vanilla Follow order (its AI event 0x21C on her chart). The vanilla orders work once
  she is in clan+0xB8 - CheckAccompanyType 0x197 sees the entry on the client (0x9FB sync works).
- Next: does she fight, keep up over distance, survive fast travel / reload; then the plugin starts Follow itself
  (SetAccompanyType 1 + Ack so vanilla Follow shows at once, or raise the follow event), and Recruit on Greymanes.

## Experiment 2 (2026-10-03): F9 + pump + logout distance - catch-up teleport works
- Max (0x280): registered by F9; his AI is run by the player's client (actor+0x90 = the user key 0x90100000), so
  the plugin set type Wait (vanilla Follow showed) and the player pressed Follow: he followed, but only after he
  finished the walk he was on (a client-run chart switches after its current activity).
- Logout distance 64 -> 1e6 (0x28FA550): the catch-up teleport now works - four TrAITeleportReq / 0x2AC5BF0 checks,
  all ok, she lands next to the player. So the earlier vanish was the clean-up's distance logout (leash 64 m).
- Comrades fight with the player. They do not ride horses (Damiane / Oongka do).
- Wanted next (user): a Dismiss option (back to their routine, still hired); an F11 health HUD that looks like the
  game's UI, with each companion's portrait (from their Knowledge entry) and name, for the Greymanes AND Damiane /
  Oongka; immediate follow for client-run AI; horses. Research running (gca\wf3, gca\wf4).
- Fast travel: the game removed Max's entry and logged out his camp actor (spawn type 0xC NPCSchedule, spawner =
  the player). The pump re-registered him: AccompanyMercenary summoned a NEW actor next to the player (spawn type
  0xE SummonMercenary, leader = the player, record +0xC4 = 1). But the pump then "asked" (type Wait) because a
  client runs his AI, so he stood in Standby; after a second fast travel he was still alive 3315 m away (leash) but
  in Standby, so no catch-up. Fix: a summoned comrade is left following with type 2 (as Damiane after a warp).
  Idea: Recruit could make the game SUMMON her next to the player (spawn type 0xE) instead of re-arming the camp
  actor - a summoned copy probably follows at once.
- Defeated in a fight, he knelt (downed) and got up after the fight - the user likes it (HUD: show "Down").
