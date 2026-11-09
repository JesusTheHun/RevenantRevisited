// *************************************************************************
// *                         Cinematix Revenant                            *
// *                    Copyright (C) 1998 Cinematix                       *
// *                 character.cpp - TCharacter module                     *
// *************************************************************************

#include "character.h"
#include "ctrlmap.h"

#include "rules.h"
#include "mappane.h"
#include "display.h"
#include "spell.h"
#include "sound.h"
#include "playscreen.h"
#include "gameoptions.h"
#include "multi.h"
#include "animation.h"
#include "dialog.h"
#include "effect.h"
#include "textbar.h"
#include "3dimage.h"
#include "inventory.h"
#include "exit.h"
#include "script.h"
#include "effect.h"
#include "charanimator.h"
#include "weapon.h"
#include "ammo.h"
#include "player.h"
#include "logging.h"
#include "combattrace.h"

#include <algorithm>
#include <cstdlib>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <vector>

#define GRIDSNAPSIZE    48

REGISTER_BUILDER(TCharacter)
TObjectClass CharacterClass("CHARACTER", OBJCLASS_CHARACTER, 0);

extern TObjectClass EffectClass;
extern TObjectClass AmmoClass;

// Hard coded character class stats
DEFSTAT(Character, Radius,          RAD,  0, 16, 16, 256)

// Hard coded character object stats
DEFOBJSTAT(Character, Aggressive,   AGGR, CHRFLAG_FIRST + CHRFLAG_AGGRESSIVE, 1, 0, 1)
DEFOBJSTAT(Character, Poisoned,     PSND, CHRFLAG_FIRST + CHRFLAG_POISONED, 0, 0, 1)
DEFOBJSTAT(Character, Sleeping,     SLP,  CHRFLAG_FIRST + CHRFLAG_SLEEPING, 0, 0, 1)

DEFOBJSTAT(Character, Health,       HLT,  CHRSTAT_FIRST + CHRSTAT_HEALTH, 25, 0, 10000)
DEFOBJSTAT(Character, Fatigue,      FAT,  CHRSTAT_FIRST + CHRSTAT_FATIGUE, 25, 0, 10000)
DEFOBJSTAT(Character, Mana,         MAN,  CHRSTAT_FIRST + CHRSTAT_MANA, 25, 0, 10000)

// REVSYNC: the shipped game's CHARACTER object stats 6-16 (SStatEntry
// registrations; recon/scripts/object_stats.py).
DEFOBJSTAT(Character, DmgResMisc,     DRMI, CHRRESIST_FIRST + CHRRESIST_MISC,     0, -100, 100)
DEFOBJSTAT(Character, DmgResHand,     DRHA, CHRRESIST_FIRST + CHRRESIST_HAND,     0, -100, 100)
DEFOBJSTAT(Character, DmgResPuncture, DRPU, CHRRESIST_FIRST + CHRRESIST_PUNCTURE, 0, -100, 100)
DEFOBJSTAT(Character, DmgResCut,      DRCU, CHRRESIST_FIRST + CHRRESIST_CUT,      0, -100, 100)
DEFOBJSTAT(Character, DmgResChop,     DRCH, CHRRESIST_FIRST + CHRRESIST_CHOP,     0, -100, 100)
DEFOBJSTAT(Character, DmgResBludgeon, DRBL, CHRRESIST_FIRST + CHRRESIST_BLUDGEON, 0, -100, 100)
DEFOBJSTAT(Character, DmgResMagical,  DRMA, CHRRESIST_FIRST + CHRRESIST_MAGICAL,  0, -100, 100)
DEFOBJSTAT(Character, DmgResBurn,     DRBU, CHRRESIST_FIRST + CHRRESIST_BURN,     0, -100, 100)
DEFOBJSTAT(Character, DmgResFreeze,   DRFR, CHRRESIST_FIRST + CHRRESIST_FREEZE,   0, -100, 100)
DEFOBJSTAT(Character, DmgResPoison,   DRPO, CHRRESIST_FIRST + CHRRESIST_POISON,   0, -100, 100)
DEFOBJSTAT(Character, DamageMod,      DMGM, CHRVAL_DAMAGEMOD,                     0, -100, 100)

extern TDialogPane DialogPane;

// Some character defines
#define IMPACT_THRESHOLD    4   // how much damage has to be done before they enter impact state
#define MAXENEMYRANGE       512 // Maximum range of enemy
#define MAXTURNRATE         8   // Maximum turn rate
#define MAXCHAINHITS        3   // Maximum number of times player can hit chain btn in row

// Maximum Z hit distance
#define MAXZHITRANGE        40  // Maximum Z hit range

// Returns a turn rate value based on how far character is turning
#define MAKETURNRATE(diff) (MAXTURNRATE + max(0, (diff) - 32) / 32 * (MAXTURNRATE / 2))

// How long a character remembers one it saw or heard (HasSeenMe): retail
// 0x438, 45 seconds (the 1998 source had ten).
constexpr int32_t kMaxSeenTime = FRAMERATE * 45;

namespace
{
// The characters within `range` of pos (a square) on `level`, in map order,
// until `visit` returns false: retail's map iterator 0x0044ceb0 / 0x0044d080
// as CharBlocking and FindCharacters make it (characters, flags 0xe0: no
// inventories, the map rectangle, the loaded sectors -- what the level
// constructor sets), or the A/B fixture's.
template <class Visit>
void ForCharactersNear(int32_t level, const S3DPoint& pos, int32_t range, Visit&& visit)
{
    if (TCharacter::nearbyCharactersSeam)
    {
        for (TCharacter* c : TCharacter::nearbyCharactersSeam(pos, range))
            if (!visit(c))
                return;
        return;
    }
    SRect r{pos.x - range, pos.y - range, pos.x + range, pos.y + range};
    for (TMapIterator i(level, &r, CHECK_NOINVENT, OBJSET_CHARACTER); i; i++)
        if (!visit(static_cast<TCharacter*>(i.Item())))
            return;
}

// REVSYNC: WaypointReachable @ 0x00528850 (a helper's method, the
// character its argument) -- the line of sight from the waypoint, at the
// character's height, to the character, over the waypoint's level.
bool WaypointReachable(const TObjectInstance* waypoint, const TObjectInstance* who)
{
    const S3DPoint from(waypoint->Pos().x, waypoint->Pos().y, who->Pos().z);
    return MapPane.LineOfSight(from, who->Pos(), waypoint->GetLevel());
}

// Retail's three moving actions (walk, combat, bow steps).
bool IsMoveAction(const TActionBlock* ab)
{
    return ab && (ab->action == ACTION_MOVE || ab->action == ACTION_COMBATMOVE || ab->action == ACTION_BOWMOVE);
}

// The turn rate retail gives a step or pivot for an angle `diff` (0..128)
// still to turn: 8 up to 45 degrees, +4 per further 45.
int32_t StepTurnRate(int32_t diff)
{
    return (std::max)(0, diff - 32) / 32 * 4 + 8;
}

// The TOHITDAMAGE row a to-hit margin (to-hit less the roll) falls in, as
// IsValidAttack (0x004d18d2) and ResolveHit (0x004c64e0) look it up: the
// margin held to the first and last rows' keys, then the first row it
// reaches. Its damage factor is (100 + DamagePercent); false when no row is
// reached, which only keys out of order allow.
bool ToHitDamageFactor(int32_t margin, int32_t& factor)
{
    const auto& table = Rules.tohitdamage;
    if (margin >= table.front().minvalue)
        margin = table.front().minvalue;
    if (margin <= table.back().minvalue)
        margin = table.back().minvalue;
    for (const TRules::SToHitDamage& row : table)
        if (margin >= row.minvalue)
        {
            factor = row.damagepercent + 100;
            return true;
        }
    return false;
}

// REVSYNC: 0x0046e7d0 / 0x0046e7f0 -- a name as the text bar shows it: its
// letters and digits read as a dialog tag, the name itself on a miss; at
// most 99 characters.
std::string ShownName(const char* name)
{
    constexpr size_t kLength = 100;
    if (!name)
        return {};
    std::string tag;
    for (const char* c = name; *c && tag.size() < kLength - 2; ++c)
        if (isalnum((unsigned char)*c))
            tag.push_back(*c);
    const char* line = DialogList.GetLine(tag.c_str());
    std::string shown = (line && line[0] != '[') ? line : name;
    if (shown.size() > kLength - 1)
        shown.resize(kLength - 1);
    return shown;
}

// Where a character's combat target stands, as Damage filters its
// CHARIMPACTs (0x004c4c78): the absolute bearing to it, not turned by the
// character's facing, in three quarters about 0x80; none for 0xe0-0x1f.
int32_t BearingFilter(int32_t bearing)
{
    if (bearing >= 0x20 && bearing < 0x60)
        return CAI_TURNPLUS90;
    if (bearing >= 0x60 && bearing < 0xa0)
        return CAI_TURN180;
    if (bearing >= 0xa0 && bearing < 0xe0)
        return CAI_TURNMINUS90;
    return 0;
}

// The angle a death or impact block takes: the bearing to whoever it
// answers, turned as its impact's flags say (0x004c4eaf, 0x004c53c0).
int32_t ImpactTurn(int32_t bearing, int32_t impflags)
{
    if (impflags & CAI_TURN180)
        return (bearing - 0x80) & 0xff;
    if (impflags & CAI_TURNPLUS90)
        return (bearing + 0x40) & 0xff;
    if (impflags & CAI_TURNMINUS90)
        return (bearing - 0x40) & 0xff;
    return bearing;
}

// The slot after an impact list's first `used`, where retail's searches end
// when nothing matches: a zeroed record while the list has room.
SCharAttackImpact* SlotAfter(SCharAttackImpact* list, int32_t used, int32_t capacity)
{
  // REVSYNC-DIVERGENCE: after a full list retail reads past it; an empty
  // impact stands in.
    static SCharAttackImpact none{};
    return used < capacity ? &list[used] : &none;
}

// Retail's 32-bit product, wrapped as its imul wraps it: the recovery and
// poison arithmetic overflows after a long absence (CHARACTER_PULSE.md §2.2).
constexpr int32_t Wrap32(int64_t value)
{
    return static_cast<int32_t>(static_cast<uint32_t>(value));
}

// What `elapsed` hundredths of a second add towards a vital's next point,
// in 1/10000 points: `value` points every `rate` (0x004c2b71).
int32_t Recovery(int32_t elapsed, int32_t value, int32_t rate)
{
    return Wrap32(int64_t(elapsed) * value * 10000) / rate;
}

// REVSYNC: 0x004c1d27 -- blood from a body: a "blood" effect `height` above
// its feet, sprayed toward `hangle` (its SetParams, slot 0x200).
void SprayBlood(const S3DPoint& feet, int32_t height, int32_t hangle, int32_t vangle, int32_t hspread)
{
    SObjectDef def{};
    def.objclass = OBJCLASS_EFFECT;
    def.objtype = EffectClass.FindObjType("blood");
    def.level = MapPane.GetMapLevel();
    def.pos = feet;
    def.pos.z += height;
    if (auto* blood = dynamic_cast<TBloodEffect*>(MapPane.GetInstance(MapPane.NewObject(&def))))
        blood->SetParams(height, hangle & 0xff, vangle, hspread, 5, 1);
}

// The whole points an accumulator holds: 10000 comes off while more than
// 10000 is left (so exactly 10000 waits for the next tick).
int32_t TakePoints(int32_t& accum)
{
    int32_t points = 0;
    while (accum > 10000)
    {
        accum -= 10000;
        ++points;
    }
    return points;
}
}

// *************************************************************
// *                          Character                        *
// *************************************************************

void TCharacter::ClearChar()
{
    flags |= OF_MOVING | OF_PULSE | OF_ANIMATE | OF_AI; // Is moving object, pulse, animate, and has AI

    autocombat = AutoBeginCombat;
    
    forcecommanddone = false;
    forcenomove = false;
    is_invisible = false;
    magic_resistance = 0.0f;

    // When character loaded or created, make exit timestamp current..
    // Prevents ONEXIT flag from expiring when character is saved on top of a
    // destination exit. The game session can build the world before the
    // PlayScreen runs; its frame count starts at 0 when it does, which is
    // what retail's load inside TPlayScreen::Initialize saw.
    exittimestamp = (CurrentScreen == &PlayScreen) ? PlayScreen.FrameCount() : 0;

  // Set root state
  // NONE: DefaultRootState() will NOT be virtual when ClearChar()
  // is called in the constructor!
    if (!root)
        root = doing = desired = new TActionBlock(DefaultRootState() /* SEE ABOVE NOTE */);
    else
        strcpy(root->name, DefaultRootState());

    int32_t newstate = FindState(root->name);
    if (newstate < 0)
    {
        strcpy(root->name, "walk");
        newstate = FindState(root->name);
    }
    if (newstate < 0)
    {
        strcpy(root->name, "combat");
        newstate = FindState(root->name);
    }
    if (newstate < 0)
        newstate = 0;
    state = prevstate = newstate;

  // Set character data pointer
    chardata = Rules.GetCharData(objtype, objclass);

  // Clear moveto values
    bool movetopos = false;
    S3DPoint movepos;
    movepos.x = movepos.y = movepos.z = 0;

  // Set Burn to nullptr
    ClearBurn();
    burnticks = 0;

  // invoke delay
    invokedelay = 0;

  // combatflash
    combatflashticks = 0;

  // Set initial health/fatigue/mana values. If rules are missing chardata
  // (e.g. partial rules.ini parse), skip the stat init rather than deref null.
    if (chardata)
    {
        SetHealth(MaxHealth());
        SetFatigue(MaxFatigue());
        SetMana(MaxMana());
    }

  // Reset AI data. REVSYNC: ClearChar 0x004c18a0 starts both attack timers
  // at 1 (the AI's first attack tick counts them down to 0).
    nextattack = 1;
    magictimer = 1;
    chainhits = 0;
    shovedir = -1;                // Block go around direction choice
    glimpse = noise = -1;         // Reset glimpse and noise values
    snapticks = -1;               // No snapping
    lastbowshot = 0;              // Reset bow shot time so we can shoot bow immediately
  // REVSYNC: 0x004c1ab8 -- no boss kind yet (AI_PerMonster looks again),
  // no boss state, no materials saved.
    monsterkind = 0;
    bossstate = std::monostate{};
    savedmaterials.clear();

  // Reset hasseen list
    memset(&hasseen, 0, sizeof(SHasSeen) * MAXHASSEEN);

  // Clear Fade (retail ClearChar 0x004c18a0): fully visible. The step of 5
  // toward a limit of 100 settles to still on the first pulse.
    fade = 100;
    fade_step = 5;
    fade_limit = 100;
    fade_direction = 0;

  // Clear the Invisible Spell
    invisible_spell = false;

  // Clear the poison
    SetPoisoned(false);

  // Clear Teleport Coordinates
    teleport_position.x = -1;
    teleport_position.y = -1;
    teleport_position.z = -1;
    teleport_level = -1;

  // REVSYNC: 0x004c1a5d -- the recovery clocks start now, nothing gathered
  // (the poison stamp and accumulator are Pulse's to set).
    lasthealthrecov = lastfatiguerecov = lastmanarecov = lastrunfatigue = Clock();
    healthaccum = fatigueaccum = manaaccum = runaccum = 0;

  // Attach the animator component up front. Characters carry the
  // animator for the lifetime of the object (see IsAnimatorPermanent
  // override) -- no lazy create/free during sector load/unload or
  // state change. ClearChar runs from both TCharacter ctors and from
  // explicit re-init paths (e.g. spawn-time Aggressive reset). The
  // imagery-side builder lookup uses GetClassName() which is keyed
  // off the cl pointer set during TObjectInstance construction, so
  // even ctor-time invocation routes to the correct subclass animator
  // (TCharAnimator / TPlayerAnimator).
    if (imagery && !HasAnimator())
        CreateAnimator();
}

// REVSYNC: TCharacter::Pulse @ 0x004c1bb0 -- a character's tick after its
// move (forensics/CHARACTER_PULSE.md §2): the spells, the animator and the
// action state; the blood of the scripted deaths and the impale holds; Died
// told once. A corpse plays its death and in time fades out and goes. The
// living chain their attacks, the player falls into combat with an enemy
// ahead, the AI runs, the vitals recover, poison bites, and the invoke,
// burn and flail actions end on time.
void TCharacter::Pulse()
{
    ai_pulse_count++;

  // The spells first: the manager's cooldown and each spell's timers and
  // effects run on the game tick (TSpellManager::Pulse 0x00540750, whose
  // only caller this is). Retail skips the whole tick while PlayScreen
  // +0x5d4 (0x0065d0c4) is set; that flag isn't identified in the port yet.
  // (docs/gameplay/forensics/SPELLS_MISSILES.md)
    SpellManager.Pulse();

  // The animator's tick: every character's, paralysed or iced too (the
  // rising damage numbers count down in it).
    if (TObjectAnimator* anim = GetAnimator())
        anim->Pulse();

  // A command forced to be done: the doing block stops waiting.
    if (forcecommanddone)
    {
        if (doing)
            doing->wait = 0;
        commanddone = true;
        forcecommanddone = false;
    }

    if (combatflashticks > 0)
        combatflashticks--;

  // The action state's tick (UpdateAction with the move's bits).
    TComplexObject::Pulse();

  // The one we fight hears of it (slot 0x23c, empty in retail). The 1998
  // build also put the player's target's name and health on the text bar
  // here; retail dropped it (TTextBar::SetHealthDisplay 0x0054ca20 has one
  // caller, the map loader's progress bar; the target's health is
  // TPlyrStatusBar's).
    if (TCharacter* target = Fighting())
        target->SignalHostility(this, target);

    // Check to see if exit flag has expired (exit flags are set by exit objects)
    // Exits search through the player list to tag a player that is on an exit.  Characters
    // then check the time stamp and clear themselves once off every strip for a while.  This
    // system prevents the old reflective exit bug, where an exit takes a character to another
    // exit, which then takes him back to the first... etc. etc. as the character can only
    // activate an exit when he was previously not already on one.
    // REVSYNC: 0x004c1c7d -- retail waits more than 5 frames (1998: 2). Retail
    // also forgets the exit it stood on after 24; nothing reads that, so the
    // port doesn't keep it (EXITS.md §1.6).
    if (CurrentScreen->FrameCount() - exittimestamp > 5)
        SetFlag(OF_ONEXIT, false);

  // REVSYNC: 0x004c1cc3 -- blood from the scripted deaths by animation
  // frame, from a character that bleeds; Rand's "walk to dead" (his alone,
  // bleeder or not) too. The 1998 build sprayed for "impdecap" instead.
    const bool bleeds = !(chardata->flags & CF_BADBLEEDER) && chardata->bleeder;
    if (doing->Is("decapdeath") && bleeds && frame > 5 && frame < 65 && frame % 5 == 0)
        SprayBlood(pos, 75, GetFace() - 0x5b, 0x40, 0x5b);
    if (doing->Is("combat to ripinhalfed") && bleeds && frame > 65 && frame < 73 && frame % 2 == 0)
        SprayBlood(pos, 105, GetFace() - 0x5b, 0x40, 0x5b);
    if (doing->Is("ripkilldeath") && bleeds && frame > 115 && frame < 126)
    {
        SprayBlood(pos, 130, GetFace() - 0x5b, 0x40, 0x5b);
        SprayBlood(pos, 130, GetFace() - 0x5b, 0x40, 0x5b);
    }
    if (stricmp(GetName(), "Rand") == 0 && doing->Is("walk to dead") && frame % 5 == 0)
        SprayBlood(pos, 40, GetFace() - 0x80, 0x20, 0x20);

  // REVSYNC: 0x004c1fa3 -- the impale holds. An impaling attacker lands no
  // killing blow until its "atkimpale to walk" (the player's hold ends with
  // the impale itself); Yhagoro's "impimpale" lets go of the player, who
  // walks on at his next tick.
    if (doing->Is("atkimpale to walk") || (doing->Is("atkimpale") && ObjClass() == OBJCLASS_PLAYER))
        charflags &= ~kCharFlagNoKill;
    if (monsterkind == kMonsterYhagoro && doing->Is("impimpale") && Player)
        static_cast<TCharacter*>(Player)->charflags |= kCharFlagImpaleRelease;

  // One step of any fade, dead or alive (UpdateFade's only caller in retail).
    UpdateFade();

  // REVSYNC: 0x004c20f8 -- Died is told once, with no killer (Damage tells
  // it the killer); a corpse can't stay paralysed. When health comes back
  // the mark goes (retail then calls slot 0x24c, empty in every class).
    if (Health() <= 0)
    {
        if (!(charflags & kCharFlagDead))
        {
            charflags |= kCharFlagDead;
            Died(nullptr);
        }
        ClearFlag(OF_PARALIZE);
    }
    else if (charflags & kCharFlagDead)
        charflags &= ~kCharFlagDead;

    if (Health() <= 0 && !(charflags & kCharFlagUnkillable))
    {
        PulseCorpse();
        return;
    }

    // ****************** all processing below is for the living (and the unkillable) *********

    // Is the character doing an autocombo or chain attack?
    if (
        lastattack &&
        PlayScreen.GameFrame() >= lastattackticks + lastattack->nextwait &&
        ((lastattack->flags & CA_AUTOCOMBO) ||  // Is autocombo or..
         ((lastattack->flags & CA_CHAIN) && chainhits > 0)) // Is chain attack..
        )
    {
        bool foundchain = false;

      // Are we in time (note: AUTO's don't expire like chains)
        if (!(lastattack->flags & CA_CHAIN) ||  // We're a chain
            (PlayScreen.GameFrame() - lastattackticks < lastattack->chainexptime)) // We haven't expired
        {
          // Get attack whose chainname matches last attack name
            for (int32_t c = 0; c < chardata->attacks.NumItems(); c++)
            {
                SCharAttackData* ad = &(chardata->attacks[c]);

                if (!stricmp(lastattack->attackname, ad->chainname))
                {
                    if (lastattack->flags & CA_CHAIN)
                        chainhits--; // Decrease key hits for attack
                    SpecificAttack(c); // Do the next attack boyz
                    foundchain = true;
                    break;
                }
            }
        }

      // Couldn't find another chain or auto so clear chainhits
        if (!foundchain)
            chainhits = 0;
    }

    if (ObjClass() == OBJCLASS_PLAYER)
        PulseAutoCombat();

    if (charflags & kCharFlagImpaleRelease)
        PulseImpaleRelease();

  // REVSYNC: 0x004c29a2 -- one AI-driven (charflags 0x100000) wanders, then
  // runs the AI, whatever its flags; any other runs the AI with OF_AI.
    if (charflags & kCharFlagPlayerAI)
    {
        Wander();
        AI();
    }
    else if (flags & OF_AI)
        AI();

    PulseRecovery();
    PulseActionTimers();
}

// REVSYNC: 0x004c2187 -- a dead character's tick. Until the death plays:
// the transition to it from the root ("<root> to <prefix>dead"), when there
// is one, then the dead loop (the prefixed "dead", else the plain one) as
// the new root. The script hears of it every tick and the corpse stops
// fidgeting. After 240 ticks it fades out over 100 more, and then it goes.
// First, as long as it carries anything, a body that isn't a player's drops
// it round itself (DropLoot, 0x004c21e4; docs/gameflow/forensics/DEATH_LOOT.md).
void TCharacter::PulseCorpse()
{
    if (ObjClass() != OBJCLASS_PLAYER && NumInventoryItems() > 0)
        DropLoot();

    if (doing->action != ACTION_DEAD)
    {
        doing->priority = doing->interrupt = false;
        char name[RESNAMELEN * 2];
        snprintf(name, sizeof(name), "%s to %sdead", root->name,
                 ObjClass() == OBJCLASS_PLAYER ? AnimPrefix() : "c");
        if (HasActionAni(name))
        {
            auto* fall = new TActionBlock(name, ACTION_DEAD);
            fall->priority = fall->interrupt = fall->loop = true;
            if (ForceCommand(fall, 0, 0) != COM_EXECUTING)
                DropUnheld(fall);
        }
        CombatAnimName(name, "dead");
        if (!HasActionAni(name))
            strncpyz(name, "dead", sizeof(name));
        auto* dead = new TActionBlock(name, ACTION_DEAD);
        if (doing->action == ACTION_DEAD)
            SetDesired(dead, 0);
        else
            ForceCommand(dead, 0, 0);
        SetRoot(dead);
    }

    if (script)
    {
        script->Trigger(TRIGGER_DEAD);
        SetIncidentals(false);
    }

  // (In a network game a player's corpse stays.)
    if (++deadticks >= 240)
    {
        if (fade <= 0)
            SetFlag(OF_KILL);
        else if (fade_direction == 0)
        {
            fade = 100;
            fade_step = 1;
            fade_limit = 0;
            fade_direction = -1;
        }
    }
}

// REVSYNC: 0x004c25d5 -- the player falls into combat on his own, every 32
// ticks (staggered by his id) when not running or sneaking: with the
// nearest enemy within 0x20 of his facing, a valid target he hasn't walked
// away from (the memory's no-autocombat mark), closer than his COMBATRANGE
// and within 0x30 of where he faces, while the AutoBeginCombat option and
// his own switch are on and he has control. Finding no one turns his
// switch back on. (The 1998 build looked every 24 ticks, from any root,
// within 32 of his facing.)
void TCharacter::PulseAutoCombat()
{
    if (IsRunMode() || IsSneakMode() || ((PlayScreen.GameFrame() ^ GetMapIndex()) & 0x1f) != 0)
        return;
    TCharacter* target = nullptr;
    if (FindCharacters(&target, 1, -1, GetFace(), 0x20, 7) <= 0 || !target)
    {
        autocombat = true;
        return;
    }
    if (!IsValidTarget(target) || IsCombat() || IsBowMode() || IsSneakMode())
        return;
  // REVSYNC-DIVERGENCE: retail takes another player only when the game type
  // allows player killing (a network game's); single player has no other.
    if (target->ObjClass() == OBJCLASS_PLAYER)
        return;
    if (!AutoBeginCombat || !autocombat || !HasSeenAutoCombat(target))
        return;
    if (Distance(target) >= chardata->combatrangemin || std::abs(AngleDiff(GetFace(), AngleTo(target))) >= 0x30)
        return;
    if (PlayScreen.IsControlOn() || PlayScreen.IsDemoMode())
        BeginFighting(target, ACTION_COMBAT);
}

// REVSYNC: 0x004c2867 -- let go by Yhagoro's impale: unless already
// walking, the hold is dropped -- the root and the desired block lose their
// priority, the walk is forced (unless a doing block with priority still
// waits on another) and wanted next, facing as he faces.
void TCharacter::PulseImpaleRelease()
{
    if (!(desired && desired->Is("walk") && root->Is("walk")))
    {
        root->priority = false;
        if (!(doing->priority && desired != doing))
        {
            auto* walk = new TActionBlock("walk", ACTION_ANIMATE);
            walk->angle = GetFace();
          // REVSYNC-DIVERGENCE: retail leaks the block when it is refused.
            if (ForceCommand(walk, 0, 0) != COM_EXECUTING)
                DropUnheld(walk);
        }
        desired->priority = false;
        auto* walk = new TActionBlock("walk", ACTION_ANIMATE);
        walk->angle = GetFace();
        if (!SetDesired(walk, 0))
            DropUnheld(walk);
    }
    charflags &= ~kCharFlagImpaleRelease;
}

// REVSYNC: 0x004c29d7 -- recovery on the character's clock (CHARACTER_PULSE.md
// §2.1). A vital below its maximum gathers `value` points every `rate`
// hundredths (the player adds his per-minute percentages), in 1/10000s, and
// the whole points go on; running costs fatigue instead (RUNFATIGUE).
// Poison bites through Damage, short of the last point, which cures it. A
// stamp in the future or more than 50000 off is pulled to now first. Under
// the alreadydead cheat the player is simply kept full. (The 1998 build
// gave whole percentages of the maximum each period and poisoned straight
// off the health.)
void TCharacter::PulseRecovery()
{
    const int32_t now = Clock();
    for (int32_t* stamp : {&lasthealthrecov, &lastfatiguerecov, &lastmanarecov, &lastrunfatigue, &lastpoisondamage})
        if (*stamp > now || std::abs(int64_t(now) - *stamp) > 50000)
            *stamp = now;

    if (CheatAlreadyDead && ObjClass() == OBJCLASS_PLAYER)
    {
        if (Health() < MaxHealth())
            SetHealth(MaxHealth());
        if (Mana() < MaxMana())
            SetMana(MaxMana());
        if (Fatigue() < MaxFatigue())
            SetFatigue(MaxFatigue());
        SetPoisoned(false);
        return;
    }

    if (Health() < MaxHealth())
    {
        healthaccum += Recovery(now - lasthealthrecov, Rules.healthrecovval, Rules.healthrecovrate);
        healthaccum += RecoveryBonus(EVital::Health);
        if (const int32_t points = TakePoints(healthaccum); points > 0)
            SetHealth((std::min)(Health() + points, MaxHealth()));
    }
    lasthealthrecov = now;

    if (Mana() < MaxMana())
    {
        manaaccum += Recovery(now - lastmanarecov, Rules.manarecovval, Rules.manarecovrate);
        manaaccum += RecoveryBonus(EVital::Mana);
        if (const int32_t points = TakePoints(manaaccum); points > 0)
            SetMana((std::min)(Mana() + points, MaxMana()));
    }
    lastmanarecov = now;

    if (IsRunMode() && doing != root)
    {
        if (lastrunfatigue != -1 && Fatigue() > 0)
        {
            runaccum += Recovery(now - lastrunfatigue, chardata->runfatigue[0], chardata->runfatigue[1]);
            if (const int32_t points = TakePoints(runaccum); points > 0)
                SetFatigue((std::max)(Fatigue() - points, 0));
        }
        lastrunfatigue = now;
    }
    else
    {
        lastrunfatigue = -1;
        if (Fatigue() < MaxFatigue())
        {
            fatigueaccum += Recovery(now - lastfatiguerecov, Rules.fatiguerecovval, Rules.fatiguerecovrate);
            fatigueaccum += RecoveryBonus(EVital::Fatigue);
            if (const int32_t points = TakePoints(fatigueaccum); points > 0)
                SetFatigue((std::min)(Fatigue() + points, MaxFatigue()));
        }
    }
    lastfatiguerecov = now;

    if (Poisoned())
    {
        if (lastpoisondamage != -1)
        {
            if (Health() > 1)
            {
                const int32_t bite = Wrap32(int64_t(now - lastpoisondamage) * Rules.poisondamageval * 240000)
                                   / (Rules.poisondamagerate * 100);
                poisonaccum += Wrap32(int64_t(100 - DamageModifier(DT_POISON)) * bite) / 100;
                int32_t points = TakePoints(poisonaccum);
                if (points >= Health())
                    points = Health() - 1;
                if (points > 0)
                    Damage(points, DT_POISON, 0, nullptr, nullptr);
            }
            else
                SetPoisoned(false);
        }
        lastpoisondamage = now;
    }
    else
        lastpoisondamage = -1;

    if (doing->action == ACTION_INVOKE && invokedelay > 0 && --invokedelay == 0)
    {
        root->interrupt = true;
        SetDesired(root);
    }
}

// REVSYNC: 0x004c3131 -- a burn runs 50 ticks (Burn's timer); a flail its
// animation, and ends back in combat.
void TCharacter::PulseActionTimers()
{
    if (doing->action == ACTION_BURN && burnticks > 0 && --burnticks == 0)
    {
        root->interrupt = true;
        SetDesired(root);
    }
    if (doing->action == ACTION_FLAIL && commanddone)
    {
        root->interrupt = true;
        SetDesired(root);
        BeginFighting(nullptr, ACTION_COMBAT);
    }
}

// REVSYNC: slot 0x314 @ 0x004d5f50 -- the PlayScreen's game time.
int32_t TCharacter::Clock()
{
    return PlayScreen.GameTime();
}

// REVSYNC: TCharacter::UpdateAction @ 0x004c3260 -- one tick of the
// action state machine, after the last Move (`bits`). The doing block's
// resolver; with no opinion, done when the animation is (or there is no
// animator, or the character is invisible), and always done for the root
// unless it is mid-transition; a done block loses its priority. Then the
// script hears whether it's done. Falling: the "fall" animation, unless the
// doing block has priority. Otherwise the desired block is tried.
void TCharacter::UpdateAction(int32_t bits)
{
  // ResetStealthValues @ 0x004cdbb0 off the 24-frame beat, and on it for a
  // finished non-root action or a negative glimpse / noise.
    if (PlayScreen.GameFrame() % 24 != 0 || (commanddone && root != doing) || glimpse < 0 || noise < 0)
        ResetStealthValues();

    if (root->action != ACTION_SLEEP && Sleeping())
        SetSleeping(0);

    int32_t comstate = ResolveAction(bits);
    if (comstate == COM_DONE)
    {
        comstate = (commanddone || !HasAnimator() || (flags & OF_INVISIBLE)) ? COM_DONE : COM_EXECUTING;
        if (doing == root && !doing->transition)
            comstate = COM_DONE;
        if (comstate == COM_DONE)
            doing->priority = false;
    }
    doing->firsttime = false;

  // REVSYNC: 0x004c3371 -- run the script, telling it whether the current
  // action is done. Waits belong to the script (SCRIPT_ENGINE.md §5).
    ContinueScript(comstate == COM_DONE);

    if (bits & MOVE_FALLING)
    {
        if (HasActionAni("fall") && !doing->priority)
        {
            auto* fall = new TActionBlock("fall", ACTION_MOVE);
            if (ForceCommand(fall, 0, 0) != COM_EXECUTING && fall != root && fall != doing && fall != desired)
                delete fall;
        }
    }
  // REVSYNC: 0x004c3429 -- with incidentals off the next state is always
  // the 100% variant (TryCommand's flag).
    else
        TryCommand(desired, bits, Incidentals() ? 0 : kCommandNoIncidentals);

    if (doing && doing->wait > 0)
        doing->wait--;
}

// REVSYNC: TCharacter::ResolveAction @ 0x004c3490 -- the doing block's
// resolver by action: walking (and a running root's steps) to ResolveMove,
// then attack, impact / knockdown / stun, block, combat steps, the combat
// root, leap, bow aim and shot, say, pivot, dead.
int32_t TCharacter::ResolveAction(int32_t bits)
{
    int32_t comstate = TComplexObject::ResolveAction(bits);
    if (comstate != COM_DONE)
        return comstate;

    if (doing->action == ACTION_MOVE || (IsRunMode() && IsMoving()))
        return ResolveMove(doing, bits);
    switch (doing->action)
    {
      case ACTION_ATTACK: return ResolveAttack(doing, bits);
      case ACTION_IMPACT:
      case ACTION_KNOCKDOWN:
      case ACTION_STUN: return ResolveImpact(doing, bits);
      case ACTION_BLOCK: return ResolveBlock(doing, bits);
      case ACTION_COMBATMOVE:
      case ACTION_BOWMOVE: return ResolveCombatMove(doing, bits);
      case ACTION_COMBAT:
      case ACTION_BOW: return ResolveCombat(doing, bits);
      case ACTION_COMBATLEAP: return ResolveLeap(doing, bits);
      case ACTION_BOWAIM: return ResolveBowAim(doing, bits);
      case ACTION_BOWSHOOT: return ResolveBowShoot(doing, bits);
      case ACTION_SAY: return ResolveSay(doing, bits);
      // REVSYNC-DIVERGENCE: retail never dispatches ACTION_PULL (its
      // ResolvePull @ 0x004c83f0 returns 0) and pulls levers elsewhere, not
      // yet found; the port's lever pull stays here until that is.
      case ACTION_PULL: return ResolvePull(doing, bits);
      case ACTION_PIVOT: return ResolvePivot(doing, bits);
      case ACTION_DEAD: return ResolveDead(doing, bits);
      default: return COM_DONE;
    }
}

void TCharacter::Animate(bool draw)
{
    TObjectInstance::Animate(draw);

    if (HasAnimator() && draw && doing &&
        doing->action == ACTION_SAY &&
        doing->data &&
        doing->wait > 12) // Leave a half a second between sentences
    {
        SRect r;
        Display.GetClipRect(r);

        SColor color = { 0, 150, 255 };

        if (this == (TCharacter*)Player)
        {
            color.red = 200;
            color.green = 0;
            color.blue = 0;
        }

        int32_t x, y;
        WorldToScreen(pos, x, y);

        PlayScreen.AddPostCharText((char *)doing->data, x, y - 120, &color, r.right - r.left + 1 - 32);
    }
}

void TCharacter::Notify(int32_t notify, void *ptr)
{
    // **** WARNING!!! MAKE SURE YOU CHECK FOR BROKEN LINKS AND DELETED OBJECTS HERE!!! ****
    // If you want to be notified, you must call SetNotify() in your contsructor

    TComplexObject::Notify(notify, ptr);

    if (Fighting() && (notify == N_DELETINGOBJECT || notify == N_DELETINGSECTOR))
    {
        if (NOTIFY_DELETED(ptr, Fighting()))
            SetFighting(nullptr);
    }

  // REVSYNC: TCharacter::Notify @ 0x004c36c0 -- every notice reaches the
  // spells (0x004c3965: TSpellManager::ObjectRemoved, which also runs each
  // spell's Timer).
    SpellManager.ObjectRemoved(notify, ptr);

  // Check hasseen list for deleted objects
    for (int32_t c = 0; c < MAXHASSEEN; c++)
    {
        
        if (hasseen[c].chr && NOTIFY_DELETED(ptr, hasseen[c].chr))
        {
            hasseen[c].chr = nullptr;
            hasseen[c].time = 0;
            hasseen[c].noautocombat = false;
        }
    }
}

namespace
{
constexpr int32_t kMaxStepHeight = 0x20;        // a step up or down a mover takes
constexpr int32_t kMaxMoveSteps = 10;           // MoveSteps toward a MoveTo target
constexpr int32_t kStepSpan = 8;                // units a substep may cover
constexpr int32_t kStepCorrection = 100;        // the substep nudge, in 1/0x10000 units

// x * y in 32 bits as retail's imul wraps, then / 0x10000 toward zero.
int32_t FixedTimes(int32_t x, int32_t y)
{
    return (int32_t)((uint32_t)x * (uint32_t)y) / ROLLOVER;
}
}  // namespace

// REVSYNC: FindClearPath @ 0x004c39d0 -- is the step from pos to newpos
// blocked: by the ground (a hole, a step of more than 0x20 between pos and
// the ground at newpos, or between two walk cells within the Radius of it,
// or no walkmap), or by a character standing there -- unless this one is
// dead, in an interactive attack, flying, invisible or on a MoveTo, or the
// two already overlap (so they can part). `height` gets the ground at
// newpos, `bychar` the character in the way. MOVE_NOTMOVING asks for the
// ground under newpos alone.
bool TCharacter::Blocked(S3DPoint &pos, S3DPoint &newpos, uint32_t bits, int32_t *height, TCharacter* *bychar)
{
    if (blockedSeam)
        return blockedSeam(this, pos, newpos, bits, bychar);

    int32_t h;
    if (!height)
        height = &h;
    TCharacter* by;
    if (!bychar)
        bychar = &by;
    *bychar = nullptr;
    *height = pos.z;
    blockedby = EBlockedBy::None;

    int32_t maxdelta;
    if (bits & MOVE_NOTMOVING)
    {
        *height = MapPane.GetWalkHeight(newpos);
        maxdelta = 0;
    }
    else
    {
        bool hole;
        MapPane.GetWalkHeightRadius(newpos, Radius(), maxdelta, *height, hole);
        if (hole)
        {
            blockedby = EBlockedBy::Hole;
            return true;
        }
    }
    if (abs(pos.z - *height) > kMaxStepHeight || maxdelta > kMaxStepHeight || *height == 0)
    {
        blockedby = abs(pos.z - *height) > kMaxStepHeight ? EBlockedBy::Height
                  : maxdelta > kMaxStepHeight            ? EBlockedBy::Step
                                                         : EBlockedBy::NoWalkmap;
        return true;
    }

    if (Health() <= 0)
        return false;
    if (doing->attack && (doing->attack->flags & CA_INTERACTIVE))
        return false;
    if (GetAniFlags() & AF_FLY)
        return false;
    if (flags & OF_INVISIBLE)
        return false;
    if (movetopos)
        return false;
    // (A remote player in a network game is never blocked; offline, nothing.)
    TCharacter* b = CharBlocking(this, newpos, Radius());
    *bychar = b;
    if (!b || ::Distance(pos, b->Pos()) < b->Radius() + Radius())
        return false;
    blockedby = EBlockedBy::Character;
    return true;
}

namespace
{
const char* BlockedByName(TCharacter::EBlockedBy why)
{
    switch (why)
    {
      case TCharacter::EBlockedBy::Hole: return "a cell with no walkmap in reach";
      case TCharacter::EBlockedBy::Height: return "the ground there more than 0x20 above or below";
      case TCharacter::EBlockedBy::Step: return "a step over 0x20 between cells in reach";
      case TCharacter::EBlockedBy::NoWalkmap: return "no walkmap there";
      case TCharacter::EBlockedBy::Character: return "a character";
      default: return "nothing";
    }
}
}  // namespace

// REVSYNC: TCharacter::Move @ 0x004c46d0 -- MoveStep, repeated (at most ten
// times) while a MoveTo target is still ahead and the last step moved; the
// MoveTo is spent either way. The network bookkeeping in between is inert
// offline.
uint32_t TCharacter::Move()
{
    uint32_t bits;
    int32_t steps = kMaxMoveSteps;
    do
    {
        bits = MoveStep();
        --steps;
    } while (movetopos && pos != movepos && (bits & MOVE_MOVED) && steps > 0);
    movetopos = false;
    return bits;
}

// REVSYNC: TCharacter::MoveStep @ 0x004c3bc0 -- one tick's displacement.
// The ground first: more than 16 below it snaps up at once, more than 16
// above it drops to it at once (MOVE_FALLING, vel.z gathering gravity).
// Then the move: toward a MoveTo target, or the walk speed of the root (all
// -1 in the shipped char.def, so never), or the animation's motion
// (GetNextMove), plus vel and the fraction carried in accum, in 1/0x10000
// units. Split into substeps of at most 8 units (nudged so they land on the
// target exactly), each one checked by FindClearPath: blocked, the
// character shoves sideways (+-0x20, then +-0x40 of the move angle, 2..6
// units, keeping the side it picked) unless a character stands in its way
// in combat; still blocked, it stays put. Every substep reports MOVED.
uint32_t TCharacter::MoveStep()
{
    if ((flags & OF_IMMOBILE) || inventnum >= 0 || (flags & OF_PARALIZE))
        return MOVE_NOTHING;

    uint32_t r = 0;
    const int32_t ground = MapPane.GetWalkHeight(pos);
    const int32_t above = pos.z - ground;
    if (above < -16)
        ForcePos(S3DPoint(pos.x, pos.y, ground));       // retail writes pos.z, no SetPos
    else if (above > 16)
    {
        vel.z = (std::max)(vel.z - GRAVITY, -TERMINAL_VELOCITY);
        SetPos(S3DPoint(pos.x, pos.y, ground));
        r = MOVE_FALLING;
    }
    if (above < 1)
        vel.z = 0;

    if (forcenomove)
    {
        forcenomove = false;
        return MOVE_NOTMOVING;
    }

    S3DPoint total;                                     // this tick's move, 1/0x10000 units
    if (movetopos)
    {
        total = S3DPoint((movepos.x - pos.x) * ROLLOVER, (movepos.y - pos.y) * ROLLOVER,
                         (movepos.z - pos.z) * ROLLOVER);
        accum = S3DPoint();
        if (total.x == 0 && total.y == 0 && total.z == 0)
            return r | MOVE_NOTMOVING;
        shovedir = -1;
    }
    else
    {
        S3DPoint next;
        const bool waitpivot = doing && doing->waitpivot;
        if (chardata->combatwalkspeed > 0 && IsDoing(ACTION_COMBATMOVE) && !waitpivot)
            ConvertToVector(moveangle, chardata->combatwalkspeed * ROLLOVER, next);
        // REVSYNC-DIVERGENCE: retail asks the class for its walk root (slot
        // 0x30c): "walk", or for the player "torch" with a light in hand
        // (+0x2b8, slot not yet confirmed). Only reached with a positive
        // walk speed, which the shipped char.def never gives.
        else if (chardata->walkspeed > 0 && IsDoing(ACTION_MOVE) && !waitpivot && root->Is("walk"))
            ConvertToVector(moveangle, chardata->walkspeed * ROLLOVER, next);
        // Retail gates sneaking on the walk speed, then moves at the sneak speed.
        else if (chardata->walkspeed > 0 && IsDoing(ACTION_MOVE) && !waitpivot && root->Is("sneak"))
            ConvertToVector(moveangle, chardata->sneakspeed * ROLLOVER, next);
        else if (chardata->runspeed > 0 && IsDoing(ACTION_MOVE) && !waitpivot && root->Is("run"))
            ConvertToVector(moveangle, chardata->runspeed * ROLLOVER, next);
        else
            GetNextMove(next);
        if (next.x == 0 && next.y == 0 && next.z == 0 && vel.x == 0 && vel.y == 0 && vel.z == 0)
        {
            accum = S3DPoint();
            shovedir = -1;
            return r | MOVE_NOTMOVING;
        }
        total = accum + next + vel;
    }

    const S3DPoint target(pos.x + total.x / ROLLOVER, pos.y + total.y / ROLLOVER, pos.z + total.z / ROLLOVER);

    // Substeps: the longer axis (x on a tie) decides how many.
    int32_t steps = 1;
    const int32_t ax = abs(total.x), ay = abs(total.y);
    constexpr int32_t kSpan = kStepSpan * ROLLOVER;
    if (ax >= ay && ax >= kSpan)
        steps = (ax + kSpan - 1) / kSpan;
    else if (ay > ax && ay >= kSpan)
        steps = (ay + kSpan - 1) / kSpan;
    S3DPoint step(total.x / steps, total.y / steps, total.z / steps);

    // Nudge each axis until the substeps land on the target exactly.
    auto land = [steps](int32_t& per, int32_t from, int32_t to) {
        while (from + FixedTimes(steps, per) < to)
            per += kStepCorrection;
        while (from + FixedTimes(steps, per) > to)
            per -= kStepCorrection;
    };
    land(step.x, pos.x, target.x);
    land(step.y, pos.y, target.y);
    land(step.z, pos.z, target.z);

    for (int32_t i = 0; i < steps; ++i)
    {
        S3DPoint m = step;
        S3DPoint np = pos;
        rollover(m.x, np.x);
        rollover(m.y, np.y);
        rollover(m.z, np.z);

        int32_t h1;
        TCharacter* by1;
        const S3DPoint wanted = np;
        EBlockedBy why = EBlockedBy::None;
        if (Blocked(pos, np, r, &h1, &by1))
        {
            why = blockedby;
            r |= MOVE_BLOCKED;
            int32_t h2;
            TCharacter* by2;
            Blocked(pos, pos, r, &h2, &by2);             // asks only who stands here
            if (by1 && by2)
                r &= ~MOVE_BLOCKED;                     // already in someone: let him through
        }
        else
        {
            shovedir = -1;
            vel = S3DPoint();
            if (np.z != h1)
                np.z += (h1 - np.z) / 2;
            // (Retail would end here with MOVE_NOTMOVING set; it never is by now.)
        }

        bool shoved = false;
        if (r & MOVE_BLOCKED)
        {
            const bool facingfoe = root && (root->action == ACTION_COMBAT || root->action == ACTION_BOW) &&
                                   root->obj && by1;
            if (!facingfoe)
            {
                constexpr int32_t kSides[4] = {-0x20, 0x20, -0x40, 0x40};
                const int32_t committed[1] = {shovedir};
                const int32_t* sides = shovedir == -1 ? kSides : committed;
                const int32_t nsides = shovedir == -1 ? 4 : 1;
                int32_t best = 0;
                S3DPoint bestpos;
                for (int32_t k = 0; k < nsides; ++k)
                    for (int32_t d = 2; d < 8; d += 2)
                    {
                        S3DPoint v;
                        ConvertToVector((sides[k] + moveangle) & 0xff, d, v);
                        const S3DPoint c = pos + v;
                        S3DPoint probe = c;
                        if (Blocked(pos, probe, 0))
                            break;
                        if (d > best)
                        {
                            best = d;
                            shovedir = sides[k];        // retail commits the side as it probes
                            bestpos = c;
                        }
                    }
                if (best > 0)
                {
                    np = bestpos;
                    if (!Blocked(pos, np, 0))
                    {
                        r &= ~MOVE_BLOCKED;
                        shoved = true;
                        m = S3DPoint();
                        accum = S3DPoint();
                        i = steps;                      // this was the last substep
                    }
                    else
                        np = pos;
                }
                else
                    r |= MOVE_BLOCKED;
            }
            if (r & MOVE_BLOCKED)
            {
                np = pos;
                // Diagnostics: the player's refused step, once per place and reason.
                static S3DPoint lastpos;
                static EBlockedBy lastwhy = EBlockedBy::None;
                if (this == Player && (pos != lastpos || why != lastwhy))
                {
                    log_debug("[move] %s blocked at (%d,%d,%d) toward (%d,%d,%d), move angle %d: %s%s%s", name,
                              pos.x, pos.y, pos.z, wanted.x, wanted.y, wanted.z, moveangle, BlockedByName(why),
                              by1 ? " " : "", by1 ? by1->name : "");
                    lastpos = pos;
                    lastwhy = why;
                }
                if (retreating)
                {
                    retreatframes = 0;
                    retreating = false;
                    retreatlatch = false;
                }
            }
        }

        accum = m;                                      // the fraction left, blocked or not
        if (np.z < MapPane.GetWalkHeight(pos))
            np.z = MapPane.GetWalkHeight(pos);          // the ground at the old position
        if (np != pos)
            SetPos(np);
        r |= MOVE_MOVED;
        if (shoved && movetopos)
            return r;
    }
    return r;
}

int32_t TCharacter::GetDamageType(int32_t weapontype, int32_t attackflags)
{
    if (weapontype == WT_HAND)
    {
        return DT_HAND;
    }
    else if (weapontype == WT_KNIFE || weapontype == WT_SWORD)
    {
        if (attackflags & CA_THRUST)
            return DT_PUNCTURE;
        else if (attackflags & CA_SLASH)
            return DT_CUT;
        else if (attackflags & CA_CHOP)
            return DT_CHOP;
    }
    else if (weapontype == WT_BLUDGEON)
    {
        if (attackflags & CA_THRUST)
            return DT_NONE;
        else if (attackflags & CA_SLASH)
            return DT_BLUDGEON;
        else if (attackflags & CA_CHOP)
            return DT_BLUDGEON;
    }
    else if (weapontype == WT_AXE)
    {
        if (attackflags & CA_THRUST)
            return DT_NONE;
        else if (attackflags & CA_SLASH)
            return DT_CHOP;
        else if (attackflags & CA_CHOP)
            return DT_CHOP;
    }
    else if (weapontype == WT_BOW)
    {
        return DT_NONE; // Bow can't hurt anybody, only arrows
    }

    return DT_NONE;
}

// REVSYNC: CalculateDamage @ 0x004c4860 -- the damage taken from `damage`
// of `damagetype` with the attacker's `modifier` percent: the modifier,
// then this character's resistance (percent off) and armour (points off),
// at least 1; then magic (6-9) halved by kCharFlagHalfMagic and nothing at
// all for Baez, physical halved by kCharFlagHalfPhysical; then a seventh
// with kCharFlagDamageSeventh unless it's freezing. No type: no damage.
int32_t TCharacter::CalculateDamage(int32_t damage, int32_t damagetype, int32_t modifier)
{
    if (damagetype == DT_NONE)
        return 0;

    const int32_t modified = (modifier + 100) * damage / 100;
    int32_t taken = (100 - DamageModifier(damagetype)) * modified / 100 - ArmorValue();
    if (taken < 1)
        taken = 1;

    if (damagetype >= DT_MAGICAL && damagetype <= DT_POISON)
    {
        if (charflags & kCharFlagHalfMagic)
            taken /= 2;
        if (monsterkind == kMonsterBaez)
            taken = 0;
    }
    else if (charflags & kCharFlagHalfPhysical)
        taken /= 2;

    if ((charflags & kCharFlagDamageSeventh) && damagetype != DT_FREEZE)
        taken /= 7;
    return taken;
}

// REVSYNC: TCharacter::Damage @ 0x004c4950 (vtable +0x228; COMBAT_HIT.md
// §3.4) -- this character takes `damage` from `attacker`: as it is with
// DT_NONE, else through CalculateDamage with the attacker's `modifier`.
// Then the block that shows it: a death or an impact, the given `action`
// (ResolveHit's hit block) when it suits, else one of this character's
// CHARIMPACTs or the stock names; then the impact's snap, and the retreat
// a low-health hit starts. Damage owns `action`: it ends as one of this
// character's blocks or is freed.
void TCharacter::Damage(int32_t damage, int32_t damagetype, int32_t modifier,
    TActionBlock* action, TCharacter* attacker)
{
    if (damageSeam)
    {
        damageSeam(this, damage, damagetype, modifier, action, attacker);
        return;
    }

  // The given block and the one Damage makes: each ends as root, doing or
  // desired, or is freed here. (Retail frees a given block it replaces at
  // once and leaves the rest to its caller, ResolveHit, which frees the
  // block unless installed.)
    struct SOwnedBlocks
    {
        TCharacter* self;
        TActionBlock* given;
        TActionBlock* made = nullptr;
        ~SOwnedBlocks()
        {
            self->DropUnheld(given);
            if (made != given)
                self->DropUnheld(made);
        }
    } owned{this, action};

    (void)Health();     // retail reads it first, for the network's damage message

    if (attacker && (attacker->flags & (OF_ICED | OF_PARALIZE)))
        return;
    if (CheatAlreadyDead && ObjClass() == OBJCLASS_PLAYER)
        return;
    if (flags & OF_INVULNERABLE)
        return;
    if (CheatNahkranoth && attacker && attacker->ObjClass() == OBJCLASS_PLAYER)
        damage = 100000;
    if (attacker && !IsEnemy(attacker))
        return;
    if (ObjClass() == OBJCLASS_PLAYER && (static_cast<TPlayer*>(this)->PlayerState() & 2))
        return;

    if (damagetype >= 0)
    {
        damage = CalculateDamage(damage, damagetype, modifier);
      // The victim's edge bonus for a player with an edged weapon (knife,
      // sword, axe): IsValidAttack's line for the attacker, here for the
      // one struck.
        if (ObjClass() == OBJCLASS_PLAYER)
        {
            const int32_t wt = WeaponType();
            if (wt > 0 && (wt <= 2 || wt == 4))
                damage = damage * (static_cast<TPlayer*>(this)->EdgeBonus() + 100) / 100;
        }
    }
    if ((charflags & kCharFlagUnkillable) && damage >= Health() - 1)
        damage = Health() - 1;

  // REVSYNC: 0x004c4af0..0x004c4be9 -- the blow's number floats up over
  // the character (its animator's rising numbers, 0x004da3b0): minus the
  // damage, at most the health it had, from 135 px above its screen point;
  // red when under a quarter of its health is left, yellow under half,
  // else white.
    if (TObjectAnimator* animator = GetAnimator())
    {
        int32_t sx = 0, sy = 0;
        GetScreenPos(sx, sy);
        const int32_t left = (std::max)(Health() - damage, 0);
        const int32_t maxhealth = MaxHealth();
        const int32_t percent = maxhealth > 0 ? left * 100 / maxhealth : 0;   // retail divides unguarded
        const uint32_t rgb = percent < 25 ? 0xff0000 : percent < 50 ? 0xffff00 : 0xffffff;
        animator->ShowNumber(-(std::min)(Health(), damage), sx, sy - 135, rgb);
    }

  // Held in an interactive move (another's), a character loses no health
  // and shows no block.
    if (!InteractiveLocked() && damage)
        TObjectInstance::Damage(damage, 0);
    if (CombatTrace::Enabled())
        CombatTrace::Event(this, "damage", "amount=%d\ttype=%d\thp=%d\tby=%s", damage, damagetype, Health(),
                           attacker && attacker->GetName() ? attacker->GetName() : "-");

    const int32_t dir = root && (root->action == ACTION_COMBAT || root->action == ACTION_BOW) && root->obj
                      ? BearingFilter(AngleTo(root->obj)) : 0;

    if (Health() <= 0 && !(charflags & kCharFlagDead))
    {
        charflags |= kCharFlagDead;
        if (CombatTrace::Enabled())
            CombatTrace::Event(this, "death", "by=%s", attacker && attacker->GetName() ? attacker->GetName() : "-");
        Died(attacker);
        if (attacker)
            attacker->Killed(this);
    }

    if (!InteractiveLocked())
    {
        SCharAttackImpact* imp = Health() <= 0 ? DamageDeath(action, owned.made, damage, dir, attacker)
                                               : DamageImpact(action, owned.made, damage, dir, attacker);
        if (imp && imp->snapdist > 0 && attacker)
        {
            snapticks = imp->snaptime;
            const int32_t away = (attacker->GetFace() - 0x80) & 0xff;
            SetRotateZ(away);
            SetMoveAngle(away);
            S3DPoint p;
            GetSnapPos(attacker, imp->snapdist, p);
            MoveTo(p);
        }
    }

    if (Health() <= chardata->retreatat && Health() >= 1)
        retreatframes = chardata->retreatfor;
}

// Damage's death (0x004c4d45): the given block when it is a death, or
// carries an interactive attack, else this character's first CHARIMPACT
// death that fits, else the stock names; faced at the attacker. `made`:
// the block made here. The impact whose snap follows.
SCharAttackImpact* TCharacter::DamageDeath(TActionBlock* given, TActionBlock*& made, int32_t damage, int32_t dir,
    TCharacter* attacker)
{
    TActionBlock* ab = given;
    SCharAttackImpact* imp = nullptr;
  // (Retail reads the given attack's flags unguarded.)
    const bool usable = given && !(given->impact && !(given->impact->flags & CAI_DEATH) &&
                                   !(given->attack && (given->attack->flags & CA_INTERACTIVE)));
    if (usable)
    {
        imp = given->impact;
        if (!imp && given->attack)
        {
          // The attack's impact of the block's name, or the slot after the last.
            SCharAttackData* ad = given->attack;
            int32_t k = 0;
            while (k < ad->numimpacts && strcmp(ad->impacts[k].impactname, given->name) != 0)
                ++k;
            imp = SlotAfter(ad->impacts, k, MAXATTACKIMPACTS);
        }
    }
    else
    {
        char name[0x44] = "";
        int32_t k = 0;
        for (; k < chardata->numimpacts; ++k)
        {
            SCharAttackImpact& ci = chardata->impacts[k];
            strncpyz(name, ci.impactname, sizeof(name));
            if (!HasActionAni(name))
                CombatAnimName(name, ci.impactname);
            if ((ci.flags & dir) != dir || !(ci.flags & CAI_DEATH))
                continue;
            if (((ci.flags & CAI_WHENSTUNNED) && doing->action != ACTION_STUN) ||
                ((ci.flags & CAI_WHENDOWN) && doing->action != ACTION_KNOCKDOWN))
                continue;
            if (!HasActionAni(name) || (ci.loopname[0] && !HasActionAni(ci.loopname)))
                continue;
            const int32_t share = DamageShare(damage);
            if (share >= ci.damagemin && share <= ci.damagemax)
                break;
        }
        imp = SlotAfter(chardata->impacts, k, MAXCHARIMPACTS);
        if (k >= chardata->numimpacts || !name[0])
        {
            char dead[0x44];
            CombatAnimName(name, "dead");
            if (!HasActionAni(name))
            {
                CombatAnimName(dead, "dead");
                snprintf(name, sizeof(name), "%s to %s", root->name, dead);
                if (!HasActionAni(name))
                {
                    snprintf(name, sizeof(name), "%s to dead", root->name);
                    if (!HasActionAni(name))
                    {
                        strncpyz(name, "dead", sizeof(name));
                        if (!HasActionAni(name))
                            name[0] = '\0';
                    }
                }
            }
        }
      // No name: the given block (if any) stands, as retail leaves it.
        if (name[0])
        {
            ab = made = new TActionBlock(name, ACTION_DEAD);
            ab->impact = imp;
        }
    }

    if (attacker)
    {
        const int32_t a = AngleTo(attacker);
        SetRotateZ(a);
        SetMoveAngle(a);
    }
    if (!ab)
        return imp;
    ab->damage = damage;
    ab->obj = doing->obj;
    ab->priority = ab->interrupt = true;
    if (!strstr(ab->name, " to "))
        ab->loop = true;
    ab->angle = ab->moveangle = doing->angle;
    if (attacker && attacker->doing)
        ab->attack = attacker->doing->attack;
    ForceDamageBlock(ab, imp);
    return imp;
}

// Damage's impact (0x004c52ab): blocking, a "blockimpact" unless the hit is
// interactive; else the given block unless its impact is a death; else
// this character's first CHARIMPACT that fits, or "impact". `made`: the
// block made here. The impact whose snap follows.
SCharAttackImpact* TCharacter::DamageImpact(TActionBlock* given, TActionBlock*& made, int32_t damage, int32_t dir,
    TCharacter* attacker)
{
    TActionBlock* ab = given;
    SCharAttackImpact* imp = nullptr;
    if (doing->action == ACTION_BLOCK)
    {
      // (Retail reads the given attack's flags unguarded.)
        const bool interactive = given && given->impact &&
            ((given->attack && (given->attack->flags & CA_INTERACTIVE)) || (given->impact->flags & CAI_INTERACTIVE));
        if (!interactive)
        {
            char name[0x44];
            CombatAnimName(name, "blockimpact");
            if (!HasActionAni(name))
            {
                strncpyz(name, "blockimpact", sizeof(name));
                if (!HasActionAni(name))
                    name[0] = '\0';
            }
            if (name[0])
            {
              // Retail's action 15 (the 1998 FLYBACK): played once, back to the root.
                ab = made = new TActionBlock(name, ACTION_FLYBACK);
                ab->priority = true;
                ab->impact = nullptr;
                ab->angle = GetFace();
            }
        }
    }
    else if (given && !(given->impact && (given->impact->flags & CAI_DEATH)))
        imp = given->impact;
    else
    {
        const char* name = "impact";
        ACTION act = ACTION_IMPACT;
        int32_t k = 0;
        for (; k < chardata->numimpacts; ++k)
        {
            SCharAttackImpact& ci = chardata->impacts[k];
            const int32_t share = DamageShare(damage);
            if (share < ci.damagemin || share > ci.damagemax)
                continue;
            if ((ci.flags & dir) != dir || (ci.flags & CAI_DEATH))
                continue;
            if (((ci.flags & CAI_WHENSTUNNED) && doing->action != ACTION_STUN) ||
                ((ci.flags & CAI_WHENDOWN) && doing->action != ACTION_KNOCKDOWN))
                continue;
            if (!HasActionAni(ci.impactname) || (ci.loopname[0] && !HasActionAni(ci.loopname)))
                continue;
            imp = &ci;
            name = ci.impactname;
            act = (ci.flags & CAI_STUN) ? ACTION_STUN : (ci.flags & CAI_KNOCKDOWN) ? ACTION_KNOCKDOWN : ACTION_IMPACT;
            break;
        }
        if (imp || HasActionAni(name))
        {
            ab = made = new TActionBlock(name, act);
            ab->impact = imp;
            if (imp)
                ab->wait = imp->looptime;
        }
    }

    if (!ab)
        return imp;
    ab->damage = damage;
    ab->obj = doing->obj;
    ab->interrupt = true;
    if (attacker && attacker->doing)
        ab->attack = attacker->doing->attack;
    ForceDamageBlock(ab, imp);
    SetRotateZ(ab->angle);
    SetMoveAngle(ab->angle);
    return imp;
}

// Damage's last steps for its block: an impact that doesn't snap or keep
// the angle turns it from whoever it answers; then the root (for the
// "combat to" transition) and the block, forced past any priority.
void TCharacter::ForceDamageBlock(TActionBlock* ab, const SCharAttackImpact* imp)
{
    if (doing->obj && imp && imp->snapdist == 0 && !(imp->flags & CAI_KEEPANGLE))
        ab->angle = ab->moveangle = ImpactTurn(AngleTo(doing->obj), imp->flags);
    doing->priority = false;
    if (desired)        // (retail's is never empty)
        desired->priority = false;
    ForceCommand(root);
    ForceCommand(ab);
}

// The share of what's left that `damage` is, capped at 100: damage·100 /
// max(1, max(Health(), damage)), Health() after the damage, read as
// retail's code reads it. CHARIMPACT's damage range is in these terms.
int32_t TCharacter::DamageShare(int32_t damage)
{
    const int32_t most = Health() > damage ? Health() : damage;
    const int32_t denom = most < 1 ? 1 : (Health() > damage ? Health() : damage);
    return damage * 100 / denom;
}

void TCharacter::RestoreHealth()
{
    SetPoisoned(false);
    SetHealth(MaxHealth());
}

// REVSYNC: TCharacter::GetFieldText = retail 0x004d5260 (vtable +0xc8). The
// stat sheet's character figures, over this character's spells
// (SpellManager, retail +0x170: GetDefense 0x005407d0, GetOffense
// 0x00540820):
//   armor       ArmorValue; the main player adds his DmgResMisc and his
//               spells' defense
//   attackpct   the player: his spells' offense + LuckMod + Offense (Offense
//               counts the spells again). Anyone else: his Offense + LuckMod
//               less the player's Defense, LuckMod and DmgResMisc and --
//               retail's slip -- his own spells' defense, not the player's
//   defensepct  the player: his spells' defense + LuckMod + Defense +
//               DmgResMisc. Anyone else: his Defense + LuckMod less the
//               player's Offense, LuckMod and (the same slip) his own spells'
//               offense
//   damage      WeaponDamage scaled by DamageMod %, then for a player's knife,
//               sword or axe by EdgeBonus % (retail calls the player-only slot
//               0x3d4 on any character; the port asks only a player)
//   stealth     StealthMod
// Without a main player, attackpct and defensepct take the player's formula.
bool TCharacter::GetFieldText(const char *field, char *buf, int32_t buflen)
{
    if (!field || !buf || buflen <= 0)
        return false;

    const bool versusplayer = Player && this != Player;
    int32_t value = 0;
    if (stricmp(field, "armor") == 0)
    {
        value = ArmorValue();
        if (this == Player)
            value += Player->DmgResMisc() + SpellManager.GetDefense();
    }
    else if (stricmp(field, "maxhealth") == 0)
        value = MaxHealth();
    else if (stricmp(field, "maxfatigue") == 0)
        value = MaxFatigue();
    else if (stricmp(field, "maxmana") == 0)
        value = MaxMana();
    else if (stricmp(field, "attackpct") == 0)
        value = versusplayer ? Offense() + LuckMod() - Player->Defense() - Player->LuckMod() - Player->DmgResMisc()
                                   - SpellManager.GetDefense()
                             : SpellManager.GetOffense() + LuckMod() + Offense();
    else if (stricmp(field, "defensepct") == 0)
        value = versusplayer ? Defense() + LuckMod() - Player->Offense() - Player->LuckMod() - SpellManager.GetOffense()
                             : SpellManager.GetDefense() + LuckMod() + Defense() + DmgResMisc();
    else if (stricmp(field, "damage") == 0)
    {
        value = (DamageMod() + 100) * WeaponDamage() / 100;
        const int32_t weapon = WeaponType();
        if (ObjClass() == OBJCLASS_PLAYER && (weapon == WT_KNIFE || weapon == WT_SWORD || weapon == WT_AXE))
            value = (static_cast<TPlayer *>(this)->EdgeBonus() + 100) * value / 100;
    }
    else if (stricmp(field, "stealth") == 0)
        value = StealthMod();
    else
        return TComplexObject::GetFieldText(field, buf, buflen);

    snprintf(buf, buflen, "%d", value);
    return true;
}

// REVSYNC: HasSeenMe @ 0x004c58f0 -- 'me' is in my memory, seen or heard
// within kMaxSeenTime (FindCharacters inlines it).
bool TCharacter::HasSeenMe(TCharacter* me)
{
    for (int32_t c = 0; c < MAXHASSEEN; c++)
    {
        if (hasseen[c].chr == me &&
            PlayScreen.GameFrame() - hasseen[c].time < kMaxSeenTime)
                return true;
    }

    return false;
}

// Add me to the HASSEEN list
void TCharacter::SetHasSeen(TCharacter* me)
{
    int32_t lowest = 0x7FFFFFFF;
    int32_t lowestnum = 0;

    for (int32_t c = 0; c < MAXHASSEEN; c++)
    {
        if (hasseen[c].chr == me)
        {
            hasseen[c].time = PlayScreen.GameFrame();
            return;
        }
        if (hasseen[c].time < lowest)
        {
            lowest = hasseen[c].time;
            lowestnum = c;
        }
    }

    hasseen[lowestnum].chr = me;
    hasseen[lowestnum].time = PlayScreen.GameFrame();
    hasseen[lowestnum].noautocombat = false;    // First time seen char.. set autocombat on
}

// Returns true if character has seen 'me'
bool TCharacter::HasSeenAutoCombat(TCharacter* me)
{
    for (int32_t c = 0; c < MAXHASSEEN; c++)
    {
        if (hasseen[c].chr == me)
            return !hasseen[c].noautocombat;
    }

    return true;
}

// Returns true if character has seen 'me'
void TCharacter::SetHasSeenAutoCombat(bool on)
{
    for (int32_t c = 0; c < MAXHASSEEN; c++)
    {
        hasseen[c].noautocombat = !on;
    }
}

// REVSYNC: TCharacter::Transparency @ 0x004c5a50 -- how visible the
// character's imagery is, 0..100: the fade. (In a network game a player in
// player state 2 is capped at 40; no multiplayer in the port.) The
// pre-release hid aggressive monsters the player hadn't seen yet; retail
// dropped that.
int32_t TCharacter::Transparency()
{
    return std::clamp(fade, 0, 100);
}

// REVSYNC: Visibility @ 0x004c5aa0 -- the map's ambient light, capped at
// 255, as a percentage. (The 1998 sum of the lights' illumination and the
// ambient colour is gone.)
int32_t TCharacter::Visibility()
{
    return (std::min)(MapPane.GetAmbientLight(), 255) * 100 / 255;
}

void TCharacter::AdvanceAngles(int32_t faceang, int32_t moveang, int32_t maxturn)
{
    int32_t dif;

  // Do face angle
    faceang &= 255;

    dif = faceang - GetFace();
    if (dif > 128)
        dif = dif - 256;
    if (dif < -128)
        dif = dif + 256;

    int32_t newfaceang;
    if (dif < 0)
        newfaceang = (GetFace() + max(dif, -maxturn)) & 255;
    else
        newfaceang = (GetFace() + min(dif, maxturn)) & 255;

  // Do move angle
    moveang &= 255;

    dif = moveang - GetMoveAngle();
    if (dif > 128)
        dif = dif - 256;
    if (dif < -128)
        dif = dif + 256;

    int32_t newmoveang;
    if (dif < 0)
        newmoveang = (GetMoveAngle() + max(dif, -maxturn)) & 255;
    else
        newmoveang = (GetMoveAngle() + min(dif, maxturn)) & 255;

    if (newfaceang != GetFace())
        FaceOnly(newfaceang);

    if (newmoveang != GetMoveAngle())
        SetMoveAngle(newmoveang);
}

#define MAXCHANGE   200
#define STOPHEIGHT  50

int32_t TCharacter::UpdateAngle(int32_t angle)
{
/*
    int32_t ba = angle;
    int32_t bd = 1000000;

    for (int32_t a = angle - 16; a < angle + 16; a += 1)
    {
        S3DPoint vect;
        ConvertToVector(a, 250, vect);

        int32_t lh = MapPane.GetWalkHeight(pos), delta = 0, h;
        S3DPoint targ = pos, i = pos;
        targ += vect;

        if (vect.x == 0)
        {
            for (i.y = pos.y + 1; i.y < targ.y; i.y++)
            {
                h = MapPane.GetWalkHeight(i);
                delta += absval(h - lh);
                lh = h;

                if (h > STOPHEIGHT || delta > MAXCHANGE)
                    break;
            }
        }
        else if (vect.y == 0)
        {
            for (i.x = pos.x + 1; i.x < targ.x; i.x++)
            {
                h = MapPane.GetWalkHeight(i);
                delta += absval(h - lh);
                lh = h;

                if (h > STOPHEIGHT || delta > MAXCHANGE)
                    break;
            }
        }
        else
        {
            int32_t dist = (int32_t)sqrt((double)(sqr(vect.x) + sqr(vect.y)));
            float yr = (float)vect.y / (float)dist;
            float xr = (float)vect.x / (float)dist;
            S3DPoint oldpoint = i;

            for (int32_t j = 1; j < dist; j++)
            {
                i.x = (int32_t)(xr * (float)j);
                i.y = (int32_t)(yr * (float)j);
                i += pos;

                if (oldpoint.x != i.x && oldpoint.y != i.y)
                {
                    h = MapPane.GetWalkHeight(i);
                    delta += absval(h - lh);
                    lh = h;
                }

                oldpoint = i;

                if (h > STOPHEIGHT || delta > MAXCHANGE)
                    break;
            }
        }

        int32_t d = (int32_t)sqrt((double)SQRDIST(i, targ));

        if (d < bd)
        {
            bd = d;
            ba = a;
        }
    }

    return ba;
*/
    int32_t ch = MapPane.GetWalkHeight(pos);
    int32_t nudge = 0;
    S3DPoint c;
    c.z = 0;

    // hum...need to make this keep some sort of static nudge
    // value in order to avoid the quivering affect.
    // possibly need to reset that when the target location
    // is changed, as well.
    // also - in the case of a coridor, need to make sure that
    // no matter what way they are facing it always sends them
    // straight down it.

    for (c.y = pos.y - 32; c.y <= (pos.y + 32); c.y += 16)
        for (c.x = pos.x - 32; c.x <= (pos.x + 32); c.x += 16)
        {
            if (c.y == pos.y && c.x == pos.x)
                continue;

            if (absval(MapPane.GetWalkHeight(c) - ch) < 30)
                continue;

            int32_t dist = (sqr(64) - SQRDIST(c, pos)) / 100;
            dist = min(50, max(1, dist));

            int32_t ang = ConvertToFacing(pos, c) - GetFace();

            int32_t weight = (dist * (64 - absval(ang))) / 100;

            if (weight < 1)
                continue;

            if (ang > 0)
                nudge -= weight;
            else
                nudge += weight;
        }

    return angle;
}

TObjectInstance* TCharacter::FindObjAhead()
{
    S3DPoint pos, v;
    GetPos(pos);
    ConvertToVector(GetFace(), 60, v);
    pos += v;

    int32_t list[MAXFOUNDOBJS];
    int32_t n = MapPane.FindObjectsInRange(pos, list, 60);

    TObjectInstance* best = nullptr;
    int32_t bestdist;

    for (int32_t i = 0; i < n; i++)
    {
        TObjectInstance* oi = MapPane.GetInstance(list[i]);
        if (!oi)
            continue;

        if (oi->CursorType() != CURSOR_NONE || oi->IsInventoryItem())
        {
            int32_t dist = Distance(oi);

            if (best == nullptr || dist <= bestdist)
            {
                best = oi;
                bestdist = dist;
            }
        }
    }

    return best;
}

// ***********************
// * General AI Routines *
// ***********************

// REVSYNC: ResolveMove @ 0x004c5e90 -- a walk's tick.
// - Pivoting first (waitpivot): stand still and turn to the block's angle
//   (the root animation turned by hand, or a pivot animation that turns
//   itself); then the first step, "f", else "l", else "r" (none: back to
//   the root).
// - Blocked by the last Move: bounce off at an angle by octant, a little to
//   the right, and back to the root.
// - A Goto: there within 8 (MoveTo onto the point, and the item it carries
//   is picked up), else head for it.
// - Then turn toward the block's angle and, each time a step's animation
//   ends, take the next step, left and right in turn, until stopped.
int32_t TCharacter::ResolveMove(TActionBlock* ab, int32_t bits)
{
    if (ab->waitpivot)
    {
        Halt();
        if (!stricmp(ab->name, root->name))
        {
            if (GetFace() != ab->angle)
            {
                AdvanceAngles(ab->angle, ab->angle, ab->turnrate);
                return COM_DONE;
            }
        }
        else if (!commanddone)
            return COM_DONE;

        TActionBlock* step = nullptr;
        for (const char* side : {"f", "l", "r"})
            if (HasActionAni(StName(root->name, side)))
            {
                step = new TActionBlock(*doing, StName(root->name, side));
                break;
            }
        if (!step)
        {
            SetDesired(root);
            return COM_DONE;
        }
        step->waitpivot = false;
        step->turnrate = MAXTURNRATE;
        step->angle = step->moveangle = ab->angle;
        ForceCommand(step);
        return COM_DONE;
    }

    if (bits & MOVE_BLOCKED)
    {
        int32_t a = ab->angle;
        if (a >= 0x7f)
        {
            if (a <= 0xa0)
                a += 0x20;
            else if (a > 0xe0 || a < 0xbf)
                a -= 0x20;
        }
        else if (a >= 0x60)
            a -= 0x20;
        else if (a < 0x20 || a > 0x40)
            a += 0x20;
        a = (a + 0x10) & 0xff;
        ab->angle = a;
        Face(a);
        ForceCommand(root, 0, 0);
        return COM_DONE;
    }

    if (ab->target.x != 0 || ab->target.y != 0 || ab->target.z != 0)
    {
        if (dist(pos.x, pos.y, ab->target.x, ab->target.y) < 8)
        {
            ab->target.z = pos.z;
            MoveTo(ab->target);
            ab->nowaitdone = true;
            // A Goto's item is picked up on arrival (retail 0x004c6155).
            if (TObjectInstance* item = gotoitem.Get())
            {
                Pickup(item);
                gotoitem = nullptr;
            }
            return COM_DONE;
        }
        ab->angle = ab->moveangle = ConvertToFacing(pos, ab->target);
    }

    SetMoveAngle(GetFace());
    AdvanceAngles(ab->angle, ab->angle, ab->turnrate);

    if (commanddone && !ab->stop)
    {
        auto* step = new TActionBlock(*doing);
        if (doing->IsLeft(root->name))
            strcpy(step->name, StName(root->name, "r"));
        else if (doing->IsRight(root->name))
            strcpy(step->name, StName(root->name, "l"));
        ForceCommand(step);
    }
    return COM_EXECUTING;
}

// This function is called by the ResolveAttack() function to resolve hits for
// multiple characters.  The characters are usually found by calling the FindCharacters()
// function, then calling this function for each character.  Returns true if hit.
// REVSYNC: TCharacter::ResolveHit @ 0x004c62b0 -- one character struck by an
// attack at its impact frame (COMBAT_HIT.md §3.3). No dice: the to-hit and
// the roll were fixed when the attack was chosen. A target blocking or
// dodging and facing the blow takes 50 off the to-hit (the damage re-tiered
// at the new margin). A failed roll is a glance, not a miss: the target
// still takes the damage and plays the impact for it -- all but a blocking
// player -- and the attacker plays its miss. The player and his foes get
// the result on the text bar; the player earns experience.
bool TCharacter::ResolveHit(TCharacter* targ, SCharAttackData* attack, SCharAttackImpact* impact,
    int32_t damage, int32_t tohit, int32_t roll)
{
    if (!targ || !IsEnemy(targ))
        return false;
    const int32_t dist = Distance(targ);
    const int32_t angle = FaceAngleTo(targ);
    if (dist < attack->hitminrange || dist > attack->hitmaxrange)
        return false;
    if (abs(targ->pos.z - pos.z) > MAXZHITRANGE)
        return false;
    if (abs(angle) > attack->hitangle)
        return false;
    if (!IsValidTarget(targ))
        return false;
  // Someone held in an interactive move can only be struck by its maker.
    if (targ->InteractiveLocked() && targ->Fighting() != this)
        return false;

    targ->SetHasSeen(this);
    S3DPoint push;
    ConvertToVector(GetFace(), 4 * ROLLOVER, push);
    targ->vel += push;

  // A guard counts against a blow the target faces, unless it is a death
  // blow (WeighBlow does the arithmetic).
    const int32_t facing = targ->FaceAngleTo(this);
    const bool guarding = !(impact && (impact->flags & CAI_DEATH)) && abs(facing) < 0x30 && targ->doing &&
                          (targ->doing->action == ACTION_BLOCK || targ->doing->action == ACTION_DODGE);
    const SBlowOutcome weighed = WeighBlow(damage, tohit, roll, guarding);
    bool hit = weighed.hit;
    const int32_t dmg = weighed.damage;
    const bool blocked = weighed.blocked;
    tohit = weighed.tohit;
  // An interactive move lands on a target that is moving and not blocking.
    if ((attack->flags & CA_INTERACTIVE) && (targ->accum.x || targ->accum.y) &&
        !(targ->doing && targ->doing->action == ACTION_BLOCK))
        hit = true;

    int32_t def = targ->LuckMod();
    def += targ->Defense();
    int32_t off = LuckMod();
    off += Offense();

  // An interactive move can't kill without a death impact.
    if (hit && (attack->flags & CA_INTERACTIVE) && targ->Health() - dmg < 1 &&
        !(impact && (impact->flags & CAI_DEATH)))
        hit = false;
    if (!hit && (attack->flags & CA_INTERACTIVE))
        return false;
    CombatTrace::Event(this, hit ? "hit" : "miss", "target=%s\troll=%d\ttohit=%d\tdamage=%d",
                       targ->GetName() ? targ->GetName() : "-", roll, tohit, dmg);

  // The block the target plays: a death named by the impact when the blow
  // kills, else the impact itself; a real hit (not a glance) also draws
  // the target into the fight.
    TActionBlock* hitab = nullptr;
    const char* tag = "BASEGLANCE";
    if (targ->Health() - dmg < 1)
    {
        if (impact && (impact->flags & CAI_DEATH) && targ->HasActionAni(impact->impactname))
        {
            hitab = new TActionBlock(impact->impactname, ACTION_DEAD);
            hitab->priority = true;
            hitab->attack = attack;
            hitab->obj = this;
            hitab->impact = nullptr;
            hitab->damage = dmg;
            targ->SetFighting(this);
        }
        targ->combatflashticks = 5;
    }
    else
    {
        if (hit && !targ->Fighting() && !autocombat && dist <= targ->chardata->combatrangemin)
            targ->BeginFighting(this, ACTION_COMBAT);
        if (impact)
        {
            char name[RESNAMELEN];
            if (impact->flags & (CAI_DEATH | CAI_INTERACTIVE))
                strncpyz(name, impact->impactname, RESNAMELEN);
            else
                targ->CombatAnimName(name, impact->impactname);
            if (targ->HasActionAni(name))
            {
                targ->doing->priority = targ->doing->interrupt = false;
                const ACTION action = (impact->flags & CAI_STUN) ? ACTION_STUN
                                    : (impact->flags & CAI_KNOCKDOWN) ? ACTION_KNOCKDOWN : ACTION_IMPACT;
                hitab = new TActionBlock(name, action);
                hitab->priority = hitab->interrupt = true;
                hitab->attack = attack;
                hitab->impact = impact;
                hitab->wait = impact->looptime;
                hitab->obj = this;
                hitab->damage = dmg;
                targ->SetFighting(this);
            }
        }
    }
    if (hit)
    {
        const int32_t margin = tohit - roll;
        const auto& tiers = Rules.tohitdamage;
        tag = (attack->flags & CA_FATIGUEATTACK) ? "BASEFATIGUE"
            : margin > tiers[0].minvalue ? "BASEDOUBLE"
            : margin > tiers[1].minvalue ? "BASEHIT"
            : margin > tiers[2].minvalue ? "BASEMINOR"
            : margin > tiers[3].minvalue ? "BASESMALL" : "BASEGLANCE";
    }

  // The result line, for fights the player is in.
    if ((!blocked || ((attack->flags & CA_INTERACTIVE) && hit)) &&
        (this == Player || targ == Player) && !NoCombatResults)
    {
        char line[100];
        if (DialogList.FindLine("FULLCOMBATRES") >= 0)
        {
            const char* what = DialogList.GetLine(tag);
            const char* format = DialogList.GetLine("FULLCOMBATRES");
            snprintf(line, sizeof(line), format, what, targ->GetName(), dmg, def, off);
        }
        else
        {
            const char* offense = DialogList.GetLine("BASEOFF");
            const char* defense = DialogList.GetLine("BASEDEF");
            const char* damaged = DialogList.GetLine("BASEDMG");
            const char* what = DialogList.GetLine(tag);
            snprintf(line, sizeof(line), "%s %s %s:%d %s:%d %s:%d", what, ShownName(targ->GetName()).c_str(), damaged, dmg,
                     defense, def, offense, off);
        }
        TextBar.Print("%s", line);
        if (CombatTrace::Enabled())
            CombatTrace::Event(this, "result", "text=%s", line);
    }

  // A blocking player takes nothing from a glance.
    const bool lands =
        hit || !(targ->ObjClass() == OBJCLASS_PLAYER && targ->doing && targ->doing->action == ACTION_BLOCK);
    LandBlow(*targ, dmg, lands, hitab);
    return hit;
}

SBlowOutcome TCharacter::WeighBlow(int32_t damage, int32_t tohit, int32_t roll, bool guarding)
{
    SBlowOutcome blow;
    blow.damage = damage;
    blow.tohit = tohit;
    blow.hit = tohit >= roll;
    if (guarding)
    {
      // Undo the tier the damage carries, take the guard's 50 off the
      // to-hit, and tier it again.
        int32_t factor;
        if (damage > 0 && ToHitDamageFactor(tohit - roll, factor))
            blow.damage = damage * 100 / factor;
        blow.tohit = std::clamp(tohit - 50, 10, 100);
        if (ToHitDamageFactor(blow.tohit - roll, factor))
            blow.damage = (std::max)(1, factor * blow.damage / 100);
        blow.hit = blow.tohit >= roll;
        blow.blocked = !blow.hit;
    }
    blow.damage = (std::max)(blow.damage, 1);
    return blow;
}

void TCharacter::LandBlow(TCharacter& target, int32_t damage, bool lands, TActionBlock* action)
{
  // Damage keeps or frees the block it is given (retail frees it in
  // ResolveHit when Damage didn't install it, after a Damage that may have
  // freed it already).
    if (lands)
        target.Damage(damage, DT_NONE, 0, action, this);
    else
        delete action;  // REVSYNC-DIVERGENCE: ownership passes to Damage (no double free; see TCharacter::Damage)

    if (target.ObjClass() == OBJCLASS_PLAYER && target.root && target.root->action == ACTION_BOW)
        target.BeginFighting(nullptr, ACTION_COMBAT);
    if (ObjClass() == OBJCLASS_PLAYER)
    {
        TPlayer* player = static_cast<TPlayer*>(this);
        player->AwardKillExp(&target);
        player->AwardSkillExp(SK_WEAPONSKILLS + WeaponType(), &target);
        player->AwardStealthExp(&target);
    }
}

// Maximum number of characters an attack strikes at once (retail's 0x20).
constexpr int32_t kMaxHitChars = 32;

// REVSYNC: TCharacter::ResolveAttack @ 0x004c6dd0 -- every tick of an ATTACK
// block (COMBAT_HIT.md §3.2): the first tick tells the target, until the
// impact frame a close target holds still, at the impact frame each
// character in reach is struck (ResolveHit), a miss plays the miss
// animation (and against a guard sparks and a block sound), and the attack's
// fatigue is paid -- a quarter for a missed interactive move.
int32_t TCharacter::ResolveAttack(TActionBlock* ab, int32_t bits)
{
    if (flags & (OF_ICED | OF_PARALIZE))
        return COM_DONE;
    if (charflags & kCharFlagNoTurn)
        return COM_DONE;
    SCharAttackImpact* impact = ab->impact;
    SCharAttackData* attack = ab->attack;
    TCharacter* targ = (TCharacter*)ab->obj;
    if (ab->firsttime)
    {
        lastattack = attack;
        lasthit = 0;
        lastattackticks = PlayScreen.GameFrame();
    }
    int32_t dist = 0;
    if (targ)
    {
        dist = Distance(targ);
        FaceAngleTo(targ);      // retail computes it and drops it
    }
    if (ab->firsttime && targ && ab->attack && !(ab->attack->flags & CA_PLAYANIM))
        targ->SignalAttack(this, ab->obj, (ab->attack->flags & CA_MAGICATTACK) ? 2 : 0);
    if (!attack)
        return COM_DONE;        // REVSYNC-DIVERGENCE: retail reads through the null record
    if (targ && !(attack->flags & (CA_INTERACTIVE | CA_NOPUSH)) && !(GetAniFlags() & AF_FLY) &&
        !(targ->GetAniFlags() & AF_FLY) && !(impact && impact->snapdist > 0) && targ->Health() > 0 &&
        !targ->invisible_spell && dist <= 10 && GetFrame() < attack->impacttime)
    {
      // Close in: hold still until the blow lands.
        S3DPoint next;
        GetNextMove(next);
        next.x = next.y = 0;
        SetNextMove(next);
    }

    if (GetFrame() == attack->impacttime && !(attack->flags & (CA_PLAYANIM | CA_MAGICATTACK)))
    {
        bool hit = false;
        if (targ)
            hit = ResolveHit(targ, ab->attack, ab->impact, ab->damage, ab->tohit, ab->roll);
        lasthit = hit;
        if (!(attack->flags & CA_ONETARGET))
        {
            TCharacter* chars[kMaxHitChars];
            const int32_t n = FindCharacters(chars, kMaxHitChars, attack->hitmaxrange, GetFace(), attack->hitangle,
                                             FINDCHAR_ENEMY);
            for (int32_t c = 0; c < n; c++)
                if (chars[c] != targ)
                    hit |= ResolveHit(chars[c], ab->attack, ab->impact, ab->damage, ab->tohit, ab->roll);
        }

        int32_t cost = attack->fatigue;
        if (!hit)
        {
            if (!(attack->flags & CA_NOMISS))
            {
                TActionBlock* miss;
                if (HasActionAni(attack->missname))
                {
                    miss = new TActionBlock(attack->missname, ACTION_MISS);
                    miss->attack = attack;
                }
                else
                    miss = new TActionBlock(root->name, ACTION_COMBAT);
                miss->obj = ab->obj;
                doing->priority = false;
                miss->priority = true;
                SetMoveDist(0);
                ForceCommand(miss);
                PlayWave(listrnd(chardata->misssounds));
            }
          // Struck a guard: sparks between two weapons, and the clash.
            if (targ && targ->doing->action == ACTION_BLOCK && Distance(targ) <= attack->hitmaxrange)
            {
                if (WeaponType() && targ->WeaponType())
                    EffectBurst("sparks", 0x32);
                if (TCharacter* opponent = Fighting())
                {
                    const int32_t mine = WeaponType();
                    const int32_t theirs = opponent->WeaponType();
                    const int32_t which = random(1, 2);
                    const bool edged = theirs == WT_KNIFE || theirs == WT_SWORD || theirs == WT_AXE;
                    const char* clash = "dull";
                    if (mine == WT_KNIFE || mine == WT_SWORD || mine == WT_AXE)
                        clash = theirs ? "sword" : "dull";
                    else if (mine >= WT_BLUDGEON && mine <= 7)
                        clash = edged ? "sword" : "dull";
                    char sound[32];
                    snprintf(sound, sizeof(sound), "block%d%s", which, clash);
                    PlayWave(sound);
                }
            }
            if (attack->flags & CA_INTERACTIVE)
                cost = attack->fatigue / 4;
        }
        if (Fatigue() - cost > 0)
            SetFatigue(Fatigue() - cost);
        else
            SetFatigue(0);
    }

    if ((attack->flags & CA_PLAYANIM) && Fighting())
        SetRotateZ(AngleTo(Fighting()));
    return COM_DONE;
}

// REVSYNC: TCharacter::OnAttacked @ 0x004cdce0 (slot 0x240) -- a monster told
// an attack on it has begun (COMBAT_ATTACK_CHOICE.md §3.13): it turns on the
// attacker unless its own target is much closer, counts the attacker's
// repeats of one button, answers a fatigue attack with an attack of its own
// and a third repeat with its guard, and else blocks by its BLOCK
// frequency. The player isn't told.
void TCharacter::SignalAttack(TObjectInstance* actor, TObjectInstance* target, int32_t flag)
{
    if (signalAttackSeam)
    {
        signalAttackSeam(this, actor, target, flag);
        return;
    }
    if (target != this || InteractiveLocked() || objclass == OBJCLASS_PLAYER || !actor)
        return;
    TCharacter* attacker = static_cast<TCharacter*>(actor);
    if (TCharacter* current = Fighting())
    {
        const int32_t nearer = Distance(current) * 75 / 100;
        if (Distance(attacker) < nearer)
            SetFighting(attacker);
    }
    else
        SetFighting(attacker);

    bool repeat = false;
    const SCharAttackData* a = attacker->GetDoing()->attack;
    if (a)
    {
        if (a->button == lastbutton)
        {
            if (++buttonrepeat >= 3)
                repeat = true;
        }
        else
            buttonrepeat = 1;
        lastbutton = a->button;
    }
    else
    {
        lastbutton = -1;
        buttonrepeat = 0;
    }
    if (a && (a->flags & CA_FATIGUEATTACK))
    {
        requestbits |= kRequestAttackNow | kRequestNoPlayAnim | kRequestInteractive;
        return;
    }
    if (!repeat)
    {
        if (flag || Fighting() != attacker)
            return;
        const int32_t chance = BlockPcnt();
        if (random(1, 100) > chance || attacker->InteractiveLocked())
            return;
    }
    Block(-1);
}

// REVSYNC: TCharacter::ResolveImpact @ 0x004c74b0 (vtable +0x338; COMBAT_HIT.md
// §3.7) -- each tick of an impact, stun or knockdown block: blood (one
// tick in six of an "impale"; the first tick of a blow that took over 1% of
// the character's health, unless its attack draws none), the combat flash;
// when the animation ends, back to the root once the wait is out, else on
// into the impact's loop.
int32_t TCharacter::ResolveImpact(TActionBlock* ab, int32_t bits)
{
    const float share = (float)ab->damage / (float)chardata->health;
    const auto bleeds = [this] { return !(chardata->flags & CF_BADBLEEDER) && chardata->bleeder; };
    if (ab->Is("impale") && random(0, 5) == 1 && bleeds())
        EffectBurst("blood", ab->Is("impale") ? 40 : 50);
    if (ab->firsttime && bleeds() && share > 0.01f && (!ab->attack || (ab->attack->flags & CA_BLOOD)))
        EffectBurst("blood", ab->Is("impale") ? 40 : 50);
    if (ab->firsttime)
    {
        EffectCombatFlash();
        combatflashticks = 5;
    }

    if (commanddone)
    {
        if (ab->wait <= 0)
        {
            doing->priority = false;
            root->interrupt = true;
            SetDesired(root);
        }
        else if (ab->impact && ab->Is(ab->impact->impactname) && ab->impact->loopname[0] &&
                 FindState(ab->impact->loopname) >= 0)
        {
            auto* loop = new TActionBlock(*ab, ab->impact->loopname, ab->action);
            loop->priority = loop->interrupt = true;
            desired->priority = false;
            doing->priority = false;
          // Forced while the character plays its last attack's animation (or
          // that attack is a PLAYANIM); else it waits its turn.
          // REVSYNC-DIVERGENCE: retail reads the last attack unguarded; with
          // none the loop waits.
            if (lastattack && ((lastattack->flags & CA_PLAYANIM) || !stricmp(lastattack->attackname, doing->name)))
                ForceCommand(loop);
            else
                SetDesired(loop);
            DropUnheld(loop);
        }
    }
    if (commanddone && ab->wait <= 0)
    {
        auto* back = new TActionBlock(root->name, ACTION_COMBAT);
        back->interrupt = true;
        SetDesired(back);
        DropUnheld(back);
    }
    return COM_DONE;
}

// REVSYNC: TCharacter::ResolveBlock @ 0x004c77a0 (vtable +0x334) -- a block
// faces whoever it answers, and holds until its wait is out or it is
// stopped.
int32_t TCharacter::ResolveBlock(TActionBlock* ab, int32_t bits)
{
    if (doing && doing->obj)
    {
        const int32_t a = AngleTo(doing->obj);
        SetRotateZ(a);
        SetMoveAngle(a);
    }
    if (ab->wait > 0 && !(doing && doing->stop))
        return COM_EXECUTING;
    ab->wait = 0;
    SetDesired(nullptr);
    return COM_DONE;
}

// REVSYNC: TCharacter::ResolveDead @ 0x004c7810 (vtable +0x33c) -- the first
// tick's blood (as retail tests it: only for a character whose BLEEDER
// value failed to parse) and combat flash; when the dying animation ends,
// the death impact's loop becomes the root.
int32_t TCharacter::ResolveDead(TActionBlock* ab, int32_t bits)
{
    if (ab->firsttime && (!ab->attack || (ab->attack->flags & CA_BLOOD)) && chardata->bleeder &&
        (chardata->flags & CF_BADBLEEDER))
        EffectBurst("blood", ab->Is("impale") ? 40 : 50);
    if (ab->firsttime)
    {
        EffectCombatFlash();
        combatflashticks = 5;
    }
    if (commanddone && ab->impact && ab->impact->loopname[0] && !ab->Is(ab->impact->loopname) &&
        FindState(ab->impact->loopname) >= 0)
    {
        auto* loop = new TActionBlock(*ab, ab->impact->loopname, ab->action);
        loop->priority = loop->interrupt = true;
        desired->priority = false;
        doing->priority = false;
        SetDesired(loop);
        SetRoot(loop);
        DropUnheld(loop);
    }
    return COM_EXECUTING;
}

// REVSYNC: ResolveCombat @ 0x004c7980 -- every tick of a combat root or
// combat step (TPlayer's 0x00519210 only forwards here). Retarget on the
// frame cadence (the player every 8 frames, everyone every 32 while the AI
// is on), take the root's target, then: a pivot block turns until it faces
// and becomes the strafe step; a moving player whose step no longer fits
// gets a new step at the target (the orbit); a standing player or a
// monster turns in place. docs/gameplay/forensics/COMBAT_MOVEMENT.md §3.
int32_t TCharacter::ResolveCombat(TActionBlock* ab, int32_t bits)
{
  // No item to walk to (retail +0x288, not ported): no goto target.
    ab->target = S3DPoint(0, 0, 0);

    const int32_t slot = PlayScreen.GameFrame() ^ GetMapIndex();
    const bool cadence = (ObjClass() == OBJCLASS_PLAYER && (slot & 7) == 0) || (!NoAI && (slot & 0x1f) == 0);

    auto* targ = static_cast<TCharacter*>(ab->obj);
    TCharacter* found = nullptr;
    if (!targ || !IsValidTarget(targ))
    {
        if (cadence && FindCharacters(&found, 1, -1, -1, 32, FINDCHAR_ENEMY | FINDCHAR_HEAR | FINDCHAR_SEE) < 1)
            found = nullptr;
        SetFighting(found);
    }
    if (!ai_lookat)
    {
        if (cadence)
        {
            TCharacter* ahead = nullptr;
            found = FindCharacters(&ahead, 1, -1, ab->moveangle, 32,
                                   FINDCHAR_ENEMY | FINDCHAR_HEAR | FINDCHAR_SEE) >= 1 ? ahead : nullptr;
        }
        if (found && found != targ)
        {
          // Switch unless the current target is near (within 32) or in the
          // direction moved (within 45 degrees), and no farther.
            const int32_t dfound = Distance(found);
            bool keep = false;
            if (targ)
            {
                const int32_t dtarg = Distance(targ);
                const int32_t off = std::abs(AngleDiff(AngleTo(targ), ab->moveangle));
                keep = (dtarg <= 32 || off <= 32) && dfound >= dtarg;
            }
            if (!keep)
                SetFighting(found);
        }
    }

    TObjectInstance* obj = (root && (root->action == ACTION_COMBAT || root->action == ACTION_BOW)) ? root->obj : nullptr;
    ab->obj = obj;

    if (ab->waitpivot)
    {
        Halt();
        if (!stricmp(ab->name, root->name))
        {
            if (GetFace() != ab->angle)
            {
                AdvanceAngles(ab->angle, ab->moveangle, ab->turnrate);
                return 0;
            }
        }
        else if (!commanddone)          // a pivot animation turns by itself
            return 0;

      // Turned: the step for the way to go.
        char animname[RESNAMELEN];
        GetAngleMoveAnim(ab->moveangle, ab->angle, root->name, animname, RESNAMELEN);
        auto* step = new TActionBlock(animname, doing->action);
        if (!HasActionAni(animname))
        {
            delete step;                // REVSYNC-DIVERGENCE: retail leaks it
            return 0;
        }
        step->moveangle = ab->moveangle;
        step->obj = ab->obj;
        step->angle = ab->angle;
        step->waitpivot = false;
        step->turnrate = 8;
        if (doing && doing->priority && desired != doing)
        {
            delete step;
            return 0;
        }
        ForceCommand(step);
        return 0;
    }

  // Whom to face: the target when allowed and visible, else the AI's goal.
    const int32_t blockangle = ab->angle;
    int32_t angle = blockangle;          // retail keeps both the block's angle
    int32_t angle8 = blockangle & 0xff;  // and its low byte until a facing is found
    bool face = ObjClass() == OBJCLASS_CHARACTER ||
                (ObjClass() == OBJCLASS_PLAYER && (CombatFace || !IsMoveAction(doing)));
    if (ab->walkto)
        face = false;
    TObjectInstance* look = nullptr;
    if (obj)
    {
        if (!CanSeeCharacter(static_cast<TCharacter*>(obj), -1) && ai_lookat)
            face = false;
        if (face && !retreating)
            look = obj;
    }
    if (!look)
        look = ai_lookat;
    if (look)
        angle = angle8 = ConvertToFacing(pos, look->Pos());

    if (ObjClass() == OBJCLASS_PLAYER)
    {
        if (IsMoveAction(doing))
        {
          // Moving: the step for moving one way and facing the other.
            char animname[RESNAMELEN];
            GetAngleMoveAnim(ab->moveangle, angle, root->name, animname, RESNAMELEN);
            if (stricmp(ab->name, animname) != 0)
            {
                auto* step = new TActionBlock(animname, doing->action);
                step->angle = (angle8 + 15) & 0xe0;
                step->moveangle = ab->moveangle;
                const int32_t diff = std::abs(AngleDiff(GetFace(), step->angle));
                step->obj = ab->obj;
                step->turnrate = (std::max)(0, diff - 32) / 32 * 8 + 16;
                SetMoveAngle(step->moveangle);
                ForceCommand(step);
                return 0;
            }
        }
        else if (ab->angle != angle8)
        {
            const int32_t diff = std::abs(AngleDiff(GetFace(), angle8));
            ab->moveangle = ab->angle = angle8;
            ab->turnrate = StepTurnRate(diff);
        }
    }
    else if (ab->angle != angle8)
    {
        const int32_t diff = std::abs(AngleDiff(GetFace(), angle8 & 0xff));
        ab->angle = angle8;
        ab->turnrate = StepTurnRate(diff);
        ab->moveangle = ab->action == ACTION_COMBATMOVE ? angle8 : GetMoveAngle();
    }

    SetMoveAngle(ab->moveangle);
    AdvanceAngles(ab->angle, ab->moveangle, ab->turnrate);
    return 0;
}

// REVSYNC: ResolveCombatMove @ 0x004c7f80 -- a combat step: blocked or
// stopped, back to the root; re-desire the step while the root is desired;
// a Goto's walk to an item (the walkto mark) picks it up within 8 of the
// point, else steps toward it; then ResolveCombat.
int32_t TCharacter::ResolveCombatMove(TActionBlock* ab, int32_t bits)
{
    if ((bits & MOVE_BLOCKED) || ab->stop)
    {
        Halt();
        ForceCommand(root);
        return 0;
    }
    if (desired == root)
        SetDesired(doing);
    if (gotoitem.Get() && ab->walkto && (ab->target.x != 0 || ab->target.y != 0 || ab->target.z != 0))
    {
        if (dist(pos.x, pos.y, ab->target.x, ab->target.y) < 8)
        {
            ab->target.z = pos.z;
            MoveTo(ab->target);
            ab->walkto = false;
            ab->nowaitdone = true;
            Pickup(gotoitem.Get());
            gotoitem = nullptr;
            return 0;
        }
        ab->moveangle = ConvertToFacing(pos, ab->target);
    }
    return ResolveCombat(ab, bits);
}

// REVSYNC: TCharacter::ResolveBowAim @ 0x004c80c0 -- hold still and turn to
// the aim; done once a shot is called for and the facing is there. The
// root's and the doing block's angles follow the aim.
int32_t TCharacter::ResolveBowAim(TActionBlock* ab, int32_t bits)
{
    Halt(); // Make sure there's no movement

    if (ab->stop && GetFace() == ab->angle)
        return COM_DONE;

  // Do turning
    AdvanceAngles(ab->angle, ab->angle, ab->turnrate);
    root->moveangle = doing->moveangle = ab->angle;
    root->angle = doing->angle = ab->angle;

    return COM_EXECUTING; // Allow to loop indefinitely until cancelled
}

// REVSYNC: TCharacter::ResolveBowShoot @ 0x004c8130 -- a player's queued
// shots, one when the last is more than BOWWAIT ticks old: facing the aim,
// an arrow of the equipped ammo's type flies from ARROWPOS (turned with the
// facing) at ARROWSPEED, his; when it is made, one ammo leaves the
// inventory (the slot is emptied with the last) and "bowshoot1".."6"
// plays. Executing while shots are queued, done after; others shoot nothing.
int32_t TCharacter::ResolveBowShoot(TActionBlock* ab, int32_t bits)
{
    if (ObjClass() != OBJCLASS_PLAYER)
        return COM_DONE;
    auto* player = static_cast<TPlayer*>(this);
    const bool due = PlayScreen.GameFrame() - lastbowshot > chardata->bowwait;
    if (bowshots > 0 && due)
    {
        // Retail leaves a multiplayer client's arrow to the server; the
        // network game isn't ported, so this is always the one shooting.
        if (TObjectInstance* ammo = player->GetEquip(EQ_AMMO))
        {
            SetRotateZ(ab->angle & 255);
            SetMoveAngle(ab->angle);

            SObjectDef def{};
            def.objclass = OBJCLASS_AMMO;
            def.objtype = AmmoClass.FindObjType(ammo->GetName());
            def.flags = OF_WEIGHTLESS;
            def.pos = pos;
            S3DPoint offset;
            ConvertToVector((GetFace() + ConvertToFacing(chardata->arrowpos)) & 255, ::Distance(chardata->arrowpos),
                            offset);
            def.pos.x += offset.x;
            def.pos.y += offset.y;
            def.pos.z += chardata->arrowpos.z;
            def.level = MapPane.GetMapLevel();
            ConvertToVector(GetFace(), chardata->arrowspeed << 16, def.vel);
            def.facing = GetFace();
            // No arrow made (or not an AMMO one): no ammo spent, no sound.
            TObjectInstance* arrow = MapPane.GetInstance(MapPane.NewObject(&def));
            if (arrow && arrow->ObjClass() == OBJCLASS_AMMO)
            {
                static_cast<TAmmo*>(arrow)->SetShooter(this);
                // (Retail then adds it to the map, 0x00451090: NewObject has.)

                const int32_t left = ammo->Amount();
                DeleteFromInventory(ammo->GetName(), 1);
                if (left == 1)
                    player->Equip(nullptr, EQ_AMMO);

                char sound[] = "bowshoot*";
                sound[8] = char('1' + random(0, 5));
                PlayAt(sound, pos);
            }
        }
        lastbowshot = PlayScreen.GameFrame();
        bowshots--;
    }
    return bowshots <= 0 ? COM_DONE : COM_EXECUTING;
}

int32_t TCharacter::ResolveLeap(TActionBlock* ab, int32_t bits)
{
    return ResolveCombat(ab, bits);
}

int32_t TCharacter::ResolvePull(TActionBlock* ab, int32_t bits)
{
    int32_t start;

    if (!ab->data)
        return 0;

    start = ((int32_t *)ab->data)[0];

    if ((start == LEVER_PULLSOUTH) || (start == LEVER_PULLNORTH))
    {
        if (commanddone && ab->obj)
        {
            if (start == LEVER_PULLNORTH)
            {
                if (ab->obj->GetState() == EXIT_OPEN)
                    ab->obj->SetState( EXIT_CLOSING);
            }
            else
            {
                if (ab->obj->GetState() == EXIT_CLOSED)
                    ab->obj->SetState( EXIT_OPENING);
            }
        }
    }

    if ((start == LEVER_PUSHSOUTH) || (start == LEVER_PUSHNORTH))
    {
        if (ab->firsttime && ab->obj)
        {
            if (start == LEVER_PUSHNORTH)
            {
                if (ab->obj->GetState() == EXIT_OPEN)
                    ab->obj->SetState( EXIT_CLOSING);
            }
            else
            {
                if (ab->obj->GetState() == EXIT_CLOSED)
                    ab->obj->SetState( EXIT_OPENING);
            }
        }
    }

    return 0;
}

int32_t TCharacter::ResolveSay(TActionBlock* ab, int32_t bits)
{
    if (ab->wait <= 0 || (doing && doing->stop))
    {
        ab->wait = 0;
        ForceCommand(root, 0, Incidentals() ? 0 : kCommandNoIncidentals);   // REVSYNC: 0x004c8437
        return COM_DONE;
    }

    return COM_EXECUTING;
}

int32_t TCharacter::ResolvePivot(TActionBlock* ab, int32_t bits)
{
    Halt(); // Make sure there's no movement

    if (!stricmp(ab->name, root->name)) // Is a normal root animation we're manually turning...
    {
        if (GetFace() == ab->angle)
        {
            SetDesired(nullptr);
            return COM_DONE;
        }
        else
            AdvanceAngles(ab->angle, ab->angle, ab->turnrate);
    }
    else if (commanddone)               // Is a special pivot animation which turns itself!
    {
        SetDesired(nullptr);
        return COM_DONE;
    }

    return COM_EXECUTING;
}

// REVSYNC: TCharacter::EffectCombatFlash @ 0x004c8500 -- the flash of a
// blow: a "combatflash" effect at (30, 30, 80) from the character, in an
// even state of its imagery short of the last (2·random(0, states / 2)).
void TCharacter::EffectCombatFlash()
{
    if (effectCombatFlashSeam)
    {
        effectCombatFlashSeam(this);
        return;
    }
    extern TObjectClass EffectClass;
    SObjectDef def{};
    def.objclass = OBJCLASS_EFFECT;
    def.objtype = EffectClass.FindObjType("combatflash");
    def.level = MapPane.GetMapLevel();
    def.pos = pos + S3DPoint(30, 30, 80);
    TObjectInstance* flash = MapPane.GetInstance(MapPane.NewObject(&def));
    if (!flash)
        return;
    const int32_t states = flash->GetImagery() ? flash->GetImagery()->NumStates() : 1;
    flash->SetState(states > 0 ? (std::min)(2 * random(0, states / 2), states - 2) : 0);
}

void TCharacter::EffectBurst(const char *name, int32_t height)
{
    if (effectBurstSeam)
    {
        effectBurstSeam(this, name, height);
        return;
    }
//  return;
    // hack? maybe not... just no blood if i'm burning
    if (burning && !stricmp(name, "blood"))
        return;

    extern TObjectClass EffectClass;

    SObjectDef def;
    memset(&def, 0, sizeof(SObjectDef));

    def.objclass = OBJCLASS_EFFECT;
    def.objtype = EffectClass.FindObjType(name);

    def.level = MapPane.GetMapLevel();

    if (!stricmp(name, "blood"))
    {
      // REVSYNC: 0x004c862e..0x004c8682 -- the spray starts `height` above
      // the bleeder and heads away from its facing (facing + 128, unmasked;
      // the animator wraps it), elevation 3, both spreads 5. The count,
      // random(1, 5), is drawn once the object exists; retail's animator
      // never reads it (effects/retailblood.cpp).
        def.pos = pos;
        def.pos.z += height;

        def.facing = 0;

        int32_t index = MapPane.NewObject(&def);
        TObjectInstance* inst = MapPane.GetInstance(index);
        if (!inst)
            return;
        const int32_t heading = GetFace() + 0x80;
        const int32_t count = random(1, 5);
        if (auto* blood = dynamic_cast<TBloodEffect*>(inst))
            blood->SetParams(height, heading, 3, 5, 5, count);
    }
    else if (!stricmp(name, "sparks"))
    {
        def.pos = pos;

        int32_t index = MapPane.NewObject(&def);
        TObjectInstance* inst = MapPane.GetInstance(index);
        if (!inst)
            return;

        int32_t ang = (GetFace() + random(-80, 80)) & 0xff;

        S3DPoint vect;
        ConvertToVector(ang, 100, vect);

        if (doing && doing->obj)
        {
            SParticleParams pr = {};

            S3DPoint vect0, tpos;
            doing->obj->GetPos(tpos);
            ang = ConvertToFacing(pos, tpos);
            int32_t dist = TObjectInstance::Distance(doing->obj);
            ConvertToVector(ang, dist / 2, vect0);
            vect0.z += 45;

            pr.particles = random(15, 25);
            pr.pos.X = (float)vect0.x;
            pr.pos.Y = (float)vect0.y;
            pr.pos.Z = (float)vect0.z;
            pr.pspread.X = (float)3.0;
            pr.pspread.Y = (float)3.0;
            pr.pspread.Z = (float)3.0;
            pr.dir.X = (float)((float)vect.x / (float)100.0);
            pr.dir.Y = (float)((float)vect.y / (float)100.0);
            pr.dir.Z = (float)((float)vect.z / (float)100.0);
            pr.spread.X = (float)0.5;
            pr.spread.Y = (float)0.5;
            pr.spread.Z = (float)0.5;
            // Retail EffectBurst caller tuning (cls_0x5a7b98.cpp:4645–4651).
            pr.gravity = 0.25f;
            pr.trails = 2;
            pr.minstart = 0;
            pr.maxstart = 8;
            pr.minlife = 20;
            pr.maxlife = 40;
            pr.bounce = true;
            pr.killobj = true; 
            pr.objflags = 1 << (ObjId() & 0x3);
            pr.seektargets = false;
            pr.numtargets = 0;

            if (!TSparkEffect::AttachBurst(*inst, pr))
            {
                log_warn("[combat] could not attach typed Sparks burst");
                inst->SetFlags(OF_KILL);
            }
        }
        else
        {
            log_warn("[combat] Sparks burst has no combat target");
            inst->SetFlags(OF_KILL);
        }
    }
    else
    {
        // Other generic particle callers need their own recovered profiles;
        // do not reinterpret a generic animator as TParticle3DAnimator.
        log_warn("[combat] unsupported EffectBurst particle type '%s'", name);
    }
}

int32_t TalismanStat(int32_t tal, char *statname)
{
    extern TObjectClass TalismanClass;

    return TalismanClass.GetStat(tal, statname);
}

bool TCharacter::IsFinalState()
{
    if (doing && (doing->Is("collapse") || doing->Is("dead") || doing->Is("impale") || doing->Is("fall")))
        return true;

    return false;
}

// REVSYNC: IsEnemy @ 0x004c89c0 -- never an idle player (state bit 2), nor,
// between players, one of my team or either of us not a player killer;
// always the one fighting me; else one named, typed or grouped among my
// ENEMIES, if he's aggressive or a player.
bool TCharacter::IsEnemy(TCharacter* chr)
{
    if (isEnemySeam)
        return isEnemySeam(this, chr);
    if (chr->ObjClass() == OBJCLASS_PLAYER)
    {
        const auto* other = static_cast<const TPlayer*>(chr);
        if (other->PlayerState() & 2)
            return false;
        if (ObjClass() == OBJCLASS_PLAYER)
        {
            const auto* me = static_cast<const TPlayer*>(this);
            if (!me->Team().name.empty() && stricmp(me->Team().name.c_str(), other->Team().name.c_str()) == 0)
                return false;
            if (!other->IsPlayerKiller() || !me->IsPlayerKiller())
                return false;
        }
    }
  // Is this character attacking me
    if (chr->Fighting() == this)
        return true;    // That makes me hostile no matter what

  // Is this character a PARTICULAR enemy of mine (i.e. enemy by name)
    if (listin(chardata->enemies, chr->GetName()) ||
        listin(chardata->enemies, chr->GetTypeName()) )
            return chr->Aggressive() || chr->ObjClass() == OBJCLASS_PLAYER; // If I'm aggressive

  // Is this character in a GROUP I don't like
    int32_t numgroups = listnum(chr->chardata->groups);
    for (int32_t c = 0; c < numgroups; c++)
    {
        if (listin(chardata->enemies, listget(chr->chardata->groups, c)))
            return chr->Aggressive() || chr->ObjClass() == OBJCLASS_PLAYER; // If I'm aggressive
    }

    return false;
}

// ***********************
// * General AI Routines *
// ***********************

// REVSYNC: TCharacter::AI @ 0x004c8b60 (COMBAT_ATTACK_CHOICE.md §3.10) --
// a character's brain, each tick Pulse runs it: the gates and the bosses'
// overlay; keep the combat target or look for one (every 32 frames); then,
// standing in the combat stance (or the walk of a walk-fighter, or asked by
// OnAttacked), the attack branch, else the move branch (run from the target
// while retreating, chase it seen or heard, search for it by waypoints, or
// step aside of whoever stands in the way); last the retreat state.
// (Retail first asks NetOwner, slot 0x178: 1 in single player.)
void TCharacter::AI()
{
    ai_ai_count++;

    if (ObjClass() == OBJCLASS_PLAYER && !(charflags & kCharFlagPlayerAI))
        return;
    if ((flags & OF_DISABLED) || Health() <= 0 || NoAI)
    {
        if (IsMoveAction(doing))
            Stop();
        return;
    }
    if (Editor)
        return;
    if (Aggressive())
        AIPerMonster();

    TCharacter* target = Fighting();
    if (!IsValidTarget(target) || !Fighting())
    {
        target = nullptr;
        if (!Editor && Aggressive() && !(charflags & kCharFlagNoTurn) &&
            !((PlayScreen.GameFrame() ^ GetMapIndex()) & 0x1f))
        {
            TCharacter* found = nullptr;
            if (FindCharacters(&found, 1, -1, -1, 0x20, FINDCHAR_ENEMY | FINDCHAR_HEAR | FINDCHAR_SEE) > 0)
                target = found;
            if (target && !retreating)
                BeginFighting(target, ACTION_COMBAT);
        }
    }

    if (target && doing)
    {
        const bool attacking = (doing->Is("walk") && !retreating && (charflags & kCharFlagWalkFighter)) ||
                               (doing->action == ACTION_COMBAT && !retreating) ||
                               ((requestbits & kRequestAttackNow) &&
                                (requestbits & (kRequestNoPlayAnim | kRequestInteractive)));
        if (attacking)
            AIAttack(target);
        else
            AIMove(target);
    }

  // The retreat: on while health is at RETREATAT or below (or it was on)
  // and its frames last, which count down.
    retreating = retreatlatch = (Health() <= chardata->retreatat || retreatlatch) && retreatframes != 0;
    if (retreatframes > 0)
        retreatframes--;
}

// AI's attack branch (0x004c8cd9-0x004c8f5b; §3.10.2). The attack timer
// counts down and gates IsValidAttack; an attempt costs a second tick and
// re-arms it from ATTACKFREQ the same tick, the magic timer from MAGICFREQ.
void TCharacter::AIAttack(TCharacter* target)
{
    ai_lookat = nullptr;
    if (nextattack > 0)
        nextattack--;
    if (magictimer > 0)
        magictimer--;
    if (nextattack == 0 || magictimer == 0 || (requestbits & kRequestAttackNow))
    {
        requestbits &= ~kRequestAttackNow;
        if (lastattack && doing->Is(lastattack->attackname))
        {
            // still making it
        }
        else if (Distance(target) > chardata->maxattackrange)
        {
            Go(AngleTo(target));
            lastattack = nullptr;
            lasthit = 0;
        }
        else
        {
            const bool attacked = RandomAttack(random(1, 100));
          // Someone else in the way and no attack: step aside.
            TCharacter* blocker = CharBlocking(this, pos, Radius() * 2);
            if (blocker && blocker != target && !attacked)
            {
                const int32_t side = FaceAngleTo(blocker);
                if (side > 0x20 && side < 0x60)
                    SideStep('l');
                else if (side < -0x20 && side > -0x60)
                    SideStep('r');
            }
            if (attacked)
                attackcount--;
            else
            {
                lastattack = nullptr;
                lasthit = 0;
            }
        }
        if (!lastattack || !(lastattack->flags & CA_PLAYANIM))
            nextattack--;
    }
    if (nextattack < 0)
        nextattack = random(chardata->minattackfreq * FRAMERATE / 100, chardata->maxattackfreq * FRAMERATE / 100);
    if (magictimer < 0)
        magictimer = random(chardata->minmagicfreq * FRAMERATE / 100, chardata->maxmagicfreq * FRAMERATE / 100);
}

// AI's move branch (0x004c8f60-0x004c93c9; §3.10.3-3.10.5).
void TCharacter::AIMove(TCharacter* target)
{
    if (retreating && !(charflags & kCharFlagNoTurn))
    {
        ai_lookat = nullptr;
        Go((AngleTo(target) + 0x7f) & 0xff);       // straight away from it
        return;
    }
    if (!IsMoveAction(doing))
    {
      // Standing out of the stance, someone in the way: step aside, and he
      // the other way unless he is the target.
        TCharacter* blocker = nullptr;
        if (accum.x == 0 && accum.y == 0 && Blocked(pos, pos, 8, nullptr, &blocker) && blocker)
        {
            const char side = FaceAngleTo(blocker) >= 0 ? 'r' : 'l';
            SideStep(side);
            if (blocker != Fighting())
                blocker->SideStep(side == 'l' ? 'r' : 'l');
        }
        return;
    }

    if (Distance(target) < chardata->maxattackrange && !ai_lookat)
    {
        Stop();
        return;
    }
    if (lastattack && doing->Is(lastattack->attackname) && (lastattack->flags & CA_PLAYANIM))
        return;

    const auto heard = [&] {
        if (target->flags & OF_INVISIBLE)
            return false;
        const int32_t dist = Distance(target);
        return dist <= chardata->hearingrange && target->LastNoise() > 100 - Hearing(dist);
    };
    if (CanSeeCharacter(target, -1) || heard())
    {
      // Chase: a committed waypoint holds a few ticks; face it, or the target.
        if (ai_lookat)
        {
            if (waypointticks == 0)
                ai_lookat = nullptr;
            else
                waypointticks--;
        }
        target_last_position = target->Pos();
        doing->angle = AngleTo(ai_lookat && waypointticks ? ai_lookat : target);
        return;
    }

  // Lost: if the target is fighting someone else in sight, both step
  // aside; then the waypoint nearest the target's last known place (or,
  // the committed one reached, its place now), or that place itself.
    S3DPoint centre = target_last_position;
    TCharacter* other = target->Fighting();
    if (other && other != this && CanSeeCharacter(other, -1))
    {
        const char side = random(0, 1) ? 'l' : 'r';
        SideStep(side);
        other->SideStep(side == 'l' ? 'r' : 'l');
    }
    if (!ai_lookat || Distance(ai_lookat) < 5)
    {
        if (ai_lookat)
            centre = target->Pos();
        TObjectInstance* waypoint = NearestWaypoint(centre);
        if (ai_lookat == waypoint)
            ai_lookat = nullptr;
        else
        {
            waypointticks = 6;
            ai_lookat = waypoint;
        }
    }
    doing->angle = ai_lookat ? AngleTo(ai_lookat) : ConvertToFacing(pos, centre);
}

// AI's waypoint search (0x004c9158-0x004c9272): of the first ten helpers
// within 250 of `centre`, the "waypoint"s in line of sight
// (WaypointReachable 0x00528850), the nearest to it under 1000 -- else the
// first. (Each new nearest also becomes the next move, SetNextMove
// 0x00470bc0, with the waypoint's position as the vector, as retail
// does.)
TObjectInstance* TCharacter::NearestWaypoint(const S3DPoint& centre)
{
    constexpr int32_t kMaxFound = 10;
    TObjectInstance* found[kMaxFound] = {};
    int32_t n;
    if (waypointsSeam)
        n = waypointsSeam(this, centre, found, kMaxFound);
    else
    {
        int32_t index[kMaxFound];
        n = (std::min)(MapPane.FindObjectsInRange(centre, GetLevel(), index, 250, 0, OBJCLASS_HELPER, kMaxFound,
                                                  OBJSET_ALL), kMaxFound);
        for (int32_t i = 0; i < n; ++i)
            found[i] = MapPane.GetInstance(index[i]);
    }
    n = (std::min)(n, kMaxFound);
    TObjectInstance* waypoints[kMaxFound];
    int32_t count = 0;
    for (int32_t i = 0; i < n; ++i)
    {
        TObjectInstance* o = found[i];
        if (o && !stricmp(o->GetTypeName(), "waypoint") && WaypointReachable(o, this))
            waypoints[count++] = o;
    }
    if (count == 0)
        return nullptr;
    int32_t best = 0, bestdist = 1000;
    for (int32_t i = 0; i < count; ++i)
    {
        S3DPoint at = waypoints[i]->Pos();
        const int32_t d = ::Distance(centre, at);
        if (d < bestdist)
        {
            bestdist = d;
            best = i;
            SetNextMove(at);
        }
    }
    return waypoints[best];
}

// REVSYNC: AI_PerMonster @ 0x004c9b70 -- the boss kind, by type name, once
// (+0x280: Baez 1, Solifuge 2, Jhaga 3, Yhagoro 4, any other -1), a boss
// with a fresh state (+0x284); then the boss's own tick (the switch at
// 0x004c9ce1; src/bossai.cpp, docs/gameplay/forensics/BOSS_AI.md). AI
// ignores what retail returns (1 for a boss).
void TCharacter::AIPerMonster()
{
    if (monsterkind == 0)
    {
        const char* type = GetTypeName();
        monsterkind = -1;
        if (!stricmp(type, "Baez"))
        {
            monsterkind = kMonsterBaez;
            bossstate.emplace<SBaezState>();
        }
        else if (!stricmp(type, "Solifuge"))
        {
            monsterkind = kMonsterSolifuge;
            bossstate.emplace<SSolifugeState>();
        }
        else if (!stricmp(type, "Jhaga"))
        {
            monsterkind = kMonsterJhaga;
            bossstate.emplace<SJhagaState>();
        }
        else if (!stricmp(type, "Yhagoro"))
        {
            monsterkind = kMonsterYhagoro;
            bossstate.emplace<SYhagoroState>();
        }
    }
    switch (monsterkind)
    {
        case kMonsterBaez:
            BossBaez(BossStateOf<SBaezState>(bossstate));
            break;
        case kMonsterSolifuge:
            BossSolifuge(BossStateOf<SSolifugeState>(bossstate));
            break;
        case kMonsterJhaga:
            BossJhaga(BossStateOf<SJhagaState>(bossstate));
            break;
        case kMonsterYhagoro:
            BossYhagoro(BossStateOf<SYhagoroState>(bossstate));
            break;
        default:
            break;
    }
}

// REVSYNC: Wander @ 0x004c9790 (slot 0x234; COMBAT_ATTACK_CHOICE.md §3.15).
// Pulse runs it before the AI for a character with charflags 0x100000:
// retail's automatic network player, the port's --playerai.
void TCharacter::Wander()
{
    if (wanderidle)
    {
        --wanderidle;
        return;
    }

  // A run in a fighting root becomes a walk, and the fight goes on in the
  // combat root (action 3 even from a bow root, 0x004c9894).
    if (root)
    {
        const bool fighting = IsRoot(ACTION_COMBAT) || IsRoot(ACTION_BOW);
        const bool running = (IsRoot(ACTION_COMBAT) && (root->Is("combatrun") || root->Is("handrun"))) ||
                             (IsRoot(ACTION_BOW) && root->Is("bowrun")) || root->Is("run");
        if (running && fighting && root->obj)
        {
            SetWalkMode();
            TObjectInstance* opponent = IsRoot(ACTION_COMBAT) || IsRoot(ACTION_BOW) ? root->obj : nullptr;
            BeginFighting(static_cast<TCharacter*>(opponent), ACTION_COMBAT);
        }
    }

  // In a combat root: with no one to fight, leave the fight and rest 100
  // ticks; retreating, now and then a run of 800 to 2000 ticks.
    if (IsRoot(ACTION_COMBAT))
    {
        if (!root->obj)
        {
            EndFighting();
            wanderidle = 100;
        }
        else if (retreating)
        {
            if (!runticks)
            {
                if (random(1, 100) > 30)
                {
                    runticks = random(800, 2000);
                    SetRunMode();
                }
            }
            else if (!--runticks)
                SetWalkMode();
        }
        return;
    }
    WanderRunning();
    WanderPatrol();
}

// Wander out of a combat root (0x004c98ba..0x004c99a9): at full fatigue a
// run burst one tick in a hundred, of 100 to 800 ticks; a run ends early
// half the times it's tired (under half its fatigue), and most times in a
// bow fight (the combat root was left above).
void TCharacter::WanderRunning()
{
    if (!runticks)
    {
        const int32_t full = MaxFatigue();
        if (Fatigue() == full && random(1, 100) == 100)
        {
            runticks = random(100, 800);
            SetRunMode();
        }
        return;
    }
    --runticks;
    const int32_t half = MaxFatigue() / 2;
    if (Fatigue() < half && random(1, 100) > 50)
        runticks = 0;
    else if (IsRoot(ACTION_BOW) && root->obj && random(1, 100) > 15)
        runticks = 0;
    if (!runticks)
        SetWalkMode();
}

// Wander's patrol (0x004c99ae..0x004c9a6e): with no waypoint, pick one and
// walk there (none: rest 1000 ticks); with one, walk on, and within 25 of
// it (the octagonal distance, height included) pick the next, the one
// reached becoming the previous.
void TCharacter::WanderPatrol()
{
    if (!ai_lookat)
    {
        ai_lookat = PickWanderWaypoint();
        if (ai_lookat)
            Goto(ai_lookat->Pos().x, ai_lookat->Pos().y);
        else
            wanderidle = 1000;
        return;
    }
    Goto(ai_lookat->Pos().x, ai_lookat->Pos().y);
    const S3DPoint at = ai_lookat->Pos();
    if (dist(pos.x, pos.y, pos.z, at.x, at.y, at.z) < 25)
    {
        TObjectInstance* reached = ai_lookat;
        ai_lookat = PickWanderWaypoint();       // passes over the one reached and the one before it
        lastwaypoint = reached;
    }
}

// REVSYNC: 0x004c95e0 -- of the first ten helpers within 2000, the
// "waypoint"s that are neither this leg's nor the last one's, can see here
// (WaypointReachable 0x00528850) and pass ProbeLedge; then the one most
// nearly ahead (the first of equals, the first when none is within 0x80)
// six times in ten, else any of them. None: the previous waypoint.
TObjectInstance* TCharacter::PickWanderWaypoint()
{
    constexpr int32_t kMaxFound = 10;
    int32_t index[kMaxFound];
    const int32_t n = (std::min)(MapPane.FindObjectsInRange(pos, GetLevel(), index, 2000, 0, OBJCLASS_HELPER,
                                                            kMaxFound, OBJSET_ALL), kMaxFound);
    TObjectInstance* const previous = lastwaypoint.Get();
    TObjectInstance* kept[kMaxFound];
    int32_t count = 0;
    for (int32_t i = 0; i < n; ++i)
    {
        TObjectInstance* o = MapPane.GetInstance(index[i]);
        if (o && !stricmp(o->GetTypeName(), "waypoint") && o != previous && o != ai_lookat &&
            WaypointReachable(o, this) && ProbeLedge(o->Pos()))
            kept[count++] = o;
    }
    if (count == 0)
        return previous;

    int32_t best = 0;
    for (int32_t i = 0, bestdiff = 0x80; i < count; ++i)
    {
        const int32_t diff = abs(FaceAngleTo(kept[i]));
        if (diff < bestdiff)
        {
            bestdiff = diff;
            best = i;
        }
    }
    return random(1, 100) < 60 ? kept[best] : kept[random(0, count - 1)];
}

// REVSYNC: 0x004c9460 -- the walk heights (this level's) from here, a step
// at a time AWAY from `to`, each step an eighth of the way between, as
// many steps as the distance (octagonal) holds radii: true when two in a
// row differ by 32 or more. PickWanderWaypoint keeps a waypoint only then:
// on open ground none passes (as retail; the direction and the test read as
// the reverse of what the source meant). A radius below 1 takes no step
// (retail would divide by zero).
bool TCharacter::ProbeLedge(const S3DPoint& to)
{
    constexpr int32_t kLedge = 32;
    const int32_t stepx = (pos.x - to.x) / 8;
    const int32_t stepy = (pos.y - to.y) / 8;
    const int32_t radius = Radius();
    int32_t steps = radius > 0 ? dist(pos.x, pos.y, to.x, to.y) / radius : 0;
    S3DPoint next(pos.x + stepx, pos.y + stepy, pos.z);
    int32_t h = MapPane.GetWalkHeight(pos, GetLevel());
    int32_t hnext = MapPane.GetWalkHeight(next, GetLevel());
    for (; steps > 0; --steps)
    {
        if (abs(h - hnext) >= kLedge)
            return true;
        h = hnext;
        next.x += stepx;
        next.y += stepy;
        hnext = MapPane.GetWalkHeight(next, GetLevel());
    }
    return abs(h - hnext) >= kLedge;
}

bool TCharacter::CanHearCharacter(TCharacter* chr)
{
    if (chr->flags & OF_INVISIBLE)
        return false;
    const int32_t dist = Distance(chr);
    const int32_t noise = chr->LastNoise();
    return dist <= chardata->hearingrange && noise > 100 - Hearing(dist);
}

// REVSYNC: CanSeeCharacter @ 0x004cd540 -- eye to eye (LIGHTINGCHARHEIGHT
// above each position), centre to centre: within SIGHTRANGE, within
// SIGHTANGLE of `angle` (the facing below 1), in line of sight, and lit
// enough -- the glimpse he gave off (always seen with infravision, reversed
// when light-blind) at least 100 less this sight (Sight). Never an
// invisible object.
bool TCharacter::CanSeeCharacter(TCharacter* chr, int32_t angle)
{
    if (canSeeSeam)
        return canSeeSeam(this, chr, angle);
    if (chr->flags & OF_INVISIBLE)
        return false;
    S3DPoint from = pos, to = chr->Pos();
    from.z += LIGHTINGCHARHEIGHT;
    to.z += LIGHTINGCHARHEIGHT;
    if (angle < 1)
        angle = GetFace();
    const int32_t dist = ::Distance(from, to);
    const int32_t anglediff = abs(AngleDiff(angle, AngleTo(chr)));

    int32_t glimpse = chr->LastGlimpse();
    if (chardata->flags & CF_INFRAVISION)
        glimpse = 10000; // Always sees
    else if (chardata->flags & CF_LIGHTBLIND)
        glimpse = 100 - glimpse; // Reverse glimpse value so more light is less visible!

    return dist <= chardata->sightrange && anglediff <= chardata->sightangle &&
           MapPane.LineOfSight(from, to, GetLevel()) && glimpse >= 100 - Sight(dist);
}

// Finds characters in range, with closest guy at head of list
// REVSYNC: TCharacter::Distance @ 0x004d61b0 (slot 4) -- edge to edge.
int32_t TCharacter::Distance(const TObjectInstance* inst) const
{
    auto* self = const_cast<TCharacter*>(this);         // Radius() is a stat accessor
    int32_t d = ::Distance(pos, inst->Pos()) - self->Radius();
    if (inst->IsCharacter())
        d -= const_cast<TCharacter*>(static_cast<const TCharacter*>(inst))->Radius();
    return (std::max)(d, 0);
}

// REVSYNC: IsValidTarget @ 0x004cd990 -- single player (the network branch,
// a player-state bit 4 test, isn't ported).
bool TCharacter::IsValidTarget(TCharacter* target)
{
    if (!target)
        return false;
    if (target->charflags & kCharFlagNotTargetable)
        return false;
    if (target->Health() <= 0)
        return false;
    if (target->IsInvisibleSpell())
        return false;
    if (target->flags & OF_INVISIBLE)
        return false;
    if (Distance(target) > chardata->combatrangemax)
        return false;
    if (PlayScreen.IsControlOn() || PlayScreen.IsDemoMode())
        return true;
    return target->ObjClass() != OBJCLASS_PLAYER;      // a script holds control: not the player
}

// REVSYNC: FindCharacters @ 0x004cd690 -- the characters near me, in map
// order, the best at the head. The range is at least the hearing range when
// listening and the sight range when looking (and when below 0). Not me,
// nor one under the invisibility spell. Looking for enemies, one who isn't
// is marked as seen and passed over, and an invalid target passed over.
// Listening or looking, one neither heard nor seen is kept only if I
// remember him (HasSeenMe); one heard or seen is remembered. With an angle,
// only those within `anglerange` of it, the score the edge distance times
// how far off it they are (plus 1); without, the edge distance alone. A
// better (or equal) score goes to the head, the one it displaces to the
// end -- but the first found is never scored: the best starts at 10000.
int32_t TCharacter::FindCharacters(TCharacter* chars[], int32_t maxchars, int32_t range, int32_t angle,
                                   int32_t anglerange, int32_t flags)
{
    if (findCharactersSeam)
        return findCharactersSeam(this, chars, maxchars, range, angle, anglerange, flags);

    if (maxchars < 1)
        return 0;

    chars[0] = nullptr;
    if (range < 0 || (flags & FINDCHAR_HEAR))
        range = max(range, chardata->hearingrange);
    if (range < 0 || (flags & FINDCHAR_SEE))
        range = max(range, chardata->sightrange);

    int32_t numchars = 0;
    int32_t best = 10000;
    ForCharactersNear(GetLevel(), pos, range, [&](TCharacter* chr) {
        if (chr == this || chr->IsInvisibleSpell())
            return true;
        if (flags & FINDCHAR_ENEMY)
        {
            if (!IsEnemy(chr))
            {
                SetHasSeen(chr);
                return true;
            }
            if (!IsValidTarget(chr))
                return true;
        }
        int32_t score = Distance(chr);
        if (score > range)
            return true;
        const int32_t angleto = AngleTo(chr);

        const bool hear = !(flags & FINDCHAR_HEAR) || CanHearCharacter(chr);
        const bool see = !(flags & FINDCHAR_SEE) || CanSeeCharacter(chr, angle);
        if (flags & (FINDCHAR_HEAR | FINDCHAR_SEE))
        {
            if (hear || see)
                SetHasSeen(chr);
            else if (!HasSeenMe(chr))
                return true;
        }

        if (angle >= 0)
        {
            const int32_t diff = abs(AngleDiff(angle, angleto));
            if (diff > anglerange)
                return true;
            score *= anglerange - diff + 1;
        }

        if (chars[0] && score <= best)
        {
            std::swap(chars[0], chr);
            best = score;
        }
        if (numchars < maxchars)
            chars[numchars++] = chr;
        return true;
    });
    return numchars;
}

TCharacter* TCharacter::FindCharacter(int32_t range, int32_t angle, int32_t anglerange, int32_t flags)
{
    TCharacter* chr;
    int32_t numchars = FindCharacters(&chr, 1, range, angle, anglerange, flags);
    if (numchars > 0)
        return chr;
    else
        return nullptr;
}

TCharacter* TCharacter::FindCharacterAhead(int32_t angle, int32_t anglerange)
{
    return FindCharacter(512, angle, anglerange, 0);
}

TCharacter* TCharacter::FindClosestEnemy(int32_t angle, int32_t anglerange)
{
    return FindCharacter(-1, angle, anglerange, FINDCHAR_ENEMY | FINDCHAR_SEE | FINDCHAR_HEAR);
}

// REVSYNC: Hearing @ 0x004cda80 (slot 0x2e0) -- 0 beyond HEARINGRANGE (the
// distance less my radius and 32), else 100: the scaling (a draw of -2..2
// on 100, times the distance over the range, over 100; halved asleep) is
// then held to at most 0 and floored at 100 -- a clamp to 0..100 turned
// inside out. So anything in range making any noise is heard. (The 1998
// source scaled from HEARINGMAX close to HEARINGMIN at the range; retail
// reads neither.)
int32_t TCharacter::Hearing(int32_t dist)
{
    dist = max(0, dist - Radius() - 32);
    if (dist > chardata->hearingrange)
        return 0;

    int32_t hearing = (random(-2, 2) + 100) * dist / chardata->hearingrange / 100;
    if (Sleeping())
        hearing /= 2;
    hearing = min(hearing, 0);
    return hearing > 100 ? hearing : 100;
}

// REVSYNC: Sight @ 0x004cdb30 (slot 0x2e4) -- 0 asleep or beyond SIGHTRANGE
// (centre to centre), else a draw of -2..2 on 100, times the distance over
// the range, over 100, held to 0..100: 0 or 1. So CanSeeCharacter wants a
// glimpse of 99 or more. (The 1998 source scaled from SIGHTMAX close to
// SIGHTMIN at the range, less the radius and 32; retail reads neither.)
int32_t TCharacter::Sight(int32_t dist)
{
    dist = max(dist, 0);
    if (Sleeping() || dist > chardata->sightrange)
        return 0;

    return std::clamp((random(-2, 2) + 100) * dist / chardata->sightrange / 100, 0, 100);
}

// Resets the noise and glimpse values to control whether monsters see you or not
// REVSYNC: ResetStealthValues @ 0x004cdbb0 -- the noise and the glimpse
// this tick's action gives off: 100 for an attack, else 70, halved when
// sneaking, scaled by a draw of 1..25; the glimpse also by the visibility
// plus that draw (10..100).
void TCharacter::ResetStealthValues()
{
    int32_t loud = (doing && doing->action == ACTION_ATTACK) ? 100 : 70;
    if (root && root->Is("sneak"))
        loud /= 2;
    const int32_t r = random(1, 25);
    noise = 2 * r * loud / 100;
    const int32_t seen = std::clamp(Visibility() + r, 10, 100);
    glimpse = std::clamp(seen * loud / 100, 0, 100);
}

void TCharacter::SignalMovement(TObjectInstance* actor)
{
}

void TCharacter::SignalHostility(TObjectInstance* actor, TObjectInstance* target)
{
    if (target != this)
        return;
}


void TCharacter::SetOnExit()
{
    SetFlag(OF_ONEXIT, true);
    exittimestamp = CurrentScreen->FrameCount();
}

// ***********************************************************************************
// * Access functions - called by script or player to make the character do whatever *
// ***********************************************************************************

// REVSYNC: TCharacter::Go @ 0x004ce350 -- walk in direction `angle` (held
// input, AI and Goto all come here). docs/gameplay/forensics/COMBAT_MOVEMENT.md
// section 2. In a combat or bow root (and not running) it is the orbit:
// the facing stays on the target (CombatFace, monsters always) and the step
// animation is the angle between moving and facing; a player never pivots.
// Elsewhere it walks: monsters never pivot, a player does past 45/90 degrees.
// On success the root keeps the held angle.
bool TCharacter::Go(int32_t angle)
{
    const ACTION moveaction = GetMoveAction(root->action);
    bool made = false;                     // the block is ours to free if refused

    if (ObjClass() == OBJCLASS_PLAYER)
    {
        auto* player = static_cast<TPlayer*>(this);
        if (player->PlayerState() & 2)
            player->SetPlayerState(player->PlayerState() & ~2);
    }

  // An interactive move (charflags 0x80000) skips the gates.
    if (!(charflags & kCharFlagInteractive))
    {
        if (!doing || (doing->action != root->action && doing->action != moveaction) || Health() <= 0)
            return false;
        if (doing->attack && (doing->attack->flags & CA_INTERACTIVE))
            return false;
        if (doing->impact && (doing->impact->flags & CAI_INTERACTIVE))
            return false;
    }

  // Combat or bow root, not running: the orbit path.
    const bool combat = (root->action == ACTION_COMBAT || root->action == ACTION_BOW) &&
        !(root->action == ACTION_COMBAT && (root->Is("combatrun") || root->Is("handrun"))) &&
        !(root->action == ACTION_BOW && root->Is("bowrun")) &&
        !root->Is("run");

  // Look 4 ahead before stepping off: always in combat with CombatFace, else
  // only when already (nearly) facing the way.
    if ((combat && CombatFace) || std::abs(AngleDiff(GetFace(), angle)) < 16)
    {
        S3DPoint ahead;
        ConvertToVector(angle, 4, ahead);
        ahead += pos;
        if (Blocked(pos, ahead))
            return false;
    }

  // Whoever is in the direction held (it may become the target).
    TCharacter* found = nullptr;
    if (FindCharacters(&found, 1, -1, angle, 32, FINDCHAR_ENEMY | FINDCHAR_HEAR | FINDCHAR_SEE) < 1)
        found = nullptr;

    TActionBlock* ab = nullptr;

    if (retreating)
    {
      // Retreating: turn toward the direction held at a speed scaled to
      // the turn (retail's float, 0x004ceb13).
        const int32_t diff = std::abs(AngleDiff(GetFace(), angle));
        auto scaled = [diff](int32_t rate) {
            return (int32_t)((double)rate / ((double)diff * (double)(1.0f / 127.0f)) * (double)2.2f);
        };
        if (diff < 32 || IsMoveAction(doing))
        {
            if (IsMoveAction(doing))
            {
                doing->moveangle = doing->angle = angle;
                doing->turnrate = scaled(StepTurnRate(diff));
                return true;
            }
            // Standing, nearly facing: back to the root (ab stays null).
        }
        else
        {
            ab = new TActionBlock(root->name, moveaction);
            ab->waitpivot = true;
            ab->turnrate = scaled(StepTurnRate(diff));
            ab->angle = ab->moveangle = angle;
            ab->interrupt = true;
            ab->noroot = true;
            made = true;
        }
    }
    else if (combat)
    {
        if (IsMoveAction(doing) && doing->moveangle == angle)
            return true;

      // A new target in the direction held, as retail decides it (it only
      // looks while the current target is valid; it keeps the current one
      // when the new one is 48 or more away and no nearer).
        auto* targ = static_cast<TCharacter*>(doing->obj);
        if (found && found != targ && IsValidTarget(targ))
        {
            if (!targ || Distance(found) < 48 || Distance(found) < Distance(targ))
            {
                SetFighting(found);
                targ = found;
            }
        }

        int32_t face = angle;
        if (targ && IsValidTarget(targ) && (CombatFace || ObjClass() == OBJCLASS_CHARACTER) &&
            !retreating)
            face = AngleTo(doing->obj);
        const int32_t face8 = (face + 15) & 0xe0;

        if (doing->action != root->action && doing->action != moveaction)
            return false;

        char animname[RESNAMELEN];
        GetAngleMoveAnim(angle, face, root->name, animname, RESNAMELEN);
        const int32_t diff = std::abs(AngleDiff(GetFace(), face8));

        if (ObjClass() == OBJCLASS_PLAYER || (diff <= 64 && (doing->action == moveaction || diff <= 32)))
        {
            ab = doing;                    // already stepping: turn this step
            if (doing == root)
            {
                ab = new TActionBlock(animname, moveaction);
                ab->waitpivot = false;
                made = true;
            }
        }
        else
        {
            ab = new TActionBlock(root->name, moveaction);
            ab->waitpivot = true;          // a monster turns first
            made = true;
        }
        if (doing)
            ab->obj = doing->obj;
        ab->moveangle = angle;
        ab->turnrate = StepTurnRate(diff);
        ab->angle = face;                  // unrounded
    }
    else
    {
        if (doing->action == moveaction && doing->moveangle == angle && doing->angle == angle)
            return true;

        const int32_t diff = std::abs(AngleDiff(GetFace(), angle));
        if (IsMoveAction(doing) && diff <= 64)
        {
            doing->moveangle = doing->angle = angle;
            doing->turnrate = StepTurnRate(diff);
            return true;
        }

        if (ObjClass() == OBJCLASS_CHARACTER || (diff <= 64 && (doing->action == moveaction || diff <= 32)))
        {
            for (const char* step : {"f", "l", "r"})
                if (HasActionAni(StName(root->name, step)))
                {
                    ab = new TActionBlock(StName(root->name, step), moveaction);
                    break;
                }
            if (!ab)
                return false;
        }
        else
        {
            ab = new TActionBlock(root->name, moveaction);
            ab->waitpivot = true;          // a player turns first
            ab->turnrate = StepTurnRate(diff);
        }
        made = true;
        SetMoveAngle(GetFace());
        ab->obj = doing->obj;
        ab->moveangle = ab->angle = angle;
    }

    if (ab && !(retreating))
    {
        ab->interrupt = true;
        ab->noroot = true;
        if (!HasActionAni(ab->name))
        {
            if (ab != doing && ab != desired && ab != root)
                delete ab;
            return false;
        }
    }

    if (!SetDesired(ab) && made && ab)
        delete ab;
    root->moveangle = root->angle = angle;
    return true;
}

bool TCharacter::Go(S3DPoint vect)
{
    S3DPoint zero;
    memset(&zero, 0, sizeof(S3DPoint));
    int32_t angle = ConvertToFacing(zero, vect);
    return Go(angle);
}

// REVSYNC: Goto @ 0x004cedb0 -- start walking toward (x, y) (Go), then give
// the walk its target, which ResolveMove walks to and snaps onto within 8.
// The target goes on the block Go queued, the desired one, or on the one
// being done when nothing is queued (desired is the root): the walk started
// at once, or Go turned the current step. The block is marked as a Goto's walk
// (retail +0x60 bit 0x1000) and an item given (+0x288, kept when none is) is
// picked up when that walk arrives (ResolveMove 0x004c6155, and in combat
// ResolveCombatMove 0x004c7fc3, where ResolveCombat 0x004c7980 also stops
// facing the target): the map pane's walk to an item out of reach.
bool TCharacter::Goto(int32_t x, int32_t y, TObjectInstance* pickup)
{
    const int32_t angle = ConvertToFacing(pos, S3DPoint(x, y, pos.z));
    if (!Go(angle))
        return false;

    TActionBlock* ab = desired != root ? desired : doing;
    ab->target = S3DPoint(x, y, pos.z);
    ab->walkto = true;
    if (pickup)
        gotoitem = pickup;
    return true;
}

// REVSYNC: Stop @ 0x004cee70 -- end a walk, a step or a pivot (or the
// named action): the root, interrupting, takes the doing block's angles and
// becomes desired (with incidentals off, its 100% variant). The player also
// lets go of every held control and of the right-button walk (the map
// pane's right button up). The network notify is inert offline.
bool TCharacter::Stop(char *name)
{
    if (!IsMoving() && !IsDoing(ACTION_PIVOT) &&
        (!name || !doing->Is(name)))                                   // Is a use specified command
        return false;

    root->interrupt = true;
    root->angle = doing->angle;
    root->moveangle = doing->moveangle;

    SetDesired(root, Incidentals() ? 0 : kCommandNoIncidentals);

    if (this == static_cast<TCharacter*>(Player))
    {
        ControlMap.ReleaseAll();
        MapPane.PlayMouseClick(MB_RIGHTUP, 0, 0);
    }
    return true;
}

bool TCharacter::Disable()
{
    SetFlags(OF_DISABLED);
    if (doing != root)
        ForceCommand(root);
    return true;
}

// Causes character to jump (in normal mode, use Leap in Combat mode)
bool TCharacter::Jump()
{
    return false;
}

// REVSYNC: TCharacter::SetWalkMode @ 0x004cf000 -- the walking root of the
// mode the character is in: the combat or bow root, or the walk root
// (GetWalkRoot: "torch" for a player holding a light). Not while held in an
// interactive move, or dead. A root that is a walking one and the walk root
// stays; any other (a run, a sneak, "walk" with a torch now held or "torch"
// without, and the combat and bow roots every time) is rebuilt as a copy of
// the old root under the new name, a step in progress turned into its step,
// and started at once. Not ported (multiplayer): the network message.
bool TCharacter::SetWalkMode()
{
    if (setWalkModeSeam)
        return setWalkModeSeam(this);
    if (InteractiveLocked() || Health() <= 0)
        return false;
    if (IsWalkMode() && root->Is(GetWalkRoot()))
        return true;

    char aniname[RESNAMELEN];
    switch (root->action)
    {
      case ACTION_COMBAT:  strcpy(aniname, GetCombatRoot()); break;
      case ACTION_BOW:     strcpy(aniname, GetBowRoot());    break;
      case ACTION_ANIMATE: strcpy(aniname, GetWalkRoot());   break;
      default:             return false;    // retail read a name never set here
    }
    if (!HasActionAni(aniname))
        return false;

    TActionBlock* ab = new TActionBlock(*root, aniname, root->action);
    SetRoot(ab);

    if (IsMoveAction(doing))    // a step under way becomes the new root's
    {
        const char* name = HasActionAni(StName(aniname, "r")) ? StName(aniname, "r") : StName(aniname, "f");
        ab = new TActionBlock(*doing, name, doing->action);
    }

    ab->interrupt = true;
    SetDesired(ab);

    return true;
}

// Sets sneak mode
bool TCharacter::SetSneakMode()
{
    if (IsSneakMode())
        return true;

    TActionBlock* ab = new TActionBlock("sneak");
    SetRoot(ab);

    if (doing && doing->action == ACTION_MOVE)      // If moving, change next step to new root
    {
        char *name;
        if (HasActionAni("sneakr"))
            name = "sneakr";
        else
            name = "sneakf";
        ab = new TActionBlock(*doing, name, ACTION_MOVE);
    }

    ab->interrupt = true;
    SetDesired(ab);

    return true;
}

// Sets run mode
bool TCharacter::SetRunMode()
{
    if (setRunModeSeam)
        return setRunModeSeam(this);
    if (IsRunMode())
        return true;

    char aniname[RESNAMELEN];
    if (root->Is(ACTION_ANIMATE))
        strcpy(aniname, "run");
    else if (root->Is(ACTION_COMBAT))
        strcpy(aniname, StName(GetCombatRoot(), "run"));
    else if (root->Is(ACTION_BOW))
        strcpy(aniname, StName(GetBowRoot(), "run"));

    if (!HasActionAni(aniname))
        return false;

    TActionBlock* ab = new TActionBlock(aniname, root->action);
    SetRoot(ab);

    if (doing && IsMoving())        // If moving, change next step to new root
    {
        char *name;
        if (HasActionAni(StName(aniname, "r")))
            name = StName(aniname, "r");
        else
            name = StName(aniname, "l");
        ab = new TActionBlock(*doing, name, doing->action);
    }

    ab->interrupt = true;
    SetDesired(ab);

    return true;
}

// Attempt to play a pivot animation towards the given delta angle
bool TCharacter::Pivot(int32_t angle)
{
    if (IsDoing(ACTION_PIVOT) && doing->angle == angle)
        return true;

    if (doing != root && !IsMoving())
        return false;

    angle = angle & 255;

    if (angle == GetFace())
        return true;

    char buf[30];

  // Get name of pivot animation for this direction
    int32_t anglediff = AngleDiff(GetFace(), angle);
    int32_t absdiff = abs(anglediff);
    strcpy(buf, root->name);
    strcat(buf, "pivot");
    if (anglediff == 32)
        strcat(buf, "fl");
    else if (anglediff == -32)
        strcat(buf, "fr");
    else if (anglediff == 64)
        strcat(buf, "l");
    else if (anglediff == -64)
        strcat(buf, "r");
    else if (anglediff == 96)
        strcat(buf, "bl");
    else if (anglediff == -96)
        strcat(buf, "br");
    else if (anglediff == 128)
        strcat(buf, "al");
    else if (anglediff == -128)
        strcat(buf, "ar");

  // If no real pivot animation, just turn the root state
    int32_t turnrate = MAXTURNRATE;
    if (!HasActionAni(buf))
    {
        strcpy(buf, root->name);
      // Make turn faster further around he goes
        turnrate = MAKETURNRATE(absdiff);
    }

    TActionBlock* ab = new TActionBlock(buf, ACTION_PIVOT);
    ab->angle = ab->moveangle = angle;
    ab->turnrate = turnrate;
    ab->interrupt = true;
    ab->noroot = true;
    SetDesired(ab);

    return true;
}

// This obviously does nothing right now
bool TCharacter::FollowChar(TObjectInstance* inst)
{
    return false;
}

// The pickup line retail builds in 0x004cfef0 (see Pickup).
static std::string PickupLine(TObjectInstance* inst)
{
    const std::string name = ShownName(inst->GetTypeName());
    const char* verb = DialogList.GetLine("CONTPICKUP");
    const int32_t objclass = inst->ObjClass();
    char line[256];

    if (objclass == OBJCLASS_MONEY || objclass == OBJCLASS_AMMO)
    {
        const bool money = objclass == OBJCLASS_MONEY;
        const char* full = money ? "FULLMONEYPICKUP" : "FULLAMMOPICKUP";
        const char* reversed = money ? "FULLMONEYPICKUPREV" : "FULLAMMOPICKUPREV";
        const int32_t amount = inst->Amount();
        if (DialogList.FindLine(full) >= 0)
            snprintf(line, sizeof(line), DialogList.GetLine(full), amount, name.c_str());
        else if (DialogList.FindLine(reversed) >= 0)
            snprintf(line, sizeof(line), DialogList.GetLine(reversed), name.c_str(), amount);
        else
            snprintf(line, sizeof(line), "%i %s %s.", amount, name.c_str(), verb);
    }
    else if (DialogList.FindLine("FULLCONTPICKUP") >= 0)
        snprintf(line, sizeof(line), DialogList.GetLine("FULLCONTPICKUP"), name.c_str());
    else
        snprintf(line, sizeof(line), "%s %s.", name.c_str(), verb);
    return line;
}

// REVSYNC: Pickup @ 0x004cfef0 -- `to` (this character when null) takes the
// item into its inventory (AddToInventory 0x0046f3d0, vtable 0x58). The main
// player is told what it picked up first: money (class 8) and ammo (21) with
// their amount (FULLMONEYPICKUP / FULLAMMOPICKUP with amount and name, else
// their ...REV form with name and amount, else "%i %s %s." with CONTPICKUP),
// anything else by name (FULLCONTPICKUP, else "%s %s."); the shipped
// english.def has only CONTPICKUP ("Picked up"): "Short Sword Picked up.".
// Then the item's class sound. The item leaves the map before the add
// (RemoveFromMap, vtable 0x90). When `to` can't take it (no free slot: a
// pile it could join doesn't help, GRIND.md §4.3), "CARRYFULL2" shows and
// the item is lost: retail leaves it off the map and in no inventory, the
// port deletes it. Not ported: the multiplayer request (0x005845c0 /
// 0x00584500) and the item's vtable 0x144 hook (empty in the base).
bool TCharacter::Pickup(TObjectInstance* inst, TObjectInstance* to)
{
    if (!inst || !inst->IsInventoryItem() || inst == this)
        return false;
    if (!to)
        to = this;

    if (this == Player)
        TextBar.Print("%s", PickupLine(inst).c_str());
    inst->PlayItemSound();                  // 0x004d0176

    inst->RemoveFromMap();
    if (!to->AddToInventory(inst))
    {
        TextBar.Print("%s", DialogList.GetLine("CARRYFULL2"));
        log_debug("[inv] %s can't carry %s: it is lost", to->GetName(), inst->GetName());
        MapPane.DeleteObject(inst);
    }
    return true;
}

// REVSYNC: DropLoot @ 0x004d6570 (docs/gameflow/forensics/DEATH_LOOT.md §2).
// A bag named "Swag Bag" comes out to the top of the inventory (it stays with
// the body: it can't be traded); the "gold" sound plays; then each thing at
// the top of the inventory that can be traded leaves it. Gold lands at the
// body's feet, (30, 30) off. Anything else walks out from the body in steps
// of 16 along a random heading, as far as about twice the body's radius
// while the steps stay walkable; a step that isn't, within the body's own
// radius, starts again along another heading. It is thrown there from the
// body (60 up but for armor) by its animator (TObjectAnimator::Toss) and
// makes its own sound. Deviations (DEATH_LOOT.md §8): retail tests each step
// a step late, at the last spot it accepted, so a thing could come to rest
// a step into a wall or over a ledge -- the port tests the step itself;
// retail tried headings without end where every one is walled in close by
// -- the port gives up after kHeadingTries and leaves it at the body; and
// retail ran (and played the sound) every pulse while a dead body held
// anything at all -- the port only when it holds something to drop.
void TCharacter::DropLoot()
{
    constexpr int32_t kGoldOffset   = 30;
    constexpr int32_t kStep         = 16;
    constexpr int32_t kMaxWalkStep  = 0x20;
    constexpr int32_t kThrowHeight  = 60;
    constexpr int32_t kHeadingTries = 32;

    std::vector<TObjectInstance*> loot;
    for (TInventoryIterator i(this); i; i++)
        if (i.Item()->CanTrade())
            loot.push_back(i.Item());
    if (loot.empty())
        return;
    log_info("[loot] %s drops %zu thing(s) at (%d,%d,%d)", GetName(), loot.size(), pos.x, pos.y, pos.z);

    if (TObjectInstance* swag = FindObjInventory("Swag Bag"))
        AddToInventory(swag);

    const int32_t sound = SoundPlayer.FindSound("gold");
    if (sound >= 0 && SoundPlayer.Mount(sound))
        SoundPlayer.Play(sound);

    const S3DPoint body = pos;
    const int32_t radiussteps = Radius() / kStep;
    for (TObjectInstance* item : loot)
    {
        if (!stricmp(item->GetTypeName(), "gold"))
        {
            item->SetPos(S3DPoint{ body.x + kGoldOffset, body.y + kGoldOffset, body.z }, level);
            item->RemoveFromInventory();
            item->AddToMap();
            log_info("[loot] %s at its feet", item->GetName());
            continue;
        }

        S3DPoint drop = body;
        float x = float(body.x), y = float(body.y);
        float dx = 0.0f, dy = 0.0f;
        auto heading = [&]() {
            const double a = double(random(0, 0x100)) * 6.283185307179586 * 0.00390625;
            dx = float(std::cos(a) * kStep);
            dy = float(std::sin(a) * kStep);
        };
        heading();
        int32_t tries = 1;
        for (int32_t n = 0; n <= radiussteps * 2; ++n)
        {
            x += dx;
            y += dy;
            const S3DPoint step{ int32_t(x), int32_t(y), drop.z };
            int32_t maxdelta = 0, height = 0;
            bool hole = false;
            MapPane.GetWalkHeightRadius(step, kStep, maxdelta, height, hole);
            if (std::abs(maxdelta) <= kMaxWalkStep && !hole)
                drop = S3DPoint{ int32_t(x), int32_t(y), height };
            else if (n < radiussteps && tries < kHeadingTries)
            {
                x = float(body.x);
                y = float(body.y);
                drop = body;
                heading();
                ++tries;
                n = 0;                  // retail counts the restart as a step
            }
            else
                break;
        }

        item->SetPos(drop, level);
        item->RemoveFromInventory();
        item->AddToMap();
        if (item->NeedsAnimator())
            item->CreateAnimator();
        S3DPoint from = body;
        if (stricmp(item->GetClassName(), "Armor") != 0)
            from.z += kThrowHeight;
        TObjectAnimator* animator = item->GetAnimator();
        if (animator)
            animator->Toss(from, drop);
        log_info("[loot] %s to (%d,%d,%d)%s", item->GetName(), drop.x, drop.y, drop.z,
                 animator && animator->GetToss() ? ", thrown" : "");
        item->PlayItemSound();
    }
}

bool TCharacter::Pull(TObjectInstance* inst)
{
    TActionBlock* ab = new TActionBlock("Pull Front");
    int32_t state = inst->GetState();
    int32_t direction = (((TLever*)inst)->targetpos.z / 64);

    ab->obj = inst;
    ab->priority = true;    // Won't do it otherwise

    ab->data = malloc(sizeof(int32_t));

    if ((direction == 3) && (state == EXIT_OPEN))
        state = LEVER_PUSHNORTH;
    else if ((direction == 3) && (state == EXIT_CLOSED))
        state = LEVER_PULLSOUTH;
    else if ((direction == 1) && (state == EXIT_OPEN))
        state = LEVER_PULLNORTH;
    else if ((direction == 1) && (state == EXIT_CLOSED))
        state = LEVER_PUSHSOUTH;

    ((int32_t *)ab->data)[0] = state;

    SetDesired(ab);

    return true;
}

namespace {

// What Use and Get look through: up to 10 objects within `reach` of the
// point `ahead` in front of the character, nearest first
// (FindObjectsInRange 0x00452060, objects of any class).
int32_t ObjectsAhead(TCharacter* chr, int32_t ahead, int32_t reach, int32_t (&list)[10])
{
    S3DPoint pos, step;
    chr->GetPos(pos);
    ConvertToVector(chr->GetFace(), ahead, step);
    pos += step;
    return MapPane.FindObjectsInRange(pos, list, reach, 0, -1, 10);
}

} // namespace

// REVSYNC: TCharacter::Use @ 0x004d01e0 (Command 0xe, the Use control:
// GRIND.md §4.2) -- the nearest thing to use 60 ahead, within 60 of that
// point: a container or an exit (classes 5 and 10), or a character that isn't
// this one or its enemy (IsEnemy 0x004c89c0); nothing flagged invisible. The
// main player reads "Talk to <type>" (FULLTALKTO, else TALKTO) or "Use
// <name>" (FULLUSE, else BASEUSE), or USENOTHING. A dead character does
// nothing.
TObjectInstance* TCharacter::FindUseTarget(TObjectInstance* target)
{
    if (Health() < 1)
        return nullptr;

    int32_t list[10];
    const int32_t n = ObjectsAhead(this, 60, 60, list);
    TObjectInstance* best = nullptr;
    int32_t bestdist = 0;
    for (int32_t i = 0; i < n; i++)
    {
        TObjectInstance* oi = MapPane.GetInstance(list[i]);
        if (!oi || (target && oi != target) || (oi->GetFlags() & OF_INVISIBLE))
            continue;

        const int32_t objclass = oi->ObjClass();
        const bool character = objclass == OBJCLASS_PLAYER || objclass == OBJCLASS_CHARACTER;
        if (objclass != OBJCLASS_CONTAINER && objclass != OBJCLASS_EXIT &&
            !(character && oi != this && !IsEnemy(static_cast<TCharacter*>(oi))))
            continue;

        const int32_t dist = Distance(oi);
        if (!best || dist <= bestdist)
        {
            best = oi;
            bestdist = dist;
        }
    }

    return best;
}

bool TCharacter::TryUse()
{
    if (Health() < 1)
        return false;
    TObjectInstance* best = FindUseTarget();
    if (!best)
    {
        if (this == Player)
            TextBar.Print("%s", DialogList.GetLine("USENOTHING"));
        return false;
    }

    if (this == Player)
    {
        const bool talk = best->ObjClass() == OBJCLASS_PLAYER || best->ObjClass() == OBJCLASS_CHARACTER;
        const std::string name = ShownName(talk ? best->GetTypeName() : best->GetName());
        const char* full = talk ? "FULLTALKTO" : "FULLUSE";
        if (DialogList.FindLine(full) >= 0)
            TextBar.Print(DialogList.GetLine(full), name.c_str());
        else
            TextBar.Print("%s %s", DialogList.GetLine(talk ? "TALKTO" : "BASEUSE"), name.c_str());
    }
    best->Use(this, -1);
    return true;
}

// REVSYNC: Get @ 0x004d0430, the Get key (DEATH_LOOT.md §7): the nearest of
// the first ten objects within 80 of the point 40 ahead that offer a cursor
// or can be picked up and aren't invisible, picked up. The main player is
// told "There is nothing to get" (GETNOTHING), or what it takes (FULLGET
// with the name, else BASEGET and the name: "Get Short Sword").
bool TCharacter::TryGet()
{
    if (Health() <= 0)
        return false;

    int32_t list[10];
    const int32_t n = ObjectsAhead(this, 40, 80, list);
    TObjectInstance* best = nullptr;
    int32_t bestdist = 0;
    for (int32_t i = 0; i < n; i++)
    {
        TObjectInstance* oi = MapPane.GetInstance(list[i]);
        if (!oi || oi == this || (oi->GetFlags() & OF_INVISIBLE))
            continue;
        if (oi->CursorType() == CURSOR_NONE && !oi->IsInventoryItem())
            continue;
        const int32_t dist = Distance(oi);
        if (!best || dist <= bestdist)
        {
            best = oi;
            bestdist = dist;
        }
    }

    if (!best)
    {
        if (this == Player)
            TextBar.Print("%s", DialogList.GetLine("GETNOTHING"));
        return false;
    }

    if (this == Player)
    {
        const char* name = best->GetName();
        if (DialogList.FindLine("FULLGET") >= 0)
            TextBar.Print(DialogList.GetLine("FULLGET"), name);
        else
            TextBar.Print("%s %s", DialogList.GetLine("BASEGET"), name);
    }
    Pickup(best);
    return true;
}

// REVSYNC: Say @ 0x004d0610 (DIALOG.md §3.1). Plays the voice unpositioned,
// starts the say action for the voice's length (or the line's), and puts the
// line in the dialog pane. `wait` (ticks), when given, wins.
//   - The voice paces the line whenever it exists: retail measured the playing
//     sample, so with sound output off it paced by text; the port's decoded
//     length keeps silenced and test runs paced like normal play.
//   - Retail starts the action with TryCommand; the port's TryCommand drops a
//     block it can't start yet, so the action is set desired (which owns it).
bool TCharacter::Say(const char *string, int32_t wait, const char *anim, const char *sound)
{
    if (!string || Health() <= 0)
        return false;

    voice = -1;
    int32_t voicems = 0;
    if (sound && PlaySpeech)
    {
        voice = SoundPlayer.FindSound(sound);
        if (voice >= 0)
        {
            if (SoundPlayer.Mount(voice))
            {
                SoundPlayer.Play(voice);            // full volume, no position
                SoundPlayer.Unmount(voice);
            }
            voicems = SoundPlayer.SampleLengthMs(voice);
        }
    }

    char line[256];
    DialogLine(string, line, sizeof(line));

    TActionBlock* ab = new TActionBlock(anim ? anim : "say", ACTION_SAY);
    // The action's copy of the line; ShowDialog off blanks it when a voice
    // speaks it (nothing draws it -- the pane always shows the line).
    ab->data = (voicems > 0 && !ShowDialog) ? nullptr : (void *)strdup(line);
    ab->wait = SpeechTicks(wait, voicems, line);
    ab->loop = true;
    const int32_t ticks = ab->wait;

    // Step 7 (DIALOG.md §3.1): start the say animation now -- retail's
    // vtable 0x218 at 0x004db4d0, the port's ForceCommand -- rather than
    // queue it as desired. A queued say waited behind whatever the speaker
    // was doing (Kylie held each line ~40 s), or forever when that never
    // ended (the level-46 slaves), and the script's speech wait, which wants
    // the speaker idle in its root state, never came. A speaker who can't
    // take it still speaks the line; the refused block, which retail leaked,
    // is freed.
    ForceCommand(ab);
    if (doing != ab && desired != ab)
        delete ab;

    DialogPane.AddSpeech(this, line, ticks);
    return true;
}

// REVSYNC: 0x004d084b..0x004d08e0 (Say's duration). Retail: `12 -
// ftol(ms * 0.001f * -24.0f)` in x87 extended precision, which truncates to
// the same tick as the integer form here for any length under ~14 minutes;
// a voice of length 0 paces by the text. Checked against retail by the A/B
// (docs/gameflow/RETAIL_AB.md, say-duration).
int32_t TCharacter::SpeechTicks(int32_t wait, int32_t voicems, const char *line)
{
    if (wait >= 0)
        return wait;
    if (voicems > 0)
        return 12 + voicems * 24 / 1000;
    return 2 * (int32_t)strlen(line) + 36;
}

// REVSYNC: SayIndex @ 0x004d09b0 -- dialog line `tagid`, with the voice its
// tag names.
bool TCharacter::SayTag(int32_t tagid, int32_t wait, const char *anim)
{
    return Say(DialogList.GetLine(tagid), wait, anim, DialogList.GetTag(tagid));
}

// REVSYNC: SayTag @ 0x004d0a20
bool TCharacter::SayTag(const char *tag, int32_t wait, const char *anim)
{
    const int32_t tagid = DialogList.FindLine(tag);
    if (tagid < 0)
        return false;
    return SayTag(tagid, wait, anim);
}

// REVSYNC: 0x004d6000
void TCharacter::StopTalking()
{
    if (voice >= 0)
        SoundPlayer.Stop(voice);
    ForceCommandDone();
}

// REVSYNC: TCharacter::DrawBow @ 0x004d0aa0 -- start aiming, in the bow
// root and not aiming yet, alive and not held in an interactive move. A
// player needs ammunition: an arrow in the inventory, which is equipped when
// the ammo slot is empty. A walk or step stops first; then "bowaim"
// (interrupting, from frame 20) at the facing is desired. Retail's network
// notice (0x0057d9d0) is for a multiplayer game.
bool TCharacter::DrawBow()
{
    if (!IsBowMode() || IsBowDrawn() || Health() <= 0 || InteractiveLocked())
        return false;
    if (ObjClass() == OBJCLASS_PLAYER)
    {
        auto* player = static_cast<TPlayer*>(this);
        const bool hasammo = FindObjInventory(OBJCLASS_AMMO, AT_ARROW) != nullptr;
        if (!player->GetEquip(EQ_AMMO))
            player->Equip(FindObjInventory(OBJCLASS_AMMO, AT_ARROW), EQ_AMMO);
        if (!hasammo)
            return false;
    }

    if (doing && (doing->action == ACTION_MOVE || doing->action == ACTION_COMBATMOVE ||
                  doing->action == ACTION_BOWMOVE))
        Stop();

    auto* ab = new TActionBlock("bowaim", ACTION_BOWAIM);
    ab->interrupt = true;
    ab->angle = ab->moveangle = GetFace();
    ab->frame = 20;
    SetDesired(ab);
    return true;
}

// REVSYNC: TCharacter::AimBow @ 0x004d0c70 -- turn the drawn bow toward
// `angle` (nothing once the shot is on its way), the turn rate from how far.
bool TCharacter::AimBow(int32_t angle)
{
    if (!IsBowMode() || !IsBowDrawn() || Health() <= 0 || InteractiveLocked())
        return false;

    if (doing->stop)    // No more aiming if arrow was shot
        return true;

    doing->angle = doing->moveangle = angle;
    int32_t absdiff = abs(AngleDiff(GetFace(), angle));
    doing->turnrate = MAKETURNRATE(absdiff);

    return true;
}

// REVSYNC: 0x004d0dc0
bool TCharacter::AimBowLeft()
{
    return AimBow((GetFace() - chardata->bowaimspeed) & 255);
}

// REVSYNC: 0x004d0de0
bool TCharacter::AimBowRight()
{
    return AimBow((GetFace() + chardata->bowaimspeed) & 255);
}

// REVSYNC: TCharacter::ShootBow @ 0x004d0e00 -- loose an arrow at `angle`:
// the aim stops (after its pivot), "bowshoot" is desired and one more shot
// is queued for ResolveBowShoot, which keeps the pace (BOWWAIT). An idle
// player (state bit 2) is idle no more.
bool TCharacter::ShootBow(int32_t angle)
{
    if (!IsBowMode() || !IsBowDrawn() || Health() <= 0 || InteractiveLocked())
        return false;
    if (ObjClass() == OBJCLASS_PLAYER)
    {
        auto* player = static_cast<TPlayer*>(this);
        if (player->PlayerState() & 2)
            player->SetPlayerState(player->PlayerState() & ~2);
    }

  // Inform aim animation that it should stop (will wait for pivot though)
    doing->stop = true;
    doing->angle = doing->moveangle = angle;
    int32_t absdiff = abs(AngleDiff(GetFace(), angle));
    doing->turnrate = MAKETURNRATE(absdiff);

  // Queue the shoot action (after pivot)
    auto* ab = new TActionBlock("bowshoot", ACTION_BOWSHOOT);
    ab->angle = ab->moveangle = angle;
    SetDesired(ab);
    bowshots++;
    return true;
}

// REVSYNC: TCharacter::IsBowDrawn @ 0x004d1050 -- doing the bow root's "aim"
bool TCharacter::IsBowDrawn()
{
    if (!IsBowMode())
        return false;

    return !stricmp(StName(root->name, "aim"), doing->name);
}

// *****************************
// * Choosing and making attacks *
// *****************************
//
// docs/gameplay/forensics/COMBAT_ATTACK_CHOICE.md §3.1-§3.6 (the searches)
// and COMBAT_HIT.md §3.1 (the numbers an attack carries).

// REVSYNC: the five-line gate retail opens ButtonAttack 0x004d2480,
// RandomAttack 0x004d2900, SpecificAttack 0x004d2a60 (and SetFighting, Go,
// EndFighting, Block, Dodge; Damage spares a held victim) with: held in
// another's interactive move (its attack or an interactive impact).
bool TCharacter::InteractiveLocked() const
{
    if (charflags & kCharFlagInteractive)
        return false;
    return (doing->attack && (doing->attack->flags & CA_INTERACTIVE)) ||
           (doing->impact && (doing->impact->flags & CAI_INTERACTIVE));
}

// REVSYNC: TCharacter::Offense @ 0x004d72e0
int32_t TCharacter::Offense()
{
    const int32_t spells = SpellManager.GetOffense();
    return chardata->attackmod + spells + GetStat("Value") * Rules.tohitrangechar;
}

// REVSYNC: TCharacter::Defense @ 0x004d72a0
int32_t TCharacter::Defense()
{
    const int32_t spells = SpellManager.GetDefense();
    return chardata->defensemod + spells + GetStat("Value") * Rules.tohitrangechar;
}

// REVSYNC: TCharacter::AnimPrefix @ 0x004cdf60 (slot 0x310; the player's too)
const char *TCharacter::AnimPrefix()
{
    const bool running = IsRunMode();
    if (root && root->Is("hand"))
        return running ? "hr" : "h";
    if (root && root->action == ACTION_BOW)
        return running ? "br" : "b";
    if (root && root->action == ACTION_COMBAT)
        return running ? "cr" : "c";
    if (root && root->Is("sneak"))
        return "s";
    if (running)
        return HoldsLight() ? "tr" : "r";
    return HoldsLight() ? "t" : "w";
}

// REVSYNC: TCharacter::CombatAnimName @ 0x004ce1b0
void TCharacter::CombatAnimName(char *buf, const char *name)
{
    snprintf(buf, RESNAMELEN, "%s%s", objclass == OBJCLASS_PLAYER ? AnimPrefix() : "c", name);
    if ((charflags & kCharFlagWalkPrefix) && root && root->Is("walk"))
    {
        const char first = buf[0];
        buf[0] = 'w';
        if (!HasActionAni(buf))
            buf[0] = first;
    }
}

// IsValidAttack's tier (0x004d18d2), ToHitDamageFactor's row.
int32_t SAttackRoll::Tiered() const
{
    int32_t factor;
    if (ToHitDamageFactor(tohit - roll, factor))
        return (std::max)(1, factor * damage / 100);
    return damage;
}

// The dice of one blow (COMBAT_HIT.md §3.1), IsValidAttack's (0x004d16ea..
// 0x004d18c9) with the target's stance handed in. The damage: the target's
// CalculateDamage of this character's weapon, scaled by DamageMod plus
// `dmgpcnt` and by a player's EdgeBonus for an edged weapon. The to-hit:
// tohitcenter less the target's luck and defence plus this character's,
// tohitface more against an open target (100 more for the player under the
// cheat), held to 10..100, then 5 more one time in ten. The roll:
// random(1, 50) + random(0, 50). Only the dice still -1 in `thrown` are
// thrown, in that order; both sides' luck and defence are read whatever is
// left to throw, as retail reads them.
SAttackRoll TCharacter::RollAttack(TCharacter& target, const SCharAttackData& attack, int32_t dmgpcnt,
    bool targetopen, SAttackRoll thrown)
{
    SAttackRoll dice = thrown;
    if (dice.damage == -1)
    {
        const int32_t type = GetDamageType(WeaponType(), attack.flags);
        const int32_t weapon = WeaponDamage();
        const int32_t d = target.CalculateDamage(weapon, type, attack.damagemod);
        dice.damage = d * (DamageMod() + dmgpcnt + 100) / 100;
        if (CombatTrace::Enabled())
            CombatTrace::Event(this, "damage-roll", "attack=%s\ttarget=%s\tweapon=%d\ttype=%d\tmod=%d\t"
                               "armor=%d\tresist=%d\ttaken=%d\tdmgmod=%d\tdmgpcnt=%d\tdamage=%d",
                               attack.attackname, target.GetName() ? target.GetName() : "-", weapon, type,
                               attack.damagemod, target.ArmorValue(),
                               type >= 0 && type < NUMDAMAGETYPES ? target.DamageModifier(type) : 0,
                               d, DamageMod(), dmgpcnt, dice.damage);
        if (objclass == OBJCLASS_PLAYER)
        {
            const int32_t wt = WeaponType();
            if (wt == WT_KNIFE || wt == WT_SWORD || wt == WT_AXE)
                dice.damage = dice.damage * (static_cast<TPlayer*>(this)->EdgeBonus() + 100) / 100;
        }
    }
    int32_t def = target.LuckMod();
    def += target.Defense();
    int32_t off = LuckMod();
    off += Offense();
    if (dice.tohit == -1)
    {
        int32_t t = Rules.tohitcenter - def + off;
        if (targetopen)
            t += Rules.tohitface;
        if (objclass == OBJCLASS_PLAYER && CheatNahkranoth)
            t += 100;
        t = std::clamp(t, 10, 100);
        dice.tohit = t + (random(0, 9) == 0 ? 5 : 0);
    }
    if (dice.roll == -1)
    {
        const int32_t high = random(1, 50);
        dice.roll = high + random(0, 50);
    }
    return dice;
}

int32_t TCharacter::RollDamagePercent()
{
    const int32_t first = random(1, 50);
    return first + random(1, 50);
}

// REVSYNC: TCharacter::IsValidAttack @ 0x004d1120. The shape gates
// (COMBAT_ATTACK_CHOICE.md §3.1), then with a target the dice the attack
// will carry (RollAttack; a player's StrengthMod takes `dmgpcnt`'s place):
// the damage, the to-hit and the roll once per search, and the TOHITDAMAGE
// tier every time a candidate gets that far, so a search that goes on past
// a later refusal compounds it. Then the impact the target will play, and
// the glancing-blow guard.
bool TCharacter::IsValidAttack(int32_t attacknum, int32_t &impactnum, int32_t &damage, int32_t &tohit,
    int32_t &roll, int32_t tdist, int32_t button, int32_t pcnt, int32_t dmgpcnt, int32_t flagmask,
    int32_t flags, TCharacter* targ)
{
    impactnum = -1;
    if ((uint32_t)attacknum >= (uint32_t)chardata->attacks.NumItems())
        return false;
    SCharAttackData* ad = &chardata->attacks[attacknum];

    if ((requestbits & kRequestNoPlayAnim) && (ad->flags & CA_PLAYANIM))
        return false;
    if (!HasActionAni(ad->attackname))
        return false;

  // Fatigue: a FATIGUEATTACK only while tired enough, anything else only
  // with the fatigue it costs. With the cheat the player skips this, and
  // can't make fatigue attacks.
    if (CheatNahkranoth && objclass == OBJCLASS_PLAYER)
    {
        if (ad->flags & CA_FATIGUEATTACK)
            return false;
    }
    else if (ad->flags & CA_FATIGUEATTACK)
    {
        if (Fatigue() > ad->maxfatigue)
            return false;
    }
    else if (Fatigue() < ad->fatigue)
        return false;

    if ((ad->flags & flagmask) != flags)
        return false;
    if (button >= 0 && ad->button != button)
        return false;
    if (ad->attackpcnt < pcnt)
        return false;
    if (ad->maxdist > 0 && targ && (tdist < ad->mindist || tdist > ad->maxdist))
        return false;

  // The mode it's made in: sneaking, walking, the bow, or the combat stance.
    if (ad->flags & CA_SNEAKMODE)
    {
        if (!root || !root->Is("sneak"))
            return false;
    }
    else if (ad->flags & CA_WALKMODE)
    {
        if (!root || !root->Is("walk"))
            return false;
    }
    else if (ad->flags & CA_BOWMODE)
    {
        if (!root || root->action != ACTION_BOW)
            return false;
    }
    else if (!root || root->action != ACTION_COMBAT || root->Is("walk"))
        return false;

    if (ad->flags & CA_PLAYANIM)
    {
        if (!(charflags & kCharFlagPlayAnimRoots))
            return true;
        const char first = ad->attackname[0];
        if ((first == 'c' || first == 'C') && (!root || !root->Is("combat")))
            return false;
        if (first == 'w' || first == 'W')
            return root && root->Is("walk");
        return true;
    }

  // A spell: its own timer and condition, nothing else.
    if (ad->flags & CA_MAGICATTACK)
    {
        if (magictimer != 0)
            return false;
        switch (ad->condition)
        {
          case MASTAT_HEALTHLT: if (Health() > ad->conditionvalue) return false; break;
          case MASTAT_HEALTHGT: if (Health() <= ad->conditionvalue) return false; break;
          case MASTAT_MANALT:   if (Mana() > ad->conditionvalue) return false; break;
          case MASTAT_MANAGT:   if (Mana() <= ad->conditionvalue) return false; break;
          default: break;
        }
        magictimer--;
        return true;
    }

  // A monster attacks only when the AI's timer has run out; the player's
  // presses aren't timed. Nobody starts one while the last is still early.
    if (nextattack != 0 && objclass != OBJCLASS_PLAYER)
        return false;
    if (doing->action == ACTION_ATTACK && doing->attack && GetFrame() < doing->attack->nextwait)
        return false;   // REVSYNC-DIVERGENCE: retail doesn't test doing->attack (an ATTACK block always has one)
    if (!HasActionAni(ad->attackname))
        return false;
    if ((ad->flags & CA_MOVING) && !IsMoveAction(doing))
        return false;
    if ((ad->flags & CA_RUNNING) && !IsRunMode())
        return false;

  // What the target is doing: a stun / knockdown follow-up, a response.
    if (targ)
    {
        if ((ad->flags & CA_ATTACKSTUN) && (!targ->doing || targ->doing->action != ACTION_STUN))
            return false;
        if ((ad->flags & CA_ATTACKDOWN) && (!targ->doing || targ->doing->action != ACTION_KNOCKDOWN))
            return false;
        if (ad->responsename[0] && !targ->doing->Is(ad->responsename))
            return false;
    }

  // A chain link follows its attack in time.
    if ((ad->flags & (CA_CHAIN | CA_AUTOCOMBO)) && ad->chainname[0])
    {
        if (!lastattack || stricmp(lastattack->attackname, ad->chainname) != 0)
            return false;
        if (PlayScreen.GameFrame() - lastattackticks > lastattack->chainexptime)
            return false;
    }

  // An interactive attack needs a target that isn't held in one already;
  // nobody attacks a target held in one.
    if (ad->flags & CA_INTERACTIVE)
    {
        if (!targ)
            return false;
        if (!(targ->charflags & kCharFlagInteractive))
        {
            const TActionBlock* td = targ->doing;
            if ((td->attack && (td->attack->flags & CA_INTERACTIVE)) ||
                (td->impact && (td->impact->flags & CAI_INTERACTIVE)))
                return false;
        }
    }
    if (targ && targ->doing->impact && (targ->doing->impact->flags & CAI_INTERACTIVE))
        return false;

  // The player's weapon and skills; his StrengthMod takes the damage
  // percentage's place.
    if (objclass == OBJCLASS_PLAYER)
    {
        if (!(ad->weaponmask & (1 << WeaponType())))
            return false;
        if (GetObjStat(PLRVAL_FIRST + PLRVAL_ATTACKLEVEL) < ad->attackskill)
            return false;
        if (GetObjStat(SK_FIRST + SK_WEAPONSKILLS + WeaponType()) < ad->weaponskill)
            return false;
        dmgpcnt = StrengthMod();
        if (!strcmp(ad->attackname, "sunsetflipper") &&
            (!targ || !targ->GetName() || stricmp(targ->GetName(), "Baez") != 0))
            return false;   // (retail would crash on a nameless target)
    }

    if (targ)
    {
      // Easier against a target facing away, or one swinging itself.
        const bool open = abs(targ->FaceAngleTo(this)) >= 0x30 ||
                          (targ->doing && targ->doing->action == ACTION_ATTACK);
        const SAttackRoll dice = RollAttack(*targ, *ad, dmgpcnt, open, {damage, tohit, roll});
        damage = dice.Tiered();
        tohit = dice.tohit;
        roll = dice.roll;
    }

    if ((ad->flags & CA_DEATH) && (!targ || damage < targ->Health()))
        return false;

  // Retail clamps the target's health against the damage here and drops
  // the result (two min/max expressions); their Health() calls remain.
    if (targ)
    {
        const int32_t h = targ->Health() > damage ? targ->Health() : damage;
        if (h >= 1 && targ->Health() > damage)
            targ->Health();
    }

  // The impact the target will play: the first that fits, or a later death
  // impact when the blow kills. A target that can't play one refuses the
  // attack. Their damage ranges aren't read.
    if (ad->numimpacts > 0)
    {
        if (!targ)
            return true;
        const int32_t h = targ->Health() > damage ? targ->Health() : damage;
        if (h >= 1 && targ->Health() > damage)
            targ->Health();

        for (int32_t k = 0; k < ad->numimpacts; k++)
        {
            const SCharAttackImpact* imp = &ad->impacts[k];
            const bool kills = damage >= targ->Health();
            if (!(kills && (imp->flags & CAI_DEATH)) && impactnum >= 0)
                continue;

          // A held impact snaps the target in front of us: there must be room.
            if ((imp->flags & CAI_INTERACTIVE) && imp->snapdist > 0)
            {
                S3DPoint from = targ->Pos(), to;
                GetSnapPos(this, imp->snapdist, to);
                TCharacter* blocker = nullptr;
                if (targ->Blocked(from, to, 0, nullptr, &blocker) && blocker != this)
                    return false;
            }

            if (imp->flags & CAI_DEATH)
            {
                if (!targ->HasActionAni(imp->impactname))
                {
                  // The death the name stands for: "<root> to <prefix>dead" or
                  // "<prefix>dead".
                    char dead[RESNAMELEN];
                    targ->CombatAnimName(dead, "dead");
                    if (!stricmp(imp->impactname, "combat to dead"))
                    {
                        char name[RESNAMELEN * 2];
                        snprintf(name, sizeof(name), "%s to %sdead", targ->root->name,
                                 targ->ObjClass() == OBJCLASS_PLAYER ? targ->AnimPrefix() : "c");
                        if (!targ->HasActionAni(name))
                            return false;
                    }
                    else if (stricmp(imp->impactname, "dead") != 0 || !targ->HasActionAni(dead))
                        return false;
                  // Retail takes FindState's index for a yes/no here: a loop
                  // state found at index 0 counts as missing, -1 as present.
                    if (imp->loopname[0] && targ->FindState(imp->loopname) == 0)
                    {
                        if (stricmp(imp->loopname, "dead") != 0 || targ->FindState(dead) == 0)
                            return false;
                    }
                }
            }
            else
            {
                char name[RESNAMELEN];
                if (imp->flags & CAI_INTERACTIVE)
                    strncpyz(name, imp->impactname, RESNAMELEN);
                else
                    targ->CombatAnimName(name, imp->impactname);
                if (!targ->HasActionAni(name))
                    return false;
                if (imp->loopname[0] && targ->FindState(imp->loopname) == 0)   // (index 0: as above)
                    return false;
            }
            impactnum = k;
        }

      // Retail tests the record one past the last impact here: zero, unless
      // the attack has all six, when it reads past the record.
        const bool kills = damage >= targ->Health();
        if (kills && ad->numimpacts < MAXATTACKIMPACTS &&
            (ad->impacts[ad->numimpacts].flags & CAI_INTERACTIVE))
            return false;   // REVSYNC-DIVERGENCE: with six impacts retail reads heap memory; the port reads nothing
    }

    if (!targ)
        return true;
    if ((charflags & kCharFlagNoKill) && targ->Health() <= damage &&
        !((ad->flags & CA_INTERACTIVE) && (ad->flags & CA_DEATH)))
        return false;

  // A blow that will only glance makes a monster raise its guard as the
  // swing begins.
    if (tohit - roll <= Rules.tohitdamage[3].minvalue && targ->ObjClass() == OBJCLASS_CHARACTER)
        targ->Block(-2);
    return true;
}

// REVSYNC: TCharacter::FindButtonAttack @ 0x004d1dd0 (recon labels it
// TPlayer::meth_0x4d1dd0; 0x004d1ff0 is FindInteractiveAttack). `isaction`
// asks for a CA_ACTION flag the mask can't see, so ButtonAction never finds
// anything -- as in retail.
bool TCharacter::FindButtonAttack(int32_t button, int32_t dmgpcnt, int32_t &attacknum, int32_t &impactnum,
    int32_t &damage, int32_t &tohit, int32_t &roll, bool isaction, TCharacter* targ)
{
    const int32_t tdist = targ ? Distance(targ) : 10000;
    for (int32_t mode = 0; mode < 3; mode++)
    {
        int32_t want = mode == 0 ? CA_RESPONSE : mode == 1 ? CA_SPECIAL : 0;
        if (isaction)
            want |= CA_ACTION;
        for (int32_t i = 0; i < chardata->attacks.NumItems(); i++)
            if (IsValidAttack(i, impactnum, damage, tohit, roll, tdist, button, 0, dmgpcnt,
                              CA_RESPONSE | CA_SPECIAL, want, targ))
            {
                attacknum = i;
                return true;
            }
    }
    return false;
}

// REVSYNC: TCharacter::FindPcntAttack @ 0x004d1eb0
bool TCharacter::FindPcntAttack(int32_t pcnt, int32_t dmgpcnt, int32_t &attacknum, int32_t &impactnum,
    int32_t &damage, int32_t &tohit, int32_t &roll)
{
    if (!Fighting())
        return false;
    const int32_t tdist = Distance(Fighting());
    const int32_t n = chardata->attacks.NumItems();
    for (int32_t k = 0; k < n * 2; k++)
    {
        const int32_t i = random(0, n - 1);
        if (IsValidAttack(i, impactnum, damage, tohit, roll, tdist, -1, pcnt, dmgpcnt, 0, 0, Fighting()))
        {
            attacknum = i;
            return true;
        }
    }
    return false;
}

// REVSYNC: TCharacter::FindInteractiveAttack @ 0x004d1ff0
bool TCharacter::FindInteractiveAttack(int32_t pcnt, int32_t dmgpcnt, int32_t &attacknum, int32_t &impactnum,
    int32_t &damage, int32_t &tohit, int32_t &roll)
{
    if (!Fighting())
        return false;
    const int32_t tdist = Distance(Fighting());
    for (int32_t i = 0; i < chardata->attacks.NumItems(); i++)
        if ((chardata->attacks[i].flags & CA_INTERACTIVE) &&
            IsValidAttack(i, impactnum, damage, tohit, roll, tdist, -1, pcnt, dmgpcnt, 0, 0, Fighting()))
        {
            attacknum = i;
            return true;
        }
    return false;
}

// REVSYNC: TCharacter::DoAttack @ 0x004d2120. Not ported: the multiplayer
// gate and the network notify (0x00584150).
bool TCharacter::DoAttack(int32_t attacknum, int32_t impactnum, int32_t damage, int32_t tohit, int32_t roll,
    TCharacter* targ)
{
    if (doing && doing->action == ACTION_INVOKE)
        return false;
    if (Health() <= 0)
        return false;
    if (flags & (OF_ICED | OF_PARALIZE))
        return false;
    if ((uint32_t)attacknum >= (uint32_t)chardata->attacks.NumItems())
        return false;
    if (objclass == OBJCLASS_PLAYER)
    {
        TPlayer* player = static_cast<TPlayer*>(this);
        if (player->PlayerState() & 2)
            player->SetPlayerState(player->PlayerState() & ~2);
    }
    SCharAttackData* ad = &chardata->attacks[attacknum];

    if (ad->flags & CA_MAGICATTACK)
    {
        if (Health() <= 0 || InteractiveLocked() || (flags & OF_IMMOBILE) || (flags & (OF_ICED | OF_PARALIZE)))
            return false;
        CombatTrace::Event(this, "cast", "spell=%s\ttarget=%s", ad->spellname,
                           targ && targ->GetName() ? targ->GetName() : "-");
        TObjectInstance* target = targ;
        return CastVariant(ad->spellname, &target, targ ? 1 : 0, &ad->spellsource);
    }
    CombatTrace::Event(this, "attack", "attack=%s\ttarget=%s\tdamage=%d", ad->attackname,
                       targ && targ->GetName() ? targ->GetName() : "-", damage);

  // A PLAYANIM plays as an attack too. A special can't be cut short; a
  // chain link waits for the attack before it.
    auto* ab = new TActionBlock(ad->attackname, ACTION_ATTACK);
    ab->obj = targ;
    ab->attack = ad;
    ab->impact = impactnum >= 0 ? &ad->impacts[impactnum] : nullptr;
    ab->tohit = tohit;
    ab->damage = damage;
    ab->roll = roll;
    ab->priority = (ad->flags & CA_SPECIAL) != 0;
    ab->interrupt = !((ad->flags & CA_CHAIN) && ad->chainname[0]);
    if (ad->flags & CA_INTERACTIVE)
        requestbits &= ~kRequestInteractive;
    if (!(ad->flags & CA_PLAYANIM))
        requestbits &= ~kRequestNoPlayAnim;

  // The swing keeps the heading the character had; the root remembers it.
    root->angle = root->moveangle = doing->angle;
    ab->moveangle = doing->angle;
    if (ab->interrupt)
        ab->angle = doing->angle;
    ab->noroot = true;
    if (!SetDesired(ab))
    {
        delete ab;
        return true;
    }
    if (!(ad->flags & CA_PLAYANIM) && !(charflags & kCharFlagNoTurn))
        SetMoveAngle(GetFace());
    else if (TCharacter* t = Fighting())
        SetRotateZ(AngleTo(t));
    return true;
}

// REVSYNC: TCharacter::ButtonAttack @ 0x004d2480 -- the player's attack
// buttons. A press while a chain attack's window is open only banks a
// chain hit (Pulse spends them). The third and later presses of the same
// button against the same monster swing with the worst roll (100), and one
// time in eleven the monster counters at once instead: its timer cleared,
// its fatigue refilled. The target is looked up before the chain test.
bool TCharacter::ButtonAttack(int32_t button)
{
    if (Health() <= 0 || InteractiveLocked())
        return false;
    bool repeat = false;
    TCharacter* targ = Fighting();
    if (!targ)
    {
        TCharacter* found = nullptr;
        if (FindCharacters(&found, 1, -1, GetFace(), 0x20, FINDCHAR_ENEMY | FINDCHAR_HEAR | FINDCHAR_SEE) > 0)
            targ = found;
    }

    if (lastattack && (lastattack->flags & CA_CHAIN) &&
        PlayScreen.GameFrame() - lastattackticks <= lastattack->chainexptime && chainhits < MAXCHAINHITS)
    {
        chainhits++;
        return true;
    }

    if (objclass == OBJCLASS_PLAYER && targ && targ->ObjClass() != OBJCLASS_PLAYER)
    {
        if (button == lastbutton)
        {
            if (++buttonrepeat >= 3)
                repeat = true;
        }
        else
            buttonrepeat = 1;
        lastbutton = button;

        if (repeat && random(0, 10) == 0)
        {
            const int32_t counter = random(1, 50) + 50;
            int32_t a = -1, imp = -1, dmg = -1, th = -1, rl = 100;
            targ->nextattack = 0;
            targ->SetFatigue(targ->MaxFatigue());
            bool found = false;
            for (int32_t k = 0; k < 10; k++)
            {
                found = targ->FindInteractiveAttack(0, counter, a, imp, dmg, th, rl) ||
                        targ->FindPcntAttack(0, counter, a, imp, dmg, th, rl);
                if (found && !(targ->chardata->attacks[a].flags & (CA_PLAYANIM | CA_MAGICATTACK)))
                    break;
                found = false;
            }
            if (found && targ->DoAttack(a, imp, dmg, th, 0, this))
                return false;
        }
    }

    const int32_t dmgpcnt = RollDamagePercent();
    int32_t a = -1, imp = -1, dmg = -1, th = -1, rl = repeat ? 100 : -1;
    if (FindButtonAttack(button, dmgpcnt, a, imp, dmg, th, rl, false, targ))
        return DoAttack(a, imp, dmg, th, rl, targ);
    return false;
}

// REVSYNC: TCharacter::ButtonAction @ 0x004d27f0 (finds nothing: see
// FindButtonAttack).
bool TCharacter::ButtonAction(int32_t button)
{
    if (Health() <= 0 || InteractiveLocked())
        return false;
    TCharacter* found = nullptr;
    TCharacter* targ = FindCharacters(&found, 1, 0x200, GetFace(), 0x20, 0) > 0 ? found : nullptr;
    const int32_t dmgpcnt = RollDamagePercent();
    int32_t a = -1, imp = -1, dmg = -1, th = -1, rl = -1;
    if (FindButtonAttack(button, dmgpcnt, a, imp, dmg, th, rl, true, targ))
        return DoAttack(a, imp, dmg, th, rl, targ);
    return false;
}

// REVSYNC: TCharacter::RandomAttack @ 0x004d2900 -- the AI's attack: an
// interactive one first when OnAttacked asked for it, else a random pick.
bool TCharacter::RandomAttack(int32_t pcnt)
{
    if (Health() <= 0 || InteractiveLocked())
        return false;
    const int32_t dmgpcnt = RollDamagePercent();
    int32_t a = -1, imp = -1, dmg = -1, th = -1, rl = -1;
    bool found = false;
    if (lastbutton >= 0 && (requestbits & kRequestInteractive))
    {
        found = FindInteractiveAttack(pcnt, dmgpcnt, a, imp, dmg, th, rl);
        requestbits &= ~kRequestInteractive;
    }
    if (!found && !FindPcntAttack(pcnt, dmgpcnt, a, imp, dmg, th, rl))
        return false;
    return DoAttack(a, imp, dmg, th, rl, Fighting());
}

// REVSYNC: TCharacter::SpecificAttack @ 0x004d2a60 -- one attack by number
// (Pulse's chains, the `specificattack` script command), its timer cleared.
bool TCharacter::SpecificAttack(int32_t attacknum)
{
    if (Health() <= 0 || InteractiveLocked())
        return false;
    TCharacter* targ = Fighting();
    if (!targ)
    {
        TCharacter* found = nullptr;
        if (FindCharacters(&found, 1, -1, GetFace(), 0x20, FINDCHAR_ENEMY | FINDCHAR_HEAR | FINDCHAR_SEE) > 0)
            targ = found;
    }
    const int32_t tdist = targ ? Distance(targ) : 10000;
    const int32_t dmgpcnt = RollDamagePercent();
    int32_t imp = -1, dmg = -1, th = -1, rl = -1;
    nextattack = 0;
    if (!IsValidAttack(attacknum, imp, dmg, th, rl, tdist, -1, -1, dmgpcnt, 0, 0, targ))
        return false;
    return DoAttack(attacknum, imp, dmg, th, rl, targ);
}

// REVSYNC: Leap @ 0x004d2be0 -- in a combat or bow root, alive, and (unless
// the move is interactive, charflags 0x80000) not in an interactive attack
// or impact: the root's leap toward `angle`, one of eight by the angle off
// the facing's octant ("<root>leapf", "...fr", ... "...fl"). A player's
// idle mark (state bit 2) goes. The block interrupts and keeps the doing
// block's target; true once it's desired (SetDesired's answer isn't read).
bool TCharacter::Leap(int32_t angle)
{
    if (!IsFighting() || Health() <= 0)
        return false;
    if (!(charflags & kCharFlagInteractive))
    {
        if (doing->attack && (doing->attack->flags & CA_INTERACTIVE))
            return false;
        if (doing->impact && (doing->impact->flags & CAI_INTERACTIVE))
            return false;
    }
    if (ObjClass() == OBJCLASS_PLAYER)
    {
        auto* player = static_cast<TPlayer*>(this);
        if (player->PlayerState() & 2)
            player->SetPlayerState(player->PlayerState() & ~2);
    }

    static constexpr const char* kSuffix[8] = {"leapf", "leapfr", "leapr", "leapbr",
                                               "leapb", "leapbl", "leapl", "leapfl"};
    const int32_t octant = ((angle - ((GetFace() + 15) & 0xe0)) & 0xff) / 32;
    char name[RESNAMELEN];
    snprintf(name, sizeof(name), "%s%s", root->name, kSuffix[octant]);
    if (!HasActionAni(name))
        return false;

    auto* ab = new TActionBlock(name, ACTION_COMBATLEAP);
    ab->interrupt = true;
    if (doing)
        ab->obj = doing->obj;
    SetDesired(ab, 0);
    return true;
}

// REVSYNC: StartRetreat @ 0x004d5fc0 -- give up the fight and run: no target,
// the walk stopped (unless already retreating), and 96 frames of retreat.
void TCharacter::StartRetreat()
{
    SetFighting(nullptr);
    if (!retreating)
        Stop();
    retreatframes = 0x60;
    retreating = true;
    retreatlatch = true;
}

// REVSYNC: TCharacter::Block @ 0x004d2e30 (COMBAT_HIT.md §3.9) -- raise a
// guard for `frames` (below 0: BLOCK's random range) in a combat or bow root:
// not while flinching, dead or held, nor with the root's target over 120
// away. A monster blocks only an attack it can still catch (the attacker
// short of its block time), unless `frames` is -2 (IsValidAttack's forced
// block); the player needs no timing. The block keeps its target, loops,
// and interrupts.
bool TCharacter::Block(int32_t frames)
{
    if (blockSeam)
        return blockSeam(this, frames);
    if (!root || (root->action != ACTION_COMBAT && root->action != ACTION_BOW))
        return false;
    if (doing && doing->action == ACTION_IMPACT)
        return false;
    if (Health() <= 0 || InteractiveLocked())
        return false;
    if (root->obj && Distance(root->obj) > 120)
        return false;
    if (ObjClass() != OBJCLASS_PLAYER)
    {
        auto* attacker = dynamic_cast<TCharacter*>(doing->obj);
        if (!attacker)
            return false;
        const TActionBlock* blow = attacker->doing;
        const bool catchable = blow && blow->action == ACTION_ATTACK && blow->attack &&
                               attacker->GetFrame() < blow->attack->blocktime;
        if (!catchable && frames != -2)
            return false;
    }
    char name[RESNAMELEN];
    CombatAnimName(name, "block");
    if (!HasActionAni(name))
        return false;

    auto* ab = new TActionBlock(name, ACTION_BLOCK);
    ab->obj = doing->obj;
    ab->wait = frames < 0 ? random(chardata->blockmin, chardata->blockmax) : frames;
    ab->priority = false;
    ab->interrupt = true;
    ab->loop = true;
    SetDesired(ab);
    DropUnheld(ab);
    if (!(charflags & kCharFlagNoTurn))
        SetMoveAngle(GetFace());
    return true;
}

// REVSYNC: TCharacter::StopBlock @ 0x004d30f0 -- ends a guard: its wait to 0
// (ResolveBlock lets it go).
bool TCharacter::StopBlock()
{
    if (!root || (root->action != ACTION_COMBAT && root->action != ACTION_BOW) || !doing ||
        doing->action != ACTION_BLOCK)
        return false;
    doing->wait = 0;
    return true;
}

namespace
{
// Dodge's roll by the character's facing (eight sectors, the last the
// rest) and the direction held (0-7): the last letter of "crollb".
constexpr char kDodgeRolls[8][9] = {"bflrbfbf", "bfbfblrf", "rlbfbbff", "fbbfrbfl",
                                    "fbrlfbfb", "fbfbfrlb", "lrfbffbb", "bffblfbr"};

int32_t DodgeSector(int32_t facing)
{
    constexpr int32_t kFirst[7] = {0x10, 0x31, 0x51, 0x71, 0x92, 0xb3, 0xd3};
    constexpr int32_t kLast[7] = {0x30, 0x50, 0x70, 0x91, 0xb2, 0xd2, 0xf2};
    for (int32_t s = 0; s < 7; ++s)
        if (facing >= kFirst[s] && facing <= kLast[s])
            return s;
    return 7;
}
}

// REVSYNC: TCharacter::Dodge @ 0x004d3150 (COMBAT_HIT.md §3.10) -- a roll
// out of the combat stance, toward `dir` (0-7, the direction held; any
// other keeps the back roll): "crollb" ("hrollb" from the "hand" root), its
// last letter by the facing and `dir`. Not dead or held; a stopped player
// is let go. (The debug camera's turn of the facing, 0x006671f0, isn't
// ported, as in UpdateMove.)
bool TCharacter::Dodge(int32_t dir)
{
    if (!root || (root->action != ACTION_COMBAT && root->action != ACTION_BOW))
        return false;
    if (!doing || doing->action != ACTION_COMBAT || Health() <= 0 || InteractiveLocked())
        return false;
    if (ObjClass() == OBJCLASS_PLAYER)
    {
        TPlayer* player = static_cast<TPlayer*>(this);
        if (player->PlayerState() & 2)
            player->SetPlayerState(player->PlayerState() & ~2);
    }

    char name[] = "crollb";
    if (root->Is("hand"))
        name[0] = 'h';
    if (dir >= 0 && dir <= 7)
        name[5] = kDodgeRolls[DodgeSector(GetFace())][dir];
    if (!HasActionAni(name))
        return false;
    auto* ab = new TActionBlock(name, ACTION_DODGE);
    ab->obj = doing->obj;
    ab->interrupt = false;
    ab->priority = true;
    SetDesired(ab);
    DropUnheld(ab);
    return true;
}

// REVSYNC: SideStep @ 0x004d6220 -- a step to the side, "sidestepl" or
// "sidestepr" (either at random when `dir` names neither), unless one is
// already under way. The block keeps the doing block's target and angle,
// and the character faces that angle at once; it moves off the move angle
// by a quarter turn (left +0x40), with priority and a pivot first
// (waitpivot), turn rate 8. A block SetDesired refuses is dropped. (The
// return value is retail's leftover register; here, whether it was taken.)
bool TCharacter::SideStep(char dir)
{
    if (!doing || strncasecmp(doing->name, "sidestep", 8) == 0)
        return false;
    if (dir != 'l' && dir != 'r')
        dir = random(0, 1) ? 'l' : 'r';

    char name[16] = "sidestep";
    name[8] = dir;
    if (!HasActionAni(name))
        return false;

    auto* ab = new TActionBlock(name, ACTION_COMBAT);
    ab->obj = doing->obj;
    ab->angle = doing->angle;
    FaceOnly(doing->angle);
    ab->moveangle = (moveangle + (dir == 'l' ? 0x40 : 0xc0)) & 0xff;
    ab->interrupt = false;
    ab->priority = true;
    ab->waitpivot = true;
    ab->turnrate = 8;
    if (!SetDesired(ab, 0))
    {
        delete ab;
        return false;
    }
    return true;
}

bool TCharacter::Pulp(S3DPoint vel, int32_t piece_count, int32_t blood_count)
{
    // I'm not dead yet. I think I'll go for a walk.
    // oh wait, I am pulped!
    if (IsDoing(ACTION_PULP) || Flags() & OF_DISABLED)
        return false;

    // create the action block
    TActionBlock* ab = new TActionBlock("pulped", ACTION_PULP);
    ab->priority = true;
    ab->action = ACTION_PULP;
    ForceCommand(ab);
    SetRoot(ab);
    SetDesired(ab);
    Disable();

    SObjectDef def;
    memset(&def, 0, sizeof(SObjectDef));
    def.objclass = OBJCLASS_EFFECT;
    def.level = MapPane.GetMapLevel();
    def.pos = Pos();
    def.facing = GetFace();
    def.objtype = EffectClass.FindObjType("PULP");

    TPulpEffect* pulpstuff = (TPulpEffect*)MapPane.GetInstance(MapPane.NewObject(&def));
    if (!pulpstuff)
        return false;

    // init all the params
    pulpstuff->Set(vel, this, piece_count, blood_count);

    return true;
}

// REVSYNC: TCharacter::Burn @ 0x004d3590 -- set alight: not the player
// under the alreadydead cheat, nor one without an "onfire" animation. The
// burn runs 50 ticks (Pulse ends it); a new one starts the burn effect and
// forces "onfire", one already burning restarts its effect. Not ported:
// the "onfire" sound at the body.
bool TCharacter::Burn()
{
    if (CheatAlreadyDead && ObjClass() == OBJCLASS_PLAYER)
        return false;
    if (!HasActionAni("onfire"))
        return false;

    burnticks = 50;
    if (!burning)
    {
        SObjectDef def;
        memset(&def, 0, sizeof(SObjectDef));
        def.objclass = OBJCLASS_EFFECT;
        def.level = MapPane.GetMapLevel();
        def.pos = Pos();
        def.facing = GetFace();
        def.objtype = EffectClass.FindObjType("BURN");

        TBurnEffect* burn = (TBurnEffect*)MapPane.GetInstance(MapPane.NewObject(&def));
        if (!burn)
            return false;
        burning = burn;

        // init all the params
        burn->Set(this);

        // now make him do the uncontrolable chicken on fire dance of death!
        // create action block
        TActionBlock* ab = new TActionBlock("onfire", ACTION_BURN);
        ab->priority = true;
        ForceCommand(ab);
    }
    else
    {
        ((TBurnEffect*)burning)->ResetFrameCount();
    }

    return true;
}


// REVSYNC: KnockBack @ 0x004d3750 -- thrown back by a blow from `frompos`:
// an IMPACT with priority, forced at once. From behind (the facing more than
// 0x48 off the blow's bearing, which retail measures without wrapping) the
// back impact "impb" when the character has one, keeping its facing; else
// one of five (`variant` 0..4, else random) turning to face the blow. An
// idle player isn't knocked back; a paralysed character is left as it is
// (true). The network notify is inert offline.
bool TCharacter::KnockBack(S3DPoint frompos, int32_t variant)
{
    if (knockBackSeam)
    {
        knockBackSeam(this, frompos, variant);
        return true;
    }
    if (ObjClass() == OBJCLASS_PLAYER && (static_cast<TPlayer*>(this)->PlayerState() & 2))
        return false;
    if (flags & OF_PARALIZE)
        return true;
    combatflashticks = 5;

    static constexpr const char* kImpacts[5] = {"impk", "imphh", "imph", "implh", "impl"};
    char name[RESNAMELEN];
    const int32_t bearing = ConvertToFacing(pos, frompos);
    bool back = false;
    if (std::abs(GetFace() - bearing) > 0x48)
    {
        CombatAnimName(name, "impb");
        back = HasActionAni(name);
    }
    if (!back)
    {
        if (variant < 0 || variant > 4)
            variant = random(0, 4);
        CombatAnimName(name, kImpacts[variant]);
        Face(bearing);
    }
    if (!HasActionAni(name))
        return false;

    auto* ab = new TActionBlock(name, ACTION_IMPACT);
    ab->priority = true;
    ForceCommand(ab, 0, 0);
    return true;
}

// Returns the correct angle movce animation given the current movedir, facedir, and root name
char *TCharacter::GetAngleMoveAnim(int32_t movedir, int32_t facedir, char *root, char *animname, int32_t buflen)
{
    int32_t faceangle = ((facedir + 15) & 0xE0);
    int32_t dirangle = ((movedir + 15) & 0xE0); // round to 8 dirs
    int32_t diff = (dirangle - faceangle) & 255;
    int32_t anim = diff / 32;

    char *sfx;
    switch (anim)
    {
        case 0: sfx = "f"; break;
        case 1: sfx = "fr"; break;
        case 2: sfx = "r"; break;
        case 3: sfx = "br"; break;
        case 4: sfx = "b"; break;
        case 5: sfx = "bl"; break;
        case 6: sfx = "l"; break;
        case 7: sfx = "fl"; break;
    }
    
    strncpyz(animname, StName(root, sfx), buflen);

    if (!HasActionAni(animname))
        strncpyz(animname, StName(root, "f"), buflen);

    return animname;
}

ACTION TCharacter::GetMoveAction(ACTION action)
{
    switch (action)
    {
      case ACTION_ANIMATE:
      case ACTION_MOVE:
        return ACTION_MOVE;
      case ACTION_COMBAT:
      case ACTION_COMBATMOVE:
        return ACTION_COMBATMOVE;
      case ACTION_BOW:
      case ACTION_BOWMOVE:
        return ACTION_BOWMOVE;
    }
    return ACTION_NONE;
}

ACTION TCharacter::GetLeapAction(ACTION action)
{
    switch (action)
    {
      case ACTION_ANIMATE:
      case ACTION_MOVE:
        return ACTION_LEAP;
      case ACTION_COMBAT:
      case ACTION_COMBATMOVE:
        return ACTION_COMBATLEAP;
    }
    return ACTION_NONE;
}

// REVSYNC: TCharacter::BeginFighting @ 0x004d3b90 -- into the combat (or
// bow) root, on `target` or the nearest enemy ahead. Refused while running
// or sneaking or invoking; only then are the characters seen marked as
// fought (no auto-combat on them). Already in that root: SetFighting. The
// new root is a fight in the walk root for a walk fighter (charflags
// 0x4000), and it needs its state; a step stops first; it takes priority
// only with a transition into it, and lets the walk root (or the other
// fighting root) go. It faces the target unless retreating; its move angle
// is the step's while stepping, else that facing. A refused root is freed.
bool TCharacter::BeginFighting(TCharacter* target, ACTION action)
{
    if (beginFightingSeam)
        return beginFightingSeam(this, target, action);
    if (root)
    {
        if ((root->action == ACTION_COMBAT && (root->Is("combatrun") || root->Is("handrun"))) ||
            (root->action == ACTION_BOW && root->Is("bowrun")) || root->Is("run") || root->Is("sneak"))
            return false;
    }
    if (doing && doing->action == ACTION_INVOKE)
        return false;

  // Every character seen is now one fought: auto-combat leaves them be.
    SetHasSeenAutoCombat(false);

  // Don't target dead guys
    if (target && (target->IsDead() || target->IsInvisibleSpell()))
        target = nullptr;

  // If player doesn't have bow, escape out
    if (action == ACTION_BOW && ObjClass() == OBJCLASS_PLAYER && !static_cast<TPlayer*>(this)->RangedWeapon())
        return false;

    if (!target)
        target = FindCharacter(-1, GetFace(), 32, FINDCHAR_ENEMY | FINDCHAR_HEAR | FINDCHAR_SEE);

  // REVSYNC: 0x004d3d2d -- a fight starting commits the player's typed line.
    TextBar.CommitInput();

    if (root->action == action)
        return SetFighting(target);

    const char* rootname = nullptr;
    if (charflags & kCharFlagWalkFighter)
        rootname = GetWalkRoot();
    else if (action == ACTION_BOW)
        rootname = GetBowRoot();
    else if (action == ACTION_COMBAT)
        rootname = GetCombatRoot();
    else
        FatalError("Invalid fighting action");

  // Doesn't have a combat state.. don't keep calling this function!
    if (FindState(rootname) < 0)
        return false;

    const bool stepping = doing && (doing->action == ACTION_MOVE || doing->action == ACTION_COMBATMOVE ||
                                    doing->action == ACTION_BOWMOVE);
    if (stepping)
        Stop();

    auto* ab = new TActionBlock(rootname, action);
    ab->obj = target;
  // Priority only with a transition into it; else it goes to its state at
  // once, with nothing to wait out.
    ab->priority = (FindTransitionState(doing->name, ab->name) >= 0);
    ab->interrupt = true;
    if (doing->Is(GetWalkRoot()) || (action == ACTION_BOW && (doing->action == ACTION_COMBAT ||
                                                              doing->action == ACTION_IMPACT)) ||
        (action == ACTION_COMBAT && doing->action == ACTION_BOW))
        doing->priority = false;

    ab->angle = (target && !retreating) ? AngleTo(target) : GetFace();
    ab->moveangle = stepping ? GetMoveAngle() : ab->angle;

    if (!SetDesired(ab))
        delete ab;

  // REVSYNC: 0x004d3f8b -- the opponent is both the block's user and its
  // enemy; a trigger that ran turns the incidentals off.
    if (GetScript())
    {
        TCharacter* enemy = Fighting();
        if (GetScript()->Trigger(TRIGGER_COMBAT, nullptr, nullptr, enemy, kAliasUser, enemy, kAliasEnemy))
            charflags |= kCharFlagNoIncidentals;
    }

    nextattack = -1;

    return true;
}

// REVSYNC: TCharacter::EndFighting @ 0x004d3fd0 -- out of the combat or bow
// root into the walk root (slot 0x30c: the player's "torch" with a light in
// hand). Done already outside them; refused when dead, in an interactive
// attack or impact (unless charflags 0x80000), or with no walk root. A step
// stops first. Through a transition the walk takes priority and is marked
// a transition (0x004d4164); the root as doing lets go of its priority. A
// refused block is freed. The move angle follows the facing again unless
// it doesn't turn (charflags 4), and the characters seen are marked fought.
bool TCharacter::EndFighting()
{
    if (endFightingSeam)
        return endFightingSeam(this);
    if (!IsFighting())
        return true;
    if (Health() <= 0)
        return false;
    if (!(charflags & kCharFlagInteractive) &&
        ((doing->attack && (doing->attack->flags & CA_INTERACTIVE)) ||
         (doing->impact && (doing->impact->flags & CAI_INTERACTIVE))))
        return false;

    const char* walkroot = GetWalkRoot();
    if (FindState(walkroot) < 0)      // Avoid trying to set walk state in the future
        return false;

    if (doing && (doing->action == ACTION_MOVE || doing->action == ACTION_COMBATMOVE ||
                  doing->action == ACTION_BOWMOVE))
        Stop();

    auto* ab = new TActionBlock(walkroot, ACTION_ANIMATE);
    const bool transition = FindTransitionState(doing->name, ab->name) >= 0;
    ab->priority = transition;
    ab->transition = transition;
    if (doing == root)
        doing->priority = false; // Allow him to override a BeginCombat() action
    if (!SetDesired(ab))
        delete ab;

  // Make sure moving angle equals face (it doesn't during a combat move)
    if (!(charflags & kCharFlagNoTurn))
        SetMoveAngle(GetFace());

  // Every character seen is now one fought: auto-combat leaves them be.
    SetHasSeenAutoCombat(false);

    return true;
}

// REVSYNC: 0x004d4790 (in part). No target drops the current one from the
// doing, root and desired actions without starting combat -- what a
// teleport does (EXITS.md §3.1); 1998 entered combat with nobody. Retail's
// gates for a live target (busy attack/impact blocks, the player's pending
// attack fields) aren't compared yet.
// REVSYNC: SetFighting @ 0x004d4790 -- fight `newtarget` (nullptr: no one).
// Refused while dead or in an interactive move, for itself or a dead
// target; outside a combat/bow root a target starts the fight
// (BeginFighting); else every block takes the target and faces it (not
// while retreating). The network message (0x21) isn't ported.
bool TCharacter::SetFighting(TCharacter* newtarget)
{
    if (Health() <= 0)
        return false;
    if (!(charflags & kCharFlagInteractive))
    {
        if (doing->attack && (doing->attack->flags & CA_INTERACTIVE))
            return false;
        if (doing->impact && (doing->impact->flags & CAI_INTERACTIVE))
            return false;
    }
    if (newtarget == this)
        return false;
    if (newtarget && newtarget->Health() <= 0)
        return false;

    const bool fighting = root && (root->action == ACTION_COMBAT || root->action == ACTION_BOW);
    if (!fighting && newtarget)
        return BeginFighting(newtarget, ACTION_COMBAT);

    if (doing->obj == newtarget)
        return true;
    doing->obj = desired->obj = root->obj = newtarget;
    if (newtarget)
    {
        const int32_t angle = AngleTo(newtarget);
        if (!retreating)
            doing->angle = desired->angle = root->angle = angle;
    }
    if (ObjClass() == OBJCLASS_PLAYER)
    {
        lastbutton = -1;
        buttonrepeat = 0;
    }
    return true;
}

bool TCharacter::PlayAnim(char *string)
{
    if (!HasActionAni(string))
        return false;

    TActionBlock* ab = new TActionBlock(string, ACTION_ANIMATE);
    SetDesired(ab);

    return true;
}

bool TCharacter::ExecutingQueued()
{
    if (desired && (desired->Is("walkl") || desired->Is("walkr")) &&
        (desired->target.x || desired->target.y))
        return true;

    return false;
}

int32_t TCharacter::CursorType(TObjectInstance* inst)
{
    if (inst)
        return CURSOR_NONE;

    if (IsDead())
    {
        if (RealNumInventoryItems() > 0)
            return CURSOR_HAND;         // loot the corpse
        else
            return CURSOR_NONE;         // He's dead, Jim
    }

    if (Aggressive())
        return CURSOR_NONE;
//      return CURSOR_SWORDS;           // kick its ass

    return CURSOR_MOUTH;                // chat for a bit
}

// REVSYNC: Use @ 0x004d4a60. An object used on a character is a GET for
// the character and a GIVE for the giver; a character used without one is
// looted when dead and talked to otherwise. Not ported yet: when GET, GIVE or
// DIALOG fires, retail also turns incidentals off for both characters, stops
// them (0x004cee70) and marks the character (+0x108).
bool TCharacter::Use(TObjectInstance* user, int32_t with)
{
    if (TObjectInstance::Use(user, with))   // objects that combine
        return true;

    if (with >= 0)
    {
        TObjectInstance* item = MapPane.GetInstance(with);
        if (!item)
            return false;
        if (GetScript())
            GetScript()->Trigger(TRIGGER_GET, item->GetName(), nullptr, user, kAliasUser, item, kAliasItem);
        if (user && user->GetScript())
            user->GetScript()->Trigger(TRIGGER_GIVE, item->GetName(), nullptr, this, kAliasUser, item, kAliasItem);
        return true;
    }

    if (Health() <= 0 && user)
    {
        // loot the corpse
        TInventoryIterator i(this);
        TObjectInstance* oi = i.Item();

        if (oi)
        {
            if ((uint32_t)user->FindFreeInventorySlot() >= MAXINVITEMS)
                TextBar.Print("Can't carry any more.");
            else
            {
                // Before the add: gold or food may merge into a pile and be deleted
                char buf[80];
                snprintf(buf, sizeof(buf), "%s taken from corpse of %s.", oi->GetName(), GetName());

                oi->RemoveFromInventory();
                user->AddToInventory(oi);

                TextBar.Print("%s", buf);
            }

            return true;
        }

        return false;
    }

    if (Aggressive() || !user || user->ObjClass() != OBJCLASS_PLAYER)
        return false;

  // Face each other, then start the DIALOG block with the player as its user.
    S3DPoint upos;
    user->GetPos(upos);
    int32_t angle = ConvertToFacing(pos, upos);
    Face(angle);
    angle = ConvertToFacing(upos, pos);
    user->Face(angle);

    if (GetScript())
        GetScript()->Trigger(TRIGGER_DIALOG, nullptr, nullptr, user, kAliasUser);

    return true;
}

// REVSYNC: CharBlocking @ 0x004d4db0 -- the first character, in map order
// within 0x80 of pos, that would stop a mover of `radius` there: not inst,
// alive, not in an interactive attack, not flying, visible, not on a MoveTo,
// touching (edge to edge, 0 or less), and not an idle player (state bit 2).
TCharacter* TCharacter::CharBlocking(TObjectInstance* inst, const S3DPoint& pos, int32_t radius)
{
    constexpr int32_t kRange = 0x80;
    auto blocks = [&](TCharacter* c) {
        if (!c || c == inst || c->Health() <= 0)
            return false;
        if (c->doing->attack && (c->doing->attack->flags & CA_INTERACTIVE))
            return false;
        if ((c->GetAniFlags() & AF_FLY) || (c->flags & OF_INVISIBLE) || c->movetopos)
            return false;
        if (::Distance(pos, c->Pos()) - radius - c->Radius() > 0)
            return false;
        return !(c->ObjClass() == OBJCLASS_PLAYER && (static_cast<TPlayer*>(c)->PlayerState() & 2));
    };

    TCharacter* found = nullptr;
    ForCharactersNear(inst->GetLevel(), pos, kRange, [&](TCharacter* c) {
        if (!blocks(c))
            return true;
        found = c;
        return false;
    });
    return found;
}

// ------------- Streaming functions ------------------

// REVSYNC: TCharacter::Load @ 0x004d4eb0 (SAVE_GAME.md §11.3).
void TCharacter::Load(RTInputStream is, int32_t version, int32_t objversion)
{
    uint8_t basever = 0;
    if (objversion >= 3)
        is >> basever;
    TComplexObject::Load(is, version, basever);

  // Get saved last pulse values (so we can figure what has happened to char)
    if (objversion < 1)
        return;

  // Last time any health/fatigue/mana was recovered
    is >> lasthealthrecov;
    is >> lastfatiguerecov;
    is >> lastmanarecov;

    if (objversion >= 4)
    {
        is >> lastpoisondamage;
    }
    else
    {
        lastpoisondamage = -1;
    }

    if (objversion >= 2)
    {
        is >> teleport_position.x;
        is >> teleport_position.y;
        is >> teleport_position.z;
        is >> teleport_level;
    }
    else
    {
        teleport_position.x = teleport_position.y = teleport_position.z = -1;
        teleport_level = -1;
    }

    if (ObjClass() == OBJCLASS_CHARACTER && chardata)
    {
        if (Health() > chardata->health)
            SetHealth(chardata->health);
        if (MaxHealth() > chardata->health)
            SetMaxHealth(chardata->health);
        if (Fatigue() > chardata->fatigue)
            SetFatigue(chardata->fatigue);
        if (MaxFatigue() > chardata->fatigue)
            SetMaxFatigue(chardata->fatigue);
        if (Mana() > chardata->mana)
            SetMana(chardata->mana);
        if (MaxMana() > chardata->mana)
            SetMaxMana(chardata->mana);
    }

  // A character saved dead is removed on its first frame.
    if (Health() < 1)
        ResetFlags(flags | OF_KILL);

  // Fully visible, not fading.
    fade = 100;
    fade_step = 0;
    fade_limit = 100;
    fade_direction = 0;
}

// REVSYNC: TCharacter::Save @ 0x004d50d0.
void TCharacter::Save(RTOutputStream os)
{
    os << (uint8_t)TComplexObject::ObjVersion();
    TComplexObject::Save(os);

  // Last time any health/fatigue/mana was recovered
    os << lasthealthrecov;
    os << lastfatiguerecov;
    os << lastmanarecov;

  // Last time poison damage was charged
    os << lastpoisondamage;

  // Our teleport positions
    os << teleport_position.x;
    os << teleport_position.y;
    os << teleport_position.z;
    os << teleport_level;
}

// make a character visible
void TCharacter::MakeVisible()
{
    is_invisible = false;

    TCharAnimator* animator = (TCharAnimator*)GetAnimator();

    for(int32_t i = 0; i < animator->Get3DImagery()->NumMaterials(); ++i)
    {
        S3DMat mat;
        animator->Get3DImagery()->GetMaterial(i, &mat);

        S3DMaterial &m = mat.matdesc;

        m.ambient.a = 1.0f;
        m.diffuse.a = 1.0f;
        m.specular.a = 1.0f;

        animator->Get3DImagery()->SetMaterial(i, &mat);
    }
}

// make a character invisible
void TCharacter::MakeInvisible()
{
    is_invisible = true;

    TCharAnimator* animator = (TCharAnimator*)GetAnimator();

    for(int32_t i = 0; i < animator->Get3DImagery()->NumMaterials(); ++i)
    {
        S3DMat mat;
        animator->Get3DImagery()->GetMaterial(i, &mat);
        S3DMaterial &m = mat.matdesc;

        m.ambient.a = .5f;
        m.diffuse.a = .5f;
        m.specular.a = .5f;

        animator->Get3DImagery()->SetMaterial(i, &mat);
    }
}

// REVSYNC: TCharacter::Fade @ 0x004d56c0 (`fadecharacterin/out`) -- +1
// fades back in to 100, but only while the character is alive; anything
// else fades out to 0. 5 a pulse either way.
void TCharacter::Fade(int32_t direction)
{
    fade_direction = direction;
    if (direction == 1)
    {
        if (Health() > 0)
        {
            fade_step = -5;
            fade_limit = 100;
        }
        return;
    }

    fade_step = 5;
    fade_limit = 0;
    fade_direction = -1;
}

// REVSYNC: TCharacter::SetFade @ 0x004d5730 -- a dead character can only
// fade out. A negative 'amt' keeps the current visibility.
void TCharacter::SetFade(int32_t amt, int32_t amt2, int32_t amt3)
{
    if (Health() <= 0 && amt2 < 0)
        return;

    if (amt >= 0)
        fade = amt;
    fade_step = amt2;
    fade_limit = amt3;
    fade_direction = (amt2 > 0) ? -1 : 1;
}

// REVSYNC: TCharacter::UpdateFade @ 0x004d57a0 -- one pulse of the fade.
// Fading in past 0 makes an object-invisible character visible again; the
// fade stops (step and direction 0) at its limit, at 0, or at 100.
void TCharacter::UpdateFade()
{
    if (fade_step == 0)
        return;

    fade -= fade_step;
    if (fade > 0 && fade_step < 0 && (flags & OF_INVISIBLE))
        SetFlag(OF_INVISIBLE, false);

    if (fade_limit == -1)
    {
        if (fade < 0)
        {
            fade = 0;
            fade_step = fade_direction = 0;
        }
    }
    else if (fade_step < 0)
    {
        if (fade > fade_limit)
        {
            fade = fade_limit;
            fade_step = fade_direction = 0;
        }
        if (fade < 0)
        {
            fade = 0;
            fade_step = fade_direction = 0;
        }
        return;     // retail skips the 100 cap on the way in
    }
    else if (fade < fade_limit)
    {
        fade = fade_limit;
        fade_step = fade_direction = 0;
    }

    if (fade > 100)
    {
        fade = 100;
        fade_step = fade_direction = 0;
    }
}

// REVSYNC: TCharacter::SetInvisible @ 0x004d5880 -- the invisibility spell
// fades its target to 30; ending it fades a living character back to 100.
void TCharacter::SetInvisibleSpell(bool on)
{
    if (invisible_spell == on)
        return;

    invisible_spell = on;
    if (on)
    {
        fade_step = 5;
        fade_limit = 30;
        fade_direction = -1;
    }
    else if (Health() > 0)
    {
        fade_step = -5;
        fade_limit = 100;
        fade_direction = 1;
    }
}

// The invoke animation named by the spell's ANIMATION: one with "inv" in
// it becomes the character's own "<prefix>inv<last char>" (invoke2 ->
// "cinv2", "hinv2", ...); without it, "invoke"; without that, none. A
// priority-free ACTION_INVOKE block waits the invoke delay, which the
// character keeps too, and is forced; one ForceCommand didn't take (doing
// has priority) is dropped.
bool TCharacter::SetCast(const char* ani, TObjectInstance* target, int32_t invoke_delay)
{
    char name[RESNAMELEN];
    if (strstr(ani, "inv"))
    {
        char base[RESNAMELEN];
        snprintf(base, sizeof(base), "inv%s", ani + strlen(ani) - 1);
        CombatAnimName(name, base);
    }
    else
        strncpyz(name, ani, RESNAMELEN);
    if (!HasActionAni(name))
    {
        strncpyz(name, "invoke", RESNAMELEN);
        if (!HasActionAni(name))
            return false;
    }

    auto* ab = new TActionBlock(name, ACTION_INVOKE);
    ab->priority = false;
    invokedelay = invoke_delay;
    ab->wait = invoke_delay;
    ab->obj = target;
    ForceCommand(ab, 0, 0);
    if (doing && doing->priority && desired != doing)
        delete ab;
    return true;
}

bool TCharacter::MayCast()
{
    return Health() > 0 && !InteractiveLocked() && !(flags & OF_IMMOBILE) && !(flags & (OF_ICED | OF_PARALIZE));
}

bool TCharacter::Cast(char* talismans, S3DPoint* sourcepos)
{
    if (!MayCast())
        return false;
    TObjectInstance* targ = nullptr;
    if (root && (root->action == ACTION_COMBAT || root->action == ACTION_BOW))
        targ = root->obj;
    return CastByTalismans(talismans, &targ, targ ? 1 : 0, sourcepos);
}

bool TCharacter::CastByName(const char* name, TObjectInstance* *target, int32_t numtargs, S3DPoint* sourcepos)
{
    if (castByNameSeam)
        return castByNameSeam(this, name, target, numtargs, sourcepos);
    if (!MayCast())
        return false;
    return CastVariant(name, target, numtargs, sourcepos);
}

bool TCharacter::CastVariant(const char* name, TObjectInstance* *target, int32_t numtargs, S3DPoint* sourcepos)
{
    if (castSeam)
        return castSeam(this, name, target, numtargs, sourcepos);
    SSpellVariant* variant = SpellList.GetVariantDataByName(name);
    if (!variant)
        return false;
    return CastByTalismans(variant->talismans, target, numtargs, sourcepos);
}

// A player's state bit 2 goes off before the cast. Network play -- the
// cast's authority test, the echo to the other players, their SPLCASTOK /
// SPLCASTFAIL notes -- isn't ported: the port is single player, where retail
// casts locally.
bool TCharacter::CastByTalismans(char* talismans, TObjectInstance* *target, int32_t numtargs, S3DPoint* sourcepos)
{
    if (castByTalismansSeam)
        return castByTalismansSeam(this, talismans, target, numtargs, sourcepos);
    if (!MayCast())
        return false;
    if (objclass == OBJCLASS_PLAYER)
    {
        auto* player = static_cast<TPlayer*>(this);
        if (player->PlayerState() & 2)
            player->SetPlayerState(player->PlayerState() & ~2);
    }
    if (SpellManager.CastByTalismans(talismans, this, target, numtargs, sourcepos))
        return true;

    if (objclass != OBJCLASS_PLAYER)
        return false;
    const SSpellVariant* variant = SpellList.GetVariantDataByTalismans(talismans);
    if (!variant || variant->mana <= 0 || !MayCast())
        return false;
    if (SSpellVariant* fizzle = SpellList.GetVariantDataByName("fizzle"))
        CastByTalismans(fizzle->talismans, nullptr, 0, nullptr);
    return false;
}

// Flail - Make the character act a fool
bool TCharacter::Flail()
{
    if (IsFlailing())
        return true;

    TActionBlock* ab = new TActionBlock("impact", ACTION_FLAIL);
    ab->priority = true;
    ForceCommand(ab);

    return true;
/*
    if (!burning)
    {
        SObjectDef def;
        memset(&def, 0, sizeof(SObjectDef));
        def.objclass = OBJCLASS_EFFECT;
        def.level = MapPane.GetMapLevel();
        def.pos = Pos();
        def.facing = GetFace();
        def.objtype = EffectClass.FindObjType("BURN");

        TBurnEffect* burn = (TBurnEffect*)MapPane.GetInstance(MapPane.NewObject(&def));
        if (!burn)
            return false;
        burning = burn;

        // init all the params
        burn->Set(this);

        // now make him do the uncontrolable chicken on fire dance of death!
        // create action block
        TActionBlock* ab = new TActionBlock("onfire", ACTION_BURN);
        ab->priority = true;
        ForceCommand(ab);
    }
    else
    {
        ((TBurnEffect*)burning)->ResetFrameCount();
    }

    return true;
*/
}
