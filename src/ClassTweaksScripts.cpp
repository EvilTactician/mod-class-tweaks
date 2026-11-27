/*
 * mod-class-tweaks: config, per-player state, hooks and the chat command.
 *
 * One script object per hook type, each registered only for the hooks it uses, so the core
 * never calls this module for anything else. Every hook rejects the common case (wrong opcode,
 * not a player, wrong class, a playerbot, wrong spell family) before doing any work.
 */

#include "ClassTweaks.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Opcodes.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "WorldPacket.h"
#include <algorithm>

namespace ClassTweaks
{
    Config config;

    PlayerState* GetState(Player* player)
    {
        // Static key: DataMap takes a std::string, and a literal this long would be heap
        // allocated on every call.
        static std::string const key = "mod-class-tweaks";
        return player->CustomData.GetDefault<PlayerState>(key);
    }

    void SendAddonMessage(Player* player, std::string const& text)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, ADDON_PREFIX + text);
        player->SendDirectMessage(&data);
    }
}

using namespace ClassTweaks;
using namespace Acore::ChatCommands;

namespace
{
    // Multipliers below 1 aren't supported (they'd turn the additive bonuses negative).
    float Multiplier(char const* name, float defaultValue)
    {
        return std::max(1.0f, sConfigMgr->GetOption<float>(name, defaultValue));
    }
}

class ClassTweaksWorldScript : public WorldScript
{
public:
    ClassTweaksWorldScript() : WorldScript("ClassTweaksWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        // Rogue
        config.comboEnabled            = sConfigMgr->GetOption<bool>("ClassTweaks.Rogue.ComboPoints.Enable", true);
        config.comboKeepAfterKill      = sConfigMgr->GetOption<uint32>("ClassTweaks.Rogue.ComboPoints.KeepAfterKill", 20000);

        config.lockpickRightClick      = sConfigMgr->GetOption<bool>("ClassTweaks.Rogue.Lockpick.RightClick", true);
        config.lockpickOpenAfterUnlock = sConfigMgr->GetOption<bool>("ClassTweaks.Rogue.Lockpick.OpenAfterUnlock", true);

        // 0 or 60 = stock: leave the core's duration alone.
        uint32 const poisonMinutes     = sConfigMgr->GetOption<uint32>("ClassTweaks.Rogue.Poison.DurationMinutes", 60);
        config.poisonDurationMs        = (poisonMinutes && poisonMinutes != 60) ? poisonMinutes * MINUTE * IN_MILLISECONDS : 0;
        config.poisonAutoApply         = sConfigMgr->GetOption<bool>("ClassTweaks.Rogue.Poison.AutoApply", true);
        config.poisonAutoApplyOutside  = sConfigMgr->GetOption<bool>("ClassTweaks.Rogue.Poison.AutoApplyOutside", false);
        config.poisonAllowOutside      = sConfigMgr->GetOption<bool>("ClassTweaks.Rogue.Poison.AllowOutside", true);
        config.poisonNotify            = sConfigMgr->GetOption<bool>("ClassTweaks.Rogue.Poison.Notify", true);
        config.poisonAPMultiplier      = Multiplier("ClassTweaks.Rogue.Poison.APMultiplier", 1.15f);

        config.findWeaknessBypass      = sConfigMgr->GetOption<bool>("ClassTweaks.Rogue.FindWeakness.PoisonBypass", true);
        config.findWeaknessNotify      = sConfigMgr->GetOption<bool>("ClassTweaks.Rogue.FindWeakness.Notify", true);

        config.ruptureMultiplier       = Multiplier("ClassTweaks.Rogue.Rupture.DamageMultiplier", 1.5f);

        // Priest
        config.smiteHolyFireMultiplier     = Multiplier("ClassTweaks.Priest.SmiteHolyFire.DamageMultiplier", 1.25f);
        config.holyConcentrationAoE        = sConfigMgr->GetOption<bool>("ClassTweaks.Priest.HolyConcentration.AoEProcs", true);
        config.holyAoESpellPowerMultiplier = Multiplier("ClassTweaks.Priest.HolyAoE.SpellPowerMultiplier", 1.12f);

        // All classes
        config.talentResetEnabled          = sConfigMgr->GetOption<bool>("ClassTweaks.TalentReset.Enable", true);
    }

    // Item, spell and spell data are loaded; the world isn't running yet.
    void OnStartup() override
    {
        PoisonBuildTables();
        RogueBalanceOnStartup();
        PriestBalanceOnStartup();
    }
};

class ClassTweaksServerScript : public ServerScript
{
public:
    ClassTweaksServerScript() : ServerScript("ClassTweaksServerScript", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

    // Both opcodes are handled with the player's map (or world session) update, where nothing
    // else touches the player.
    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        uint16 const opcode = packet.GetOpcode();
        if (opcode != CMSG_SET_SELECTION && opcode != CMSG_OPEN_ITEM)
            return true;

        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld() || !IsRealClass(player, CLASS_ROGUE))
            return true;

        if (opcode == CMSG_SET_SELECTION)
        {
            if (config.comboEnabled && packet.size() >= 8)
                ComboOnSelect(player, ObjectGuid(packet.read<uint64>(0)));
            return true;
        }

        if (config.lockpickRightClick)
            return LockpickOnOpenItem(player, packet);

        return true;
    }
};

class ClassTweaksPlayerScript : public PlayerScript
{
public:
    ClassTweaksPlayerScript() : PlayerScript("ClassTweaksPlayerScript",
        { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_SPELL_CAST, PLAYERHOOK_CAN_APPLY_ENCHANTMENT, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        if (!IsRealClass(player, CLASS_ROGUE))
            return;

        PlayerState* state = GetState(player);

        if (state->hasPendingPoison)
            PoisonOnUpdate(player, state);

        if (!config.comboEnabled)
            return;

        if (!player->IsAlive())
        {
            state->lastPoints = 0;
            state->lastTarget.Clear();
            state->savedPoints = 0;
            return;
        }

        ComboOnUpdate(player, state);
    }

    // Before the spell's effects.
    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (config.comboEnabled && IsRealClass(player, CLASS_ROGUE))
            ComboOnSpellCast(player, spell);
    }

    // The core calls this with apply = false and apply_dur = false when a temporary enchant
    // runs out, while it is still on the item. Never blocks anything.
    bool OnPlayerCanApplyEnchantment(Player* player, Item* item, EnchantmentSlot slot, bool apply, bool apply_dur, bool /*ignore_condition*/) override
    {
        if (apply || apply_dur || slot != TEMP_ENCHANTMENT_SLOT || !item || !config.poisonAutoApply)
            return true;

        if (IsRealClass(player, CLASS_ROGUE))
            PoisonOnEnchantRemoved(player, item);

        return true;
    }

    // Addon messages whispered to oneself ("ClassTweaks\t..."): answered and swallowed.
    // "TALENTS ..." is the talent reset (any class); everything else is the rogue poison options.
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
    {
        if (language != LANG_ADDON || type != CHAT_MSG_WHISPER || receiver != player)
            return true;

        if (TalentResetOnAddonMessage(player, msg))
            return false;

        return !PoisonOnAddonMessage(player, msg);
    }
};

class ClassTweaksSpellScript : public AllSpellScript
{
public:
    ClassTweaksSpellScript() : AllSpellScript("ClassTweaksSpellScript",
        { ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_CAST_CANCEL, ALLSPELLHOOK_ON_SPELL_CHECK_CAST }) { }

    // After the cast's effects (instant and cast-time spells; poisons and Pick Lock are both).
    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || !IsRealPlayer(player))
            return;

        // Any class: a skeleton key (blacksmiths) also opens the box right away.
        if (config.lockpickOpenAfterUnlock || config.lockpickRightClick)
            LockpickOnSpellCast(player, spell, spellInfo);

        if (player->getClass() == CLASS_ROGUE)
            PoisonOnSpellCast(player, spell, spellInfo);
    }

    void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/, bool /*bySelf*/) override
    {
        if (Player* player = AsRealClass(caster, CLASS_ROGUE))
            LockpickOnSpellCastCancel(player, spell);
    }

    // Before every cast. Only rogue spells go any further.
    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& res) override
    {
        SpellInfo const* spellInfo = spell->GetSpellInfo();
        if (spellInfo->SpellFamilyName == SPELLFAMILY_ROGUE)
            RogueOnSpellCheckCast(spell, spellInfo, res);
    }
};

class ClassTweaksUnitScript : public UnitScript
{
public:
    ClassTweaksUnitScript() : UnitScript("ClassTweaksUnitScript", true,
        { UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN }) { }

    // Direct spell damage, after the target's class mitigation.
    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (!attacker || !spellInfo)
            return;

        switch (spellInfo->SpellFamilyName)
        {
            case SPELLFAMILY_ROGUE:  RogueOnSpellDamage(target, attacker, damage, spellInfo); break;
            case SPELLFAMILY_PRIEST: PriestOnSpellDamage(target, attacker, damage, spellInfo); break;
            default: break;
        }
    }

    // Each damage-over-time tick, before the target's mitigation.
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (!attacker || !spellInfo)
            return;

        switch (spellInfo->SpellFamilyName)
        {
            case SPELLFAMILY_ROGUE:  RogueOnPeriodicDamage(target, attacker, damage, spellInfo); break;
            case SPELLFAMILY_PRIEST: PriestOnPeriodicDamage(target, attacker, damage, spellInfo); break;
            default: break;
        }
    }
};

// .classtweaks poison                     show this character's poison auto-apply settings
// .classtweaks poison auto on|off         auto-apply in dungeons and raids
// .classtweaks poison outside on|off      also auto-apply outside dungeons and raids
class ClassTweaksCommandScript : public CommandScript
{
public:
    ClassTweaksCommandScript() : CommandScript("ClassTweaksCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable classTweaksTable =
        {
            { "poison", HandlePoisonCommand, SEC_PLAYER, Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "classtweaks", classTweaksTable }
        };

        return commandTable;
    }

    static bool HandlePoisonCommand(ChatHandler* handler, Optional<std::string> option, Optional<std::string> value)
    {
        Player* player = handler->GetPlayer();
        if (!player || player->getClass() != CLASS_ROGUE || !config.poisonAutoApply)
        {
            handler->SendSysMessage("Poison auto-apply is only available to rogues on this realm.");
            return true;
        }

        if (option && value)
        {
            std::string const opt = *option;
            std::string const val = *value;
            bool const on = (val == "on" || val == "1");
            bool const off = (val == "off" || val == "0");

            if ((opt == "auto" || opt == "outside") && (on || off))
            {
                if (opt == "auto")
                    PoisonSetAutoApply(player, on);
                else if (config.poisonAllowOutside)
                    PoisonSetAutoApplyOutside(player, on);
                else
                    handler->SendSysMessage("Auto-apply outside dungeons and raids is disabled on this realm.");

                PoisonSendState(player);  // keeps an addon's checkboxes in step
            }
            else
            {
                handler->SendSysMessage("Usage: .classtweaks poison [auto|outside] [on|off]");
                return true;
            }
        }

        handler->PSendSysMessage("Poison auto-apply in dungeons and raids: {}. Outside dungeons and raids: {}.",
            PoisonAutoApplyOn(player) ? "on" : "off",
            !config.poisonAllowOutside ? "disabled on this realm" : (PoisonAutoApplyOutsideOn(player) ? "on" : "off"));
        return true;
    }
};

void AddClassTweaksScripts()
{
    new ClassTweaksWorldScript();
    new ClassTweaksServerScript();
    new ClassTweaksPlayerScript();
    new ClassTweaksSpellScript();
    new ClassTweaksUnitScript();
    new ClassTweaksCommandScript();
    AddPriestSpellScripts();
}
