# mod-e-interact

*[Русская версия](README_RU.md)*

Play WotLK 3.3.5a with one interact key. Press **E** and the game does the obvious thing:

| Situation | What E does | Needs this module |
|---|---|---|
| Corpse with loot nearby | loots it: money and every item you may take go straight to your bags | yes |
| Looted corpse you can skin, herb, mine or salvage | starts gathering it | yes |
| NPC nearby | opens its gossip, quest, vendor, trainer, bank or auction window | yes |
| Quest dialog is open | next page → objectives/rewards → accept or turn in | no |
| Group loot roll is open | rolls Need (Greed when Need is not allowed), confirms bind-on-pickup | no |

Your current target goes first. Without one, the nearest corpse is used, then the nearest NPC.
No more clicking on corpses that are hard to see.

The project has two parts:

* **the server module** (this repository) — finds the corpse or NPC and does the looting, gathering
  or talking, because the 3.3.5 client cannot look for units around the player and
  `InteractUnit` is protected;
* **the client addon** [`addon/EInteract`](addon/EInteract) — binds the key and talks to the module.
  Quest dialogs and loot rolls are handled by the addon alone, so it is useful on any server.
  On a server without the module it says so once and keeps those two features.

Quest dialogs work with the standard quest frame and with the
[Storyline](https://www.curseforge.com/wow/addons/storyline) addon.

## Server installation

1. Clone the module into the `modules` directory of your AzerothCore source:

   ```sh
   cd azerothcore-wotlk/modules
   git clone https://github.com/rainblower/mod-e-interact.git
   ```

2. Re-run CMake and rebuild the worldserver (with Docker: `docker compose build`).
3. Copy `mod_e_interact.conf.dist` to `mod_e_interact.conf` in your module config directory
   if you want to change the defaults. No SQL is needed.

[How to install AzerothCore modules](https://www.azerothcore.org/wiki/installing-a-module)

## Configuration

| Option | Default | Description |
|---|---|---|
| `EInteract.Enable` | 1 | answer requests from the addon |
| `EInteract.Loot` | 1 | loot corpses |
| `EInteract.Gather` | 1 | skin, herb, mine and salvage corpses |
| `EInteract.Talk` | 1 | open NPC windows |

The options can be changed at runtime with `.reload config`.

## Addon installation (every player)

Copy the `addon/EInteract` folder to `World of Warcraft/Interface/AddOns/` and enable it on the
character selection screen. The key is **E** by default; it overrides the normal binding only
while the addon is enabled.

Commands:

```
/ei                     help and current state (including whether the server module was found)
/ei on | off            enable or disable the key
/ei key <key>           use another key, e.g. /ei key F
/ei loot on|off         autoloot
/ei dialog on|off       E in quest dialogs
/ei roll on|off         E in group loot rolls
/ei debug               print server replies
```

## How it works

The addon sends an addon whisper `EINT\tnear` to the player themselves. The module catches it
in `OnPlayerCanUseChat` (so it never reaches the chat), picks the target and:

* **loot** — the 3.3.5 client silently ignores a corpse loot window it did not request itself,
  so the server performs the whole chain on its behalf: `CMSG_LOOT` → `CMSG_LOOT_MONEY` →
  `CMSG_AUTOSTORE_LOOT_ITEM` for every slot the player may take → `CMSG_LOOT_RELEASE`.
  Items under a group roll, master loot or won by another player stay on the corpse.
  `CMSG_LOOT` goes through the `CanPacketReceive` hooks first, so
  [mod-aoe-loot](https://github.com/azerothcore/mod-aoe-loot) keeps working;
* **gather** — casts the skinning/herbalism/mining/engineering spell the client would cast,
  after the same skill check as `Spell::CheckCast`;
* **talk** — queues the same packet a right click would send (`CMSG_GOSSIP_HELLO`,
  `CMSG_QUESTGIVER_HELLO`, `CMSG_LIST_INVENTORY`, ...), so all the normal checks apply.

All actions use the normal interaction distance and visibility checks. The module answers with
`EINT\t<result>`; on login the addon sends `EINT\thello` and waits for `EINT\tready` to find out
whether the module is installed.

Debug logging: set `Logger.module=5,Console Server` in `worldserver.conf` and press E.

## Requirements

* AzerothCore master (uses `ConfigValueCache` and the hook-list `PlayerScript` constructor)
* WoW client 3.3.5a (12340)

## License

[MIT](LICENSE)
