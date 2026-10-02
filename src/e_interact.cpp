/*
 * mod-e-interact: server half of the EInteract addon (one key to loot, gather and talk).
 *
 * The 3.3.5 client cannot look for units around the player and InteractUnit is protected,
 * so the addon only sends an addon whisper to itself and the server does the work:
 *   "EINT\thello" -> reply "EINT\tready" (lets the addon know the module is installed)
 *   "EINT\tnear"  -> use the current target, or the nearest corpse / NPC in interaction range:
 *                    loot it (see LootCorpse), start skinning/herbing/mining it with CastSpell,
 *                    or queue the packet the client would send on right click
 *                    (CMSG_GOSSIP_HELLO, CMSG_LIST_INVENTORY, ...).
 * Every reply is an addon whisper "EINT\t<result>": loot, gather, talk, none.
 */

#include "Chat.h"
#include "ConfigValueCache.h"
#include "Creature.h"
#include "Group.h"
#include "Log.h"
#include "LootMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
constexpr char const* kPrefix = "EINT\t";
constexpr char const* kHello = "EINT\thello";
constexpr char const* kNear = "EINT\tnear";

enum class EInteractConfig
{
    ENABLED,
    LOOT,
    GATHER,
    TALK,

    NUM_CONFIGS,
};

class EInteractConfigData : public ConfigValueCache<EInteractConfig>
{
public:
    EInteractConfigData() : ConfigValueCache(EInteractConfig::NUM_CONFIGS) { }

    void BuildConfigCache() override
    {
        SetConfigValue<bool>(EInteractConfig::ENABLED, "EInteract.Enable", true);
        SetConfigValue<bool>(EInteractConfig::LOOT, "EInteract.Loot", true);
        SetConfigValue<bool>(EInteractConfig::GATHER, "EInteract.Gather", true);
        SetConfigValue<bool>(EInteractConfig::TALK, "EInteract.Talk", true);
    }
};

EInteractConfigData config;

bool Enabled(EInteractConfig option)
{
    return config.GetConfigValue<bool>(option);
}

bool CanLootCorpse(Player* player, Creature* c)
{
    if (c->IsAlive() || !c->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE))
        return false;

    if (player->isAllowedToLoot(c))
        return true;

    // isAllowedToLoot misses corpses that only have money left
    if (c->loot.gold == 0)
        return false;

    if (Group* group = player->GetGroup())
        return group == c->GetLootRecipientGroup();

    return c->GetLootRecipient() == player;
}

bool CanTalkTo(Player* player, Creature* c)
{
    uint32 const flags = uint32(c->GetNpcFlags()) & ~uint32(UNIT_NPC_FLAG_SPELLCLICK | UNIT_NPC_FLAG_PLAYER_VEHICLE);
    return flags != 0 && player->GetNPCIfCanInteractWith(c->GetGUID(), UNIT_NPC_FLAG_NONE) == c;
}

Opcodes TalkOpcode(Creature* c)
{
    if (c->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
        return CMSG_GOSSIP_HELLO;
    if (c->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
        return CMSG_QUESTGIVER_HELLO;
    if (c->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
        return CMSG_LIST_INVENTORY;
    if (c->HasNpcFlag(UNIT_NPC_FLAG_TRAINER))
        return CMSG_TRAINER_LIST;
    if (c->HasNpcFlag(UNIT_NPC_FLAG_BANKER))
        return CMSG_BANKER_ACTIVATE;
    if (c->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER))
        return MSG_AUCTION_HELLO;
    return CMSG_GOSSIP_HELLO; // flight master, innkeeper and the rest are handled by PrepareGossipMenu
}

// Result for the addon: drives its autoloot, its messages and /ei debug
void Reply(Player* player, std::string const& what)
{
    LOG_DEBUG("module", "EInteract {}: {}", player->GetName(), what);
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, kPrefix + what);
    player->SendDirectMessage(&data);
}

// Gathering spell for a corpse: skinning, herbalism, mining, engineering (all SPELL_EFFECT_SKINNING)
uint32 GatherSpell(Player* player, Creature* c)
{
    if (c->IsAlive() || !c->HasUnitFlag(UNIT_FLAG_SKINNABLE))
        return 0;
    if (!c->IsCritter() && !c->loot.isLooted())
        return 0;

    SkillType const skill = c->GetCreatureTemplate()->GetRequiredLootSkill();
    int32 const skillValue = player->GetSkillValue(skill);
    if (skillValue <= 0)
        return 0;

    // same formula as Spell::CheckCast, so we do not get stuck on a corpse above our skill
    int32 const level = c->GetLevel();
    if ((skillValue < 100 ? (level - 10) * 10 : level * 5) > skillValue)
        return 0;

    switch (skill)
    {
        case SKILL_HERBALISM:   return 32605;
        case SKILL_MINING:      return 32606;
        case SKILL_ENGINEERING: return 49383;
        default:
            for (uint32 id : { 50305u, 32678u, 10768u, 8618u, 8617u })
                if (player->HasSpell(id))
                    return id;
            return 8613;
    }
}

void Queue(Player* player, Opcodes opcode, ObjectGuid guid)
{
    WorldPacket* packet = new WorldPacket(opcode, 8);
    *packet << guid;
    player->GetSession()->QueuePacket(packet);
}

WorldPacket GuidPacket(Opcodes opcode, ObjectGuid guid)
{
    WorldPacket packet(opcode, 8);
    packet << guid;
    return packet;
}

bool CanTakeSlot(Player* player, Loot* loot, uint8 slot)
{
    QuestItem* qitem = nullptr;
    QuestItem* ffaitem = nullptr;
    QuestItem* conditem = nullptr;
    LootItem* item = loot->LootItemInSlot(slot, player, &qitem, &ffaitem, &conditem);
    if (!item)
        return false;

    bool const looted = qitem ? qitem->is_looted : ffaitem ? ffaitem->is_looted : conditem ? conditem->is_looted : item->is_looted;
    if (looted || !item->AllowedForPlayer(player, loot->sourceWorldObjectGUID))
        return false;

    // leave items that are being rolled for, under master loot or won by someone else
    if (!qitem && item->is_blocked)
        return false;
    if (item->rollWinnerGUID && item->rollWinnerGUID != player->GetGUID())
        return false;
    if (Group* group = player->GetGroup())
        if (!item->is_underthreshold && loot->roundRobinPlayer && group->GetLootMethod() == MASTER_LOOT
            && group->GetMasterLooterGuid() != player->GetGUID() && !qitem && !ffaitem && !conditem)
            return false;

    return true;
}

// The 3.3.5 client silently ignores a corpse loot window it did not request itself, so the server
// walks the whole chain for it: CMSG_LOOT -> LOOT_MONEY -> AUTOSTORE for every slot -> LOOT_RELEASE.
// CMSG_LOOT goes through the CanPacketReceive hooks first, so mod-aoe-loot can merge nearby corpses.
void LootCorpse(Player* player, Creature* c)
{
    WorldSession* session = player->GetSession();
    ObjectGuid const guid = c->GetGUID();

    WorldPacket open = GuidPacket(CMSG_LOOT, guid);
    if (sScriptMgr->CanPacketReceive(session, open))
        session->HandleLootOpcode(open);

    if (player->GetLootGUID() != guid)
        return;

    Loot* loot = &c->loot;
    if (loot->gold)
    {
        WorldPacket money(CMSG_LOOT_MONEY, 0);
        session->HandleLootMoneyOpcode(money);
    }

    std::size_t const slots = loot->items.size() + loot->quest_items.size();
    for (std::size_t slot = 0; slot < slots && slot < 255; ++slot)
    {
        if (!CanTakeSlot(player, loot, uint8(slot)))
            continue;

        WorldPacket take(CMSG_AUTOSTORE_LOOT_ITEM, 1);
        take << uint8(slot);
        session->HandleAutostoreLootItemOpcode(take);
    }

    // whatever is left (full bags, rolls) stays on the corpse and can be picked up with another press
    if (player->GetLootGUID() == guid)
    {
        WorldPacket release = GuidPacket(CMSG_LOOT_RELEASE, guid);
        session->HandleLootReleaseOpcode(release);
    }
}

bool TryInteract(Player* player, Creature* c)
{
    if (!c || !player->IsWithinDistInMap(c, INTERACTION_DISTANCE) || !player->CanSeeOrDetect(c))
        return false;

    if (Enabled(EInteractConfig::LOOT) && CanLootCorpse(player, c))
    {
        Reply(player, "loot");
        LootCorpse(player, c);
        return true;
    }

    if (Enabled(EInteractConfig::GATHER))
    {
        if (uint32 spell = GatherSpell(player, c))
        {
            if (player->IsNonMeleeSpellCast(false))
                return true; // already gathering
            Reply(player, "gather");
            player->CastSpell(c, spell, false);
            return true;
        }
    }

    if (Enabled(EInteractConfig::TALK) && c->IsAlive() && CanTalkTo(player, c))
    {
        Reply(player, "talk");
        Queue(player, TalkOpcode(c), c->GetGUID());
        return true;
    }

    return false;
}

void HandleNear(Player* player)
{
    if (!player->IsAlive() || player->IsInFlight())
        return;

    if (Unit* sel = player->GetSelectedUnit())
        if (TryInteract(player, sel->ToCreature()))
            return;

    std::list<Creature*> nearby;
    player->GetDeadCreatureListInGrid(nearby, INTERACTION_DISTANCE + 5.0f, false);
    nearby.sort([player](Creature* a, Creature* b) { return player->GetDistance(a) < player->GetDistance(b); });

    for (Creature* c : nearby)
        if (!c->IsAlive())
            LOG_DEBUG("module", "EInteract {}: corpse {} '{}' dist={:.1f} lootable={} allowed={} looted={} gold={} items={} lootType={} recipient={} group={} skinnable={}",
                player->GetName(), c->GetEntry(), c->GetName(), player->GetDistance(c),
                c->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE), player->isAllowedToLoot(c), c->loot.isLooted(),
                c->loot.gold, c->loot.items.size(), uint32(c->loot.loot_type),
                c->GetLootRecipient() ? c->GetLootRecipient()->GetName() : "-",
                c->GetLootRecipientGroup() ? 1 : 0, c->HasUnitFlag(UNIT_FLAG_SKINNABLE));

    // corpses first, then NPCs
    for (Creature* c : nearby)
        if (!c->IsAlive() && TryInteract(player, c))
            return;
    for (Creature* c : nearby)
        if (c->IsAlive() && TryInteract(player, c))
            return;

    Reply(player, "none");
}
} // namespace

class EInteractPlayerScript : public PlayerScript
{
public:
    EInteractPlayerScript() : PlayerScript("EInteractPlayerScript", { PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
    {
        if (lang != LANG_ADDON || type != CHAT_MSG_WHISPER || receiver != player || !Enabled(EInteractConfig::ENABLED))
            return true;

        if (msg == kHello)
            Reply(player, "ready");
        else if (msg == kNear)
            HandleNear(player);
        else
            return true;

        return false;
    }
};

class EInteractWorldScript : public WorldScript
{
public:
    EInteractWorldScript() : WorldScript("EInteractWorldScript", { WORLDHOOK_ON_BEFORE_CONFIG_LOAD }) { }

    void OnBeforeConfigLoad(bool reload) override
    {
        config.Initialize(reload);
    }
};

void AddSC_e_interact()
{
    new EInteractPlayerScript();
    new EInteractWorldScript();
}
