// *************************************************************************
// *                         Cinematix Revenant                            *
// *                    Copyright (C) 1998 Cinematix                       *
// *                   character.h - TCharacter module                     *
// *************************************************************************

#ifndef _CHARACTER_H
#define _CHARACTER_H

#ifndef _REVENANT_H
#include "revenant.h"
#endif

#ifndef _COMPLEXOBJ_H
#include "complexobj.h"
#endif

#ifndef _RULES_H
#include "rules.h"
#endif

#ifndef _CHARSTATS_H
#include "charstats.h"
#endif

#ifndef _SPELL_H
#include "spell.h"
#endif

#include "bossai.h"

#include <array>
#include <vector>

// FindChar flags
#define FINDCHAR_ENEMY     1    // Find only enemies
#define FINDCHAR_HEAR      2    // Find only characters we can hear
#define FINDCHAR_SEE       4    // Find only characters we can see


// Maximum number of characters a character can remember seeing
#define MAXHASSEEN 8
_STRUCTDEF(SHasSeen)
struct SHasSeen
{
    TCharacter* chr;            // Has seen this character
    int32_t time;                   // Game ticks when last seen
    bool noautocombat;          // Prevents player from toggling autocombat
};

// The dice of one blow (COMBAT_HIT.md §3.1), as TCharacter::RollAttack
// throws them: the damage before its TOHITDAMAGE tier, the to-hit and the
// roll. -1 is a die not thrown yet.
struct SAttackRoll
{
    int32_t damage = -1;
    int32_t tohit = -1;
    int32_t roll = -1;

    [[nodiscard]] bool Hits() const { return tohit >= roll; }
      // A blow that fails its roll only glances (ResolveHit)
    [[nodiscard]] int32_t Tiered() const;
      // The damage scaled by the TOHITDAMAGE tier its margin (to-hit less
      // the roll) falls in, at least 1: what the attack carries
};

// A blow weighed against the target's guard (TCharacter::WeighBlow).
struct SBlowOutcome
{
    int32_t damage  = 0;        // at least 1
    int32_t tohit   = 0;        // less the guard's 50 when it counted
    bool    hit     = false;    // the roll made: a hit, not a glance
    bool    blocked = false;    // the guard turned a hit into a glance
};

// the default invoke animation delay
#define INVOKE_DELAY    20

_CLASSDEF(TCharacter)
class TCharacter : public TComplexObject
{
  public:
    void ClearChar();       // Clear out working vars of char

    TCharacter(TObjectImagery* newim) : TComplexObject(newim) { ClearChar(); }
    TCharacter(SObjectDef* def, TObjectImagery* newim) : TComplexObject(def, newim) { ClearChar(); }

    int32_t Distance(const TObjectInstance* inst) const override;
      // REVSYNC: TCharacter::Distance @ 0x004d61b0 -- edge to edge: the
      // centre distance less this character's Radius and, for a character
      // target, its Radius; never below 0
    bool IsValidTarget(TCharacter* target);
      // REVSYNC: 0x004cd990 -- may `target` be fought: there, alive, not
      // invisible, within combat range, and the player only while he has
      // control or demo mode is on
    void SetRunsAI(bool on) { if (on) charflags |= kCharFlagPlayerAI; else charflags &= ~kCharFlagPlayerAI; }
      // A player's AI switch (retail charflags 0x100000; AI() gates on it)

  // Retail A/B fixtures only (retailab_*.cpp; docs/gameplay/COMBAT_DOJO.md
  // §6.3): when set, each answers its function in place of the world, as the
  // retail fixture's seam at the same function does.
    using FindCharactersSeam = int32_t (*)(TCharacter* self, TCharacter* chars[], int32_t maxchars,
        int32_t range, int32_t angle, int32_t anglerange, int32_t flags);
    static inline FindCharactersSeam findCharactersSeam = nullptr;
      // FindCharacters (retail 0x004cd690)
    using BlockedSeam = bool (*)(TCharacter* self, const S3DPoint& pos, const S3DPoint& newpos, uint32_t bits,
        TCharacter** bychar);
    static inline BlockedSeam blockedSeam = nullptr;
      // Blocked (retail FindClearPath 0x004c39d0)
    using CanSeeSeam = bool (*)(TCharacter* self, TCharacter* chr, int32_t angle);
    static inline CanSeeSeam canSeeSeam = nullptr;
      // CanSeeCharacter (retail 0x004cd540)
    using BlockSeam = bool (*)(TCharacter* self, int32_t frames);
    static inline BlockSeam blockSeam = nullptr;
      // Block (retail 0x004d2e30), which IsValidAttack calls on a target
      // about to be hit glancingly
    using CastSeam = bool (*)(TCharacter* self, const char* spell, TObjectInstance** targets, int32_t numtargs,
        const S3DPoint* source);
    static inline CastSeam castSeam = nullptr;
      // CastVariant (retail SpellList::Find 0x0053f010 + Cast 0x004d5c20),
      // which DoAttack calls for a MAGICATTACK and CastByName past its gates
    static inline CastSeam castByNameSeam = nullptr;
      // CastByName whole (retail 0x004d5b90), gates and all
    static inline CastSeam castByTalismansSeam = nullptr;
      // CastByTalismans (retail 0x004d5c20), as castByNameSeam for CastByName
    using IsEnemySeam = bool (*)(TCharacter* self, TCharacter* other);
    static inline IsEnemySeam isEnemySeam = nullptr;
      // IsEnemy (retail 0x004c89c0)
    using BeginFightingSeam = bool (*)(TCharacter* self, TCharacter* target, ACTION action);
    static inline BeginFightingSeam beginFightingSeam = nullptr;
      // BeginFighting (retail 0x004d3b90)
    using ModeSeam = bool (*)(TCharacter* self);
    static inline ModeSeam setWalkModeSeam = nullptr;
      // SetWalkMode (retail 0x004cf000)
    static inline ModeSeam setRunModeSeam = nullptr;
      // SetRunMode (retail 0x004cf490)
    static inline ModeSeam endFightingSeam = nullptr;
      // EndFighting (retail 0x004d3fd0), which Wander and Baez's fall call
    using DamageSeam = void (*)(TCharacter* self, int32_t damage, int32_t damagetype, int32_t modifier,
        TActionBlock* action, TCharacter* attacker);
    static inline DamageSeam damageSeam = nullptr;
      // Damage (retail 0x004c4950); the seam owns `action` as Damage does
    using EffectBurstSeam = void (*)(TCharacter* self, const char* name, int32_t height);
    static inline EffectBurstSeam effectBurstSeam = nullptr;
      // EffectBurst (retail 0x004c85d0)
    using EffectCombatFlashSeam = void (*)(TCharacter* self);
    static inline EffectCombatFlashSeam effectCombatFlashSeam = nullptr;
      // EffectCombatFlash (retail 0x004c8500)
    using WaypointsSeam = int32_t (*)(TCharacter* self, const S3DPoint& centre, TObjectInstance** found, int32_t max);
    static inline WaypointsSeam waypointsSeam = nullptr;
      // The helpers AI's waypoint search finds near a point (retail
      // TMapPane::FindObjectsInRange 0x00452060 and GetInstance 0x00452690)
    using KnockBackSeam = void (*)(TCharacter* self, const S3DPoint& from, int32_t variant);
    static inline KnockBackSeam knockBackSeam = nullptr;
      // KnockBack (retail 0x004d3750)
    using SignalAttackSeam = void (*)(TCharacter* self, TObjectInstance* actor, TObjectInstance* target, int32_t flag);
    static inline SignalAttackSeam signalAttackSeam = nullptr;
      // SignalAttack (retail OnAttacked 0x004cdce0, vtable +0x240)
    using NearbyCharactersSeam = std::vector<TCharacter*> (*)(const S3DPoint& pos, int32_t range);
    static inline NearbyCharactersSeam nearbyCharactersSeam = nullptr;
      // The characters near a point that CharBlocking and FindCharacters
      // walk (retail's map iterator 0x0044ceb0 / 0x0044d080), in map order
    using PaneObjectsSeam = std::vector<TObjectInstance*> (*)(TCharacter* self);
    static inline PaneObjectsSeam paneObjectsSeam = nullptr;
      // The objects a walk of the pane's sector window gives (retail's map
      // iterator 0x0044cf80 with no rect, then 0x0044d080), in map order
    using TintSeam = void (*)(TCharacter* self, bool tint);
    static inline TintSeam tintSeam = nullptr;
      // TintMaterials / UntintMaterials (retail 0x004d6a30 / 0x004d6b40)

    bool IsAnimatorPermanent() const override { return true; }
        // Characters always own a TObjectAnimator from construction. See
        // TObjectInstance::IsAnimatorPermanent for the contract.

    int32_t CursorType(TObjectInstance* inst = nullptr) override;
        // Talk icon if they are friendly, attack icon if aggressive, hand if dead
    virtual bool Use(TObjectInstance* user, int32_t with = -1);
        // Talk to or attack character
    virtual bool DrawShadow() { return false; }
        // no shadow // Characters get a little shadow that follows them
    virtual void Pulse();
      // Calls ExecuteAction
    virtual void Animate(bool draw);
      // Draw character
    virtual uint32_t Move();
      // Called by system when character moves
    virtual void Notify(int32_t notify, void *ptr);
        // Notify Action (check if objects we depend on are killed)
    virtual int32_t CalculateDamage(int32_t damage, int32_t damagetype, int32_t modifier);
        // Calculate the total damage for the character based on a base damage
        // value (i.e. from the weapon), the damage type (i.e. the value returned from
        // GetDamageType()), and a percentage modifier such as +10%(10) or -30%(-30).  Damage
        // is calculated as damage * (100% + modifier) * (100% + chardmgmodifier).
    virtual void Damage(int32_t damage, int32_t damagetype = DT_NONE, int32_t modifier = 0,
        PTActionBlock action = nullptr, TCharacter* attacker = nullptr);
        // This character takes 'damage' from 'attacker' (retail 0x004c4950, vtable
        // +0x228): exactly as given for DT_NONE, else through CalculateDamage. It
        // shows it with the impact or death block 'action' when that suits, else
        // one of its CHARIMPACTs or the stock "impact" / "dead" names. Damage owns
        // 'action': it becomes one of this character's blocks or is freed.
    virtual void Killed(TCharacter* /*victim*/) {}
        // This character landed a killing blow (retail vtable +0x244; the
        // player counts it)
    virtual void Died(TCharacter* /*killer*/) {}
        // This character's health reached 0 (retail +0x248): from Damage with the
        // killer, from Pulse with none
    virtual int32_t Clock();
        // REVSYNC: slot 0x314 -- the game time (1/100 s) this character's
        // recovery and poison run on: the PlayScreen's (0x004d5f50), the
        // player's own play clock (TPlayer 0x0051d800)
    enum class EVital { Health, Fatigue, Mana };
    virtual int32_t RecoveryBonus(EVital /*vital*/) { return 0; }
        // What a tick adds to a vital's recovery, in 1/10000 points: none
        // but for the player (his HealthPct / FatiguePct / ManaPct)
    void RestoreHealth();
        // Cure them of all ailments and set health to max

    virtual void AI();
        // Causes the object to perform its A.I. routines
    void Wander();
        // REVSYNC: Wander @ 0x004c9790 (slot 0x234) -- what an AI-driven
        // character (charflags 0x100000) does before its AI each tick: an
        // idle count down; a run in a fighting root turned to a walk; run
        // bursts and their ends; out of combat, a patrol from waypoint to
        // waypoint (COMBAT_ATTACK_CHOICE.md §3.15)

    virtual const char *DefaultRootState() { return (Sleeping() ? "sleep" : Aggressive() ? "combat" : IsDead() ? "dead" : "walk"); }
      // Returns the default root state for this char

  // Action response functions to trigger character AI
    virtual void SignalMovement(TObjectInstance* actor);
        // Actor is moving
    virtual void SignalHostility(TObjectInstance* actor, TObjectInstance* target);
        // Actor is hostile to target
    virtual void SignalAttack(TObjectInstance* actor, TObjectInstance* target, int32_t flag = 0);
        // Actor is attacking target (retail OnAttacked 0x004cdce0, slot 0x240; flag 2 for a spell)

  // ActionBlock generic function callers
    bool SetWalkMode();
      // Sets walk mode
    bool SetSneakMode();
      // Sets sneak mode
    bool SetRunMode();
      // Sets run mode
    bool Go(int32_t angle = -1);
      // Start a character moving in the given angle and speed (entry point)
    bool Go(S3DPoint vect);
      // Start a character moving in the given movement vector
    bool Goto(int32_t x, int32_t y, TObjectInstance* pickup = nullptr);
      // Causes character to go to x,y; an item given is picked up on arrival
    bool Stop(char *name = nullptr);
      // Stops specified action, or any action if name is nullptr
    bool Disable();
      // Disables character's AI (for freezing, etc.)
    bool Pulp(S3DPoint vel, int32_t piece_count, int32_t blood_count);
      // Causes a character to explode into body parts, blood, & guts
    bool Burn();
      // Causes a character to combust into a seething sweltering mass of flames
    void ClearBurn() { burning = nullptr; }
      // Sets burning pointer to nullptr
    bool Flail();
      // Causes a character to act a fool
    bool KnockBack(S3DPoint frompos, int32_t variant = -1);
      // Thrown back by a blow from frompos: one of five impacts (`variant`
      // 0..4, else random) facing it, or the back impact from behind (retail
      // 0x004d3750)
    bool Jump();
      // Causes character to jump (in normal mode, use Leap in Combat mode)
    bool Pivot(int32_t angle);
      // Pivots character to given direction (in 32 increments)
    bool FollowChar(TObjectInstance* inst);
      // Causes character to follow another character.
    bool Pickup(TObjectInstance* inst, TObjectInstance* to = nullptr);
      // Causes character to move to and pickup object.
    void DropLoot();
      // REVSYNC: 0x004d6570 -- a dead character's things burst out of it:
      // gold at its feet, the rest thrown out around it (DEATH_LOOT.md §2)
    bool Pull(TObjectInstance* inst);
      // Causes character to pull lever
    TObjectInstance* FindUseTarget(TObjectInstance* target = nullptr);
      // U's eligible target in its forward reach; an explicit target must
      // meet the same checks. Shared with mouse activation of exits.
    bool TryUse();
      // Uses the nearest container, exit or friendly character in front of
      // the character (the Use control; retail 0x004d01e0)
    bool TryGet();
      // Picks up the nearest item in front of the character (the Get
      // control; retail 0x004d0430)
    bool Say(const char *string, int32_t wait = -1, const char *anim = nullptr, const char *sound = nullptr);
      // Causes character to blather incessantly about something irrelevant
      // (anim is override for animation to play when saying, nullptr is "say")
      // Tag indicates that the say command is a index tag into the DialogList
      // list of dialog lines.  The tag will also be used to play the dialog wave file.
    [[nodiscard]] static int32_t SpeechTicks(int32_t wait, int32_t voicems, const char *line);
      // How long Say holds a line, in ticks (DIALOG.md §3.2): `wait` when
      // given (>= 0), else the voice's length, else the line's (`line` is
      // DialogLine's output).
    bool SayTag(int32_t tagid, int32_t wait = -1, const char *anim = nullptr);
      // Says something given a dialog tag id number
    bool SayTag(const char *tag, int32_t wait = -1, const char *anim = nullptr);
      // Says something given a dialog tag
    bool MayCast();
      // The gates every cast opens with (retail inline in 0x004d5c20, 0x004d5b90,
      // 0x004d5ae0): alive, not locked in an interactive move, not immobile,
      // iced or paralyzed
    bool CastByName(const char* name, TObjectInstance* *target = nullptr, int32_t numtargs = 0, S3DPoint* sourcepos = nullptr);
      // REVSYNC: TCharacter::CastByName @ 0x004d5b90 -- the gates, then
      // CastVariant
    bool CastVariant(const char* name, TObjectInstance* *target = nullptr, int32_t numtargs = 0, S3DPoint* sourcepos = nullptr);
      // The variant of that name (TSpellList::GetVariantDataByName 0x0053f010),
      // cast by its talismans: CastByName past its gates, and DoAttack's
      // MAGICATTACK (0x004d2286), which gates for itself
    bool CastByTalismans(char* talismans, TObjectInstance* *target = nullptr, int32_t numtargs = 0, S3DPoint* sourcepos = nullptr);
      // REVSYNC: TCharacter::CastByTalismans @ 0x004d5c20 -- the manager's
      // cast; a player's failed cast of a spell with a cost fizzles
    bool SetCast(const char* ani, TObjectInstance* target, int32_t invoke_delay = INVOKE_DELAY);
      // REVSYNC: TCharacter::SetCast @ 0x004d5900 -- the invoke animation
    bool Cast(char* talismans, S3DPoint* sourcepos = nullptr);
      // REVSYNC: TCharacter::Cast @ 0x004d5ae0 -- cast at the combat or bow
      // root's opponent
    bool BeginFighting(TCharacter* target = nullptr, ACTION action = ACTION_COMBAT);
      // Engage character in combat
    bool EndFighting();
      // Leave combat mode
    bool BeginCombat(TCharacter* target = nullptr)
      { return BeginFighting(target, ACTION_COMBAT); }
      // Enter combat mode
    bool EndCombat()
      { return EndFighting(); }
      // Leave combat mode
    bool BeginBowMode(TCharacter* target = nullptr)
      { return BeginFighting(target, ACTION_BOW); }
      // Enter bow combat mode
    bool EndBowMode()
      { return EndFighting(); }
      // Leave bow combat mode
    bool DrawBow();
      // Begins drawing bow or crossbow
    bool AimBow(int32_t angle);
      // Causes character to aim at given angle before shooting bow
    bool AimBowLeft();
      // Causes character to pivot to left when aiming bow
    bool AimBowRight();
      // Causes character to pivot to right when aiming bow
    bool ShootBow(int32_t angle);
      // Shoots bow or crossbow
    bool ButtonAttack(int32_t buttonid);
      // Find attack based on the button the player pressed
    bool ButtonAction(int32_t buttonid);
      // Use attack list in RULES.DEF to do an action (usually a PLAYANIM or MAGIC tag)
    bool RandomAttack(int32_t pcnt);
      // Find a random attack (for a monster) based on a value from 1-100
    bool SpecificAttack(int32_t attacknum);
      // Do a specific attack given the attacknum

  // The attack search (retail's signatures). Every search shares one set of
  // out-values -- impact, damage, to-hit, roll -- that its caller starts at
  // -1 (the roll at 100 for a repeated button); the first candidate that
  // gets as far as the damage fixes them for the rest of the search.
    bool IsValidAttack(int32_t attacknum, int32_t &impactnum, int32_t &damage, int32_t &tohit, int32_t &roll,
        int32_t tdist, int32_t button, int32_t pcnt, int32_t dmgpcnt, int32_t flagmask, int32_t flags,
        TCharacter* targ);
      // REVSYNC: 0x004d1120 -- whether attack `attacknum` can be made now at `targ` (null: none,
      // `tdist` then 10000), fixing the out-values on the way
    [[nodiscard]] SAttackRoll RollAttack(TCharacter& target, const SCharAttackData& attack, int32_t dmgpcnt,
        bool targetopen, SAttackRoll thrown = {});
      // The dice of `attack` at `target`, as IsValidAttack throws them, with
      // no animation read: `targetopen` is a target facing away or swinging
      // itself, `dmgpcnt` the attack choice's RollDamagePercent (the
      // player's StrengthMod instead). A die `thrown` already stays.
    [[nodiscard]] static int32_t RollDamagePercent();
      // The damage percentage an attack choice throws before its search:
      // random(1, 50) + random(1, 50)
    [[nodiscard]] static SBlowOutcome WeighBlow(int32_t damage, int32_t tohit, int32_t roll, bool guarding);
      // ResolveHit's arithmetic for a blow carrying `damage` (tiered) at
      // `tohit` against `roll`: a target guarding it (blocking or dodging,
      // facing the blow) takes 50 off the to-hit, and the damage is tiered
      // again; a failed roll glances; at least 1 either way
    void LandBlow(TCharacter& target, int32_t damage, bool lands, TActionBlock* action);
      // ResolveHit's end: `target`'s Damage when the blow lands (it owns
      // `action`; one that doesn't land is freed), a bow-mode player drawn
      // back into combat, and a player's experience for the blow (each award
      // returns unless the target is dead)
    bool FindButtonAttack(int32_t button, int32_t dmgpcnt, int32_t &attacknum, int32_t &impactnum,
        int32_t &damage, int32_t &tohit, int32_t &roll, bool isaction, TCharacter* targ);
      // REVSYNC: 0x004d1dd0 -- the first valid attack on `button`: responses, then
      // specials, then the rest, each in table order
    bool FindPcntAttack(int32_t pcnt, int32_t dmgpcnt, int32_t &attacknum, int32_t &impactnum,
        int32_t &damage, int32_t &tohit, int32_t &roll);
      // REVSYNC: 0x004d1eb0 -- up to 2n random picks of a valid attack whose
      // percentage is at least `pcnt`, against the fighting target
    bool FindInteractiveAttack(int32_t pcnt, int32_t dmgpcnt, int32_t &attacknum, int32_t &impactnum,
        int32_t &damage, int32_t &tohit, int32_t &roll);
      // REVSYNC: 0x004d1ff0 -- the first valid CA_INTERACTIVE attack, in table order
    bool DoAttack(int32_t attacknum, int32_t impactnum, int32_t damage, int32_t tohit, int32_t roll,
        TCharacter* targ);
      // REVSYNC: 0x004d2120 -- start the chosen attack: an ATTACK block carrying
      // its numbers (or the spell of a MAGICATTACK)
    void CombatAnimName(char *buf, const char *name);
      // REVSYNC: 0x004ce1b0 -- `name` with this character's animation prefix
      // ("c", or the player's weapon/mode prefix); buf holds RESNAMELEN
    const char *AnimPrefix();
      // REVSYNC: slot 0x310, 0x004cdf60 -- the player's animation prefix for his root:
      // h/hr (hand), b/br (bow), c/cr (combat), s (sneak), w/r, t/tr (with a torch)
    bool InteractiveLocked() const;
      // Held in someone's interactive attack (retail's five-line gate opening
      // ButtonAttack, RandomAttack, SpecificAttack, SetFighting, Go, ...): the doing
      // attack is CA_INTERACTIVE or its impact CAI_INTERACTIVE, unless this character
      // is the one making the move (charflags 0x80000)
    bool Swing() { return ButtonAttack(1); }
      // Character swings their weapon at their current target
    bool Thrust() { return ButtonAttack(2); }
      // Character thrusts their weapon at their current target
    bool Chop() { return ButtonAttack(3); }
      // Character chops with weapon at current target
    bool Combo(int32_t num) { return ButtonAttack(3 + num); }
      // Character does combo number num
    bool Block(int32_t frames = -1);
      // A guard for 'frames' ticks (below 0: BLOCK's random range; -2 forces a
      // monster's, as IsValidAttack does), retail 0x004d2e30
    bool StopBlock();
      // Ends a guard (retail 0x004d30f0)
    bool Dodge(int32_t dir);
      // A roll out of the combat stance toward 'dir' (0-7, retail's eight
      // Combat Dodge controls, commands 0x30-0x37), else back (retail 0x004d3150)
    bool SideStep(char dir = 0);
      // Cartwheel/sidestep: step \xc2\xb190\xc2\xb0 of facing using the "sidestepl"/"sidestepr"
      // animation if the root has it. Retail FUN_004d6220 @ 0x4d6220.
      // dir is 'l' or 'r'; pass 0 for random L/R.
    bool Leap(int32_t angle);
    void StartRetreat();
      // Give up the fight and run (retail 0x004d5fc0)
      // Character leaps in the given direction (combat mode only)
    bool PlayAnim(char *string);
      // Causes character to play animation name.
    bool ResolveHit(TCharacter* targ, SCharAttackData* attack, SCharAttackImpact* impact, int32_t damage,
        int32_t tohit, int32_t roll);
      // REVSYNC: 0x004c62b0 -- one character struck by an attack at its impact frame
      // (ResolveAttack calls it for the target and each character in reach)
    [[nodiscard]] const SCharAttackData *LastAttack() const { return lastattack; }
      // The attack last started (retail +0x160; ResolveAttack's first tick); null before any
    [[nodiscard]] bool LastAttackHit() const { return lasthit != 0; }
      // Whether it struck its target at the impact frame (retail +0x168)

  // Info functions specific to characters
    bool IsFighting() { return IsCombat() || IsBowMode(); }
      // Returns whether the character is in a fighting mode
    bool IsCombat() { return IsRoot(ACTION_COMBAT); }
      // Returns whether the character is in combat or not
    bool IsHandCombat() { return root->Is("comhand"); }
      // Returns wether we are in hand to hand combat mode
    bool IsBowMode() { return IsRoot(ACTION_BOW); }
      // Returns whether the character is in combat or not
    bool IsBowDrawn();
      // Returns true if bow is drawn and we are in aim mode
    bool IsAttack() { return IsDoing(ACTION_ATTACK); }
      // Returns whether the character is attacking or not
    TCharacter* Fighting() { if (IsFighting()) return (TCharacter*)root->obj; return nullptr; }
      // Return the current target if they are in combat, nullptr if not fighting anyone
    bool SetFighting(TCharacter* newtarget);
      // Sets the current fighting target
    bool IsTalking() { if (doing && doing->Is("say")) return true; return false; }
    void StopTalking();
      // REVSYNC: 0x004d6000 -- silence the voice and end the say action
      // Returns whether character is talking or not
    bool IsWalkMode() { if (root && 
        ((IsCombat() && (root->Is("combat") || root->Is("hand"))) ||
        (IsBowMode() && root->Is("bow")) ||
        root->Is("walk")) ) return true; return false; }
      // Returns whether character is in walk mode. REVSYNC: the unarmed
      // combat roots are "hand" and "handrun" (retail 0x004cf000 reads
      // "hand" 0x005e064c, 0x004c9790 "handrun" 0x005e0610), not 1998's
      // "comhand" / "comhandrun".
    bool IsRunMode() { if (root && 
        ((IsCombat() && (root->Is("combatrun") || root->Is("handrun"))) ||
        (IsBowMode() && root->Is("bowrun")) ||
        root->Is("run")) ) return true; return false; }
      // Returns whether character is in run mode
    bool IsSneakMode() { if (root && root->Is("sneak")) return true; return false; }
      // Returns whether character is in sneak mode
    bool IsMoving()
        { return doing && 
                 doing->action == ACTION_MOVE || 
                 doing->action == ACTION_COMBATMOVE ||
                 doing->action == ACTION_BOWMOVE; }
      // Returns true if character is doing a moving action (result of calling Go())
    bool IsGoto() 
        { return IsMoving() && (doing->target.x != 0 || doing->target.y != 0 || doing->target.z != 0); }
    bool IsDead() { return (Health() <= 0); }
      // Dammit Jim, I'm a corpse not a doctor..
    bool IsEnemy(TCharacter* chr);
      // Returns true if the character is an enemy
    bool IsFinalState();
      // Returns whether character is in their last days
    [[nodiscard]] const SCharAttackData* CurrentBlow() const
      { return doing && doing->action == ACTION_ATTACK ? doing->attack : nullptr; }
      // The attack its doing block carries while it's in an ATTACK, else
      // null: what an onlooker reads from the wind-up
    int32_t SqrRadius() { int32_t s = Radius(); s *= s; return s; }
      // Util function
    bool ExecutingQueued();
      // Find out whether character is executing a queued set of actions
    PSCharData GetCharData() { return chardata; }
      // Returns character data structure for this char
    ACTION GetMoveAction(ACTION action);
      // Returns the move action for the given root action
    ACTION GetLeapAction(ACTION action);
      // Returns the move action for the given root action
    bool IsFlailing() 
        { return doing && doing->action == ACTION_FLAIL; }
      // Returns true if character is acting like a fool (result of calling Go())

    void ForceCommandDone() { forcecommanddone = true; }
      // Forces the current command to be done

    static constexpr uint32_t kCharFlagNoIncidentals = 0x2;
    // Retail charflags (+0x110) bits the combat code reads:
    static constexpr uint32_t kCharFlagNoTurn        = 0x4;       // DoAttack / Block / EndFighting leave the move
                                                                  // angle alone; the AI doesn't acquire (name unknown)
    static constexpr uint32_t kCharFlagUnkillable    = 0x8;       // Damage leaves at least 1 health (Baez; Solifuge
                                                                  // in the air: BOSS_AI.md)
    static constexpr uint32_t kCharFlagSkyWalkf      = 0x40;      // Solifuge's "skywalkf" plays (no reader found)
    static constexpr uint32_t kCharFlagNoKill        = 0x80;      // IsValidAttack refuses a killing blow but an
                                                                  // interactive death (Baez, Solifuge; the player
                                                                  // with a sword against Yhagoro)
    static constexpr uint32_t kCharFlagBuffed        = 0x20;      // a spell's stat effect was put on (TSpell ctor
                                                                  // 0x0053f190); a buff cast ends his others
    static constexpr uint32_t kCharFlagDamageSeventh = 0x10;      // CalculateDamage /7 (not freeze); Solifuge on the ground
    static constexpr uint32_t kCharFlagHalfPhysical  = 0x100;     // CalculateDamage halves physical; Yhagoro walking
    static constexpr uint32_t kCharFlagHalfMagic     = 0x200;     // CalculateDamage halves magic (6-9); Yhagoro fighting
    static constexpr uint32_t kCharFlagWalking       = 0x400;     // Yhagoro walking (no reader found)
    static constexpr uint32_t kCharFlagFighting      = 0x800;     // Yhagoro fighting (no reader found)
    static constexpr uint32_t kCharFlagWalkPrefix    = 0x2000;    // CombatAnimName tries a "w" name in the walk root
    static constexpr uint32_t kCharFlagWalkFighter   = 0x4000;    // fights in its walk root (BeginFighting, AI)
    static constexpr uint32_t kCharFlagNotTargetable = 0x8000;    // IsValidTarget refuses (the bosses, BOSS_AI.md)
    static constexpr uint32_t kCharFlagPlayAnimRoots = 0x10000;   // a PLAYANIM "c..." / "w..." needs that root
    static constexpr uint32_t kCharFlagDeathHold     = 0x1000;    // the player's death screen waits (TPlayer::Pulse
                                                                  // 0x00518b46); Solifuge holds it while he fights
    static constexpr uint32_t kCharFlagImpaleRelease = 0x20000;   // Yhagoro's impale let go of the player (Pulse
                                                                  // 0x004c200c): his next pulse walks him on
    static constexpr uint32_t kCharFlagDead          = 0x40000;   // Died has been told (Damage; retail's Pulse
                                                                  // too, which clears it when health returns)
    static constexpr uint32_t kCharFlagInteractive   = 0x80000;   // in an interactive move: Go skips its gates
    static constexpr uint32_t kCharFlagPlayerAI      = 0x100000;  // the player runs AI() (retail: set for
                                                                  // net players at 0x0051efc4; the arena's --playerai)
    // AI request bits (retail +0x128), set by OnAttacked 0x004cdce0
    static constexpr uint32_t kRequestAttackNow   = 0x1;          // the AI tries an attack this tick
    static constexpr uint32_t kRequestNoPlayAnim  = 0x2;          // IsValidAttack refuses PLAYANIM attacks
    static constexpr uint32_t kRequestInteractive = 0x4;          // RandomAttack tries an interactive attack first
      // charflags bit (retail +0x110 & 2): `incidentals off`
    void SetIncidentals(bool on)
        { if (on) charflags &= ~kCharFlagNoIncidentals; else charflags |= kCharFlagNoIncidentals; }
      // Incidentals are the random "NN:" variants of a character's root and idle
      // states (fidgets); off, the character always plays the 100% variant
    bool Incidentals() const { return !(charflags & kCharFlagNoIncidentals); }
    void SetBuffed() { charflags |= kCharFlagBuffed; }
    [[nodiscard]] bool IsBuffed() const { return (charflags & kCharFlagBuffed) != 0; }

    // Static access functions
    static TCharacter* CharBlocking(TObjectInstance* inst, const S3DPoint& pos, int32_t radius = 0);
        // Find if a character is blocking movement to this position
    TCharacter* CharBlocking() { return CharBlocking(this, Pos(), Radius()); }
        // Calls static function above with this chars parameters
    bool Blocked(S3DPoint& pos, S3DPoint& newpos, uint32_t bits = 0, int32_t* height = nullptr, TCharacter** bychar = nullptr);
      // Returns true if character would be blocked when going to new position
    uint32_t MoveStep();
      // One tick's displacement (Move repeats it toward a MoveTo target)
    enum class EBlockedBy : uint8_t { None, Hole, Height, Step, NoWalkmap, Character };
    EBlockedBy blockedby = EBlockedBy::None;
      // Which test the last Blocked() refused on (MoveStep's [move] log)
    
  // Miscellaneous functions
    virtual void MoveTo(S3DPoint& newpos) { movepos = newpos; movetopos = true; }
        // Moves object to new position (does walk checking for characters).
        // Use this function instead of SetPos() to avoid moving objects through or onto
        // barriers.
    void SetOnExit();
      // Flags that character is on an exit
    void EffectBurst(const char *name, int32_t height = 50);
      // Create a burst effect of the given name
    void EffectCombatFlash();
      // The flash of a blow (retail 0x004c8500)
    int32_t GetCombatFlashTicks() { return combatflashticks; }
      // Returns 0 if no flash, or positive number of ticks left if flash being drawn
    void MakeInvisible();
      // Makes character invisible
    void MakeVisible();
      // Makes character visible
    PTSpellManager GetSpellManager() { return &SpellManager; }
    [[nodiscard]] uint32_t CharFlags() const { return charflags; }
    [[nodiscard]] int32_t InvokeDelay() const { return invokedelay; }
      // Pulses the invoke animation holds the spell's effect back (retail +0x188, SetCast)
      // Get spell manager object

  // Streaming functions
    virtual int32_t ObjVersion() { return 4; }
        // Returns the version id for this object for loading/saving
    virtual void Load(RTInputStream is, int32_t version, int32_t objversion);
        // Loads object data from the sector
    virtual void Save(RTOutputStream os);
        // Saves object data to the sector

  // Fading (retail TCharacter +0x194..+0x1a4): fade is the visibility 0..100
  // that Transparency() reports; each Pulse moves it by fade_step (positive
  // fades out) until it reaches fade_limit.
    void Fade(int32_t direction);
      // `fadecharacterin/out`: +1 back to fully visible (living characters only), else out to 0
    void SetFade(int32_t amt, int32_t amt2 = 5, int32_t amt3 = -1);
      // Start a fade from 'amt' (kept when < 0) by 'amt2' per pulse to 'amt3' (-1: no limit)
    int32_t GetFade() const { return fade; }
    void UpdateFade();
      // One pulse of the fade

  // Invisible Spell Functions
    bool IsInvisibleSpell(){return invisible_spell;}
    void SetInvisibleSpell(bool on);
      // Fades to 30 while the spell lasts and back afterwards

  // Teleport functions
    void SetTeleportLevel(int32_t new_level){teleport_level = new_level;}
    void SetTeleportPosition(S3DPoint new_pos)
        {teleport_position.x = new_pos.x; teleport_position.y = new_pos.y; teleport_position.z = new_pos.z;}
    int32_t GetTeleportLevel(void){return teleport_level;}
    S3DPoint GetTeleportPosition(void){return teleport_position;}

  // Functions to remember if characters are seen or not
    bool HasSeenMe(TCharacter* me);
      // Have I been seen by this character?
    void SetHasSeen(TCharacter* me);
      // Add me to the HASSEEN list
    bool HasSeenAutoCombat(TCharacter* me);
      // Should I go into combat automatically against this guy I've just seen?
    void SetHasSeenAutoCombat(bool on);
      // Sets AUTOCOMBAT mode for all recently seen characters.  If set to false, prevents
      // player from automatically entering combat when he moves towards any of the 
      // current batch of enemies.  This is called when EndCombat() is called.

  // Character class stats
    STATFUNC(Radius)


  // Character object stats
    OBJSTATFUNC(Aggressive)
    OBJSTATFUNC(Poisoned)
    OBJSTATFUNC(Sleeping)
    OBJSTATFUNC(Health)
    OBJSTATFUNC(Fatigue)
    OBJSTATFUNC(Mana)

  // Damage resistances and modifier (the shipped game's code-defined stats;
  // their values come from class.def and saves, nothing in the port reads them yet)
    OBJSTATFUNC(DmgResMisc)
      // REVSYNC: slot 0x260, 0x004d6f90 (setter 0x264)
    OBJSTAT(DmgResHand)
    OBJSTAT(DmgResPuncture)
    OBJSTAT(DmgResCut)
    OBJSTAT(DmgResChop)
    OBJSTAT(DmgResBludgeon)
    OBJSTAT(DmgResMagical)
    OBJSTAT(DmgResBurn)
    OBJSTAT(DmgResFreeze)
    OBJSTAT(DmgResPoison)
    OBJSTATFUNC(DamageMod)
      // REVSYNC: slot 0x2b0, 0x004d7220 -- percent added to the damage this character deals

   // Calculated stats
    virtual int32_t MaxHealth() { return chardata->health; }
      // Returns monster max health value
    virtual int32_t MaxFatigue() { return chardata->fatigue; }
      // Returns monster max fatigue value
    virtual int32_t MaxMana() { return chardata->mana; }
      // Returns monster max mana value
    bool GetFieldText(const char *field, char *buf, int32_t buflen) override;
      // Character fields of the stat sheet (retail 0x004d5260)
    virtual int32_t BlockPcnt() { return chardata->blockfreq; }
      // Returns percentage of time character will block an attack
    virtual int32_t ArmorValue() { return chardata->armorvalue; }
      // Return armor value
    virtual int32_t DefenseModifier() { return chardata->defensemod + SpellManager.GetDefense(); }
      // Return defense modifier
    virtual int32_t AttackModifier() { return chardata->attackmod + SpellManager.GetOffense(); }
      // Return offense modifier
    virtual int32_t FatigueModifier() { return (Fatigue() * 4 / MaxFatigue()) - 4; }
      // Return fatigue modifier
    virtual int32_t DamageModifier(int32_t damagetype)
        { return (uint32_t)damagetype < (uint32_t)NUMDAMAGETYPES ? chardata->damagemods[damagetype] : 0; }
      // Returns percentage of damage. Retail read past the table for retail's
      // poison trap's damage type 20 (TContainer::SpringTraps); the port reads 0.
    virtual int32_t WeaponType() { return chardata->weapontype; }
    int32_t CombatTargetWeaponType() override { return Fighting() ? Fighting()->WeaponType() : -1; }
      // REVSYNC: 0x0040b133..0x0040b174 -- a combat or bow root's object, its
      // WeaponType (slot 0x2cc)
      // Returns the type of weapon being used
    virtual int32_t WeaponDamage() { return chardata->weapondamage; }
      // Returns current weapon damage
    virtual int32_t Offense();
      // REVSYNC: slot 0x2c4, 0x004d72e0 -- what this character adds to its to-hit:
      // ATTACKMOD, the type's Value x TOHITRANGECHAR, its spells' offense
    virtual int32_t Defense();
      // REVSYNC: slot 0x2c0, 0x004d72a0 -- what it takes off an attacker's to-hit:
      // DEFENSEMOD, Value x TOHITRANGECHAR, its spells' defense
    virtual int32_t LuckMod() { return 0; }
      // REVSYNC: slot 0x2ec, 0x004d7370 -- added to both sides of a to-hit; 0 but
      // for the player (his Luck STATLEVEL)
    virtual int32_t StrengthMod() { return 0; }
      // REVSYNC: slot 0x2f4, 0x004d7390 -- percent added to the damage; 0 but for the
      // player (his Strength STATLEVEL)
    virtual bool HoldsLight() { return false; }
      // REVSYNC: slot 0x250, 0x004d6de0 -- holding a light source (the player's torch)
    virtual int32_t GetDamageType(int32_t weapontype, int32_t attackflags);
      // REVSYNC: slot 0x2d4, 0x004c47d0 -- the DT_XXX damage type of an
      // attack with these flags and that weapon type (the player's: TPlayer)
    virtual int32_t Transparency();
      // Controls the transparency of character's imagery (character animator).  Things
      // such as invisibility spells, visibility and stealth can affect this value.
    virtual int32_t Visibility();
      // Returns the total visibility 1-100 for character (based on lights, ambient, and fog, etc.)
    virtual int32_t Hearing(int32_t dist);
      // How well a noise `dist` away is heard: retail's is 100 within the
      // hearing range, 0 beyond it (character.cpp)
    virtual int32_t Sight(int32_t dist);
      // How well a character `dist` away is seen in the dark: retail's is 0
      // or 1 within the sight range, 0 beyond it or asleep (character.cpp)
    virtual int32_t StealthMod() { return 0; }
      // Ordinary characters dont have stealth
    virtual int32_t LastGlimpse() { return glimpse; }
      // What was the last visibility glimpse for the character.  1 is complete
      // and utter invisibility, 100 is plain as day visibility
    virtual int32_t LastNoise() { return noise; }
      // What was the last noise value (1-100) for the character. 1 is pindrop,
      // 100 is pots and pans crashing.
    virtual char *BodyType() { return "na"; }
      // Character's don't use the equipment replacement system
    virtual char *GetCombatRoot(TObjectInstance* oi = nullptr) { return "combat"; }
      // Returns combat root given current weapon or weapon type
    virtual char *GetBowRoot(TObjectInstance* oi = nullptr) { return "bow"; }
      // Returns bow root given current bow weapon type
    virtual const char *GetWalkRoot() { return "walk"; }
      // REVSYNC: slot 0x30c (0x004d73f0) -- the root it walks in (the
      // player's carries a torch: TPlayer)
    bool CanSeeCharacter(TCharacter* chr, int32_t angle = -1);
      // Returns true if this character can 'see' the last glimpse of 'chr'
    bool IsMagicResistant() { return (magic_resistance > 0.0f); }
      // gets the level of magic resistance
    void SetMagicResistance(float pct) { magic_resistance = pct; }
      // sets the level of magic resistance
    float GetMagicResistance() { return magic_resistance; }
      // gets the level of magic resistance
    bool GetAutoCombat() { return autocombat; }
      // returns the state of this characters autocombat setting
    void SetAutoCombat(bool ac_state) { autocombat = ac_state; }
      // sets the state of this characters autocombat setting

  protected:
   
    virtual void UpdateAction(int32_t bits = 0);
      // Called by Pulse() to update the action blocks


    // Resolve functions - redefine these in derived classes for different functionality
    virtual int32_t ResolveAction(int32_t bits = 0);
      // Call the resolve functions, below
    virtual int32_t ResolveMove(PTActionBlock ab, int32_t bits);
    virtual int32_t ResolveAttack(PTActionBlock ab, int32_t bits);
    virtual int32_t ResolveCombatMove(PTActionBlock ab, int32_t bits);
    virtual int32_t ResolveCombat(PTActionBlock ab, int32_t bits);
    virtual int32_t ResolveBowAim(PTActionBlock ab, int32_t bits);
    virtual int32_t ResolveBowShoot(PTActionBlock ab, int32_t bits);
    virtual int32_t ResolveBlock(PTActionBlock ab, int32_t bits);
    virtual int32_t ResolveImpact(PTActionBlock ab, int32_t bits);
    virtual int32_t ResolveDead(PTActionBlock ab, int32_t bits);
    virtual int32_t ResolveSay(PTActionBlock ab, int32_t bits);
    virtual int32_t ResolvePivot(PTActionBlock ab, int32_t bits);
    virtual int32_t ResolveLeap(PTActionBlock ab, int32_t bits);
    virtual int32_t ResolvePull(PTActionBlock ab, int32_t bits);

    // Damage's steps (retail 0x004c4950, COMBAT_HIT.md §3.4)
    SCharAttackImpact* DamageDeath(TActionBlock* given, TActionBlock*& made, int32_t damage, int32_t dir,
        TCharacter* attacker);
      // The death block (the given one, a CHARIMPACT's, or a stock name) forced;
      // the impact whose snap follows. A block it makes is left in 'made'.
    SCharAttackImpact* DamageImpact(TActionBlock* given, TActionBlock*& made, int32_t damage, int32_t dir,
        TCharacter* attacker);
      // Likewise the impact block ("blockimpact" while blocking)
    void ForceDamageBlock(TActionBlock* ab, const SCharAttackImpact* imp);
      // Turns 'ab' as its impact says, then forces the root and 'ab'
    int32_t DamageShare(int32_t damage);
      // damage as a percentage of what's left (at most 100): CHARIMPACT's range

    // AI's steps (retail 0x004c8b60, COMBAT_ATTACK_CHOICE.md §3.10)
    void AIAttack(TCharacter* target);
      // In the combat stance: the attack timers, the attack or the walk to reach it
    void AIMove(TCharacter* target);
      // Otherwise: run while retreating, chase, search, or step aside
    TObjectInstance* NearestWaypoint(const S3DPoint& centre);
      // The waypoint in sight nearest `centre`, or none
    void AIPerMonster();
      // A boss's own behaviour, before the AI's: the kind (once), then its
      // case (retail AI_PerMonster 0x004c9b70; BOSS_AI.md)

    // The bosses (src/bossai.cpp; BOSS_AI.md §2-§5)
    void BossBaez(SBaezState& s);
      // The crowd, then the stuns above 1 health or the fall at 1
    void BaezCrowd(SBaezState& s);
      // The crowd faces the fight and cheers or boos it
    void BaezStuns(SBaezState& s);
      // Above 1 health: a stun pause at each 20% lost
    void BaezFall(SBaezState& s);
      // At 1 health: the fake death, the walk to the table, the throw
    bool BaezSetUpFall(SBaezState& s);
      // Its first tick: the ring opens, the crowd leaves, the table is found
      // (false: there is none, and Baez is no boss any more)
    void BaezWalk(SBaezState& s, const S3DPoint& me);
      // The player and Baez walk to their spots
    void BaezWalkOn(SBaezState& s, const S3DPoint& me, bool playerthere);
      // Baez gets up and walks to his spot
    void BaezThrow(SBaezState& s);
      // The player throws him into the table
    void BossSolifuge(SSolifugeState& s);
      // The bands, the paralysis, the stun, the landing slam, the sky walk
    void SolifugeSkyWalk(SSolifugeState& s, const S3DPoint& me, const S3DPoint& playerpos);
      // Up into the roof, the brood called, back down
    void SolifugeBrood(SSolifugeState& s);
      // A spider for each of the three slots whose one is gone or dead
    void BossJhaga(SJhagaState& s);
      // The rest, then five teleports casting at the player
    bool JhagaSetUp(SJhagaState& s);
      // The four waypoints round him (false: fewer, and Jhaga is no boss)
    void BossYhagoro(SYhagoroState& s);
      // Walking or fighting, by the band of health he is in
    std::vector<TObjectInstance*> PaneObjects();
      // Every object of the pane's sector window, in map order (retail's
      // map iterator 0x0044cf80 with no rect)
    void TintMaterials();
    void UntintMaterials();
      // The paralysed Solifuge's blue: the imagery's materials saved and
      // tinted, then put back (retail 0x004d6a30 / 0x004d6b40)

    // Wander's steps (retail 0x004c9790, COMBAT_ATTACK_CHOICE.md §3.15)
    void WanderRunning();
      // Out of a combat root: a run burst at full fatigue, a run's end
    void WanderPatrol();
      // Walk to the waypoint; on arrival pick the next
    TObjectInstance* PickWanderWaypoint();
      // REVSYNC: 0x004c95e0 -- one of the waypoints within 2000 for the
      // next leg, else the previous one
    bool ProbeLedge(const S3DPoint& to);
      // REVSYNC: 0x004c9460 -- a step of 32 or more between the walk heights
      // met stepping from here away from `to`

    // Pulse's steps (retail 0x004c1bb0, CHARACTER_PULSE.md §2)
    void PulseCorpse();
      // A dead character's tick: its death played and rooted, the script
      // told, then the fade out and the removal
    void PulseAutoCombat();
      // The player falls into combat with an enemy ahead (every 32 ticks)
    void PulseImpaleRelease();
      // Let go by an impale: back to walking
    void PulseRecovery();
      // Health, mana and fatigue recover (fatigue drains while running),
      // poison bites, an invoke delay runs down
    void PulseActionTimers();
      // The burn and the flail end on time

    char *GetAngleMoveAnim(int32_t movedir, int32_t facedir, char *root, char *animname, int32_t buflen);
      // Returns the correct angle movce animation given the current movedir, facedir, and root name
    void AdvanceAngles(int32_t faceang, int32_t moveang, int32_t maxturn);
      // Advance the character's facing and moving angle
    int32_t UpdateAngle(int32_t angle);
      // Check path along current angle and adjuct accordingly
    TObjectInstance* FindObjAhead();
      // Find object in front of character
    bool CanHearCharacter(TCharacter* chr);
      // Returns true if this character can hear the last noise made by 'chr'
    int32_t FindCharacters(TCharacter* chars[], int32_t maxchars, 
        int32_t range = 128, int32_t angle = -1, int32_t anglerange = 32, int32_t flags = 0);
      // The characters in range (FINDCHAR_* filters), from direction 'angle'
      // within 'anglerange' when 'angle' isn't -1; the best placed at the
      // head of the list, the rest in map order. Returns how many.
    TCharacter* FindCharacter(int32_t range = 128, int32_t angle = -1, int32_t anglerange = 32, int32_t flags = 0);
      // Calls the FindCharacters function above with only 1 character
      // Finds characters given the above parameters.  Will find all chars in range from
      // direction 'angle' if not -1 with angle range of 32.
    TCharacter* FindCharacterAhead(int32_t angle, int32_t anglerange = 32);
      // Find closest character in this direction
    TCharacter* FindClosestEnemy(int32_t angle = -1, int32_t anglerange = 32);
      // Finds the closest character attacking this character
    void ResetStealthValues();
      // Based on character position, lights, and stealth, sets noise and glimpse

    // -- Data members --
    bool autocombat;            // this mimics the global, but works as a way to allow locke to flee in
                                // the face of too many enemies.

    bool movetopos;             // Set to true if character needs to move to movepos
    S3DPoint movepos;           // The position character needs to move to (while walk checking)

    PSCharData chardata;        // Pointer to global character settings for this type of char

    int32_t magictimer = 1;         // retail +0x124: frames until a MAGICATTACK may be tried
                                    // (AI re-arms it from MAGICFREQ; IsValidAttack spends it)

    bool forcecommanddone;      // For skipping past animations
    bool forcenomove;           // For forcing end movement

    uint32_t charflags = 0;        // Character flags (retail +0x110; retail's allocator zeroed it)

    int32_t exittimestamp;          // Frame the character was last on an exit; OF_ONEXIT clears 5 frames on
    bool is_invisible;          // is our character affected by invisibility

  // Move stuff
    int32_t shovedir;               // Last choice (left/right) for going around an obstacle
    
  // Stealth Stuff
    int32_t nextattack = 1;         // retail +0x120: frames until the AI may attack again
                                    // (IsValidAttack refuses a monster's attack while non-zero)
    int32_t glimpse;                // Value from 1-100 indicating how visible last move was
    int32_t noise;                  // Value from 1-100 indicating how quiet last move was

  // Recovery (TCharacter::Pulse): the Clock() time each vital last recovered,
  // and what has gathered towards its next point, in 1/10000 points.
    int32_t lasthealthrecov = 0;    // retail +0x138
    int32_t lastfatiguerecov = 0;   // retail +0x13c
    int32_t lastmanarecov = 0;      // retail +0x140
    int32_t lastrunfatigue = 0;     // retail +0x144: last running charge, -1 while not running
    int32_t healthaccum = 0;        // retail +0x148
    int32_t fatigueaccum = 0;       // retail +0x14c
    int32_t manaaccum = 0;          // retail +0x150
    int32_t runaccum = 0;           // retail +0x154: fatigue the running costs
    int32_t poisonaccum = 0;        // retail +0x158

  // Poison damage stuff
    int32_t lastpoisondamage = 0;   // retail +0x15c: last charge, -1 while not poisoned
    int32_t deadticks = 0;          // retail +0x228: ticks a corpse has lain (it fades out at 240)
    int32_t burnticks = 0;          // retail +0x1bc: ticks the burning goes on (Burn sets 50)

  // Attack stuff
    PSCharAttackData lastattack = nullptr; // retail +0x160: the attack last started (ResolveAttack)
    int32_t lastattackticks = 0;     // retail +0x164: GameFrame it started on
    int32_t lasthit = 0;             // retail +0x168: whether it hit its main target (ResolveAttack)
    int32_t chainhits = 0;           // retail +0x16c: chain presses banked (ButtonAttack, at most 3)
    uint32_t requestbits = 0;        // retail +0x128: AI requests (kRequest*), set by OnAttacked
    int32_t attackcount = 0;         // retail +0x12c: decremented by the AI per attack made (no reader found)

  // spells stuff
    TSpellManager SpellManager;  // handles the spells the character casts
    int32_t invokedelay;
    PTActionBlock oldab;

    float magic_resistance;     // between 0.0 and 1.0... percentage of magic resistance

    // Visibility
    int32_t fade = 100;             // retail +0x194, 0..100
    int32_t fade_step = 0;          // retail +0x198, subtracted from fade each pulse
    int32_t fade_limit = 100;       // retail +0x19c, where the fade stops (-1: at 0 or 100)
    int32_t fade_direction = 0;     // retail +0x1a0, -1 out / 1 in / 0 still (a corpse fades once it is 0)

  // Invisible Spell Addition
    bool invisible_spell = false;   // retail +0x1a4
    int32_t voice = -1;             // retail +0x260: the sound id of the line being spoken, -1 none

  // Teleport Coordinates
    int32_t teleport_level;
    S3DPoint teleport_position;

  // burn stuff
    TObjectInstance* burning;

  // Has seen list  
    SHasSeen hasseen[MAXHASSEEN]; // List of characters seen recently

  // Snap stuff
    int32_t snapticks = -1;           // retail +0x220: Damage stores an impact's snaptime (nothing reads it)

  // combatflash delay
    int32_t combatflashticks = 0;     // retail +0x224 (a blow sets 5; Pulse counts it down)

  // The corpse

  // Diagnostic counters. ai_pulse_count increments at every Pulse()
  // entry, ai_ai_count at every AI() entry. The overlay reads these
  // to confirm whether monsters are being ticked at all (and if so,
  // whether AI() is being reached or short-circuited upstream).
public:
    uint32_t ai_pulse_count = 0;
    uint32_t ai_ai_count    = 0;
    // Debug accessors (overlay reads these). Wrap protected action-block
    // pointers so the diagnostic UI doesn't need to be a friend.
    int32_t  DoingAction() const { return doing ? (int32_t)doing->action : -1; }
    int32_t  RootAction()  const { return root  ? (int32_t)root->action  : -1; }
    int32_t  DesiredAction() const { return desired ? (int32_t)desired->action : -1; }
    const char* DoingName() const { return (doing && doing->name[0]) ? doing->name : "?"; }
    const char* RootName()  const { return (root  && root->name[0])  ? root->name  : "?"; }
    const char* DesiredName() const { return (desired && desired->name[0]) ? desired->name : "?"; }
    int32_t  DoingTargetX() const { return doing ? doing->target.x : 0; }
    int32_t  DoingTargetY() const { return doing ? doing->target.y : 0; }
    int32_t  NextAttack()   const { return nextattack; }
    int32_t  LastBowShot()  const { return lastbowshot; }
    int32_t  BowShots()     const { return bowshots; }
protected:

  // Last bow shot ticks (so we don't shoot bow too fast)
    int32_t lastbowshot = 0;    // +0x22c
  // Shots ShootBow has queued for ResolveBowShoot
    int32_t bowshots = 0;       // +0x230

  // Retail +0x23c: where the AI last saw or heard its target (the centre of
  // its waypoint search when it loses it).
    S3DPoint target_last_position{};

  // Retail +0x248: how many more ticks the AI keeps facing its waypoint
  // (ai_lookat) while chasing; a new waypoint gives it 6.
    int32_t  waypointticks = 0;

  // retail +0x288: the item a Goto carries (the map pane's walk to an item
  // out of reach, 0x004cedb0), picked up when that walk arrives (0x004c6155,
  // 0x004c7fc3). Kept until then, or until a Goto carries another.
    TSafeRef<TObjectInstance> gotoitem;

  // Retail +0x254 / +0x258 / +0x25c: the retreat (COMBAT_ATTACK_CHOICE.md
  // §3.10.6). The AI's tail, every tick it runs: retreating = latch =
  // (Health <= RETREATAT || latch) && frames != 0, then frames counts down.
  // Damage arms the frames with RETREATFOR when a hit leaves 1 <= Health <=
  // RETREATAT; StartRetreat (0x004d5fc0) with 96 and sets both flags. A
  // retreating character runs straight away from its target (and takes
  // no new one); ResolveCombat doesn't face it; a blocked step (MoveStep)
  // clears all three. (The 1998 AI body below reads them as "target out
  // of sight" until its retail port, C3b.)
    bool     retreating = false;
    bool     retreatlatch = false;
    int32_t  retreatframes = 0;
  // Retail +0x234: an object the AI walks toward and ResolveCombat faces
  // when no visible target overrides it: the waypoint AI 0x004c8b60 heads
  // for, and the one Wander 0x004c9790 patrols to.
    TObjectInstance* ai_lookat = nullptr;
  // Retail +0x280: the boss kind AI_PerMonster 0x004c9b70 gives, once (1
  // Baez, whom magic can't hurt, 2 Solifuge, 3 Jhaga, 4 Yhagoro, -1 any
  // other); +0x284 the boss's own state (bossai.h).
    static constexpr int32_t kMonsterBaez = 1, kMonsterSolifuge = 2, kMonsterJhaga = 3, kMonsterYhagoro = 4;
    int32_t monsterkind = 0;
    TBossState bossstate;
  // Retail +0x298 / +0x29c: what TintMaterials saved, each material's
  // diffuse then emissive red, green and blue; empty when nothing is.
    std::vector<std::array<float, 6>> savedmaterials;
  // Retail +0x238 / +0x24c / +0x250, Wander's: the waypoint before the
  // current one (its pick passes over both; with none to pick it goes
  // back; retail drops it when its object goes, 0x004c38a6), the ticks it
  // still does nothing, the ticks a run has left.
    TSafeRef<TObjectInstance> lastwaypoint;
    int32_t wanderidle = 0;
    int32_t runticks = 0;
  // Retail +0x28c / +0x290: the player's last attack button and how many
  // times running it was pressed (ButtonAttack 0x004d2480's same-button
  // rule); SetFighting resets them.
    int32_t lastbutton   = -1;
    int32_t buttonrepeat = 0;
};

DEFINE_BUILDER("Character", TCharacter)

#endif
