Greymane Companions 1.0 - by Khione
===================================

Recruit the Greymanes from your camp as companions. They follow you through
the world, fight at your side, ride with you, and go back to camp when you
dismiss them.

- Walk up to a Greymane in your camp, press Ctrl for more talk options and
  choose Recruit. Up to 6 Greymanes at once, besides Damiane and Oongka.
- They drop what they were doing and follow you at once.
- They fight with you, and kneel when they are beaten, as Damiane and Oongka
  do.
- When you fall far behind them, they catch up with the same green-leaves
  teleport as Damiane and Oongka.
- When you ride, they ride with you on horses of their own, and get off when
  you do.
- They are shown on the map and the minimap.
- Talk to one of them and choose Dismiss: he walks back to what he was doing.
  One who was brought to you after a fast travel stays where he is and leaves
  when you are out of sight.
- Bring back (F10): your companions come to you.
- A notice at the top right tells you who joined, or that your party is full.

Works best with Party HUD (their health on screen) - it shows the Greymanes
recruited with this mod.


Install
-------
Needs Crimson Desert Mod Manager (DMM) 3.x: the talk options are a DMM mod.

1. Drop the archive into DMM (or its "Greymane Companions" folder into DMM's
   mods folder) and enable it. DMM puts GreymaneCompanions.asi into the game's
   bin64 and installs an ASI loader (winmm.dll) if there is none.
2. Apply the mods in DMM and start the game.


Settings: bin64\GreymaneCompanions.ini
--------------------------------------
Written on the first start. Changes apply the next time the game starts.

[Hotkeys]
BringBack = F10     your companions come to you (Ctrl+, Shift+, Alt+ work;
                    None turns the key off)

[Options]           1 on, 0 off
TeleportEffect = 1  a companion left far behind catches up with her own
                    teleport (else she is moved to you)
HideRoutine = 1     a recruited Greymane drops what she was doing at once
StayBehind = 1      a companion summoned to you after a fast travel stays
                    behind when dismissed (0: he vanishes at once)
MapIcon = 1         companions on the map and minimap
MapIconStyle = 0    0 the party icon, 1 the character icon with the name
Horses = 1          your companions ride when you ride
HorseSpawn = 1      a horse is made for each of them (0: they only take a
                    free horse nearby)
DetailedLog = 0     1 writes much more to the log (for bug reports)


Notes
-----
- The Greymanes you can recruit are the camp comrades who joined you through
  the scout missions (Gilbert, Morrow, Matilda, Luther and the others).
- Their horses are made by the mod; they do not whistle for them, and they get
  onto them at once rather than walking up to them.
- Dismiss your Greymane companions before you uninstall the mod: the party
  list is part of your save.
- It is made for the game version it was released with; after a game update
  it waits for its own update and logs why
  (bin64\GreymaneCompanions\GreymaneCompanions.log).
- Works with Character Creator, Seasons of Pywel and Party HUD.


Uninstall
---------
Dismiss your Greymane companions, save, then disable the mod in DMM and apply.
Delete bin64\GreymaneCompanions.ini and the bin64\GreymaneCompanions folder.
