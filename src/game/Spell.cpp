/*
 * Copyright (C) 2005-2012 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Spell.h"
#include "Database/DatabaseEnv.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Opcodes.h"
#include "Log.h"
#include "UpdateMask.h"
#include "World.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Player.h"
#include "Pet.h"
#include "TemporarySummon.h"
#include "Unit.h"
#include "DynamicObject.h"
#include "Group.h"
#include "UpdateData.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "CellImpl.h"
#include "Policies/Singleton.h"
#include "SharedDefines.h"
#include "LootMgr.h"
#include "VMapFactory.h"
#include "BattleGround/BattleGround.h"
#include "Util.h"
#include "Vehicle.h"
#include "Chat.h"

extern pEffect SpellEffects[TOTAL_SPELL_EFFECTS];

class PrioritizeManaUnitWraper
{
    public:
        explicit PrioritizeManaUnitWraper(Unit* unit) : i_unit(unit)
        {
            uint32 maxmana = unit->GetMaxPower(POWER_MANA);
            i_percent = maxmana ? unit->GetPower(POWER_MANA) * 100 / maxmana : 101;
        }
        Unit* getUnit() const { return i_unit; }
        uint32 getPercent() const { return i_percent; }
    private:
        Unit* i_unit;
        uint32 i_percent;
};

struct PrioritizeMana
{
    int operator()( PrioritizeManaUnitWraper const& x, PrioritizeManaUnitWraper const& y ) const
    {
        return x.getPercent() > y.getPercent();
    }
};

typedef std::priority_queue<PrioritizeManaUnitWraper, std::vector<PrioritizeManaUnitWraper>, PrioritizeMana> PrioritizeManaUnitQueue;

class PrioritizeHealthUnitWraper
{
public:
    explicit PrioritizeHealthUnitWraper(Unit* unit) : i_unit(unit)
    {
        if (!unit || unit->GetMaxHealth() == 0)
            i_percent = 100;
        else
            i_percent = unit->GetHealth() * 100 / unit->GetMaxHealth();
    }
    Unit* getUnit() const { return i_unit; }
    uint32 getPercent() const { return i_percent; }
private:
    Unit* i_unit;
    uint32 i_percent;
};

struct PrioritizeHealth
{
    int operator()( PrioritizeHealthUnitWraper const& x, PrioritizeHealthUnitWraper const& y ) const
    {
        return x.getPercent() > y.getPercent();
    }
};

typedef std::priority_queue<PrioritizeHealthUnitWraper, std::vector<PrioritizeHealthUnitWraper>, PrioritizeHealth> PrioritizeHealthUnitQueue;

bool IsQuestTameSpell(uint32 spellId)
{
    SpellEntry const *spellproto = sSpellStore.LookupEntry(spellId);
    if (!spellproto)
        return false;

    return spellproto->Effect[EFFECT_INDEX_0] == SPELL_EFFECT_THREAT
        && spellproto->Effect[EFFECT_INDEX_1] == SPELL_EFFECT_APPLY_AURA && spellproto->EffectApplyAuraName[EFFECT_INDEX_1] == SPELL_AURA_DUMMY;
}

SpellCastTargets::SpellCastTargets()
{
    m_unitTarget = NULL;
    m_itemTarget = NULL;
    m_GOTarget   = NULL;

    m_itemTargetEntry  = 0;

    m_src = m_dest = WorldLocation();
    m_strTarget = "";
    m_targetMask = 0;
    SetElevation(0.0f);
    SetSpeed(0.0f);
}

SpellCastTargets::~SpellCastTargets()
{
}

void SpellCastTargets::setUnitTarget(Unit* target)
{

    if (target && !(target->isType(TYPEMASK_UNIT)))
        return;

    if (target && !(m_targetMask & TARGET_FLAG_DEST_LOCATION))
        m_dest = WorldLocation(target->GetPosition());

    m_unitTarget = target;

    if (target)
    {
        m_unitTargetGUID = target->GetObjectGuid();
        if (!(m_targetMask & TARGET_FLAG_DEST_LOCATION))
            m_targetMask |= TARGET_FLAG_UNIT;
    }
    else
    {
        m_unitTargetGUID = ObjectGuid();
        m_targetMask &= ~TARGET_FLAG_UNIT;
    }
}

void SpellCastTargets::setDestination(WorldLocation const& loc)
{
    m_dest = loc;
    m_targetMask |= TARGET_FLAG_DEST_LOCATION;
}

void SpellCastTargets::setSource(WorldLocation const& loc)
{
    m_src = loc;
    m_targetMask |= TARGET_FLAG_SOURCE_LOCATION;
}

void SpellCastTargets::setGOTarget(GameObject *target)
{
    m_GOTarget = target;
    m_GOTargetGUID = target->GetObjectGuid();
    //    m_targetMask |= TARGET_FLAG_OBJECT;
}

void SpellCastTargets::setItemTarget(Item* item)
{
    if(!item)
        return;

    m_itemTarget = item;
    m_itemTargetGUID = item->GetObjectGuid();
    m_itemTargetEntry = item->GetEntry();
    m_targetMask |= TARGET_FLAG_ITEM;
}

void SpellCastTargets::setTradeItemTarget(Player* caster)
{
    m_itemTargetGUID = ObjectGuid(uint64(TRADE_SLOT_NONTRADED));
    m_itemTargetEntry = 0;
    m_targetMask |= TARGET_FLAG_TRADE_ITEM;

    Update(caster);
}

void SpellCastTargets::setCorpseTarget(Corpse* corpse)
{
    m_CorpseTargetGUID = corpse->GetObjectGuid();
}

void SpellCastTargets::Update(Unit* caster)
{
    m_GOTarget   = m_GOTargetGUID ? caster->GetMap()->GetGameObject(m_GOTargetGUID) : NULL;
    m_unitTarget = (m_unitTargetGUID && caster->GetMap()) ?
        (m_unitTargetGUID == caster->GetObjectGuid() ?
            caster :
            caster->GetMap()->GetUnit(m_unitTargetGUID)) :
        NULL;

    m_itemTarget = NULL;
    if (caster->GetTypeId() == TYPEID_PLAYER)
    {
        Player *player = ((Player*)caster);

        if (m_targetMask & TARGET_FLAG_ITEM)
            m_itemTarget = player->GetItemByGuid(m_itemTargetGUID);
        else if (m_targetMask & TARGET_FLAG_TRADE_ITEM)
        {
            if (TradeData* pTrade = player->GetTradeData())
                if (m_itemTargetGUID.GetRawValue() < TRADE_SLOT_COUNT)
                    m_itemTarget = pTrade->GetTraderData()->GetItem(TradeSlots(m_itemTargetGUID.GetRawValue()));
        }

        if (m_itemTarget)
            m_itemTargetEntry = m_itemTarget->GetEntry();
    }
}

void SpellCastTargets::read(ByteBuffer& data, Unit* caster)
{
    data >> m_targetMask;

    if (m_targetMask == TARGET_FLAG_SELF)
    {
        m_dest = caster->GetPosition();
        m_unitTarget = caster;
        m_unitTargetGUID = caster->GetObjectGuid();
        return;
    }

    // TARGET_FLAG_UNK2 is used for non-combat pets, maybe other?
    if ( m_targetMask & ( TARGET_FLAG_UNIT | TARGET_FLAG_UNK2 ))
        data >> m_unitTargetGUID.ReadAsPacked();

    if ( m_targetMask & ( TARGET_FLAG_OBJECT ))
        data >> m_GOTargetGUID.ReadAsPacked();

    if(( m_targetMask & ( TARGET_FLAG_ITEM | TARGET_FLAG_TRADE_ITEM )) && caster->GetTypeId() == TYPEID_PLAYER)
        data >> m_itemTargetGUID.ReadAsPacked();

    if ( m_targetMask & (TARGET_FLAG_CORPSE | TARGET_FLAG_PVP_CORPSE ) )
        data >> m_CorpseTargetGUID.ReadAsPacked();

    if (m_targetMask & TARGET_FLAG_SOURCE_LOCATION)
    {
        m_src = caster->GetPosition();
        ObjectGuid transGuid;
        data >> transGuid.ReadAsPacked();
        if (transGuid)
        {
            m_src.SetTransportGuid(transGuid);
            data >> m_src.GetTransportPosition().x >> m_src.GetTransportPosition().y >> m_src.GetTransportPosition().z;
        }
        else
            data >> m_src.x >> m_src.y >> m_src.z;

        if(!MaNGOS::IsValidMapCoord(getSource().getX(), getSource().getY(), getSource().getZ()))
            throw ByteBufferException(false, data.rpos(), 0, data.size());
    }

    if (m_targetMask & TARGET_FLAG_DEST_LOCATION)
    {
        m_dest = caster->GetPosition();
        ObjectGuid transGuid;
        data >> transGuid.ReadAsPacked();
        if (transGuid)
        {
            m_dest.SetTransportGuid(transGuid);
            data >> m_dest.GetTransportPosition().x >> m_dest.GetTransportPosition().y >> m_dest.GetTransportPosition().z;
        }
        else
            data >> m_dest.x >> m_dest.y >> m_dest.z;

        if(!MaNGOS::IsValidMapCoord(getDestination().getX(), getDestination().getY(), getDestination().getZ()))
            throw ByteBufferException(false, data.rpos(), 0, data.size());
    }

    if ( m_targetMask & TARGET_FLAG_STRING )
        data >> m_strTarget;

    // find real units/GOs
    Update(caster);
}

void SpellCastTargets::write( ByteBuffer& data ) const
{
    data << uint32(m_targetMask);

    if ( m_targetMask & ( TARGET_FLAG_UNIT | TARGET_FLAG_PVP_CORPSE | TARGET_FLAG_OBJECT | TARGET_FLAG_CORPSE | TARGET_FLAG_UNK2 ) )
    {
        if (m_targetMask & TARGET_FLAG_UNIT)
        {
            if (m_unitTarget)
                data << m_unitTarget->GetPackGUID();
            else
                data << uint8(0);
        }
        else if ( m_targetMask & TARGET_FLAG_OBJECT )
        {
            if (m_GOTarget)
                data << m_GOTarget->GetPackGUID();
            else
                data << uint8(0);
        }
        else if ( m_targetMask & ( TARGET_FLAG_CORPSE | TARGET_FLAG_PVP_CORPSE ) )
            data << m_CorpseTargetGUID.WriteAsPacked();
        else
            data << uint8(0);
    }

    if ( m_targetMask & ( TARGET_FLAG_ITEM | TARGET_FLAG_TRADE_ITEM ) )
    {
        if (m_itemTarget)
            data << m_itemTarget->GetPackGUID();
        else
            data << uint8(0);
    }

    if (m_targetMask & TARGET_FLAG_SOURCE_LOCATION)
    {
        ObjectGuid transGuid = m_src.GetTransportGuid();
        data << transGuid.WriteAsPacked();
        if (transGuid)
            data << m_src.GetTransportPos().getX() << m_src.GetTransportPos().getY() << m_src.GetTransportPos().getZ();
        else
            data << m_src.getX() << m_src.getY() << m_src.getZ();
    }

    if (m_targetMask & TARGET_FLAG_DEST_LOCATION)
    {
        ObjectGuid transGuid = m_dest.GetTransportGuid();
        data << transGuid.WriteAsPacked();
        if (transGuid)
            data << m_dest.GetTransportPos().getX() << m_dest.GetTransportPos().getY() << m_dest.GetTransportPos().getZ();
        else
            data << m_dest.x << m_dest.y << m_dest.z;
    }

    if ( m_targetMask & TARGET_FLAG_STRING )
        data << m_strTarget;
}

void SpellCastTargets::ReadAdditionalData(ByteBuffer& data)
{
    data >> m_elevation;
    data >> m_speed;

    uint8 moveFlag;
    data >> moveFlag;

    if (moveFlag)
    {
        ObjectGuid guid;                                // unk guid (possible - active mover) - unused
        MovementInfo movementInfo;                      // MovementInfo

        data >> Unused<uint32>();                       // >> MSG_MOVE_STOP
        data >> guid.ReadAsPacked();
        data >> movementInfo;
        //setSource(movementInfo.GetPos());
    }
}

bool SpellCastTargets::HasLocation() const
{
    if (m_targetMask & TARGET_FLAG_DEST_LOCATION)
        return !getDestination().IsEmpty();
    else if (m_targetMask & TARGET_FLAG_SOURCE_LOCATION)
        return !getSource().IsEmpty();
    return false;
};

WorldLocation const& SpellCastTargets::GetLocation() const
{
    if (m_targetMask & TARGET_FLAG_DEST_LOCATION)
        return getDestination();
    else if (m_targetMask & TARGET_FLAG_SOURCE_LOCATION)
        return getSource();
    // Possible need substitution for another cases
    return WorldLocation::Null;
}

Spell::Spell( Unit* caster, SpellEntry const *info, bool triggered, ObjectGuid originalCasterGUID, SpellEntry const* triggeredBy )
{
    MANGOS_ASSERT( caster != NULL && info != NULL );
    MANGOS_ASSERT( info == sSpellStore.LookupEntry( info->Id ) && "`info` must be pointer to sSpellStore element");

    if (info->SpellDifficultyId && caster->IsInWorld() && caster->GetMap()->IsDungeon())
    {
        if (SpellEntry const* spellEntry = GetSpellEntryByDifficulty(info->SpellDifficultyId, caster->GetMap()->GetDifficulty(), caster->GetMap()->IsRaid()))
            m_spellInfo = spellEntry;
        else
            m_spellInfo = info;
    }
    else
        m_spellInfo = info;

    m_triggeredBySpellInfo = triggeredBy;
    m_caster = caster;
    m_selfContainer = NULL;
    m_referencedFromCurrentSpell = false;
    m_executedCurrently = false;
    m_delayStart = 0;
    m_delayAtDamageCount = 0;
    m_damage = 0;
    damage = 0;

    m_applyMultiplierMask = 0;

    // Get data for type of attack
    m_attackType = GetWeaponAttackType(m_spellInfo);

    m_spellSchoolMask = GetSpellSchoolMask(info);           // Can be override for some spell (wand shoot for example)

    if (m_attackType == RANGED_ATTACK)
    {
        // wand case
        if((m_caster->getClassMask() & CLASSMASK_WAND_USERS) != 0 && m_caster->GetTypeId() == TYPEID_PLAYER)
        {
            if (Item* pItem = ((Player*)m_caster)->GetWeaponForAttack(RANGED_ATTACK))
                m_spellSchoolMask = SpellSchoolMask(1 << pItem->GetProto()->Damage[0].DamageType);
        }
    }
    // Set health leech amount to zero
    m_healthLeech = 0;

    m_originalCasterGuid = originalCasterGUID ? originalCasterGUID : m_caster->GetObjectGuid();

    UpdateOriginalCasterPointer();

    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (m_spellInfo->Effect[i] != SPELL_EFFECT_NONE)
            m_currentBasePoints[i] = m_spellInfo->CalculateSimpleValue(SpellEffectIndex(i));
        else
        {
            // We must make check for use NONE effects in spell.dbc instead of this hack (this - from clean mangos).
            m_currentBasePoints[i] = m_spellInfo->CalculateSimpleValue(SpellEffectIndex(i));
        }

        m_effectExecuteData[i].clear();
    }

    m_spellState = SPELL_STATE_PREPARING;

    m_castPositionX = m_castPositionY = m_castPositionZ = 0;
    m_preCastSpells.clear();
    m_TriggerSpells.clear();
    m_NotTriggerSpells.clear();
    m_IsTriggeredSpell = m_spellInfo->HasAttribute(SPELL_ATTR_EX4_FORCE_TRIGGERED) ? true : triggered;
    //m_AreaAura = false;
    m_CastItem = NULL;

    unitTarget = NULL;
    itemTarget = NULL;
    gameObjTarget = NULL;
    focusObject = NULL;
    m_cast_count = 0;
    m_glyphIndex = 0;
    m_triggeredByAuraSpell  = NULL;

    //Auto Shot & Shoot (wand)
    m_autoRepeat = IsAutoRepeatRangedSpell(m_spellInfo);

    m_runesState = 0;
    m_powerCost = 0;                                        // setup to correct value in Spell::prepare, don't must be used before.
    m_casttime = 0;                                         // setup to correct value in Spell::prepare, don't must be used before.
    m_timer = 0;                                            // will set to cast time in prepare
    m_duration = 0;

    m_needAliveTargetMask = 0;

    m_spellFlags = SPELL_FLAG_NORMAL;
    CleanupTargetList();
}

Spell::~Spell()
{
}

template<typename T> WorldObject* Spell::FindCorpseUsing(uint32 corpseTypeMask)
{
    // non-standard target selection
    SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(m_spellInfo->GetRangeIndex());
    float max_range = GetSpellMaxRange(srange);

    WorldObject* result = NULL;

    T u_check(m_caster, max_range, corpseTypeMask);
    MaNGOS::WorldObjectSearcher<T> searcher(result, u_check);

    // Search for creature corpse only if typemask found
    if (corpseTypeMask != CREATURE_TYPEMASK_NONE)
        Cell::VisitGridObjects(m_caster, searcher, max_range);

    // Players corpse be searched only if no creatures corpse found
    if (!result)
        Cell::VisitWorldObjects(m_caster, searcher, max_range);

    return result;
}

template<class T>
bool Spell::CheckTargetBeforeLimitation(T* target, SpellEffectIndex eff)
{
    DETAIL_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell::CheckTargetBeforeLimitation unhandled target check call, spell %u caster %s, target %s",
        m_spellInfo->Id,
        m_caster ? m_caster->GetObjectGuid().GetString().c_str() : "<none>",
        target ? target->GetObjectGuid().GetString().c_str() : "<none>");
    return false;
};


template<>
bool Spell::CheckTargetBeforeLimitation(Unit* target, SpellEffectIndex eff)
{
    if (!target)
        return false;
    // check right target                                                                                       // should activ for spells 72034, 72096
    if (!target->isAlive() && !IsSpellAllowDeadTarget(m_spellInfo))
        return false;
    if (m_spellInfo->HasAttribute(SPELL_ATTR_EX3_TARGET_ONLY_PLAYER) && target->GetTypeId() != TYPEID_PLAYER &&
        m_spellInfo->EffectImplicitTargetA[eff] != TARGET_SCRIPT && m_spellInfo->EffectImplicitTargetA[eff] != TARGET_SELF)
    {
        return false;
    }
    // Check Aura spell req (need for AoE spells)
    if (m_spellInfo->targetAuraSpell && !target->HasAura(m_spellInfo->targetAuraSpell))
        return false;
    if (m_spellInfo->excludeTargetAuraSpell && target->HasAura(m_spellInfo->excludeTargetAuraSpell))
        return false;
    if (m_spellInfo->TargetAuraStateNot && target->HasAura(m_spellInfo->TargetAuraStateNot))
        return false;
    return true;
}

template<class T>
bool Spell::CheckTarget(T* target, SpellEffectIndex eff)
{

    DETAIL_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell::CheckTarget unhandled target check call, spell %u, caster %s, target %s",
        m_spellInfo->Id,
        m_caster ? m_caster->GetObjectGuid().GetString().c_str() : "<none>",
        target ? target->GetObjectGuid().GetString().c_str() : "<none>");

    return false;
};

template <>
bool Spell::CheckTarget(Unit* target, SpellEffectIndex eff)
{
    if (!target)
        return false;

    // Check targets for creature type mask and remove not appropriate (skip explicit self target case, maybe need other explicit targets)
    if (m_spellInfo->EffectImplicitTargetA[eff] != TARGET_SELF)
    {
        if (!SpellMgr::IsTargetMatchedWithCreatureType(m_spellInfo, target))
            return false;
    }

    // check right target                                                                                       // should activ for spells 72034, 72096
    if (m_spellInfo->HasAttribute(SPELL_ATTR_EX3_TARGET_ONLY_PLAYER) && target->GetTypeId() != TYPEID_PLAYER &&
        m_spellInfo->EffectImplicitTargetA[eff] != TARGET_SCRIPT && m_spellInfo->EffectImplicitTargetA[eff] != TARGET_SELF)
    {
        return false;
    }
    // Check Aura spell req (need for AoE spells)
    if (m_spellInfo->targetAuraSpell && !target->HasAura(m_spellInfo->targetAuraSpell))
        return false;

    if (m_spellInfo->excludeTargetAuraSpell && target->HasAura(m_spellInfo->excludeTargetAuraSpell))
        return false;

    if (m_spellInfo->TargetAuraStateNot && target->HasAura(m_spellInfo->TargetAuraStateNot))
        return false;

    // Check targets for not_selectable unit flag and remove
    // A player can cast spells on his pet (or other controlled unit) though in any state
    if (target != m_caster && target->isType(TYPEMASK_UNIT) && target->GetCharmerOrOwnerGuid() != m_caster->GetObjectGuid())
    {
        // any unattackable target skipped
        if (target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE) && target->GetObjectGuid() != m_caster->GetCharmerOrOwnerGuid())
            return false;

        // unselectable targets skipped in all cases except TARGET_SCRIPT targeting
        // in case TARGET_SCRIPT target selected by server always and can't be cheated
        if ((!m_IsTriggeredSpell || target != m_targets.getUnitTarget()) &&
            target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE) &&
            m_spellInfo->EffectImplicitTargetA[eff] != TARGET_SCRIPT &&
            m_spellInfo->EffectImplicitTargetB[eff] != TARGET_SCRIPT &&
            m_spellInfo->EffectImplicitTargetA[eff] != TARGET_AREAEFFECT_INSTANT &&
            m_spellInfo->EffectImplicitTargetB[eff] != TARGET_AREAEFFECT_INSTANT &&
            m_spellInfo->EffectImplicitTargetA[eff] != TARGET_AREAEFFECT_CUSTOM &&
            m_spellInfo->EffectImplicitTargetB[eff] != TARGET_AREAEFFECT_CUSTOM)
            return false;
    }

    if (target != m_caster && m_caster->GetCharmerOrOwnerGuid() == target->GetObjectGuid())
    {
        if (m_spellInfo->EffectImplicitTargetA[eff] == TARGET_MASTER ||
            m_spellInfo->EffectImplicitTargetB[eff] == TARGET_MASTER)
            return true;
    }

    // Check player targets and remove if in GM mode or GM invisibility (for not self casting case)
    if (target != m_caster && target->GetTypeId() == TYPEID_PLAYER &&
        !(((Player*)target)->isGameMaster() && target->IsVehicle()))
    {
        if (((Player*)target)->GetVisibility() == VISIBILITY_OFF)
            return false;

        if (((Player*)target)->isGameMaster() && !IsPositiveSpell(m_spellInfo->Id))
            return false;
    }

    // Check Sated & Exhaustion debuffs
    if (((m_spellInfo->Id == 2825) && (target->HasAura(57724) || target->HasAura(57723))) ||
        ((m_spellInfo->Id == 32182) && (target->HasAura(57724)|| target->HasAura(57723))))
        return false;

    // Check vampiric bite
    if (m_spellInfo->Id == 70946 && target->HasAura(70867))
        return false;

    if (m_spellInfo->EffectImplicitTargetA[eff] == TARGET_ALL_RAID_AROUND_CASTER ||
        m_spellInfo->EffectImplicitTargetB[eff] == TARGET_ALL_RAID_AROUND_CASTER)
        return true;

    // Check targets for LOS visibility (except spells without range limitations)
    switch (m_spellInfo->Effect[eff])
    {
        case SPELL_EFFECT_FRIEND_SUMMON:
        case SPELL_EFFECT_SUMMON_PLAYER:                    // from anywhere
            break;
        case SPELL_EFFECT_DUMMY:
            break;
        case SPELL_EFFECT_RESURRECT_NEW:
            // player far away, maybe his corpse near?
            if (target != m_caster && !target->IsVisibleTargetForSpell(m_caster, m_spellInfo))
            {
                if (!m_targets.getCorpseTargetGuid())
                    return false;

                Corpse* corpse = m_caster->GetMap()->GetCorpse(m_targets.getCorpseTargetGuid());
                if (!corpse)
                    return false;

                if (target->GetObjectGuid() != corpse->GetOwnerGuid())
                    return false;

                if (!m_spellInfo->HasAttribute(SPELL_ATTR_EX2_IGNORE_LOS) && !corpse->IsWithinLOSInMap(m_caster))
                    return false;
            }

            // all ok by some way or another, skip normal check
            break;
        default:                                            // normal case
            // Get GO cast coordinates if original caster -> GO
            if (target != m_caster)
            {
                if (DynamicObject* dynObj = m_caster->GetDynObject(m_triggeredByAuraSpell ? m_triggeredByAuraSpell->Id : m_spellInfo->Id))
                {
                    if (!target->IsVisibleTargetForSpell(dynObj, m_spellInfo))
                        return false;
                }
                else if (WorldObject* caster = GetCastingObject())
                {
                    if (!target->IsVisibleTargetForSpell(caster, m_spellInfo))
                        return false;
                }
            }
            break;
    }

    switch (m_spellInfo->Id)
    {
        case 37433:                                         // Spout (The Lurker Below), only players affected if its not in water
            if (target->GetTypeId() != TYPEID_PLAYER || target->IsInWater())
                return false;
            break;
        case 68921:                                         // Soulstorm (FoS), only targets farer than 10 away
        case 69049:                                         // Soulstorm            - = -
            if (m_caster->IsWithinDist(target, 10.0f, false))
                return false;
            break;
        default:
            break;
    }

    return true;
}

void Spell::FillTargetMap()
{
    // TODO: ADD the correct target FILLS!!!!!!

    UnitList tmpUnitLists[MAX_EFFECT_INDEX];                // Stores the temporary Target Lists for each effect
    uint8 effToIndex[MAX_EFFECT_INDEX] = {0, 1, 2};         // Helper array, to link to another tmpUnitList, if the targets for both effects match
    for(int i = EFFECT_INDEX_0; i < MAX_EFFECT_INDEX; ++i)
    {
        // not call for empty effect.
        // Also some spells use not used effect targets for store targets for dummy effect in triggered spells
        if (m_spellInfo->Effect[i] == SPELL_EFFECT_NONE)
            continue;

        // targets for TARGET_SCRIPT_COORDINATES (A) and TARGET_SCRIPT
        // for TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT (A) all is checked in Spell::CheckCast and in Spell::CheckItem
        // filled in Spell::CheckCast call
        if (m_spellInfo->EffectImplicitTargetA[i] == TARGET_SCRIPT_COORDINATES ||
           m_spellInfo->EffectImplicitTargetA[i] == TARGET_SCRIPT ||
           m_spellInfo->EffectImplicitTargetA[i] == TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT ||
           (m_spellInfo->EffectImplicitTargetB[i] == TARGET_SCRIPT && m_spellInfo->EffectImplicitTargetA[i] != TARGET_SELF))
            continue;

        // TODO: find a way so this is not needed?
        // for area auras always add caster as target (needed for totems for example)
        if (IsAreaAuraEffect(m_spellInfo->Effect[i]))
            AddUnitTarget(m_caster, SpellEffectIndex(i));

        // no double fill for same targets
        for (uint8 j = EFFECT_INDEX_0; j < i; ++j)
        {
            if (m_spellInfo->Effect[j] == SPELL_EFFECT_NONE)
                continue;

            // Check if same target, but handle i.e. AreaAuras different
            if (m_spellInfo->EffectImplicitTargetA[i] == m_spellInfo->EffectImplicitTargetA[j] &&
                m_spellInfo->EffectImplicitTargetB[i] == m_spellInfo->EffectImplicitTargetB[j] &&
                !IsAreaAuraEffect(m_spellInfo->Effect[i]) && !IsAreaAuraEffect(m_spellInfo->Effect[j]))
                // Add further conditions here if required
            {
                effToIndex[i] = j;                          // effect i has same targeting list as effect j
                break;
            }
        }

        // New target combination and fail custom fill method
        if (!FillCustomTargetMap(SpellEffectIndex(i),tmpUnitLists[i], effToIndex[i]) && effToIndex[i] == i)
        {
            // TargetA/TargetB dependent from each other, we not switch to full support this dependences
            // but need it support in some know cases
            switch(m_spellInfo->EffectImplicitTargetA[i])
            {
                case TARGET_NONE:
                    switch(m_spellInfo->EffectImplicitTargetB[i])
                    {
                        case TARGET_NONE:
                            if (m_caster->GetObjectGuid().IsPet() && m_spellInfo->TargetCreatureType == CREATURE_TYPEMASK_NONE)
                                SetTargetMap(SpellEffectIndex(i), TARGET_SELF, tmpUnitLists[i /*==effToIndex[i]*/]);
                            else
                                SetTargetMap(SpellEffectIndex(i), TARGET_EFFECT_SELECT, tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                        default:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                    }
                    break;
                case TARGET_SELF:
                    switch(m_spellInfo->EffectImplicitTargetB[i])
                    {
                        case TARGET_NONE:
                        case TARGET_EFFECT_SELECT:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                        case TARGET_AREAEFFECT_INSTANT:         // use B case that not dependent from from A in fact
                            if((m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION) == 0)
                                m_targets.setDestination(m_caster->GetPosition());
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                        case TARGET_BEHIND_VICTIM:              // use B case that not dependent from from A in fact
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                        default:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                    }
                    break;
                case TARGET_EFFECT_SELECT:
                    switch(m_spellInfo->EffectImplicitTargetB[i])
                    {
                        case TARGET_NONE:
                        case TARGET_EFFECT_SELECT:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                        // dest point setup required
                        case TARGET_AREAEFFECT_INSTANT:
                        case TARGET_AREAEFFECT_CUSTOM:
                        case TARGET_ALL_ENEMY_IN_AREA:
                        case TARGET_ALL_ENEMY_IN_AREA_INSTANT:
                        case TARGET_ALL_ENEMY_IN_AREA_CHANNELED:
                        case TARGET_ALL_FRIENDLY_UNITS_IN_AREA:
                        case TARGET_AREAEFFECT_GO_AROUND_DEST:
                            // triggered spells get dest point from default target set, ignore it
                            if (!(m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION) || m_IsTriggeredSpell)
                                if (WorldObject* castObject = GetCastingObject())
                                    m_targets.setDestination(castObject->GetPosition());
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                        // target pre-selection required
                        case TARGET_INNKEEPER_COORDINATES:
                        case TARGET_TABLE_X_Y_Z_COORDINATES:
                        case TARGET_CASTER_COORDINATES:
                        case TARGET_SCRIPT_COORDINATES:
                        case TARGET_CURRENT_ENEMY_COORDINATES:
                        case TARGET_DUELVSPLAYER_COORDINATES:
                        case TARGET_DYNAMIC_OBJECT_COORDINATES:
                        case TARGET_POINT_AT_NORTH:
                        case TARGET_POINT_AT_SOUTH:
                        case TARGET_POINT_AT_EAST:
                        case TARGET_POINT_AT_WEST:
                        case TARGET_POINT_AT_NE:
                        case TARGET_POINT_AT_NW:
                        case TARGET_POINT_AT_SE:
                        case TARGET_POINT_AT_SW:
                        case TARGET_RANDOM_NEARBY_DEST:
                            // need some target for processing
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                        default:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                    }
                    break;
                case TARGET_CASTER_COORDINATES:
                    switch(m_spellInfo->EffectImplicitTargetB[i])
                    {
                        case TARGET_ALL_ENEMY_IN_AREA:
                            // Note: this hack with search required until GO casting not implemented
                            // environment damage spells already have around enemies targeting but this not help in case nonexistent GO casting support
                            // currently each enemy selected explicitly and self cast damage
                            if (m_spellInfo->Effect[i] == SPELL_EFFECT_ENVIRONMENTAL_DAMAGE)
                            {
                                if (m_targets.getUnitTarget())
                                    tmpUnitLists[i /*==effToIndex[i]*/].push_back(m_targets.getUnitTarget());
                            }
                            else
                            {
                                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            }
                            break;
                        case TARGET_NONE:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            tmpUnitLists[i /*==effToIndex[i]*/].push_back(m_caster);
                            break;
                        default:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                    }
                    break;
                case TARGET_TABLE_X_Y_Z_COORDINATES:
                    switch(m_spellInfo->EffectImplicitTargetB[i])
                    {
                        case TARGET_NONE:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            // need some target for processing
                            SetTargetMap(SpellEffectIndex(i), TARGET_EFFECT_SELECT, tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                        case TARGET_AREAEFFECT_INSTANT:         // All 17/7 pairs used for dest teleportation, A processed in effect code
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                        default:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                    }
                    break;
                case TARGET_SELF2:
                    switch(m_spellInfo->EffectImplicitTargetB[i])
                    {
                        case TARGET_NONE:
                        case TARGET_EFFECT_SELECT:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                        case TARGET_RANDOM_NEARBY_DEST:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                        break;
                        case TARGET_AREAEFFECT_CUSTOM:
                            // triggered spells get dest point from default target set, ignore it
                            if (!(m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION) || m_IsTriggeredSpell)
                                if (WorldObject* castObject = GetCastingObject())
                                    m_targets.setDestination(castObject->GetPosition());
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                        // most A/B target pairs is self->negative and not expect adding caster to target list
                        default:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                    }
                    break;
                case TARGET_DUELVSPLAYER_COORDINATES:
                    switch(m_spellInfo->EffectImplicitTargetB[i])
                    {
                        case TARGET_NONE:
                        case TARGET_EFFECT_SELECT:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            if (Unit* currentTarget = m_targets.getUnitTarget())
                                tmpUnitLists[i /*==effToIndex[i]*/].push_back(currentTarget);
                            break;
                        default:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                    }
                    break;
                case TARGET_AREAEFFECT_CUSTOM:
                case TARGET_DUELVSPLAYER:
                default:
                    switch(m_spellInfo->EffectImplicitTargetB[i])
                    {
                        case TARGET_NONE:
                        case TARGET_EFFECT_SELECT:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                        case TARGET_SCRIPT_COORDINATES:         // B case filled in CheckCast but we need fill unit list base at A case
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                        default:
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitLists[i /*==effToIndex[i]*/]);
                            break;
                    }
                    break;
            }
        }

        if (tmpUnitLists[effToIndex[i]].size() == 1 && *tmpUnitLists[effToIndex[i]].begin() == m_caster->getVictim())
        {
            if (Unit* pMagnetTarget = m_caster->SelectMagnetTarget(*tmpUnitLists[effToIndex[i]].begin(), this, SpellEffectIndex(i)))
            {
                if (pMagnetTarget != *tmpUnitLists[effToIndex[i]].begin())
                {
                    tmpUnitLists[effToIndex[i]].clear();
                    tmpUnitLists[effToIndex[i]].push_back(pMagnetTarget);
                    m_spellFlags |= SPELL_FLAG_REDIRECTED;
                }
            }
        }
        GuidList currentTargets;
        for (UnitList::iterator itr = tmpUnitLists[effToIndex[i]].begin(); itr != tmpUnitLists[effToIndex[i]].end();)
        {
            if (!CheckTarget(*itr, SpellEffectIndex(i)))
            {
                itr = tmpUnitLists[effToIndex[i]].erase(itr);
                continue;
            }
            else
            {
                currentTargets.push_back((*itr)->GetObjectGuid());
                ++itr;
            }
        }

        if (!currentTargets.empty())
        {
            for (GuidList::const_iterator iguid = currentTargets.begin(); iguid != currentTargets.end(); ++iguid)
                AddTarget((*iguid), SpellEffectIndex(i));
        }
    }
}

void Spell::prepareDataForTriggerSystem()
{
    //==========================================================================================
    // Now fill data for trigger system, need know:
    // an spell trigger another or not ( m_canTrigger )
    // Create base triggers flags for Attacker and Victim ( m_procAttacker and  m_procVictim)
    //==========================================================================================
    // Fill flag can spell trigger or not
    // TODO: possible exist spell attribute for this
    m_canTrigger = false;

    if (m_CastItem)
        m_canTrigger = false;                               // Do not trigger from item cast spell
    else if (!m_IsTriggeredSpell)
        m_canTrigger = true;                                // Normal cast - can trigger
    else if (!m_triggeredByAuraSpell)
        m_canTrigger = true;                                // Triggered from SPELL_EFFECT_TRIGGER_SPELL - can trigger

    if (!m_canTrigger)                                      // Exceptions (some periodic triggers)
    {
        switch (m_spellInfo->SpellFamilyName)
        {
            case SPELLFAMILY_MAGE:
                // Arcane Missiles / Blizzard triggers need do it
                if (m_spellInfo->GetSpellFamilyFlags().test<CF_MAGE_BLIZZARD, CF_MAGE_ARCANE_MISSILES2>() || m_spellInfo->GetSpellIconID() == 212)
                    m_canTrigger = true;
                // Clearcasting trigger need do it
                // Replenish Mana, item spell with triggered cases (Mana Agate, etc mana gems)
                // Fingers of Frost: triggered by Frost/Ice Armor
                // Living Bomb - can trigger Hot Streak
                else if (m_spellInfo->GetSpellFamilyFlags().test<CF_MAGE_CLEARCASTING, CF_MAGE_REPLENISH_MANA, CF_MAGE_CHILLED, CF_MAGE_LIVING_BOMB_AOE>())
                    m_canTrigger = true;
                break;
            case SPELLFAMILY_WARLOCK:
                // For Hellfire Effect / Rain of Fire / Seed of Corruption triggers need do it
                if (m_spellInfo->GetSpellFamilyFlags().test<CF_WARLOCK_RAIN_OF_FIRE, CF_WARLOCK_HELLFIRE, CF_WARLOCK_SEED_OF_CORRUPTION2>())
                    m_canTrigger = true;
                break;
            case SPELLFAMILY_PRIEST:
                // For Penance,Mind Sear,Mind Flay,Improved Devouring Plague heal/damage triggers need do it
                if (m_spellInfo->GetSpellFamilyFlags().test<CF_PRIEST_MIND_FLAY1, CF_PRIEST_PENANCE_DMG, CF_PRIEST_PENANCE_HEAL, CF_PRIEST_IMP_DEVOURING_PLAGUE, CF_PRIEST_MIND_FLAY2>())
                    m_canTrigger = true;
                break;
            case SPELLFAMILY_ROGUE:
                // For poisons need do it
                if (m_spellInfo->GetSpellFamilyFlags().test<CF_ROGUE_INSTANT_POISON, CF_ROGUE_CRIPPLING_POISON, CF_ROGUE_MIND_NUMBING_POISON, CF_ROGUE_DEADLY_POISON, CF_ROGUE_WOUND_POISON, CF_ROGUE_ANESTHETIC_POISON>())
                    m_canTrigger = true;
                break;
            case SPELLFAMILY_HUNTER:
                // Hunter Rapid Killing/Explosive Trap Effect/Immolation Trap Effect/Frost Trap Aura/Snake Trap Effect/Explosive Shot
                if (m_spellInfo->GetSpellFamilyFlags().test<CF_HUNTER_FIRE_TRAP_EFFECTS, CF_HUNTER_FROST_TRAP_EFFECTS, CF_HUNTER_SNAKE_TRAP_EFFECT, CF_HUNTER_RAPID_KILLING, CF_HUNTER_EXPLOSIVE_SHOT2>())
                    m_canTrigger = true;
                break;
            case SPELLFAMILY_PALADIN:
                // For Judgements (all) / Holy Shock triggers need do it
                if (m_spellInfo->GetSpellFamilyFlags().test<CF_PALADIN_JUDGEMENT_OF_RIGHT, CF_PALADIN_JUDGEMENT_OF_WISDOM_LIGHT, CF_PALADIN_JUDGEMENT_OF_JUSTICE, CF_PALADIN_HOLY_SHOCK1, CF_PALADIN_JUDGEMENT_ACTIVATE, CF_PALADIN_JUDGEMENT_OF_LIGHT, CF_PALADIN_JUDGEMENT_OF_BLOOD_MARTYR, CF_PALADIN_HOLY_SHOCK>())
                    m_canTrigger = true;
                break;
            case SPELLFAMILY_WARRIOR:
                break;
            default:
                break;
        }
    }

    // Get data for type of attack and fill base info for trigger
    switch (m_spellInfo->DmgClass)
    {
        case SPELL_DAMAGE_CLASS_MELEE:
            m_procAttacker = PROC_FLAG_SUCCESSFUL_MELEE_SPELL_HIT;
            if (m_attackType == OFF_ATTACK)
                m_procAttacker |= PROC_FLAG_SUCCESSFUL_OFFHAND_HIT;
            m_procVictim   = PROC_FLAG_TAKEN_MELEE_SPELL_HIT;
            break;
        case SPELL_DAMAGE_CLASS_RANGED:
            // Auto attack
            if (m_spellInfo->HasAttribute(SPELL_ATTR_EX2_AUTOREPEAT_FLAG))
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_RANGED_HIT;
                m_procVictim   = PROC_FLAG_TAKEN_RANGED_HIT;
            }
            else // Ranged spell attack
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_RANGED_SPELL_HIT;
                m_procVictim   = PROC_FLAG_TAKEN_RANGED_SPELL_HIT;
            }
            break;
        default:
            if (IsPositiveSpell(m_spellInfo->Id))                                 // Check for positive spell
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_POSITIVE_SPELL;
                m_procVictim   = PROC_FLAG_TAKEN_POSITIVE_SPELL;
            }
            else if (m_spellInfo->HasAttribute(SPELL_ATTR_EX2_AUTOREPEAT_FLAG))   // Wands auto attack
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_RANGED_HIT;
                m_procVictim   = PROC_FLAG_TAKEN_RANGED_HIT;
            }
            else                                           // Negative spell
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_NEGATIVE_SPELL_HIT;
                m_procVictim   = PROC_FLAG_TAKEN_NEGATIVE_SPELL_HIT;
            }
            break;
    }

    // some negative spells have positive effects to another or same targets
    // avoid triggering negative hit for only positive targets
    m_negativeEffectMask = 0x0;
    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
        if (!IsPositiveEffect(m_spellInfo, SpellEffectIndex(i)))
            m_negativeEffectMask |= (1<<i);

    // Hunter traps spells: Immolation Trap Effect, Frost Trap (triggering spell!!),
    // Freezing Trap Effect(+ Freezing Arrow Effect), Explosive Trap Effect, Snake Trap Effect
    if (m_spellInfo->IsFitToFamily<SPELLFAMILY_HUNTER, CF_HUNTER_FREEZING_TRAP_EFFECT, CF_HUNTER_MISC_TRAPS, CF_HUNTER_SNAKE_TRAP_EFFECT, CF_HUNTER_EXPLOSIVE_TRAP_EFFECT, CF_HUNTER_IMMOLATION_TRAP, CF_HUNTER_FROST_TRAP>())
        m_procAttacker |= PROC_FLAG_ON_TRAP_ACTIVATION;
}

void Spell::CleanupTargetList()
{
    m_UniqueTargetInfo.clear();
    m_UniqueGOTargetInfo.clear();
    m_UniqueItemInfo.clear();
    m_delayMoment = 0;
}

void Spell::AddUnitTarget(Unit* pVictim, SpellEffectIndex effIndex)
{
    if (m_spellInfo->Effect[effIndex] == SPELL_EFFECT_NONE || !pVictim)
        return;

    // Check for effect immune skip if immuned
    bool immuned = pVictim->IsImmuneToSpellEffect(m_spellInfo, effIndex);

    if (pVictim->GetTypeId() == TYPEID_UNIT && ((Creature*)pVictim)->IsTotem() && (m_spellFlags & SPELL_FLAG_REDIRECTED))
        immuned = false;

    ObjectGuid targetGUID = pVictim->GetObjectGuid();

    // Lookup target in already in list
    for(TargetList::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (targetGUID == ihit->targetGUID)                 // Found in list
        {
            if (!immuned)
                ihit->effectMask |= 1 << effIndex;          // Add only effect mask if not immuned
            return;
        }
    }

    // This is new target calculate data for him

    // Get spell hit result on target
    TargetInfo target;
    target.targetGUID = targetGUID;                         // Store target GUID
    target.effectMask = immuned ? 0 : (1 << effIndex);      // Store index of effect if not immuned
    target.processed  = false;                              // Effects not applied on target

    // Calculate hit result
    target.missCondition = m_caster->SpellHitResult(pVictim, m_spellInfo);

    // spell fly from visual cast object
    WorldObject* affectiveObject = GetAffectiveCasterObject();

    // Speed possible inherited from triggering spell
    float speed_proto = GetBaseSpellSpeed();

    // Spell have trajectory - need calculate incoming time
    if (affectiveObject && (m_targets.GetSpeed() > M_NULL_F))
    {
        float dist = 5.0f;
        if (m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION)
            dist = affectiveObject->GetDistance(m_targets.getDestination());
        else
            dist = affectiveObject->GetDistance(pVictim->GetPosition());
        float speed = m_targets.GetSpeed() * cos(m_targets.GetElevation());

        target.timeDelay = (uint64) floor(dist / speed * 1000.0f);
    }
    // Spell have speed - need calculate incoming time
    else if ((speed_proto > M_NULL_F) && affectiveObject && (pVictim != affectiveObject))
    {
        // calculate spell incoming interval
        float dist = affectiveObject->GetDistance(pVictim->GetPositionX(), pVictim->GetPositionY(), pVictim->GetPositionZ());
        if (dist < 5.0f)
            dist = 5.0f;
        target.timeDelay = (uint64) floor(dist / speed_proto * 1000.0f);
/*
this piece of code make delayed stun and other effects for charge-like spells. seems not offlike...
        if (m_spellInfo->GetAttributesEx7() & SPELL_ATTR_EX7_HAS_CHARGE_EFFECT)
        {
            if (target.timeDelay > GetDelayStart())
                SetDelayStart(target.timeDelay);
        }
*/
    }
    // Spell cast on self - mostly TRIGGER_MISSILE code
    else if ((speed_proto > M_NULL_F) && affectiveObject && (pVictim == affectiveObject))
    {
        float dist = 0.0f;
        if (m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION)
            dist = affectiveObject->GetDistance(m_targets.getDestination());

        target.timeDelay = (uint64) floor(dist / speed_proto * 1000.0f);
    }
    else
        target.timeDelay = UI64LIT(0);

    // Calculate minimum incoming time
    if (m_delayMoment == 0 || m_delayMoment > target.timeDelay)
        m_delayMoment = target.timeDelay;

    // If target reflect spell back to caster
    if (target.missCondition == SPELL_MISS_REFLECT)
    {
        // Calculate reflected spell result on caster
        target.reflectResult =  m_caster->SpellHitResult(m_caster, m_spellInfo);

        if (target.reflectResult == SPELL_MISS_REFLECT)     // Impossible reflect again, so simply deflect spell
            target.reflectResult = SPELL_MISS_PARRY;

        // Increase time interval for reflected spells by 1.5
        target.timeDelay += target.timeDelay >> 1;

        m_spellFlags |= SPELL_FLAG_REFLECTED;
    }
    else
        target.reflectResult = SPELL_MISS_NONE;

    // Add target to list
    // valgrind says, that memleak here. need research...
    m_UniqueTargetInfo.push_back(target);
}

void Spell::AddTarget(ObjectGuid targetGuid, SpellEffectIndex effIndex)
{
    if (targetGuid.IsEmpty())
        return;

    switch(targetGuid.GetHigh())
    {
        case HIGHGUID_UNIT:
        case HIGHGUID_VEHICLE:
        case HIGHGUID_PET:
        case HIGHGUID_PLAYER:
        {
            // Only for speedup (may be removed later)
            if (m_caster->GetObjectGuid() == targetGuid)
                AddUnitTarget(m_caster, effIndex);
            else if (Unit* unit = ObjectAccessor::GetUnit(*m_caster, targetGuid))
                AddUnitTarget(unit, effIndex);
            break;
        }
        case HIGHGUID_GAMEOBJECT:
        case HIGHGUID_TRANSPORT:
        case HIGHGUID_DYNAMICOBJECT:
        case HIGHGUID_MO_TRANSPORT:
        {
            if (GameObject* object = m_caster->GetMap()->GetGameObject(targetGuid))
                AddGOTarget(object, effIndex);
            break;
        }
        case HIGHGUID_CORPSE:
        case HIGHGUID_ITEM:
        {
            sLog.outError("Spell::AddTarget currently this type of targets (%s) supported by another way!", targetGuid.GetString().c_str());
            break;
        }
/*
        HIGHGUID_INSTANCE:
        HIGHGUID_GROUP:
*/
        default:
            sLog.outError("Spell::AddTarget unhandled type of spell target (%s)!", targetGuid.GetString().c_str());
            break;
    }
}

void Spell::AddGOTarget(GameObject* pVictim, SpellEffectIndex effIndex)
{
    if (m_spellInfo->Effect[effIndex] == 0)
        return;

    ObjectGuid targetGUID = pVictim->GetObjectGuid();

    // Check target is effectible to this effect ?
    GameobjectTypes gameobjectType = pVictim->GetGoType();
    switch(m_spellInfo->Effect[effIndex])
    {
        case SPELL_EFFECT_WMO_DAMAGE:
        case SPELL_EFFECT_WMO_REPAIR:
        case SPELL_EFFECT_WMO_CHANGE:
            if (gameobjectType != GAMEOBJECT_TYPE_DESTRUCTIBLE_BUILDING)
                return;
            break;
        default:
            break;
    }

    // Lookup target in already in list
    for(GOTargetList::iterator ihit = m_UniqueGOTargetInfo.begin(); ihit != m_UniqueGOTargetInfo.end(); ++ihit)
    {
        if (targetGUID == ihit->targetGUID)                 // Found in list
        {
            ihit->effectMask |= (1 << effIndex);            // Add only effect mask
            return;
        }
    }

    // This is new target calculate data for him

    GOTargetInfo target;
    target.targetGUID = targetGUID;
    target.effectMask = (1 << effIndex);
    target.processed  = false;                              // Effects not apply on target

    // spell fly from visual cast object
    WorldObject* affectiveObject = GetAffectiveCasterObject();

    // Speed possible inherited from triggering spell
    float speed_proto = GetBaseSpellSpeed();

    // Spell have trajectory - need calculate incoming time
    if (affectiveObject && m_targets.GetSpeed() > M_NULL_F)
    {
        float dist = 5.0f;
        if (m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION)
            dist = affectiveObject->GetDistance(m_targets.getDestination());
        else
            dist = affectiveObject->GetDistance(pVictim->GetPosition());

        float speed = m_targets.GetSpeed() * cos(m_targets.GetElevation());

        target.timeDelay = (uint64) floor(dist / speed * 1000.0f);
    }
    // Spell have speed - need calculate incoming time
    else if (speed_proto > M_NULL_F && affectiveObject && pVictim != affectiveObject)
    {
        // calculate spell incoming interval
        float dist = affectiveObject->GetDistance(pVictim->GetPositionX(), pVictim->GetPositionY(), pVictim->GetPositionZ());
        if (dist < 5.0f)
            dist = 5.0f;
        target.timeDelay = (uint64) floor(dist / speed_proto * 1000.0f);
    }
    else
        target.timeDelay = UI64LIT(0);

    if (m_delayMoment == 0 || m_delayMoment > target.timeDelay)
        m_delayMoment = target.timeDelay;

    // Add target to list
    m_UniqueGOTargetInfo.push_back(target);
}

void Spell::AddItemTarget(Item* pitem, SpellEffectIndex effIndex)
{
    if (m_spellInfo->Effect[effIndex] == 0)
        return;

    // Lookup target in already in list
    for(ItemTargetList::iterator ihit = m_UniqueItemInfo.begin(); ihit != m_UniqueItemInfo.end(); ++ihit)
    {
        if (pitem == ihit->item)                            // Found in list
        {
            ihit->effectMask |= (1 << effIndex);            // Add only effect mask
            return;
        }
    }

    // This is new target add data

    ItemTargetInfo target;
    target.item       = pitem;
    target.effectMask = (1 << effIndex);
    m_UniqueItemInfo.push_back(target);
}

void Spell::DoAllEffectOnTarget(TargetInfo* target)
{
    if (!target || target->processed)                       // Check target
        return;

    target->processed = true;                               // Target checked in apply effects procedure

    // Get mask of effects for target
    uint32 mask = target->effectMask;

    Unit* unit = (m_caster->GetObjectGuid() == target->targetGUID) ?
                    m_caster :
                    ObjectAccessor::GetUnit(*m_caster, target->targetGUID);

    if (!unit)
        return;

    // Get original caster (if exist) and calculate damage/healing from him data
    Unit *real_caster = GetAffectiveCaster();
    // FIXME: in case wild GO heal/damage spells will be used target bonuses
    Unit* caster = real_caster ? real_caster : m_caster;

    SpellMissInfo missInfo = target->missCondition;
    // Need init unitTarget by default unit (can changed in code on reflect)
    // Or on missInfo!=SPELL_MISS_NONE unitTarget undefined (but need in trigger subsystem)
    unitTarget = unit;

    // Reset damage/healing counter
    ResetEffectDamageAndHeal();

    // Fill base trigger info
    DamageInfo damageInfo(caster, unitTarget, m_spellInfo);
    damageInfo.procAttacker = m_procAttacker;
    damageInfo.procVictim   = m_procVictim;
    damageInfo.procEx       = PROC_EX_NONE;
    damageInfo.attackType   = m_attackType;

    // drop proc flags in case target not affected negative effects in negative spell
    // for example caster bonus or animation,
    // except miss case where will assigned PROC_EX_* flags later
    if (((damageInfo.procAttacker | damageInfo.procVictim) & NEGATIVE_TRIGGER_MASK) &&
        !(target->effectMask & m_negativeEffectMask) && missInfo == SPELL_MISS_NONE)
    {
        damageInfo.procAttacker = PROC_FLAG_NONE;
        damageInfo.procVictim   = PROC_FLAG_NONE;
    }

    // Speed possible inherited from triggering spell
    // float speed_proto = GetBaseSpellSpeed();

    if (m_spellInfo->GetSpeed() > M_NULL_F || GetDelayStart())
    {
        // mark effects that were already handled in Spell::HandleDelayedSpellLaunch on spell launch as processed
        for (int32 i = 0; i < MAX_EFFECT_INDEX; ++i)
            if (IsEffectHandledOnDelayedSpellLaunch(m_spellInfo, SpellEffectIndex(i)))
                mask &= ~(1<<i);

        // maybe used in effects that are handled on hit
        m_damage += target->damage;
    }

    // recheck for availability/visibility of target
    if (((m_spellInfo->GetSpeed() > M_NULL_F ||
        (m_spellInfo->EffectImplicitTargetA[0] == TARGET_CHAIN_DAMAGE &&
        GetSpellCastTime(m_spellInfo, this) > 0)) &&
        (!unit->isVisibleForOrDetect(m_caster, m_caster, false) && !m_IsTriggeredSpell)))
    {
        caster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_EVADE);
        missInfo = SPELL_MISS_EVADE;
        return;
    }

    if (missInfo==SPELL_MISS_NONE)                          // In case spell hit target, do all effect on that target
        DoSpellHitOnUnit(unit, mask);
    else if (missInfo == SPELL_MISS_REFLECT)                // In case spell reflect from target, do all effect on caster (if hit)
    {
        if (target->reflectResult == SPELL_MISS_NONE)       // If reflected spell hit caster -> do all effect on him
        {
            DoSpellHitOnUnit(m_caster, mask);
            damageInfo.target = m_caster;
            unitTarget = m_caster;
        }
    }
    else if (missInfo == SPELL_MISS_MISS || missInfo == SPELL_MISS_RESIST)
    {
        if (unit && real_caster && real_caster != unit)
        {
            // can cause back attack (if detected)
            if (!m_spellInfo->HasAttribute(SPELL_ATTR_EX3_NO_INITIAL_AGGRO)
                && !m_spellInfo->HasAttribute(SPELL_ATTR_EX_NO_THREAT)
                && (!IsPositiveSpell(m_spellInfo->Id) || (IsNonPositiveSpell(m_spellInfo) && !real_caster->IsFriendlyTo(unit)))
                && real_caster->isVisibleForOrDetect(unit, unit, false))
            {
                unit->AttackedBy(real_caster);
            }
        }
    }


    // All calculated do it!
    // Do healing and triggers
    if (m_healing)
    {
        bool crit = real_caster && real_caster->IsSpellCrit(unitTarget, m_spellInfo, m_spellSchoolMask);
        damageInfo.damage = m_healing;
        if (crit)
        {
            damageInfo.procEx |= PROC_EX_CRITICAL_HIT;
            damageInfo.damage  = caster->SpellCriticalHealingBonus(m_spellInfo, damageInfo.damage, NULL);
        }
        else
            damageInfo.procEx |= PROC_EX_NORMAL_HIT;

        damageInfo.SetAbsorb(0);
        damageInfo.target->CalculateHealAbsorb(damageInfo.damage, &damageInfo.absorb);
        damageInfo.damage -= damageInfo.GetAbsorb();

        // Do triggers for unit (reflect triggers passed on hit phase for correct drop charge)
        if (m_canTrigger && missInfo != SPELL_MISS_REFLECT)
        {
            uint32 saveAttacker = damageInfo.procAttacker;
            damageInfo.procAttacker = real_caster ? damageInfo.procAttacker : PROC_FLAG_NONE;
            damageInfo.attacker->ProcDamageAndSpell(&damageInfo);
            damageInfo.procAttacker = saveAttacker;
        }

        int32 gain = damageInfo.attacker->DealHeal(&damageInfo, crit);

        if (real_caster)
            unitTarget->getHostileRefManager().threatAssist(real_caster, float(gain) * 0.5f * sSpellMgr.GetSpellThreatMultiplier(m_spellInfo), m_spellInfo);
    }
    // Do damage and triggers
    else if (m_damage)
    {
        // Fill base damage struct (unitTarget - is real spell target)

        if (m_spellInfo->GetSpeed() > M_NULL_F)
        {
            damageInfo.damage = m_damage;
            damageInfo.HitInfo = target->HitInfo;
        }
        // Add bonuses and fill damageInfo struct
        else
        {
            float dmgMultiplier = 1.0f;
            if (m_damageIndex >= 0 && (m_applyMultiplierMask & (1 << m_damageIndex)))
                dmgMultiplier = m_damageMultipliers[m_damageIndex];

            damageInfo.damage = m_damage;
            caster->CalculateSpellDamage(&damageInfo, dmgMultiplier);
        }

        unitTarget->CalculateAbsorbResistBlock(caster, &damageInfo, m_spellInfo);

        caster->DealDamageMods(&damageInfo);

        // Send log damage message to client
        caster->SendSpellNonMeleeDamageLog(&damageInfo);

        damageInfo.procEx = createProcExtendMask(&damageInfo, missInfo);
        damageInfo.procVictim |= PROC_FLAG_TAKEN_ANY_DAMAGE;

        if (damageInfo.GetAbsorb() && damageInfo.damage == 0)
            damageInfo.procEx &= ~PROC_EX_DIRECT_DAMAGE;
        else if (damageInfo.damage > 0)
            damageInfo.procEx |= PROC_EX_DIRECT_DAMAGE;

        // Do triggers for unit (reflect triggers passed on hit phase for correct drop charge)
        if (m_canTrigger && missInfo != SPELL_MISS_REFLECT)
        {
            uint32 saveAttacker = damageInfo.procAttacker;
            damageInfo.procAttacker = real_caster ? damageInfo.procAttacker : PROC_FLAG_NONE;
            caster->ProcDamageAndSpell(&damageInfo);
            damageInfo.procAttacker = saveAttacker;
        }

        // trigger weapon enchants for weapon based spells; exclude spells that stop attack, because may break CC
        if (m_caster->GetTypeId() == TYPEID_PLAYER && m_spellInfo->EquippedItemClass == ITEM_CLASS_WEAPON &&
            !m_spellInfo->HasAttribute(SPELL_ATTR_STOP_ATTACK_TARGET))
            ((Player*)m_caster)->CastItemCombatSpell(unitTarget, m_attackType);

        // Haunt (NOTE: for avoid use additional field damage stored in dummy value (replace unused 100%)
        // apply before deal damage because aura can be removed at target kill
        if (m_spellInfo->SpellFamilyName == SPELLFAMILY_WARLOCK && m_spellInfo->GetSpellIconID() == 3172 &&
            m_spellInfo->GetSpellFamilyFlags().test<CF_WARLOCK_HAUNT>())
            if (Aura const* dummy = unitTarget->GetDummyAura(m_spellInfo->Id))
                dummy->GetModifier()->m_amount = damageInfo.damage + damageInfo.GetAbsorb();

        /* process anticheat check */
        if (caster->GetObjectGuid().IsPlayer())
            ((Player*)caster)->GetAntiCheat()->DoAntiCheatCheck(CHECK_DAMAGE_SPELL, m_spellInfo->Id, 0, damageInfo.damage);

        caster->DealSpellDamage(&damageInfo, true);

        // Scourge Strike, here because needs to use final damage in second part of the spell
        if (m_spellInfo->IsFitToFamily<SPELLFAMILY_DEATHKNIGHT, CF_DEATHKNIGHT_SCOURGE_STRIKE>())
        {
            uint32 count = 0;
            Unit::SpellAuraHolderMap const& auras = unitTarget->GetSpellAuraHolderMap();
            for(Unit::SpellAuraHolderMap::const_iterator itr = auras.begin(); itr!=auras.end(); ++itr)
            {
                if (itr->second->GetSpellProto()->Dispel == DISPEL_DISEASE &&
                    itr->second->GetCasterGuid() == caster->GetObjectGuid())
                    ++count;
            }

            if (count)
            {
                int32 bp = count * CalculateDamage(EFFECT_INDEX_2, unitTarget) * damageInfo.damage / 100;
                if (Aura const* dummy = caster->GetDummyAura(64736)) // Item - Death Knight T8 Melee 4P Bonus
                    bp *= ((dummy->GetModifier()->m_amount * count) + 100.0f) / 100.0f;

                if (bp)
                    caster->CastCustomSpell(unitTarget, 70890, &bp, NULL, NULL, true);
            }
        }
    }
    // Passive spell hits/misses or active spells only misses (only triggers if proc flags set)
    else if (damageInfo.procAttacker || damageInfo.procVictim)
    {
        // Fill base damage struct (unitTarget - is real spell target)
        damageInfo.procEx = createProcExtendMask(&damageInfo, missInfo);
        // Do triggers for unit (reflect triggers passed on hit phase for correct drop charge)
        if (m_canTrigger && missInfo != SPELL_MISS_REFLECT)
        {
            uint32 saveAttacker = damageInfo.procAttacker;
            damageInfo.procAttacker = real_caster ? damageInfo.procAttacker : PROC_FLAG_NONE;
            caster->ProcDamageAndSpell(&damageInfo);
            damageInfo.procAttacker = saveAttacker;
        }
    }

    // Update damage multipliers
    for (uint8 effectNumber = 0; effectNumber < MAX_EFFECT_INDEX; ++effectNumber)
        if ((mask & (1 << effectNumber)) && (m_applyMultiplierMask & (1 << effectNumber)))
        {
            // Get multiplier
            float multiplier = m_spellInfo->DmgMultiplier[effectNumber];
            // Apply multiplier mods
            if (real_caster)
                if (Player* modOwner = real_caster->GetSpellModOwner())
                    modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_EFFECT_PAST_FIRST, multiplier);
            m_damageMultipliers[effectNumber] *= multiplier;
        }

    // Call scripted function for AI if this spell is casted upon a creature
    if (unit->GetTypeId() == TYPEID_UNIT)
    {
        // cast at creature (or GO) quest objectives update at successful cast finished (+channel finished)
        // ignore pets or autorepeat/melee casts for speed (not exist quest for spells (hm... )
        if (real_caster && !((Creature*)unit)->IsPet() && !IsAutoRepeat() && !IsNextMeleeSwingSpell() && !IsChannelActive())
            if (Player* p = real_caster->GetCharmerOrOwnerPlayerOrPlayerItself())
                p->RewardPlayerAndGroupAtCast(unit, m_spellInfo->Id);

        if(((Creature*)unit)->AI())
            ((Creature*)unit)->AI()->SpellHit(m_caster, m_spellInfo);
    }

    // Call scripted function for AI if this spell is casted by a creature
    if (m_caster->GetTypeId() == TYPEID_UNIT && ((Creature*)m_caster)->AI())
        ((Creature*)m_caster)->AI()->SpellHitTarget(unit, m_spellInfo);
    if (real_caster && real_caster != m_caster && real_caster->GetTypeId() == TYPEID_UNIT && ((Creature*)real_caster)->AI())
        ((Creature*)real_caster)->AI()->SpellHitTarget(unit, m_spellInfo);
}

void Spell::DoSpellHitOnUnit(Unit* unit, uint32 effectMask)
{
    if (!unit || !unit->isType(TYPEMASK_UNIT) || (!effectMask && !damage))
        return;

    Unit* realCaster = GetAffectiveCaster();

    // Speed possible inherited from triggering spell
    // float speed_proto = GetBaseSpellSpeed();

    // Recheck effect immune (only for delayed spells)
    if (m_spellInfo->GetSpeed() > M_NULL_F)
    {
        for (int effectNumber = 0; effectNumber < MAX_EFFECT_INDEX; ++effectNumber)
        {
            if (effectMask & (1 << effectNumber))
            {
                // don't handle effect to which target is immuned
                if (unit->IsImmuneToSpellEffect(m_spellInfo, SpellEffectIndex(effectNumber)))
                    effectMask &= ~(1 << effectNumber);
            }
        }
    }

    // Recheck immune (only for delayed spells)
    if (m_spellInfo->GetSpeed() > M_NULL_F && (
        (IsSpellCauseDamage(m_spellInfo) && unit->IsImmunedToDamage(GetSpellSchoolMask(m_spellInfo))) ||
        unit->IsImmuneToSpell(m_spellInfo, GetAffectiveUnitCaster()->IsFriendlyTo(unit))) &&
        !m_spellInfo->HasAttribute(SPELL_ATTR_UNAFFECTED_BY_INVULNERABILITY))
    {
        if (realCaster)
            realCaster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_IMMUNE);

        ResetEffectDamageAndHeal();
        return;
    }

    if (unit->GetTypeId() == TYPEID_PLAYER && unit->IsInWorld())
    {
        ((Player*)unit)->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BE_SPELL_TARGET, m_spellInfo->Id);
        ((Player*)unit)->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BE_SPELL_TARGET2, m_spellInfo->Id);
    }

    if (realCaster && realCaster->GetTypeId() == TYPEID_PLAYER && realCaster->IsInWorld())
        ((Player*)realCaster)->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_CAST_SPELL2, m_spellInfo->Id, 0, unit);

    if (realCaster && realCaster != unit)
    {
        // Recheck  UNIT_FLAG_NON_ATTACKABLE for delayed spells
        if (m_spellInfo->GetSpeed() > M_NULL_F &&
            unit->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE) &&
            unit->GetCharmerOrOwnerGuid() != m_caster->GetObjectGuid())
        {
            realCaster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_EVADE);
            ResetEffectDamageAndHeal();
            return;
        }

        if (!realCaster->IsFriendlyTo(unit))
        {
            // for delayed spells ignore not visible explicit target
            if (m_spellInfo->GetSpeed() > M_NULL_F && unit == m_targets.getUnitTarget() &&
                (!unit->isVisibleForOrDetect(m_caster, m_caster, false) && !m_IsTriggeredSpell))
            {
                realCaster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_EVADE);
                ResetEffectDamageAndHeal();
                return;
            }

            if (!m_spellInfo->HasAttribute(SPELL_ATTR_EX3_NO_INITIAL_AGGRO)
                && !m_spellInfo->HasAttribute(SPELL_ATTR_EX_NO_THREAT)
                && (!IsPositiveSpell(m_spellInfo->Id) || IsNonPositiveSpell(m_spellInfo))
                && realCaster->isVisibleForOrDetect(unit, unit, false))
            {
                if (!unit->IsStandState() && !unit->hasUnitState(UNIT_STAT_STUNNED))
                    unit->SetStandState(UNIT_STAND_STATE_STAND);

                unit->AttackedBy(realCaster);
            }
        }
        else
        {
            // for delayed spells ignore negative spells (after duel end) for friendly targets
            if (m_spellInfo->GetSpeed() > M_NULL_F && !IsPositiveSpell(m_spellInfo->Id))
            {
                realCaster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_EVADE);
                ResetEffectDamageAndHeal();
                return;
            }

            // assisting case, healing and resurrection
            if (unit->hasUnitState(UNIT_STAT_ATTACK_PLAYER))
                realCaster->SetContestedPvP();

            if (unit->isInCombat() && !m_spellInfo->HasAttribute(SPELL_ATTR_EX3_NO_INITIAL_AGGRO))
            {
                realCaster->SetInCombatState(unit->GetCombatTimer() > 0);
                unit->getHostileRefManager().threatAssist(realCaster, 0.0f, m_spellInfo);
            }
        }
    }

    // Get Data Needed for Diminishing Returns, some effects may have multiple auras, so this must be done on spell hit, not aura add
    // Diminishing must not affect spells, casted on self
    if ((realCaster && unit && realCaster != unit) || (m_spellFlags & SPELL_FLAG_REFLECTED))
    {
        m_diminishGroup = GetDiminishingReturnsGroupForSpell(m_spellInfo,m_triggeredByAuraSpell);
        m_diminishLevel = unit->GetDiminishing(m_diminishGroup);
        // Increase Diminishing on unit, current informations for actually casts will use values above
        if ((GetDiminishingReturnsGroupType(m_diminishGroup) == DRTYPE_PLAYER && unit->GetCharmerOrOwnerPlayerOrPlayerItself()) ||
            GetDiminishingReturnsGroupType(m_diminishGroup) >= DRTYPE_ALL)
            unit->IncrDiminishing(m_diminishGroup);
    }

    // Apply additional spell effects to target
    CastPreCastSpells(unit);

    if (IsSpellAppliesAura(m_spellInfo, effectMask))
    {
        m_spellAuraHolder = CreateSpellAuraHolder(m_spellInfo, unit, realCaster, m_CastItem);
        m_spellAuraHolder->setDiminishGroup(m_diminishGroup);
    }

    for(int effectNumber = 0; effectNumber < MAX_EFFECT_INDEX; ++effectNumber)
    {
        if (effectMask & (1 << effectNumber))
        {
            HandleEffects(unit, NULL, NULL, SpellEffectIndex(effectNumber));
        }
    }

    // now apply all created auras
    if (m_spellAuraHolder && m_spellAuraHolder->GetTarget() == unit)
    {
        // normally shouldn't happen
        if (!m_spellAuraHolder->IsEmptyHolder())
        {
            int32 duration = m_spellAuraHolder->GetAuraMaxDuration();
            int32 originalDuration = duration;

            if (duration > 0)
            {
                int32 limitduration = GetDiminishingReturnsLimitDuration(m_diminishGroup, m_spellInfo);
                unit->ApplyDiminishingToDuration(m_diminishGroup, duration, m_caster, m_diminishLevel, limitduration, m_spellFlags & SPELL_FLAG_REFLECTED);

                // Fully diminished
                if (duration == 0)
                {
                    return;
                }
            }

            duration = unit->CalculateAuraDuration(m_spellInfo, effectMask, duration, m_caster);

            if (duration != originalDuration)
            {
                m_spellAuraHolder->SetAuraMaxDuration(duration);
                m_spellAuraHolder->SetAuraDuration(duration);
            }

            unit->AddSpellAuraHolder(m_spellAuraHolder);
        }
    }
    else if (m_spellAuraHolder)
    {
        DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST,"Spell::DoSpellHitOnUnit spell %u caster %s cannot apply spellauraholder to %s due to different targets.",
            m_spellInfo->Id,
            realCaster->GetObjectGuid().GetString().c_str(),
            unit->GetObjectGuid().GetString().c_str());
    }
}

void Spell::DoAllEffectOnTarget(GOTargetInfo *target)
{

    if (!target || target->processed)                       // Check target
        return;

    target->processed = true;                               // Target checked in apply effects procedure

    uint32 effectMask = target->effectMask;
    if(!effectMask)
        return;

    GameObject* go = m_caster->GetMap()->GetGameObject(target->targetGUID);
    if(!go)
        return;

    for(int effectNumber = 0; effectNumber < MAX_EFFECT_INDEX; ++effectNumber)
        if (effectMask & (1 << effectNumber))
            HandleEffects(NULL, NULL, go, SpellEffectIndex(effectNumber));

    // cast at creature (or GO) quest objectives update at successful cast finished (+channel finished)
    // ignore autorepeat/melee casts for speed (not exist quest for spells (hm... )
    if ( !IsAutoRepeat() && !IsNextMeleeSwingSpell() && !IsChannelActive() )
    {
        if ( Player* p = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself() )
            p->RewardPlayerAndGroupAtCast(go, m_spellInfo->Id);
    }
}

void Spell::DoAllEffectOnTarget(ItemTargetInfo *target)
{
    uint32 effectMask = target->effectMask;
    if(!target->item || !effectMask)
        return;

    for(int effectNumber = 0; effectNumber < MAX_EFFECT_INDEX; ++effectNumber)
        if (effectMask & (1 << effectNumber))
            HandleEffects(NULL, target->item, NULL, SpellEffectIndex(effectNumber));
}

void Spell::HandleDelayedSpellLaunch(TargetInfo *target)
{
     // Get mask of effects for target
    uint32 mask = target->effectMask;

    Unit* unit = m_caster->GetObjectGuid() == target->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, target->targetGUID);
    if (!unit)
        return;

    // Get original caster (if exist) and calculate damage/healing from him data
    Unit *real_caster = GetAffectiveCaster();
    // FIXME: in case wild GO heal/damage spells will be used target bonuses
    Unit *caster = real_caster ? real_caster : m_caster;

    SpellMissInfo missInfo = target->missCondition;
    // Need init unitTarget by default unit (can changed in code on reflect)
    // Or on missInfo!=SPELL_MISS_NONE unitTarget undefined (but need in trigger subsystem)
    unitTarget = unit;

    // Reset damage/healing counter
    ResetEffectDamageAndHeal(); // healing maybe not needed at this point

    // Fill base damage struct (unitTarget - is real spell target)
    DamageInfo damageInfo(caster, unitTarget, m_spellInfo);
    damageInfo.attackType = m_attackType;

    // keep damage amount for reflected spells
    if (missInfo == SPELL_MISS_NONE || (missInfo == SPELL_MISS_REFLECT && target->reflectResult == SPELL_MISS_NONE))
    {
        std::bitset<MAX_EFFECT_INDEX> update_mult;

        for (int32 effectNumber = 0; effectNumber < MAX_EFFECT_INDEX; ++effectNumber)
        {
            if ((mask & (1 << effectNumber)) && IsEffectHandledOnDelayedSpellLaunch(m_spellInfo, SpellEffectIndex(effectNumber)))
            {
                HandleEffects(unit, NULL, NULL, SpellEffectIndex(effectNumber));
                if ( m_applyMultiplierMask & (1 << effectNumber) )
                {
                    update_mult[effectNumber] = true;
                }
            }
        }

        if (m_damage > 0)
        {
            float dmgMultiplier = 1.0f;
            if (m_damageIndex >= 0 && (m_applyMultiplierMask & (1 << m_damageIndex)))
                dmgMultiplier = m_damageMultipliers[m_damageIndex];
            damageInfo.damage = m_damage;
            caster->CalculateSpellDamage(&damageInfo, dmgMultiplier);
        }

        // Update damage multipliers
        for (int32 effectNumber = 0; effectNumber < MAX_EFFECT_INDEX; ++effectNumber)
        {
            if (update_mult[effectNumber])
            {
                // Get multiplier
                float multiplier = m_spellInfo->DmgMultiplier[effectNumber];
                // Apply multiplier mods
                if (real_caster)
                    if (Player* modOwner = real_caster->GetSpellModOwner())
                        modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_EFFECT_PAST_FIRST, multiplier);
                m_damageMultipliers[effectNumber] *= multiplier;
            }
        }
    }

    target->damage = damageInfo.damage;
    target->HitInfo = damageInfo.HitInfo;
}

void Spell::InitializeDamageMultipliers()
{
    for (int32 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (m_spellInfo->Effect[i] == 0)
            continue;

        uint32 EffectChainTarget = m_spellInfo->EffectChainTarget[i];
        if (Unit* realCaster = GetAffectiveCaster())
            if (Player* modOwner = realCaster->GetSpellModOwner())
                modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_JUMP_TARGETS, EffectChainTarget);

        m_damageMultipliers[i] = 1.0f;
        if ( (m_spellInfo->EffectImplicitTargetA[i] == TARGET_CHAIN_DAMAGE || m_spellInfo->EffectImplicitTargetA[i] == TARGET_CHAIN_HEAL) &&
            (EffectChainTarget > 1) )
            m_applyMultiplierMask |= (1 << i);
    }
}

bool Spell::IsAliveUnitPresentInTargetList()
{
    // Not need check return true
    if (m_needAliveTargetMask == 0)
        return true;

    uint8 needAliveTargetMask = m_needAliveTargetMask;

    for(TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (ihit->missCondition == SPELL_MISS_NONE && (needAliveTargetMask & ihit->effectMask))
        {
            Unit *unit = m_caster->GetObjectGuid() == ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);

            // either unit is alive and normal spell, or unit dead and deathonly-spell
            if (unit && (unit->isAlive() != IsDeathOnlySpell(m_spellInfo)))
                needAliveTargetMask &= ~ihit->effectMask;   // remove from need alive mask effect that have alive target
        }
    }

    // is all effects from m_needAliveTargetMask have alive targets
    return needAliveTargetMask == 0;
}

// Helper for Chain Healing
// Spell target first
// Raidmates then descending by injury suffered (MaxHealth - Health)
// Other players/mobs then descending by injury suffered (MaxHealth - Health)
struct ChainHealingOrder : public std::binary_function<const Unit*, const Unit*, bool>
{
    const Unit* MainTarget;
    ChainHealingOrder(Unit const* Target) : MainTarget(Target) {};
    // functor for operator ">"
    bool operator()(Unit const* _Left, Unit const* _Right) const
    {
        return (ChainHealingHash(_Left) < ChainHealingHash(_Right));
    }
    int32 ChainHealingHash(Unit const* Target) const
    {
        if (Target == MainTarget)
            return 0;
        else if (Target->GetTypeId() == TYPEID_PLAYER && MainTarget->GetTypeId() == TYPEID_PLAYER &&
            ((Player const*)Target)->IsInSameRaidWith((Player const*)MainTarget))
        {
            if (Target->GetHealth() == Target->GetMaxHealth())
                return 40000;
            else
                return 20000 - Target->GetMaxHealth() + Target->GetHealth();
        }
        else
            return 40000 - Target->GetMaxHealth() + Target->GetHealth();
    }
};

class ChainHealingFullHealth: std::unary_function<const Unit*, bool>
{
    public:
        const Unit* MainTarget;
        ChainHealingFullHealth(const Unit* Target) : MainTarget(Target) {};

        bool operator()(const Unit* Target)
        {
            return (Target != MainTarget && Target->GetHealth() == Target->GetMaxHealth());
        }
};

// Helper for targets nearest to the spell target
// The spell target is always first unless there is a target at _completely_ the same position (unbelievable case)
struct TargetDistanceOrderNear : public std::binary_function<const Unit, const Unit, bool>
{
    const Unit* MainTarget;
    TargetDistanceOrderNear(const Unit* Target) : MainTarget(Target) {};
    // functor for operator ">"
    bool operator()(const Unit* _Left, const Unit* _Right) const
    {
        return MainTarget->GetDistanceOrder(_Left, _Right);
    }
};

// Helper for targets furthest away to the spell target
// The spell target is always first unless there is a target at _completely_ the same position (unbelievable case)
struct TargetDistanceOrderFarAway : public std::binary_function<const Unit, const Unit, bool>
{
    const Unit* MainTarget;
    TargetDistanceOrderFarAway(const Unit* Target) : MainTarget(Target) {};
    // functor for operator "<"
    bool operator()(const Unit* _Left, const Unit* _Right) const
    {
        return !MainTarget->GetDistanceOrder(_Left, _Right);
    }
};

void Spell::SetTargetMap(SpellEffectIndex effIndex, uint32 targetMode, UnitList& targetUnitMap)
{
    float radius = SpellMgr::GetSpellRadiusWithCustom(m_spellInfo, GetAffectiveUnitCaster(), effIndex);
    uint32 unMaxTargets = SpellMgr::GetSpellMaxTargetsWithCustom(m_spellInfo, GetAffectiveUnitCaster());
    uint32 unEffectChainTarget = SpellMgr::GetSpellTargetsForChainWithCustom(m_spellInfo, GetAffectiveUnitCaster(), effIndex);

    std::list<GameObject*> tempTargetGOList;

    switch (targetMode)
    {
        case TARGET_RANDOM_NEARBY_LOC:
            // special case for Fatal Attraction (BT, Mother Shahraz)
            if (m_spellInfo->Id == 40869)
                radius = 30.0f;

            // Get a random point in circle. Use sqrt(rand) to correct distribution when converting polar to Cartesian coordinates.
            radius *= sqrt(rand_norm_f());
            /* no break expected since we use code in case TARGET_RANDOM_CIRCUMFERENCE_POINT!!!*/
        case TARGET_RANDOM_CIRCUMFERENCE_POINT:
        {
            // Get a random point AT the circumference
            float angle = 2.0f * M_PI_F * rand_norm_f();
            if (m_targets.getUnitTarget() && m_targets.getUnitTarget() != m_caster)
                angle = MapManager::NormalizeOrientation(m_caster->GetAngle(m_targets.getUnitTarget()) - m_caster->GetOrientation());

            m_targets.setDestination(m_caster->GetClosePoint(m_caster->GetObjectBoundingRadius(), radius, angle));

            // This targetMode is often used as 'last' implicitTarget for positive spells, that just require coordinates
            // and no unitTarget (e.g. summon effects). As MaNGOS always needs a unitTarget we add just the caster here.
            // Logic: This is first target, and no second target => use m_caster -- This is second target: use m_caster if the spell is positive or a summon spell
            if ((m_spellInfo->EffectImplicitTargetA[effIndex] == targetMode && m_spellInfo->EffectImplicitTargetB[effIndex] == TARGET_NONE) ||
                (m_spellInfo->EffectImplicitTargetB[effIndex] == targetMode && (IsPositiveSpell(m_spellInfo) || m_spellInfo->Effect[effIndex] == SPELL_EFFECT_SUMMON)))
                targetUnitMap.push_back(m_caster);
            break;
        }
        case TARGET_DEST_RADIUS:
        case TARGET_RANDOM_NEARBY_DEST:
        {
            // Get a random point IN the CIRCEL around current M_TARGETS COORDINATES(!).
            if (radius > 0.0f)
            {
                // Use sqrt(rand) to correct distribution when converting polar to Cartesian coordinates.
                radius *= sqrt(rand_norm_f());
                float angle = 2.0f * M_PI_F * rand_norm_f();
                WorldLocation loc = m_targets.getDestination();
                loc.x += cos(angle) * radius;
                loc.y += sin(angle) * radius;
                loc.z = m_caster->GetPositionZ();
                m_caster->UpdateGroundPositionZ(loc.x, loc.y, loc.z);
                m_targets.setDestination(loc);
            }

            // This targetMode is often used as 'last' implicitTarget for positive spells, that just require coordinates
            // and no unitTarget (e.g. summon effects). As MaNGOS always needs a unitTarget we add just the caster here.
            // Logic: This is first target, and no second target => use m_caster -- This is second target: use m_caster if the spell is positive or a summon spell
            if ((m_spellInfo->EffectImplicitTargetA[effIndex] == targetMode && m_spellInfo->EffectImplicitTargetB[effIndex] == TARGET_NONE) ||
                (m_spellInfo->EffectImplicitTargetB[effIndex] == targetMode && (IsPositiveSpell(m_spellInfo) || m_spellInfo->Effect[effIndex] == SPELL_EFFECT_SUMMON)))
                targetUnitMap.push_back(m_caster);

            // Prevent multiple summons
            if (m_spellInfo->Effect[effIndex] == SPELL_EFFECT_SUMMON || m_spellInfo->Effect[effIndex] == SPELL_EFFECT_SUMMON_OBJECT_WILD)
                unMaxTargets = m_spellInfo->CalculateSimpleValue(effIndex);

            break;
        }
        case TARGET_LEAP_FORWARD:
        {
            Unit* target = m_targets.getUnitTarget();
            if (!target)
                target = m_caster;

            WorldLocation loc  = target->GetPosition();
            WorldLocation dest = target->GetPosition();

            float distance = radius;

            //Glyph of blink or etc this type
            if (m_caster->GetTypeId() == TYPEID_PLAYER)
                ((Player*)m_caster)->ApplySpellMod(m_spellInfo->Id, SPELLMOD_RADIUS, distance);

            dest.x += distance * cos(loc.o);
            dest.y += distance * sin(loc.o);

            MaNGOS::NormalizeMapCoord(dest.x);
            MaNGOS::NormalizeMapCoord(dest.y);

            float fz = std::max(loc.z, m_caster->GetTerrain()->GetWaterOrGroundLevel(dest.x, dest.y, dest.z + 8.0f));

            Map* pMap = target->GetMap();
            if (pMap)
            {
                if (!pMap->GetHitPosition(loc.x, loc.y, loc.z + 2.0f, dest.x, dest.y, dest.z, target->GetPhaseMask(), -0.5f))
                    DEBUG_LOG("Spell::EffectLeap/Teleport unit %u forwarded on %f", target->GetObjectGuid().GetCounter(), target->GetDistance(dest));
                else
                    DEBUG_LOG("Spell::EffectLeap/Teleport unit %u NOT forwarded on %f, real distance is %f", target->GetObjectGuid().GetCounter(), distance, target->GetDistance(dest));
            }

            m_targets.setDestination(dest);
            break;
        }
        case TARGET_RANDOM_POINT_NEAR_TARGET:
        case TARGET_RANDOM_POINT_NEAR_TARGET_2:
        {
            // First effect already may set TARGET_FLAG_DEST_LOCATION - use his for source (and set source point)
            if (m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION)
            {
                m_targets.setSource(m_targets.getDestination());
                WorldLocation loc = m_targets.getDestination();
                m_caster->GetRandomPoint(m_targets.getDestination().getX(), m_targets.getDestination().getY(), m_targets.getDestination().getZ(), radius, loc.x, loc.y, loc.z);
                m_targets.setDestination(loc);
                targetUnitMap.push_back(m_caster);
            }
            else
            {
                Unit* target = m_targets.getUnitTarget();
                if (!target)
                    target = m_caster;
                float angle = 2.0f * M_PI_F * rand_norm_f();
                m_targets.setDestination(target->GetClosePoint(0.0f, radius, angle));
                targetUnitMap.push_back(target);
            }
            break;
        }
        case TARGET_TOTEM_EARTH:
        case TARGET_TOTEM_WATER:
        case TARGET_TOTEM_AIR:
        case TARGET_TOTEM_FIRE:
        {
            float angle = m_caster->GetOrientation();
            switch (targetMode)
            {
                case TARGET_TOTEM_FIRE:  angle += M_PI_F * 0.25f; break;            // front - left
                case TARGET_TOTEM_AIR:   angle += M_PI_F * 0.75f; break;            // back  - left
                case TARGET_TOTEM_WATER: angle += M_PI_F * 1.25f; break;            // back  - right
                case TARGET_TOTEM_EARTH: angle += M_PI_F * 1.75f; break;            // front - right
            }
            WorldLocation loc = m_caster->GetClosePoint(radius > M_NULL_F ? radius - m_caster->GetObjectBoundingRadius() + 0.01f : 2.0f, angle);
            m_caster->UpdateAllowedPositionZ(loc.x, loc.y, loc.z);
            m_targets.setDestination(loc);

            if (radius > M_NULL_F)
            {
                // caster included here?
                FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_ALL);

                if (targetUnitMap.empty())
                    targetUnitMap.push_back(m_caster);
            }
            else if (IsPositiveSpell(m_spellInfo->Id))
                    targetUnitMap.push_back(m_caster);

            if (m_spellInfo->Effect[effIndex] == SPELL_EFFECT_SUMMON || m_spellInfo->Effect[effIndex] == SPELL_EFFECT_SUMMON_OBJECT_WILD)
                unMaxTargets = m_spellInfo->CalculateSimpleValue(effIndex);
            break;
        }
        case TARGET_SELF:
        case TARGET_SELF2:
            targetUnitMap.push_back(m_caster);
            break;
        case TARGET_RANDOM_ENEMY_CHAIN_IN_AREA:
        case TARGET_RANDOM_FRIEND_CHAIN_IN_AREA:
        case TARGET_RANDOM_UNIT_CHAIN_IN_AREA:
        {
            m_targets.m_targetMask = 0;
            unMaxTargets = unEffectChainTarget;
            float maxRange = radius + unMaxTargets * CHAIN_SPELL_JUMP_RADIUS;

            UnitList tempTargetUnitMap;
            switch (targetMode)
            {
                case TARGET_RANDOM_ENEMY_CHAIN_IN_AREA:
                {
                    MaNGOS::AnyAoETargetUnitInObjectRangeCheck u_check(m_caster, maxRange);
                    MaNGOS::UnitListSearcher<MaNGOS::AnyAoETargetUnitInObjectRangeCheck> searcher(tempTargetUnitMap, u_check);
                    Cell::VisitAllObjects(m_caster, searcher, maxRange);
                    break;
                }
                case TARGET_RANDOM_FRIEND_CHAIN_IN_AREA:
                {
                    MaNGOS::AnyFriendlyUnitInObjectRangeCheck u_check(m_caster, maxRange);
                    MaNGOS::UnitListSearcher<MaNGOS::AnyFriendlyUnitInObjectRangeCheck> searcher(tempTargetUnitMap, u_check);
                    Cell::VisitAllObjects(m_caster, searcher, maxRange);
                    break;
                }
                case TARGET_RANDOM_UNIT_CHAIN_IN_AREA:
                {
                    MaNGOS::AnyUnitInObjectRangeCheck u_check(m_caster, maxRange);
                    MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(tempTargetUnitMap, u_check);
                    Cell::VisitAllObjects(m_caster, searcher, maxRange);
                    break;
                }
            }

            uint32 numTargets = tempTargetUnitMap.size();
            if (!numTargets)
                break;

            if (numTargets > 1)
                tempTargetUnitMap.sort(TargetDistanceOrderNear(m_caster));

            // Now to get us a random target that's in the initial range of the spell
            numTargets = 0;
            for (UnitList::const_iterator itr = tempTargetUnitMap.begin(); itr != tempTargetUnitMap.end(); ++itr)
            {
                if ((*itr)->IsWithinDist(m_caster, radius))
                    ++numTargets;
            }

            if (!numTargets)
                break;

            if (numTargets == 1)
            {
                targetUnitMap.push_back(*tempTargetUnitMap.begin());
                break;
            }

            UnitList::iterator itr = tempTargetUnitMap.begin();
            std::advance(itr, urand(0, numTargets - 1));

            Unit* pUnitTarget = *itr;
            targetUnitMap.push_back(pUnitTarget);

            tempTargetUnitMap.erase(itr);
            tempTargetUnitMap.sort(TargetDistanceOrderNear(pUnitTarget));

            numTargets = unMaxTargets - 1;
            bool checkLOS = !m_spellInfo->HasAttribute(SPELL_ATTR_EX2_IGNORE_LOS);
            Unit* prevUnit = pUnitTarget;

            UnitList::iterator nextItr = tempTargetUnitMap.begin();
            while (numTargets && nextItr != tempTargetUnitMap.end())
            {
                if (!prevUnit->IsWithinDist(*nextItr, CHAIN_SPELL_JUMP_RADIUS))
                    break;

                if (checkLOS && !prevUnit->IsWithinLOSInMap(*nextItr))
                {
                    ++nextItr;
                    continue;
                }

                prevUnit = *nextItr;
                targetUnitMap.push_back(prevUnit);

                tempTargetUnitMap.erase(nextItr);
                if (tempTargetUnitMap.size() > 1)
                    tempTargetUnitMap.sort(TargetDistanceOrderNear(prevUnit));

                nextItr = tempTargetUnitMap.begin();
                --numTargets;
            }
            break;
        }
        case TARGET_PET:
        {
            GuidSet const& groupPets = m_caster->GetPets();
            if (!groupPets.empty())
            {
                for (GuidSet::const_iterator itr = groupPets.begin(); itr != groupPets.end(); ++itr)
                    if (Pet* pPet = m_caster->GetMap()->GetPet(*itr))
                        targetUnitMap.push_back(pPet);
            }
            break;
        }
        case TARGET_CHAIN_DAMAGE:
        {
            if (unEffectChainTarget <= 1)
            {
                if (Unit* pUnitTarget = m_caster->SelectMagnetTarget(m_targets.getUnitTarget(), this, effIndex))
                {
                    if (m_targets.getUnitTarget() != pUnitTarget)
                    {
                        m_targets.setUnitTarget(pUnitTarget);
                        m_spellFlags |= SPELL_FLAG_REDIRECTED;
                    }
                    targetUnitMap.push_back(pUnitTarget);
                }
                break;
            }

            Unit* pUnitTarget = m_targets.getUnitTarget();
            WorldObject* originalCaster = GetAffectiveCasterObject();
            if (!pUnitTarget || !originalCaster)
                break;

            unMaxTargets = unEffectChainTarget;

            float maxRange;
            if (m_spellInfo->DmgClass == SPELL_DAMAGE_CLASS_MELEE)
                maxRange = radius;
            else
                // FIXME: This very like horrible hack and wrong for most spells
                maxRange = radius + unMaxTargets * CHAIN_SPELL_JUMP_RADIUS;

            UnitList tempTargetUnitMap;
            {
                MaNGOS::AnyAoEVisibleTargetUnitInObjectRangeCheck u_check(pUnitTarget, originalCaster, maxRange);
                MaNGOS::UnitListSearcher<MaNGOS::AnyAoEVisibleTargetUnitInObjectRangeCheck> searcher(tempTargetUnitMap, u_check);
                Cell::VisitAllObjects(m_caster, searcher, maxRange);
            }

            uint32 numTargets = tempTargetUnitMap.size();
            if (!numTargets)
                break;

            if (numTargets > 1)
                tempTargetUnitMap.sort(TargetDistanceOrderNear(pUnitTarget));

            targetUnitMap.push_back(pUnitTarget);

            if (*tempTargetUnitMap.begin() == pUnitTarget)
            {
                tempTargetUnitMap.erase(tempTargetUnitMap.begin());
                if (tempTargetUnitMap.empty())
                    break;
            }

            numTargets = unMaxTargets - 1;
            bool checkLOS = !m_spellInfo->HasAttribute(SPELL_ATTR_EX2_IGNORE_LOS);
            bool checkCC = m_spellInfo->HasAttribute(SPELL_ATTR_EX6_IGNORE_CCED_TARGETS);
            Unit* prevUnit = pUnitTarget;

            UnitList::iterator nextItr = tempTargetUnitMap.begin();
            while (numTargets && nextItr != tempTargetUnitMap.end())
            {
                if (!prevUnit->IsWithinDist(*nextItr, CHAIN_SPELL_JUMP_RADIUS))
                    break;

                if (prevUnit == (Unit*)m_caster)
                    break;

                if ((checkLOS && !prevUnit->IsWithinLOSInMap(*nextItr)) || (checkCC && !(*nextItr)->CanFreeMove()))
                {
                    ++nextItr;
                    continue;
                }

                prevUnit = *nextItr;
                targetUnitMap.push_back(prevUnit);

                tempTargetUnitMap.erase(nextItr);
                if (tempTargetUnitMap.size() > 1)
                    tempTargetUnitMap.sort(TargetDistanceOrderNear(prevUnit));

                nextItr = tempTargetUnitMap.begin();
                --numTargets;
            }
            break;
        }
        case TARGET_ALL_ENEMY_IN_AREA:
        {
            FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            if (m_spellInfo->Id == 62240 || m_spellInfo->Id == 62920)      // Solar Flare
            {
                if (SpellAuraHolderPtr holder = m_caster->GetSpellAuraHolder(62239))
                    unMaxTargets = holder->GetStackAmount();
                else
                    unMaxTargets = 1;
            }
            else if (m_spellInfo->Id == 42005)                   // Bloodboil (spell hits only the 5 furthest away targets)
            {
                if (targetUnitMap.size() > unMaxTargets)
                {
                    targetUnitMap.sort(TargetDistanceOrderFarAway(m_caster));
                    targetUnitMap.resize(unMaxTargets);
                }
            }
            else
            {
                switch (m_spellInfo->Id)
                {
                    case 30843:                                             // Enfeeble
                    case 31347:                                             // Doom
                    case 37676:                                             // Insidious Whisper
                    case 38028:                                             // Watery Grave
                    case 40618:                                             // Insignificance
                    case 41376:                                             // Spite
                    case 62166:                                             // Stone Grip nh
                    case 63981:                                             // Stone Grip h
                    case 69674:                                             // Mutated Infection (ICC, Rotface)
                    case 71224:                                             // Mutated Infection
                    case 73022:                                             // Mutated Infection
                    case 73023:                                             // Mutated Infection
                        if (Unit* pVictim = m_caster->getVictim())
                            targetUnitMap.remove(pVictim);
                        break;
                    default:
                        break;
                }
            }
            break;
        }
        case TARGET_AREAEFFECT_INSTANT:
        {
            SpellTargets targetB;
            switch (m_spellInfo->Effect[effIndex])
            {
                case SPELL_EFFECT_QUEST_COMPLETE:
                case SPELL_EFFECT_KILL_CREDIT_PERSONAL:
                case SPELL_EFFECT_KILL_CREDIT_GROUP:
                    targetB = SPELL_TARGETS_ALL;
                    break;
                default:
                    // Select friendly targets for positive effect
                    if (IsPositiveEffect(m_spellInfo, effIndex))
                        targetB = SPELL_TARGETS_FRIENDLY;
                    else
                        targetB = SPELL_TARGETS_AOE_DAMAGE;
            }

            UnitList tempTargetUnitMap;
            SQLMultiStorage::SQLMSIteratorBounds<SpellTargetEntry> bounds = sSpellScriptTargetStorage.getBounds<SpellTargetEntry>(m_spellInfo->Id);

            // fill real target list if no spell script target defined
            FillAreaTargets(bounds.first != bounds.second ? tempTargetUnitMap : targetUnitMap,
                radius, PUSH_INHERITED_CENTER, bounds.first != bounds.second ? SPELL_TARGETS_ALL : targetB);

            if (!tempTargetUnitMap.empty())
            {
                for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
                {
                    if ((*iter)->GetTypeId() != TYPEID_UNIT)
                        continue;

                    for (SQLMultiStorage::SQLMultiSIterator<SpellTargetEntry> i_spellST = bounds.first; i_spellST != bounds.second; ++i_spellST)
                    {
                        if (i_spellST->CanNotHitWithSpellEffect(effIndex))
                            continue;

                        // only creature entries supported for this target type
                        if (i_spellST->type == SPELL_TARGET_TYPE_GAMEOBJECT)
                            continue;

                        if ((*iter)->GetEntry() == i_spellST->targetEntry)
                        {
                            if (i_spellST->type == SPELL_TARGET_TYPE_DEAD && ((Creature*)(*iter))->IsCorpse())
                            {
                                targetUnitMap.push_back((*iter));
                            }
                            else if (i_spellST->type == SPELL_TARGET_TYPE_CREATURE && (*iter)->isAlive())
                            {
                                targetUnitMap.push_back((*iter));
                            }

                            break;
                        }
                    }
                }
            }

            // exclude caster
            targetUnitMap.remove(m_caster);
            break;
        }
        case TARGET_AREAEFFECT_CUSTOM:
        {
            if (m_spellInfo->Effect[effIndex] == SPELL_EFFECT_PERSISTENT_AREA_AURA)
                break;
            else if (m_spellInfo->Effect[effIndex] == SPELL_EFFECT_SUMMON)
            {
                targetUnitMap.push_back(m_caster);
                break;
            }

            UnitList tempTargetUnitMap;
            SQLMultiStorage::SQLMSIteratorBounds<SpellTargetEntry> bounds = sSpellScriptTargetStorage.getBounds<SpellTargetEntry>(m_spellInfo->Id);
            // fill real target list if no spell script target defined
            FillAreaTargets(bounds.first != bounds.second ? tempTargetUnitMap : targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_ALL);

            if (!tempTargetUnitMap.empty())
            {
                for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
                {
                    if ((*iter)->GetTypeId() != TYPEID_UNIT)
                        continue;

                    for (SQLMultiStorage::SQLMultiSIterator<SpellTargetEntry> i_spellST = bounds.first; i_spellST != bounds.second; ++i_spellST)
                    {
                        if (i_spellST->CanNotHitWithSpellEffect(effIndex))
                            continue;

                        // only creature entries supported for this target type
                        if (i_spellST->type == SPELL_TARGET_TYPE_GAMEOBJECT)
                            continue;

                        if ((*iter)->GetEntry() == i_spellST->targetEntry)
                        {
                            if (i_spellST->type == SPELL_TARGET_TYPE_DEAD && ((Creature*)(*iter))->IsCorpse())
                            {
                                targetUnitMap.push_back((*iter));
                            }
                            else if (i_spellST->type == SPELL_TARGET_TYPE_CREATURE && (*iter)->isAlive())
                            {
                                targetUnitMap.push_back((*iter));
                            }

                            break;
                        }
                    }
                }
            }
            else
            {
                // remove not targetable units if spell has no script targets
                for (UnitList::iterator itr = targetUnitMap.begin(); itr != targetUnitMap.end(); )
                {
                    if (!(*itr)->isTargetableForAttack(m_spellInfo->HasAttribute(SPELL_ATTR_EX3_CAST_ON_DEAD)))
                        targetUnitMap.erase(itr++);
                    else
                        ++itr;
                }
            }
            break;
        }
        case TARGET_GO_IN_FRONT_OF_CASTER_90:
        case TARGET_AREAEFFECT_GO_AROUND_SOURCE:
        case TARGET_AREAEFFECT_GO_AROUND_DEST:
        {
            WorldLocation loc;
            if (targetMode == TARGET_AREAEFFECT_GO_AROUND_SOURCE)
            {
                if (m_targets.m_targetMask & TARGET_FLAG_SOURCE_LOCATION)
                    loc = m_targets.getSource();
                else
                    loc = m_caster->GetPosition();
            }
            else if (m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION)
            {
                loc = m_targets.getDestination();
            }
            else
                loc = m_caster->GetPosition();

            // It may be possible to fill targets for some spell effects
            // automatically (SPELL_EFFECT_WMO_REPAIR(88) for example) but
            // for some/most spells we clearly need/want to limit with spell_target_script
            // Some spells untested, for affected GO type 33. May need further adjustments for spells related.

            std::list<GameObject*> tempTargetGOList;
            float fSearchDistance = GetSpellMaxRange(sSpellRangeStore.LookupEntry(m_spellInfo->GetRangeIndex()));
            SQLMultiStorage::SQLMSIteratorBounds<SpellTargetEntry> bounds = sSpellScriptTargetStorage.getBounds<SpellTargetEntry>(m_spellInfo->Id);
            if (bounds.first !=  bounds.second)
            {
                for (SQLMultiStorage::SQLMultiSIterator<SpellTargetEntry> i_spellST = bounds.first; i_spellST != bounds.second; ++i_spellST)
                {
                    if (i_spellST->CanNotHitWithSpellEffect(effIndex))
                        continue;

                    if (i_spellST->type == SPELL_TARGET_TYPE_GAMEOBJECT)
                    {
                        // search all GO's with entry, within range of m_destN
                        MaNGOS::GameObjectEntryInPosRangeCheck go_check(*m_caster, i_spellST->targetEntry, loc.x, loc.y, loc.z, radius);
                        MaNGOS::GameObjectListSearcher<MaNGOS::GameObjectEntryInPosRangeCheck> checker(tempTargetGOList, go_check);
                        Cell::VisitGridObjects(m_caster, checker, radius + fSearchDistance);
                    }
                }
            }
            else
            {
                MaNGOS::GameObjectInRangeCheck check(m_caster, loc.x, loc.y, loc.z, radius);
                MaNGOS::GameObjectListSearcher<MaNGOS::GameObjectInRangeCheck> searcher(tempTargetGOList, check);
                Cell::VisitAllObjects(m_caster, searcher, radius + fSearchDistance );
            }

            if (!tempTargetGOList.empty())
            {
                for (std::list<GameObject*>::iterator iter = tempTargetGOList.begin(); iter != tempTargetGOList.end(); ++iter)
                {
                    switch (targetMode)
                    {
                        case TARGET_GO_IN_FRONT_OF_CASTER_90:
                            if (m_caster->isInFront(*iter, radius, M_PI_F/2))
                                AddGOTarget(*iter, effIndex);
                            break;
                        case TARGET_AREAEFFECT_GO_AROUND_SOURCE:
                        case TARGET_AREAEFFECT_GO_AROUND_DEST:
                        default:
                            AddGOTarget(*iter, effIndex);
                            break;
                    }
                }
            }
            break;
        }
        case TARGET_ALL_ENEMY_IN_AREA_INSTANT:
        {
            // targets the ground, not the units in the area
            switch(m_spellInfo->Effect[effIndex])
            {
                case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                    break;
                case SPELL_EFFECT_SUMMON:
                    targetUnitMap.push_back(m_caster);
                    break;
                default:
                    FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
                    break;
            }
            break;
        }
        case TARGET_DUELVSPLAYER_COORDINATES:
        {
            if (Unit* currentTarget = m_targets.getUnitTarget())
                m_targets.setDestination(currentTarget->GetPosition());
            break;
        }
        case TARGET_ALL_PARTY_AROUND_CASTER:
        {
            if (m_caster->GetObjectGuid().IsPet())
            {
                // only affect pet and owner
                targetUnitMap.push_back(m_caster);
                if (Unit* owner = m_caster->GetOwner())
                    targetUnitMap.push_back(owner);
            }
            else
            {
                FillRaidOrPartyTargets(targetUnitMap, m_caster, m_caster, radius, false, true, true);
            }
            break;
        }
        case TARGET_ALL_PARTY_AROUND_CASTER_2:
        case TARGET_ALL_PARTY:
        {
            FillRaidOrPartyTargets(targetUnitMap, m_caster, m_caster, radius, false, true, true);
            break;
        }
        case TARGET_ALL_RAID_AROUND_CASTER:
        {
            if (m_spellInfo->Id == 57669)                    // Replenishment (special target selection)
            {
                // in arena, target should be only caster
                if (m_caster->GetMap()->IsBattleArena())
                    targetUnitMap.push_back(m_caster);
                else
                    FillRaidOrPartyManaPriorityTargets(targetUnitMap, m_caster, m_caster, radius, 10, true, false, true);
            }
            else if (m_spellInfo->Id==52759)                // Ancestral Awakening (special target selection)
                FillRaidOrPartyHealthPriorityTargets(targetUnitMap, m_caster, m_caster, radius, 1, true, false, true);
            else if (m_spellInfo->Id == 54171)              // Divine Storm
                FillRaidOrPartyHealthPriorityTargets(targetUnitMap, m_caster, m_caster, radius, 3, true, false, true);
            else if (m_spellInfo->Id == 59725)              // Improved Spell Reflection
            {
                if (m_caster->HasAura(23920, EFFECT_INDEX_0) )
                    m_caster->RemoveAurasDueToSpell(23920); // will be replaced by imp. spell refl. aura

                Unit::AuraList const& lDummyAuras = m_caster->GetAurasByType(SPELL_AURA_DUMMY);
                for(Unit::AuraList::const_iterator i = lDummyAuras.begin(); i != lDummyAuras.end(); ++i)
                {
                    if((*i)->GetSpellProto()->GetSpellIconID() == 1935)
                    {
                        unMaxTargets = (*i)->GetModifier()->m_amount + 1;   // +1 because we are also applying this to the caster
                        break;
                    }
                }

                radius = 20.0f;     // as mentioned in the spell's tooltip (data doesn't appear in dbc)

                FillRaidOrPartyTargets(targetUnitMap, m_caster, m_caster, radius, false, false, true);
                targetUnitMap.sort(TargetDistanceOrderNear(m_caster));
                if (targetUnitMap.size() > unMaxTargets)
                    targetUnitMap.resize(unMaxTargets);
                break;
            }
            else
                FillRaidOrPartyTargets(targetUnitMap, m_caster, m_caster, radius, true, true, IsPositiveSpell(m_spellInfo->Id));
            break;
        }
        case TARGET_SINGLE_FRIEND:
        case TARGET_SINGLE_FRIEND_2:
            if (m_targets.getUnitTarget())
                targetUnitMap.push_back(m_targets.getUnitTarget());
            else if (IsSpellWithCasterSourceTargetsOnly(m_spellInfo))
                targetUnitMap.push_back(GetCaster());
            break;
        case TARGET_NONCOMBAT_PET:
            if (Unit* target = m_targets.getUnitTarget())
                if ( target->GetTypeId() == TYPEID_UNIT && ((Creature*)target)->IsPet() && ((Pet*)target)->getPetType() == MINI_PET)
                    targetUnitMap.push_back(target);
            break;
        case TARGET_UNIT_CREATOR:
        {
            WorldObject* caster = GetAffectiveCasterObject();
            if (!caster)
                return;

            if (caster->GetTypeId() == TYPEID_UNIT && ((Creature*)caster)->IsTemporarySummon())
                targetUnitMap.push_back(((TemporarySummon*)(Creature*)caster)->GetSummoner());
            else if (caster->GetTypeId() == TYPEID_GAMEOBJECT && !((GameObject*)caster)->HasStaticDBSpawnData())
                targetUnitMap.push_back(((GameObject*)caster)->GetOwner());
            else if (Unit* target = m_caster->GetCreator())
                targetUnitMap.push_back(target);
             else
                sLog.outError("Spell::SetTargetMap: Spell ID %u with target ID %u was used by unhandled object %s.", m_spellInfo->Id, targetMode, caster->GetGuidStr().c_str());
            break;
        }
        case TARGET_OWNED_VEHICLE:
            if (VehicleKitPtr vehicle = m_caster->GetVehicle())
                if (Unit* target = vehicle->GetBase())
                    targetUnitMap.push_back(target);
            break;
        case TARGET_UNIT_PASSENGER_0:
        case TARGET_UNIT_PASSENGER_1:
        case TARGET_UNIT_PASSENGER_2:
        case TARGET_UNIT_PASSENGER_3:
        case TARGET_UNIT_PASSENGER_4:
        case TARGET_UNIT_PASSENGER_5:
        case TARGET_UNIT_PASSENGER_6:
        case TARGET_UNIT_PASSENGER_7:
            if (m_caster->GetTypeId() == TYPEID_UNIT && m_caster->IsVehicle())
                if (Unit* unit = m_caster->GetVehicleKit()->GetPassenger(targetMode - TARGET_UNIT_PASSENGER_0))
                    targetUnitMap.push_back(unit);
            break;
        case TARGET_CASTER_COORDINATES:
        {
            // Check original caster is GO - set its coordinates as src cast
            if (WorldObject* caster = GetCastingObject())
                m_targets.setSource(caster->GetPosition());
            break;
        }
        case TARGET_ALL_HOSTILE_UNITS_AROUND_CASTER:
            FillAreaTargets(targetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_HOSTILE);
            break;
        case TARGET_ALL_FRIENDLY_UNITS_AROUND_CASTER:
            switch (m_spellInfo->Id)
            {
                case 54171:                                 // Divine Storm
                    FillRaidOrPartyHealthPriorityTargets(targetUnitMap, m_caster, m_caster, radius, 3, true, false, true);
                    break;
                case 56153:                                 // Guardian Aura - Ahn'Kahet
                case 50811:                                 // Shatter Dmg N - Krystallus
                case 61547:                                 // Shatter Dmg H - Krystallus
                    FillAreaTargets(targetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_FRIENDLY);
                    targetUnitMap.remove(m_caster);
                    break;
                case 63881:                                 // Malady of the Mind   Ulduar Yogg Saron
                case 70920:                                 // Unbound Plague Search Effect (ICC -Putricide)
                    FillAreaTargets(targetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_FRIENDLY, GetCastingObject());
                    targetUnitMap.remove(m_caster);
                    break;
                case 64844:                                 // Divine Hymn
                    // target amount stored in parent spell dummy effect but hard to access
                    FillRaidOrPartyHealthPriorityTargets(targetUnitMap, m_caster, m_caster, radius, 3, true, false, true);
                    break;
                case 64904:                                 // Hymn of Hope
                    // target amount stored in parent spell dummy effect but hard to access
                    FillRaidOrPartyManaPriorityTargets(targetUnitMap, m_caster, m_caster, radius, 3, true, false, true);
                    break;
                default:
                    // selected friendly units (for casting objects) around casting object
                    FillAreaTargets(targetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_FRIENDLY, GetCastingObject());
                    break;
            }
            break;
        case TARGET_ALL_FRIENDLY_UNITS_IN_AREA:
            // Circle of Healing
            if (m_spellInfo->SpellFamilyName == SPELLFAMILY_PRIEST && m_spellInfo->GetSpellVisual() == 8253)
            {
                Unit* target = m_targets.getUnitTarget();
                if(!target)
                    target = m_caster;

                uint32 count = 5;
                // Glyph of Circle of Healing
                if (Aura const* glyph = m_caster->GetDummyAura(55675))
                    count += glyph->GetModifier()->m_amount;

                FillRaidOrPartyHealthPriorityTargets(targetUnitMap, m_caster, target, radius, count, true, false, true);
            }
            // Wild Growth
            else if (m_spellInfo->SpellFamilyName == SPELLFAMILY_DRUID && m_spellInfo->GetSpellIconID() == 2864)
            {
                Unit* target = m_targets.getUnitTarget();
                if(!target)
                    target = m_caster;
                uint32 count = CalculateDamage(EFFECT_INDEX_2,m_caster); // stored in dummy effect, affected by mods

                FillRaidOrPartyHealthPriorityTargets(targetUnitMap, m_caster, target, radius, count, true, false, true);
            }
            // Item - Icecrown 25 Heroic/Normal Healer Trinket 2
            else if (m_spellInfo->Id == 71641 || m_spellInfo->Id == 71610)
            {
                FillRaidOrPartyHealthPriorityTargets(targetUnitMap, m_caster, m_caster, radius, 1, true, false, false);
            }
            else
                FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_FRIENDLY);
            break;
        // TARGET_SINGLE_PARTY means that the spells can only be casted on a party member and not on the caster (some seals, fire shield from imp, etc..)
        case TARGET_SINGLE_PARTY:
        {
            Unit *target = m_targets.getUnitTarget();
            // Those spells apparently can't be casted on the caster.
            if ( target && target != m_caster)
            {
                // Can only be casted on group's members or its pets
                Group  *pGroup = NULL;

                Unit* owner = m_caster->GetCharmerOrOwner();
                Unit *targetOwner = target->GetCharmerOrOwner();
                if (owner)
                {
                    if (owner->GetTypeId() == TYPEID_PLAYER)
                    {
                        if ( target == owner )
                        {
                            targetUnitMap.push_back(target);
                            break;
                        }
                        pGroup = ((Player*)owner)->GetGroup();
                    }
                }
                else if (m_caster->GetTypeId() == TYPEID_PLAYER)
                {
                    if ( targetOwner == m_caster && target->GetTypeId() == TYPEID_UNIT && ((Creature*)target)->IsPet())
                    {
                        targetUnitMap.push_back(target);
                        break;
                    }
                    pGroup = ((Player*)m_caster)->GetGroup();
                }

                if (pGroup)
                {
                    // Our target can also be a player's pet who's grouped with us or our pet. But can't be controlled player
                    if (targetOwner)
                    {
                        if ( targetOwner->GetTypeId() == TYPEID_PLAYER &&
                            target->GetTypeId() == TYPEID_UNIT && (((Creature*)target)->IsPet()) &&
                            target->GetOwnerGuid() == targetOwner->GetObjectGuid() &&
                            pGroup->IsMember(((Player*)targetOwner)->GetObjectGuid()))
                        {
                            targetUnitMap.push_back(target);
                        }
                    }
                    // 1Our target can be a player who is on our group
                    else if (target->GetTypeId() == TYPEID_PLAYER && pGroup->IsMember(((Player*)target)->GetObjectGuid()))
                    {
                        targetUnitMap.push_back(target);
                    }
                }
            }
            break;
        }
        case TARGET_GAMEOBJECT:
            if (m_targets.getGOTarget())
                AddGOTarget(m_targets.getGOTarget(), effIndex);
            break;
        case TARGET_IN_FRONT_OF_CASTER:
        {
            SpellNotifyPushType pushType = PUSH_IN_FRONT;
            switch (m_spellInfo->GetSpellVisual())            // Some spell require a different target fill
            {
                case 3879: pushType = PUSH_IN_BACK;     break;
                case 7441: pushType = PUSH_IN_FRONT_15; break;
                case 8669: pushType = PUSH_IN_FRONT_15; break;
            }
            FillAreaTargets(targetUnitMap, radius, pushType, SPELL_TARGETS_AOE_DAMAGE);
            break;
        }
        case TARGET_LARGE_FRONTAL_CONE:
            FillAreaTargets(targetUnitMap, radius, PUSH_IN_FRONT_90, SPELL_TARGETS_AOE_DAMAGE);
            break;
        case TARGET_FRIENDLY_FRONTAL_CONE:
            FillAreaTargets(targetUnitMap, radius, PUSH_IN_FRONT_30, SPELL_TARGETS_FRIENDLY);
            break;
        case TARGET_NARROW_FRONTAL_CONE:
            FillAreaTargets(targetUnitMap, radius, PUSH_IN_FRONT_15, SPELL_TARGETS_AOE_DAMAGE);
            break;
        case TARGET_IN_FRONT_OF_CASTER_30:
            // Incorrect name for target. Must be _FRONTAL_CONE_90
            FillAreaTargets(targetUnitMap, radius, PUSH_IN_FRONT_90, SPELL_TARGETS_AOE_DAMAGE);
            break;
        case TARGET_DUELVSPLAYER:
        {
            Unit *target = m_targets.getUnitTarget();
            if (target)
            {
                if (m_caster->IsFriendlyTo(target))
                {
                    targetUnitMap.push_back(target);
                }
                else
                {
                    if (Unit* pUnitTarget = m_caster->SelectMagnetTarget(m_targets.getUnitTarget(), this, effIndex))
                    {
                        if (m_targets.getUnitTarget() != pUnitTarget)
                        {
                            m_targets.setUnitTarget(pUnitTarget);
                            m_spellFlags |= SPELL_FLAG_REDIRECTED;
                        }
                        targetUnitMap.push_back(pUnitTarget);
                    }
                }
            }
            break;
        }
        case TARGET_GAMEOBJECT_ITEM:
            if (m_targets.getGOTargetGuid())
                AddGOTarget(m_targets.getGOTarget(), effIndex);
            else if (m_targets.getItemTarget())
                AddItemTarget(m_targets.getItemTarget(), effIndex);
            break;
        case TARGET_MASTER:
            if (Unit* owner = m_caster->GetCharmerOrOwner())
                targetUnitMap.push_back(owner);
            break;
        case TARGET_ALL_ENEMY_IN_AREA_CHANNELED:
            // targets the ground, not the units in the area
            if (m_spellInfo->Effect[effIndex]!=SPELL_EFFECT_PERSISTENT_AREA_AURA)
                FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            break;
        case TARGET_MINION:
            if (m_spellInfo->Effect[effIndex] != SPELL_EFFECT_DUEL)
                targetUnitMap.push_back(m_caster);
            break;
        case TARGET_SINGLE_ENEMY_2:
        case TARGET_SINGLE_ENEMY:
        {
            if (Unit* pUnitTarget = m_caster->SelectMagnetTarget(m_targets.getUnitTarget(), this, effIndex))
            {
                if (m_targets.getUnitTarget() != pUnitTarget)
                {
                    m_targets.setUnitTarget(pUnitTarget);
                    m_spellFlags |= SPELL_FLAG_REDIRECTED;
                }
                targetUnitMap.push_back(pUnitTarget);
            }
            break;
        }
        case TARGET_AREAEFFECT_PARTY:
        {
            Unit* owner = m_caster->GetCharmerOrOwner();
            Player *pTarget = NULL;

            if (owner)
            {
                targetUnitMap.push_back(m_caster);
                if (owner->GetTypeId() == TYPEID_PLAYER)
                    pTarget = (Player*)owner;
            }
            else if (m_caster->GetTypeId() == TYPEID_PLAYER)
            {
                if (Unit* target = m_targets.getUnitTarget())
                {
                    if ( target->GetTypeId() != TYPEID_PLAYER)
                    {
                        if(((Creature*)target)->IsPet())
                        {
                            Unit *targetOwner = target->GetOwner();
                            if (targetOwner->GetTypeId() == TYPEID_PLAYER)
                                pTarget = (Player*)targetOwner;
                        }
                    }
                    else
                        pTarget = (Player*)target;
                }
            }

            Group* pGroup = pTarget ? pTarget->GetGroup() : NULL;
            if (pGroup)
            {
                uint8 subgroup = pTarget->GetSubGroup();

                for (GroupReference* itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
                {
                    Player* Target = itr->getSource();

                    // IsHostileTo check duel and controlled by enemy
                    if (Target && Target->GetSubGroup() == subgroup && !m_caster->IsHostileTo(Target))
                    {
                        if (pTarget->IsWithinDistInMap(Target, radius))
                            targetUnitMap.push_back(Target);

                        GuidSet const& groupPets = Target->GetPets();
                        if (!groupPets.empty())
                        {
                            for (GuidSet::const_iterator itr = groupPets.begin(); itr != groupPets.end(); ++itr)
                                if (Pet* pPet = Target->GetMap()->GetPet(*itr))
                                    if (pTarget->IsWithinDistInMap(pPet, radius))
                                        targetUnitMap.push_back(pPet);
                        }
                    }
                }
            }
            else if (owner)
            {
                if (m_caster->IsWithinDistInMap(owner, radius))
                    targetUnitMap.push_back(owner);
            }
            else if (pTarget)
            {
                targetUnitMap.push_back(pTarget);

                if (Pet* pet = pTarget->GetPet())
                    if (m_caster->IsWithinDistInMap(pet, radius))
                        targetUnitMap.push_back(pet);
            }
            break;
        }
        case TARGET_SCRIPT:
        {
            if (m_targets.getUnitTarget())
                targetUnitMap.push_back(m_targets.getUnitTarget());
            if (m_targets.getItemTarget())
                AddItemTarget(m_targets.getItemTarget(), effIndex);
            break;
        }
        case TARGET_SELF_FISHING:
            targetUnitMap.push_back(m_caster);
            break;
        case TARGET_CHAIN_HEAL:
        {
            Unit* pUnitTarget = m_targets.getUnitTarget();
            if (!pUnitTarget)
                break;

            if (unEffectChainTarget <= 1)
            {
                targetUnitMap.push_back(pUnitTarget);
                break;
            }

            unMaxTargets = unEffectChainTarget;
            float maxRange = radius + unMaxTargets * CHAIN_SPELL_JUMP_RADIUS;

            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, maxRange, PUSH_SELF_CENTER, SPELL_TARGETS_FRIENDLY);

            if (m_caster != pUnitTarget && std::find(tempTargetUnitMap.begin(), tempTargetUnitMap.end(), m_caster) == tempTargetUnitMap.end())
                tempTargetUnitMap.push_front(m_caster);

            uint32 numTargets = tempTargetUnitMap.size();
            if (!numTargets)
                break;

            if (numTargets > 1)
                tempTargetUnitMap.sort(TargetDistanceOrderNear(pUnitTarget));

            targetUnitMap.push_back(pUnitTarget);

            if (*tempTargetUnitMap.begin() == pUnitTarget)
            {
                tempTargetUnitMap.erase(tempTargetUnitMap.begin());
                if (tempTargetUnitMap.empty())
                    break;
            }

            numTargets = unMaxTargets - 1;
            bool checkLOS = !m_spellInfo->HasAttribute(SPELL_ATTR_EX2_IGNORE_LOS);
            Unit* prevUnit = pUnitTarget;

            UnitList::iterator nextItr = tempTargetUnitMap.begin();
            while (numTargets && nextItr != tempTargetUnitMap.end())
            {
                if (!prevUnit->IsWithinDist(*nextItr, CHAIN_SPELL_JUMP_RADIUS))
                    break;

                if (checkLOS && !prevUnit->IsWithinLOSInMap(*nextItr))
                {
                    ++nextItr;
                    continue;
                }

                if ((*nextItr)->GetHealth() == (*nextItr)->GetMaxHealth())
                {
                    nextItr = tempTargetUnitMap.erase(nextItr);
                    continue;
                }

                prevUnit = *nextItr;
                targetUnitMap.push_back(prevUnit);

                tempTargetUnitMap.erase(nextItr);
                if (tempTargetUnitMap.size() > 1)
                    tempTargetUnitMap.sort(TargetDistanceOrderNear(prevUnit));

                nextItr = tempTargetUnitMap.begin();
                --numTargets;
            }
            break;
        }
        case TARGET_CURRENT_ENEMY_COORDINATES:
        {
            Unit* currentTarget = m_targets.getUnitTarget();
            if (currentTarget)
            {
                targetUnitMap.push_back(currentTarget);
                m_targets.setDestination(currentTarget->GetPosition());
            }
            break;
        }
        case TARGET_AREAEFFECT_PARTY_AND_CLASS:
        {
            Player* targetPlayer = m_targets.getUnitTarget() && m_targets.getUnitTarget()->GetTypeId() == TYPEID_PLAYER
                ? (Player*)m_targets.getUnitTarget() : NULL;

            Group* pGroup = targetPlayer ? targetPlayer->GetGroup() : NULL;
            if (pGroup)
            {
                for(GroupReference *itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
                {
                    Player* Target = itr->getSource();

                    // IsHostileTo check duel and controlled by enemy
                    if ( Target && targetPlayer->IsWithinDistInMap(Target, radius) &&
                        targetPlayer->getClass() == Target->getClass() &&
                        !m_caster->IsHostileTo(Target) )
                    {
                        targetUnitMap.push_back(Target);
                    }
                }
            }
            else if (m_targets.getUnitTarget())
                targetUnitMap.push_back(m_targets.getUnitTarget());
            break;
        }
        case TARGET_TABLE_X_Y_Z_COORDINATES:
        {
            WorldLocation const* st = sSpellMgr.GetSpellTargetPosition(m_spellInfo->Id);
            if (st)
            {
                m_targets.setDestination(*st);
                // TODO - maybe use an (internal) value for the map for neat far teleport handling

                // far-teleport spells are handled in SpellEffect, elsewise report an error about an unexpected map (spells are always locally)
                if (st->GetMapId() != m_caster->GetMapId() && m_spellInfo->Effect[effIndex] != SPELL_EFFECT_TELEPORT_UNITS && m_spellInfo->Effect[effIndex] != SPELL_EFFECT_BIND)
                    sLog.outError( "SPELL: wrong map (%u instead %u) target coordinates for spell ID %u", st->GetMapId(), m_caster->GetMapId(), m_spellInfo->Id);
            }
            else
                sLog.outError("SPELL: unknown target coordinates for spell ID %u", m_spellInfo->Id);
            break;
        }
        case TARGET_INFRONT_OF_VICTIM:
        case TARGET_BEHIND_VICTIM:
        case TARGET_RIGHT_FROM_VICTIM:
        case TARGET_LEFT_FROM_VICTIM:
        {
            Unit* pTarget = NULL;

            // explicit cast data from client or server-side cast
            // some spell at client send caster
            if (m_targets.getUnitTarget() && m_targets.getUnitTarget() != m_caster)
                pTarget = m_targets.getUnitTarget();
            else if (Unit* pVictim = m_caster->getVictim())
                pTarget = pVictim;
            else if (m_caster->GetTypeId() == TYPEID_PLAYER)
                pTarget = ObjectAccessor::GetUnit(*m_caster, ((Player*)m_caster)->GetSelectionGuid());
            else if (m_targets.getUnitTarget())
                pTarget = m_caster;

            if (pTarget)
            {
                float angle = 0.0f;

                switch (targetMode)
                {
                    case TARGET_INFRONT_OF_VICTIM:                        break;
                    case TARGET_BEHIND_VICTIM:      angle = M_PI_F;       break;
                    case TARGET_RIGHT_FROM_VICTIM:  angle = -M_PI_F / 2;  break;
                    case TARGET_LEFT_FROM_VICTIM:   angle = M_PI_F / 2;   break;
                }

                WorldLocation loc = pTarget->GetClosePoint(pTarget->GetObjectBoundingRadius(), radius, angle, m_caster);
                if (pTarget->IsWithinLOS(loc.x, loc.y, loc.z))
                {
                    targetUnitMap.push_back(m_caster);
                    m_targets.setDestination(loc);
                }
            }
            break;
        }
        case TARGET_DYNAMIC_OBJECT_COORDINATES:
            // if parent spell create dynamic object extract area from it
            if (DynamicObject* dynObj = m_caster->GetDynObject(m_triggeredByAuraSpell ? m_triggeredByAuraSpell->Id : m_spellInfo->Id))
                m_targets.setDestination(dynObj->GetPosition());
            // else use destination of target if no destination set (ie for Mind Sear - 53022)
            else if (!(m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION) && (m_targets.m_targetMask & TARGET_FLAG_UNIT))
                m_targets.setDestination(m_targets.getDestination());
            break;
        case TARGET_DYNAMIC_OBJECT_FRONT:
        case TARGET_DYNAMIC_OBJECT_BEHIND:
        case TARGET_DYNAMIC_OBJECT_LEFT_SIDE:
        case TARGET_DYNAMIC_OBJECT_RIGHT_SIDE:
        {
            if (!(m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION))
            {
                // General override, we don't want to use max spell range here.
                // Note: 0.0 radius is also for index 36. It is possible that 36 must be defined as
                // "at the base of", in difference to 0 which appear to be "directly in front of".
                // TODO: some summoned will make caster be half inside summoned object. Need to fix
                // that in the below code (nearpoint vs closepoint, etc).
                if (m_spellInfo->EffectRadiusIndex[effIndex] == 0)
                    radius = 0.0f;

                if (m_spellInfo->Id == 50019)               // Hawk Hunting, problematic 50K radius
                    radius = 10.0f;

                float angle = m_caster->GetOrientation();
                switch(targetMode)
                {
                    case TARGET_DYNAMIC_OBJECT_FRONT:                           break;
                    case TARGET_DYNAMIC_OBJECT_BEHIND:      angle += M_PI_F;      break;
                    case TARGET_DYNAMIC_OBJECT_LEFT_SIDE:   angle += M_PI_F / 2;  break;
                    case TARGET_DYNAMIC_OBJECT_RIGHT_SIDE:  angle -= M_PI_F / 2;  break;
                }

                m_targets.setDestination(m_caster->GetClosePoint(m_caster->GetObjectBoundingRadius(), radius, angle));
            }

            targetUnitMap.push_back(m_caster);
            break;
        }
        case TARGET_POINT_AT_NORTH:
        case TARGET_POINT_AT_SOUTH:
        case TARGET_POINT_AT_EAST:
        case TARGET_POINT_AT_WEST:
        case TARGET_POINT_AT_NE:
        case TARGET_POINT_AT_NW:
        case TARGET_POINT_AT_SE:
        case TARGET_POINT_AT_SW:
        {

            if (!(m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION))
            {
                Unit* currentTarget = m_targets.getUnitTarget() ? m_targets.getUnitTarget() : m_caster;
                float angle = currentTarget != m_caster ? currentTarget->GetAngle(m_caster) : m_caster->GetOrientation();

                switch(targetMode)
                {
                    case TARGET_POINT_AT_NORTH:                         break;
                    case TARGET_POINT_AT_SOUTH: angle +=   M_PI_F;        break;
                    case TARGET_POINT_AT_EAST:  angle -=   M_PI_F / 2;    break;
                    case TARGET_POINT_AT_WEST:  angle +=   M_PI_F / 2;    break;
                    case TARGET_POINT_AT_NE:    angle -=   M_PI_F / 4;    break;
                    case TARGET_POINT_AT_NW:    angle +=   M_PI_F / 4;    break;
                    case TARGET_POINT_AT_SE:    angle -= 3*M_PI_F / 4;    break;
                    case TARGET_POINT_AT_SW:    angle += 3*M_PI_F / 4;    break;
                }

                m_targets.setDestination(currentTarget->GetClosePoint(m_caster->GetObjectBoundingRadius(), radius, angle));
            }
            break;
        }
        case TARGET_DIRECTLY_FORWARD:
        {
            if (!(m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION))
            {
                SpellRangeEntry const* rEntry = sSpellRangeStore.LookupEntry(m_spellInfo->GetRangeIndex());
                float minRange = GetSpellMinRange(rEntry);
                float maxRange = GetSpellMaxRange(rEntry);
                float dist = minRange+ rand_norm_f()*(maxRange-minRange);
                m_targets.setDestination(m_caster->GetClosePoint(m_caster->GetObjectBoundingRadius(), dist));
            }

            targetUnitMap.push_back(m_caster);
            break;
        }
        case TARGET_EFFECT_SELECT:
        {
            // add here custom effects that need default target.
            // FOR EVERY TARGET TYPE THERE IS A DIFFERENT FILL!!
            switch(m_spellInfo->Effect[effIndex])
            {
                case SPELL_EFFECT_DUMMY:
                {
                    if (m_targets.getUnitTarget())
                        targetUnitMap.push_back(m_targets.getUnitTarget());

                    // Add AoE target-mask to self, if no target-dest provided already
                    if (!(m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION))
                        m_targets.setDestination(m_caster->GetPosition());
                    break;
                }
                case SPELL_EFFECT_BIND:
                case SPELL_EFFECT_RESURRECT:
                case SPELL_EFFECT_PARRY:
                case SPELL_EFFECT_BLOCK:
                case SPELL_EFFECT_CREATE_ITEM:
                case SPELL_EFFECT_WEAPON:
                case SPELL_EFFECT_TRIGGER_SPELL:
                case SPELL_EFFECT_TRIGGER_MISSILE:
                case SPELL_EFFECT_LEARN_SPELL:
                case SPELL_EFFECT_SKILL_STEP:
                case SPELL_EFFECT_PROFICIENCY:
                case SPELL_EFFECT_SUMMON_OBJECT_WILD:
                case SPELL_EFFECT_SELF_RESURRECT:
                case SPELL_EFFECT_REPUTATION:
                case SPELL_EFFECT_SEND_TAXI:
                    if (m_targets.getUnitTarget())
                        targetUnitMap.push_back(m_targets.getUnitTarget());
                    // Triggered spells have additional spell targets - cast them even if no explicit unit target is given (required for spell 50516 for example)
                    else if (m_spellInfo->Effect[effIndex] == SPELL_EFFECT_TRIGGER_SPELL)
                        targetUnitMap.push_back(m_caster);
                    break;
                case SPELL_EFFECT_FRIEND_SUMMON:
                case SPELL_EFFECT_SUMMON_PLAYER:
                    if (m_caster->GetTypeId()==TYPEID_PLAYER && ((Player*)m_caster)->GetSelectionGuid())
                    {
                        Player* target = sObjectMgr.GetPlayer(((Player*)m_caster)->GetSelectionGuid());
                        if (target)
                            targetUnitMap.push_back(target);
                    }
                    break;
                case SPELL_EFFECT_RESURRECT_NEW:
                    if (m_targets.getUnitTarget())
                        targetUnitMap.push_back(m_targets.getUnitTarget());
                    if (m_targets.getCorpseTargetGuid())
                    {
                        if (Corpse *corpse = m_caster->GetMap()->GetCorpse(m_targets.getCorpseTargetGuid()))
                            if (Player* owner = ObjectAccessor::FindPlayer(corpse->GetOwnerGuid()))
                                targetUnitMap.push_back(owner);
                    }
                    break;
                case SPELL_EFFECT_TELEPORT_UNITS:
                case SPELL_EFFECT_SUMMON:
                case SPELL_EFFECT_SUMMON_CHANGE_ITEM:
                case SPELL_EFFECT_TRANS_DOOR:
                case SPELL_EFFECT_ADD_FARSIGHT:
                case SPELL_EFFECT_APPLY_GLYPH:
                case SPELL_EFFECT_STUCK:
                case SPELL_EFFECT_BREAK_PLAYER_TARGETING:
                case SPELL_EFFECT_SUMMON_ALL_TOTEMS:
                case SPELL_EFFECT_FEED_PET:
                case SPELL_EFFECT_DESTROY_ALL_TOTEMS:
                case SPELL_EFFECT_SKILL:
                    targetUnitMap.push_back(m_caster);
                    break;
                case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                    if (Unit* currentTarget = m_targets.getUnitTarget())
                        m_targets.setDestination(currentTarget->GetPosition());
                    break;
                case SPELL_EFFECT_LEARN_PET_SPELL:
                    if (Pet* pet = m_caster->GetPet())
                        targetUnitMap.push_back(pet);
                    break;
                case SPELL_EFFECT_ENCHANT_ITEM:
                case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
                case SPELL_EFFECT_ENCHANT_ITEM_PRISMATIC:
                case SPELL_EFFECT_DISENCHANT:
                case SPELL_EFFECT_PROSPECTING:
                case SPELL_EFFECT_MILLING:
                    if (m_targets.getItemTarget())
                        AddItemTarget(m_targets.getItemTarget(), effIndex);
                    break;
                case SPELL_EFFECT_APPLY_AURA:
                    switch(m_spellInfo->EffectApplyAuraName[effIndex])
                    {
                        case SPELL_AURA_ADD_FLAT_MODIFIER:  // some spell mods auras have 0 target modes instead expected TARGET_SELF(1) (and present for other ranks for same spell for example)
                        case SPELL_AURA_ADD_PCT_MODIFIER:
                        case SPELL_AURA_MOD_RANGED_ATTACK_POWER:
                        case SPELL_AURA_MOD_ATTACK_POWER:
                        case SPELL_AURA_MOD_HEALING_DONE:
                        case SPELL_AURA_MOD_DAMAGE_DONE:
                            targetUnitMap.push_back(m_caster);
                            break;
                        default:                            // apply to target in other case
                            if (m_targets.getUnitTarget())
                                targetUnitMap.push_back(m_targets.getUnitTarget());
                            break;
                    }
                    break;
                case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                    // AreaAura
                    if ((m_spellInfo->GetAttributes() == (SPELL_ATTR_NOT_SHAPESHIFT | SPELL_ATTR_UNK18 | SPELL_ATTR_CASTABLE_WHILE_MOUNTED | SPELL_ATTR_CASTABLE_WHILE_SITTING)) || (m_spellInfo->GetAttributes() == SPELL_ATTR_NOT_SHAPESHIFT))
                        SetTargetMap(effIndex, TARGET_AREAEFFECT_PARTY, targetUnitMap);
                    break;
                case SPELL_EFFECT_SKIN_PLAYER_CORPSE:
                    if (m_targets.getUnitTarget())
                        targetUnitMap.push_back(m_targets.getUnitTarget());
                    else if (m_targets.getCorpseTargetGuid())
                    {
                        if (Corpse *corpse = m_caster->GetMap()->GetCorpse(m_targets.getCorpseTargetGuid()))
                            if (Player* owner = ObjectAccessor::FindPlayer(corpse->GetOwnerGuid()))
                                targetUnitMap.push_back(owner);
                    }
                    break;
                default:
                    break;
            }
            break;
        }
        default:
            //sLog.outError( "SPELL: Unknown implicit target (%u) for spell ID %u", targetMode, m_spellInfo->Id );
            break;
    }

    if (unMaxTargets && targetUnitMap.size() > unMaxTargets)
    {
        // cleanup list for a right solution (without this spells with unMaxTargets = 1 hit possible nothing, if target is not valid with CheckTarget())
        UnitList::iterator itr = targetUnitMap.begin();
        while (itr != targetUnitMap.end())
        {
            if (!CheckTargetBeforeLimitation(*itr, effIndex))
                itr = targetUnitMap.erase(itr);
            else
                ++itr;
        }
    }

    if (unMaxTargets && targetUnitMap.size() > unMaxTargets)
    {
        // make sure one unit is always removed per iteration
        uint32 removed_utarget = 0;
        UnitList::iterator itr = targetUnitMap.begin();
        while (itr != targetUnitMap.end())
        {
            if ((*itr) == m_targets.getUnitTarget())
            {
                itr = targetUnitMap.erase(itr);
                removed_utarget = 1;
            }
            else
                ++itr;
        }
        // remove random units from the map
        while (targetUnitMap.size() > unMaxTargets - removed_utarget)
        {
            UnitList::iterator iter = targetUnitMap.begin();
            advance(iter, urand(0, targetUnitMap.size()-1));
            targetUnitMap.erase(iter);
        }
        // the player's target will always be added to the map
        if (removed_utarget && m_targets.getUnitTarget())
            targetUnitMap.push_back(m_targets.getUnitTarget());
    }
    if (!tempTargetGOList.empty())                          // GO CASE
    {
        if (unMaxTargets && tempTargetGOList.size() > unMaxTargets)
        {
            // make sure one go is always removed per iteration
            uint32 removed_utarget = 0;
            for (std::list<GameObject*>::iterator itr = tempTargetGOList.begin(), next; itr != tempTargetGOList.end(); itr = next)
            {
                next = itr;
                ++next;
                if (!*itr) continue;
                if ((*itr) == m_targets.getGOTarget())
                {
                    tempTargetGOList.erase(itr);
                    removed_utarget = 1;
                    //        break;
                }
            }
            // remove random units from the map
            while (tempTargetGOList.size() > unMaxTargets - removed_utarget)
            {
                uint32 poz = urand(0, tempTargetGOList.size()-1);
                for (std::list<GameObject*>::iterator itr = tempTargetGOList.begin(); itr != tempTargetGOList.end(); ++itr, --poz)
                {
                    if (!*itr) continue;

                    if (!poz)
                    {
                        tempTargetGOList.erase(itr);
                        break;
                    }
                }
            }
        }
        // Add resulting GOs as GOTargets
        for (std::list<GameObject*>::iterator iter = tempTargetGOList.begin(); iter != tempTargetGOList.end(); ++iter)
            AddGOTarget(*iter, effIndex);
    }
}

void Spell::prepare(SpellCastTargets const* targets, Aura const* triggeredByAura)
{
    m_targets = *targets;

    m_spellState = SPELL_STATE_PREPARING;

    m_castPositionX = m_caster->GetPositionX();
    m_castPositionY = m_caster->GetPositionY();
    m_castPositionZ = m_caster->GetPositionZ();
    m_castOrientation = m_caster->GetOrientation();

    if (triggeredByAura)
        m_triggeredByAuraSpell  = triggeredByAura->GetSpellProto();

    DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell::prepare spell %u caster %s %s formal target %s %s preparing.",
        m_spellInfo->Id,
        m_caster ? m_caster->GetObjectGuid().GetString().c_str() : "<none>",
        (m_caster && !m_caster->IsInWorld()) ? "(not in World)" : "",
        m_targets.getUnitTargetGuid() ? m_targets.getUnitTargetGuid().GetString().c_str() : "<none>",
        (m_targets.getUnitTarget() && !m_targets.getUnitTarget()->IsInWorld()) ? "(not in world)" : ""
        );

    // create and add update event for this spell
    m_caster->AddEvent(new SpellEvent(this), 1);

    //Prevent casting at cast another spell (ServerSide check)
    if (m_caster->IsNonMeleeSpellCasted(false, true, true) && m_cast_count)
    {
        SendCastResult(SPELL_FAILED_SPELL_IN_PROGRESS);
        finish(false);
        return;
    }

    if (uint8 resultDisable = sObjectMgr.IsSpellDisabled(m_spellInfo->Id))
    {
        DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST,"Spell::prepare  %s try cast a spell %u which was disabled by server administrator",   m_caster ? m_caster->GetObjectGuid().GetString().c_str() : "<none>", m_spellInfo->Id);
        if (m_caster->GetTypeId() == TYPEID_PLAYER &&resultDisable == 2)
            sLog.outChar("Player %s cast a spell %u which was disabled by server administrator and marked as CheatSpell",   m_caster->GetObjectGuid().GetString().c_str(), m_spellInfo->Id);

        SendCastResult(SPELL_FAILED_SPELL_UNAVAILABLE);
        finish(false);
        return;
    }

    // Fill cost data
    m_powerCost = CalculatePowerCost(m_spellInfo, m_caster, this, m_CastItem);

    SpellCastResult result = CheckCast(true);
    if (result != SPELL_CAST_OK && !IsAutoRepeat())          //always cast autorepeat dummy for triggering
    {
        if (triggeredByAura)
        {
            SendChannelUpdate(0);
            triggeredByAura->GetHolder()->SetAuraDuration(0);
        }
        SendCastResult(result);
        finish(false);
        return;
    }

    // Prepare data for triggers
    prepareDataForTriggerSystem();

    // calculate cast time (calculated after first CheckCast check to prevent charge counting for first CheckCast fail)
    m_casttime = GetSpellCastTime(m_spellInfo, this);
    m_duration = CalculateSpellDuration(m_spellInfo, m_caster);

    // set timer base at cast time
    ReSetTimer();

    // stealth must be removed at cast starting (at show channel bar)
    // skip triggered spell (item equip spell casting and other not explicit character casts/item uses)
    if ( !m_IsTriggeredSpell && isSpellBreakStealth(m_spellInfo) )
    {
        m_caster->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_CAST);
        m_caster->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_USE);
    }

    // add non-triggered (with cast time and without)
    if (!m_IsTriggeredSpell)
    {
        // add to cast type slot
        m_caster->SetCurrentCastedSpell( this );

        // will show cast bar
        SendSpellStart();

        TriggerGlobalCooldown();
    }
    // execute triggered or without cast time explicitly in call point
    else if (m_timer == 0 || m_IsTriggeredSpell)
        cast(true);
    // else triggered with cast time will execute execute at next tick or later
    // without adding to cast type slot
    // will not show cast bar but will show effects at casting time etc

    if (m_spellInfo->GetSpeed() > 0.0f && GetCastTime())
    {
        Unit* procTarget = m_targets.getUnitTarget();
        if (!procTarget)
            procTarget = m_caster;

        m_caster->ProcDamageAndSpell(procTarget, m_procAttacker, 0, PROC_EX_CAST_END, 0, m_attackType, m_spellInfo);
    }
}

void Spell::cancel(bool force)
{
    if (m_spellState == SPELL_STATE_FINISHED)
        return;

    DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell::cancel spell %u caster %s target %s cancelled.",
        m_spellInfo->Id, (m_caster && m_caster->IsInWorld()) ? m_caster->GetObjectGuid().GetString().c_str() : "<none>",
        (m_targets.getUnitTarget() && m_targets.getUnitTarget()->IsInWorld()) ? m_targets.getUnitTarget()->GetObjectGuid().GetString().c_str() : "<none>");

    // channeled spells don't display interrupted message even if they are interrupted, possible other cases with no "Interrupted" message
    bool sendInterrupt = IsChanneledSpell(m_spellInfo) ? false : true;

    m_autoRepeat = false;
    switch (m_spellState)
    {
        case SPELL_STATE_PREPARING:
            CancelGlobalCooldown();

            //(no break)
        case SPELL_STATE_DELAYED:
        {
            SendInterrupted(0);

            if (sendInterrupt)
                SendCastResult(SPELL_FAILED_INTERRUPTED);
        } break;

        case SPELL_STATE_CASTING:
        {
            for(TargetList::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
            {
                if (ihit->missCondition == SPELL_MISS_NONE)
                {
                    Unit* unit = m_caster->GetObjectGuid() == (*ihit).targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);
                    if (unit && unit->isAlive())
                        unit->RemoveAurasByCasterSpell(m_spellInfo->Id, m_caster->GetObjectGuid(), force ? AURA_REMOVE_BY_DELETE : AURA_REMOVE_BY_DEFAULT);

                    // prevent other effects applying if spell is already interrupted
                    // i.e. if effects have different targets and it was interrupted on one of them when
                    // haven't yet applied to another
                    ihit->processed = true;
                }
            }

            SendChannelUpdate(0);
            SendInterrupted(0);

            if (sendInterrupt)
                SendCastResult(SPELL_FAILED_INTERRUPTED);
        } break;

        default:
        {
        } break;
    }

    finish(false);
    m_caster->RemoveDynObject(m_spellInfo->Id);
    m_caster->RemoveGameObject(m_spellInfo->Id, true);
}

void Spell::cast(bool skipCheck)
{
    SetExecutedCurrently(true);

    if (!m_caster->CheckAndIncreaseCastCounter())
    {
        if (m_triggeredByAuraSpell)
            sLog.outError("Spell %u triggered by aura spell %u too deep in cast chain for cast. Cast not allowed for prevent overflow stack crash.", m_spellInfo->Id, m_triggeredByAuraSpell->Id);
        else
            sLog.outError("Spell %u too deep in cast chain for cast. Cast not allowed for prevent overflow stack crash.", m_spellInfo->Id);

        SendCastResult(SPELL_FAILED_ERROR);
        finish(false);
        SetExecutedCurrently(false);
        return;
    }

    // update pointers base at GUIDs to prevent access to already nonexistent object
    UpdatePointers();

    // cancel at lost main target unit
    if (!m_targets.getUnitTarget() && m_targets.getUnitTargetGuid() && m_targets.getUnitTargetGuid() != m_caster->GetObjectGuid())
    {
        cancel();
        m_caster->DecreaseCastCounter();
        SetExecutedCurrently(false);
        DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell::cast  spell %u caster %s cancelled at lost main target",
             m_spellInfo->Id, m_caster ? m_caster->GetObjectGuid().GetString().c_str() : "<none>");
        return;
    }

    if (m_caster->GetTypeId() != TYPEID_PLAYER && m_targets.getUnitTarget() && m_targets.getUnitTarget() != m_caster)
        m_caster->SetInFront(m_targets.getUnitTarget());

    SpellCastResult castResult = CheckPower();
    if (castResult != SPELL_CAST_OK)
    {
        SendCastResult(castResult);
        finish(false);
        m_caster->DecreaseCastCounter();
        SetExecutedCurrently(false);
        DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell::cast  spell %u caster %s cancelled by unsufficient power (code %u)",
             m_spellInfo->Id, m_caster ?  m_caster->GetObjectGuid().GetString().c_str() : "<none>", castResult);
        return;
    }

    // triggered cast called from Spell::prepare where it was already checked
    if (!skipCheck)
    {
        castResult = CheckCast(false);
        if (castResult != SPELL_CAST_OK)
        {
            SendCastResult(castResult);
            finish(false);
            m_caster->DecreaseCastCounter();
            SetExecutedCurrently(false);
            DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell::cast  spell %u caster %s cancelled by non-stricted check (code %u)",
                 m_spellInfo->Id, m_caster ? m_caster->GetObjectGuid().GetString().c_str() : "<none>", castResult);
            return;
        }
    }

    // Hack for Spirit of Redemption because wrong data in dbc
    if (m_spellInfo->Id == 27827)
        if (const SpellEntry* spellInfo = sSpellStore.LookupEntry(m_spellInfo->Id))
            const_cast<SpellEntry*>(spellInfo)->AuraInterruptFlags = 0;

    // different triggered (for caster and main target after main cast) and pre-cast (casted before apply effect to each target) cases
    switch (m_spellInfo->SpellFamilyName)
    {
        case SPELLFAMILY_GENERIC:
        {
            if (m_spellInfo->Id == 20594)                  // Stoneskin
                AddTriggeredSpell(65116);                  // Stoneskin - armor 10% for 8 sec
            else if (m_spellInfo->Id == 71904)             // Chaos Bane strength buff
                AddTriggeredSpell(73422);
            else if (m_spellInfo->Id == 74607)
                AddTriggeredSpell(74610);                  // Fiery combustion
            else if (m_spellInfo->Id == 74799)
                AddTriggeredSpell(74800);                  // Soul consumption
            else if (m_spellInfo->Id == 61968)             // Flash Freeze (Hodir: Ulduar)
                AddTriggeredSpell(62148);                  // visual effect
            else if (m_spellInfo->Id == 69839)             // Unstable Ooze Explosion (Rotface)
                AddPrecastSpell(69832);                    // cast "cluster" before silence and pacify
            else if (m_spellInfo->Id == 58672)             // Impale, damage and loose threat effect (Vault of Archavon, Archavon the Stone Watcher)
                AddPrecastSpell(m_caster->GetMap()->IsRegularDifficulty() ? 58666 : 60882);
            else if (m_spellInfo->Id == 71265)             // Swarming Shadows DoT (Queen Lana'thel ICC)
                AddPrecastSpell(71277);
            else if (m_spellInfo->Id == 70923)             // Uncontrollable Frenzy (Queen Lana'thel ICC)
                AddTriggeredSpell(70924);                  // health buff etc.
            else if (m_spellInfo->Mechanic == MECHANIC_MOUNT)
            {
               // Festive Holiday Mount
               if (m_spellInfo->GetSpellIconID() != 1794 && m_caster->HasAura(62061, EFFECT_INDEX_0))
                   AddTriggeredSpell(25860);               // Reindeer Transformation
            }
            break;
        }
        case SPELLFAMILY_MAGE:
        {
            // Ice Block
            if (m_spellInfo->GetSpellFamilyFlags().test<CF_MAGE_ICE_BLOCK>())
                AddPrecastSpell(41425);                     // Hypothermia
            // Icy Veins
            else if (m_spellInfo->Id == 12472)
            {
                if (m_caster->HasAura(56374))               // Glyph of Icy Veins
                {
                    // not exist spell do it so apply directly
                    m_caster->RemoveSpellsCausingAura(SPELL_AURA_MOD_DECREASE_SPEED);
                    m_caster->RemoveSpellsCausingAura(SPELL_AURA_HASTE_SPELLS);
                }
            }
            // Fingers of Frost
            else if (m_spellInfo->Id == 44544)
                AddPrecastSpell(74396);                     // Fingers of Frost
            break;
        }
        case SPELLFAMILY_WARRIOR:
        {
            // Shield Slam
            if (m_spellInfo->GetSpellFamilyFlags().test<CF_WARRIOR_SHIELD_SLAM>() && m_spellInfo->Category==1209)
            {
                if (m_caster->HasAura(58375))               // Glyph of Blocking
                    AddTriggeredSpell(58374);               // Glyph of Blocking
            }
            // Bloodrage
            if (m_spellInfo->GetSpellFamilyFlags().test<CF_WARRIOR_BLOODRAGE>())
            {
                if (m_caster->HasAura(70844))               // Item - Warrior T10 Protection 4P Bonus
                    AddTriggeredSpell(70845);               // Stoicism
            }
            // Bloodsurge (triggered), Sudden Death (triggered)
            else if (m_spellInfo->Id == 46916 || m_spellInfo->Id == 52437)
            {
                // Item - Warrior T10 Melee 4P Bonus
                if (Aura *aur = m_caster->GetAura(70847, EFFECT_INDEX_0))
                {
                    if (roll_chance_i(aur->GetModifier()->m_amount))
                    {
                        AddTriggeredSpell(70849);           // Extra Charge!
                        // Slam! trigger Slam GCD Reduced . Sudden Death trigger Execute GCD Reduced
                        int32 gcd_spell=m_spellInfo->Id==46916 ? 71072 : 71069;
                        AddPrecastSpell(gcd_spell);
                    }
                }
            }
            break;
        }
        case SPELLFAMILY_PRIEST:
        {
            // Power Word: Shield
            if (m_spellInfo->Mechanic == MECHANIC_SHIELD &&
                m_spellInfo->GetSpellFamilyFlags().test<CF_PRIEST_POWER_WORD_SHIELD>())
                AddPrecastSpell(6788);                      // Weakened Soul
            // Prayer of Mending (jump animation), we need formal caster instead original for correct animation
            else if (m_spellInfo->GetSpellFamilyFlags().test<CF_PRIEST_PRAYER_OF_MENDING>())
                AddTriggeredSpell(41637);

            switch(m_spellInfo->Id)
            {
                case 15237: AddTriggeredSpell(23455); break;// Holy Nova, rank 1
                case 15430: AddTriggeredSpell(23458); break;// Holy Nova, rank 2
                case 15431: AddTriggeredSpell(23459); break;// Holy Nova, rank 3
                case 27799: AddTriggeredSpell(27803); break;// Holy Nova, rank 4
                case 27800: AddTriggeredSpell(27804); break;// Holy Nova, rank 5
                case 27801: AddTriggeredSpell(27805); break;// Holy Nova, rank 6
                case 25331: AddTriggeredSpell(25329); break;// Holy Nova, rank 7
                case 48077: AddTriggeredSpell(48075); break;// Holy Nova, rank 8
                case 48078: AddTriggeredSpell(48076); break;// Holy Nova, rank 9
                default:break;
            }
            break;
        }
        case SPELLFAMILY_DRUID:
        {
            // Faerie Fire (Feral)
            if (m_spellInfo->Id == 16857 && m_caster->GetShapeshiftForm() != FORM_CAT)
                AddTriggeredSpell(60089);
            // Clearcasting
            else if (m_spellInfo->Id == 16870)
            {
                if (m_caster->HasAura(70718))               // Item - Druid T10 Balance 2P Bonus
                    AddPrecastSpell(70721);                 // Omen of Doom
            }
            // Berserk (Bear Mangle part)
            else if (m_spellInfo->Id == 50334)
                AddTriggeredSpell(58923);
            break;
        }
        case SPELLFAMILY_ROGUE:
            // Fan of Knives (main hand)
            if (m_spellInfo->Id == 51723 && m_caster->GetTypeId() == TYPEID_PLAYER &&
                ((Player*)m_caster)->haveOffhandWeapon())
            {
                AddTriggeredSpell(52874);                   // Fan of Knives (offhand)
            }
            break;
        case SPELLFAMILY_HUNTER:
        {
            // Deterrence
            if (m_spellInfo->Id == 19263)
                AddPrecastSpell(67801);
            // Kill Command
            else if (m_spellInfo->Id == 34026)
            {
                if (m_caster->HasAura(37483))               // Improved Kill Command - Item set bonus
                    m_caster->CastSpell(m_caster, 37482, true);// Exploited Weakness
            }
            // Lock and Load
            else if (m_spellInfo->Id == 56453)
                AddPrecastSpell(67544);                     // Lock and Load Marker
            break;
        }
        case SPELLFAMILY_PALADIN:
        {
            // Divine Illumination
            if (m_spellInfo->Id == 31842)
            {
                if (m_caster->HasAura(70755))               // Item - Paladin T10 Holy 2P Bonus
                    AddPrecastSpell(71166);                 // Divine Illumination
            }
            // Hand of Reckoning
            else if (m_spellInfo->Id == 62124)
            {
                if (m_targets.getUnitTarget() && m_targets.getUnitTarget()->getVictim() != m_caster)
                    AddPrecastSpell(67485);                 // Hand of Reckoning
            }
            // Divine Shield, Divine Protection or Hand of Protection
            else if (m_spellInfo->GetSpellFamilyFlags().test<CF_PALADIN_HAND_OF_PROTECTION, CF_PALADIN_DIVINE_SHIELD>())
            {
                AddPrecastSpell(25771);                     // Forbearance

                // only for self cast
                if (m_caster == m_targets.getUnitTarget())
                    AddPrecastSpell(61987);                     // Avenging Wrath Marker
            }
            // Lay on Hands
            else if (m_spellInfo->GetSpellFamilyFlags().test<CF_PALADIN_LAY_ON_HANDS>())
            {
                // only for self cast
                if (m_caster == m_targets.getUnitTarget())
                {
                    AddPrecastSpell(25771);                     // Forbearance
                    AddPrecastSpell(61987);                     // Avenging Wrath Marker
                }
            }
            // Avenging Wrath
            else if (m_spellInfo->GetSpellFamilyFlags().test<CF_PALADIN_AVENGING_WRATH>())
                AddPrecastSpell(61987);                     // Avenging Wrath Marker
            break;
        }
        case SPELLFAMILY_SHAMAN:
        {
            // Bloodlust
            if (m_spellInfo->Id == 2825)
                AddPrecastSpell(57724);                     // Sated
            // Heroism
            else if (m_spellInfo->Id == 32182)
                AddPrecastSpell(57723);                     // Exhaustion
            // Spirit Walk
            else if (m_spellInfo->Id == 58875)
                AddPrecastSpell(58876);
            // Totem of Wrath
            else if (m_spellInfo->Effect[EFFECT_INDEX_0]==SPELL_EFFECT_APPLY_AREA_AURA_RAID && m_spellInfo->GetSpellFamilyFlags().test<CF_SHAMAN_TOTEM_OF_WRATH>())
                // only for main totem spell cast
                AddTriggeredSpell(30708);                   // Totem of Wrath
            break;
        }
        case SPELLFAMILY_DEATHKNIGHT:
        {
            // Chains of Ice
            if (m_spellInfo->Id == 45524)
                AddTriggeredSpell(55095);                   // Frost Fever
            break;
        }
        default:
            break;
    }

    // Linked spells (precast chain)
    SpellLinkedSet linkedSet = sSpellMgr.GetSpellLinked(m_spellInfo->Id, SPELL_LINKED_TYPE_PRECAST);
    if (linkedSet.size() > 0)
    {
        for (SpellLinkedSet::const_iterator itr = linkedSet.begin(); itr != linkedSet.end(); ++itr)
            AddPrecastSpell(*itr);
    }

    // Linked spells (triggered chain)
    linkedSet = sSpellMgr.GetSpellLinked(m_spellInfo->Id, SPELL_LINKED_TYPE_TRIGGERED);
    if (linkedSet.size() > 0)
    {
        for (SpellLinkedSet::const_iterator itr = linkedSet.begin(); itr != linkedSet.end(); ++itr)
            AddTriggeredSpell(*itr);
    }

    // Linked spells (not triggered)
    linkedSet = sSpellMgr.GetSpellLinked(m_spellInfo->Id, SPELL_LINKED_TYPE_NOT_TRIGGERED);
    if (linkedSet.size() > 0)
    {
        for (SpellLinkedSet::const_iterator itr = linkedSet.begin(); itr != linkedSet.end(); ++itr)
            AddNotTriggeredSpell(*itr);
    }

    // traded items have trade slot instead of guid in m_itemTargetGUID
    // set to real guid to be sent later to the client
    m_targets.updateTradeSlotItem();

    FillTargetMap();

    SpellCastResult checkTargetResult = CheckCastTargets();

    if (checkTargetResult != SPELL_CAST_OK)
    {
        SendCastResult(checkTargetResult);
        finish(false);
        m_caster->DecreaseCastCounter();
        SetExecutedCurrently(false);
        DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell::cast  spell %u caster %s cancelled by CheckCastTargets check (code %u)",
             m_spellInfo->Id, m_caster ? m_caster->GetObjectGuid().GetString().c_str() : "<none>", checkTargetResult);
        return;
    }

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        if (!m_IsTriggeredSpell && m_CastItem)
        {
            ((Player*)m_caster)->GetAchievementMgr().StartTimedAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_USE_ITEM, m_CastItem->GetEntry());
            ((Player*)m_caster)->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_USE_ITEM, m_CastItem->GetEntry());
        }

        ((Player*)m_caster)->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_CAST_SPELL, m_spellInfo->Id);
    }

    if (m_spellState == SPELL_STATE_FINISHED)                // stop cast if spell marked as finish somewhere in FillTargetMap
    {
        m_caster->DecreaseCastCounter();
        SetExecutedCurrently(false);
        return;
    }

    // CAST SPELL
    SendSpellCooldown();

    TakePower();
    TakeReagents();                                         // we must remove reagents before HandleEffects to allow place crafted item in same slot
    TakeAmmo();

    SendCastResult(castResult);
    SendSpellGo();                                          // we must send smsg_spell_go packet before m_castItem delete in TakeCastItem()...

    InitializeDamageMultipliers();

    Unit* procTarget = m_targets.getUnitTarget();
    if (!procTarget)
        procTarget = m_caster;

    // Okay, everything is prepared. Now we need to distinguish between immediate and evented delayed spells
    if (m_spellInfo->GetSpeed() > 0.0f || GetDelayStart() > 0)
    {

        // Remove used for cast item if need (it can be already NULL after TakeReagents call
        // in case delayed spell remove item at cast delay start
        TakeCastItem();

        // fill initial spell damage from caster for delayed casted spells
        for(TargetList::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
            HandleDelayedSpellLaunch(&(*ihit));

        // Okay, maps created, now prepare flags
        m_immediateHandled = false;
        m_spellState = SPELL_STATE_DELAYED;
        SetDelayStart(0);

        // on spell cast end proc,
        // critical hit related part is currently done on hit so proc there,
        // 0 damage since any damage based procs should be on hit
        // 0 victim proc since there is no victim proc dependent on successfull cast for caster
        // if m_casttime > 0  proc already maked in prepare()
        if (!GetCastTime())
            m_caster->ProcDamageAndSpell(procTarget, m_procAttacker, PROC_FLAG_NONE, PROC_EX_CAST_END, 0, m_attackType, m_spellInfo);
    }
    else
    {
        if (GetCastTime())
        {
            m_caster->ProcDamageAndSpell(procTarget, m_procAttacker, PROC_FLAG_NONE, PROC_EX_CAST_END, 0, m_attackType, m_spellInfo);
            // Immediate spell, no big deal
            handle_immediate();
        }
        else
        {
            handle_immediate();
            m_caster->ProcDamageAndSpell(procTarget, m_procAttacker, PROC_FLAG_NONE, PROC_EX_CAST_END, 0, m_attackType, m_spellInfo);
        }

    }

    m_caster->DecreaseCastCounter();
    SetExecutedCurrently(false);
}

void Spell::handle_immediate()
{
    // process immediate effects (items, ground, etc.) also initialize some variables
    _handle_immediate_phase();

    // start channeling if applicable (after _handle_immediate_phase for get persistent effect dynamic object for channel target
    if (IsChanneledSpell(m_spellInfo) && m_duration)
    {
        m_spellState = SPELL_STATE_CASTING;
        SendChannelStart(m_duration);
    }

    for(TargetList::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end();)
    {
        TargetInfo buffer = *ihit++;
        DoAllEffectOnTarget(&buffer);
    }

    for(GOTargetList::iterator ihit = m_UniqueGOTargetInfo.begin(); ihit != m_UniqueGOTargetInfo.end();)
    {
        GOTargetInfo buffer = *ihit++;
        DoAllEffectOnTarget(&buffer);
    }

    // spell is finished, perform some last features of the spell here
    _handle_finish_phase();

    // Remove used for cast item if need (it can be already NULL after TakeReagents call
    TakeCastItem();

    if (m_spellState != SPELL_STATE_CASTING)
        finish(true);                                       // successfully finish spell cast (not last in case autorepeat or channel spell)
}

uint64 Spell::handle_delayed(uint64 t_offset)
{
    uint64 next_time = 0;

    if (!m_immediateHandled)
    {
        _handle_immediate_phase();
        m_immediateHandled = true;
    }

    // now recheck units targeting correctness (need before any effects apply to prevent adding immunity at first effect not allow apply second spell effect and similar cases)
    for(TargetList::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (!ihit->processed)
        {
            if (ihit->timeDelay <= t_offset)
                DoAllEffectOnTarget(&(*ihit));
            else if (next_time == 0 || ihit->timeDelay < next_time)
                next_time = ihit->timeDelay;
        }
    }

    // now recheck gameobject targeting correctness
    for(GOTargetList::iterator ighit = m_UniqueGOTargetInfo.begin(); ighit != m_UniqueGOTargetInfo.end(); ++ighit)
    {
        if (!ighit->processed)
        {
            if (ighit->timeDelay <= t_offset)
                DoAllEffectOnTarget(&(*ighit));
            else if (next_time == 0 || ighit->timeDelay < next_time)
                next_time = ighit->timeDelay;
        }
    }
    // All targets passed - need finish phase
    if (next_time == 0)
    {
        // spell is finished, perform some last features of the spell here
        _handle_finish_phase();

        finish(true);                                       // successfully finish spell cast

        // return zero, spell is finished now
        return 0;
    }
    else
    {
        // spell is unfinished, return next execution time
        return next_time;
    }
}

void Spell::_handle_immediate_phase()
{
    // handle some immediate features of the spell here
    HandleThreatSpells();

    for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
    {
        if (m_spellInfo->Effect[j] == SPELL_EFFECT_NONE)
            continue;

        // apply Send Event effect to ground in case empty target lists
        if ( m_spellInfo->Effect[j] == SPELL_EFFECT_SEND_EVENT && !HaveTargetsForEffect(SpellEffectIndex(j)) )
        {
            HandleEffects(NULL, NULL, NULL, SpellEffectIndex(j));
            continue;
        }
    }

    // initialize Diminishing Returns Data
    m_diminishLevel = DIMINISHING_LEVEL_1;
    m_diminishGroup = DIMINISHING_NONE;

    // process items
    for(ItemTargetList::iterator ihit = m_UniqueItemInfo.begin(); ihit != m_UniqueItemInfo.end(); ++ihit)
    {
        ItemTargetInfo buffer = *ihit;
        DoAllEffectOnTarget(&buffer);
    }

    // process ground
    for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
    {
        // persistent area auras target only the ground
        if (m_spellInfo->Effect[j] == SPELL_EFFECT_PERSISTENT_AREA_AURA)
            HandleEffects(NULL, NULL, NULL, SpellEffectIndex(j));
    }
}

void Spell::_handle_finish_phase()
{
    // spell log
    if (IsNeedSendToClient())
        SendLogExecute();
}

void Spell::SendSpellCooldown()
{
    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        // mana/health/etc potions, disabled by client (until combat out as declarate)
        if (m_CastItem && m_CastItem->IsPotion())
        {
            // need in some way provided data for Spell::finish SendCooldownEvent
            ((Player*)m_caster)->SetLastPotionId(m_CastItem->GetEntry());
            return;
        }
    }

    // (1) have infinity cooldown but set at aura apply, (2) passive cooldown at triggering
    if (m_spellInfo->HasAttribute(SPELL_ATTR_DISABLED_WHILE_ACTIVE) || m_spellInfo->HasAttribute(SPELL_ATTR_PASSIVE))
        return;

    m_caster->AddSpellAndCategoryCooldowns(m_spellInfo, m_CastItem ? m_CastItem->GetEntry() : 0);
}

void Spell::update(uint32 difftime)
{
    // update pointers based at it's GUIDs
    UpdatePointers();

    if (m_targets.getUnitTargetGuid() && !m_targets.getUnitTarget())
    {
        cancel();
        return;
    }

    // check if the player caster has moved before the spell finished (exclude casting on vehicles)
    if (!m_caster->GetVehicle() && (m_caster->GetTypeId() == TYPEID_PLAYER && m_timer != 0) &&
        (m_castPositionX != m_caster->GetPositionX() || m_castPositionY != m_caster->GetPositionY() || m_castPositionZ != m_caster->GetPositionZ()) &&
        (m_spellInfo->Effect[EFFECT_INDEX_0] != SPELL_EFFECT_STUCK || !((Player*)m_caster)->m_movementInfo.HasMovementFlag(MOVEFLAG_FALLINGFAR)))
    {
        // always cancel for channeled spells
        if ( m_spellState == SPELL_STATE_CASTING )
            cancel();
        // don't cancel for melee, autorepeat, triggered and instant spells
        else if(!IsNextMeleeSwingSpell() && !IsAutoRepeat() && !m_IsTriggeredSpell && (m_spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_MOVEMENT))
            cancel();
    }


    switch(m_spellState)
    {
        case SPELL_STATE_PREPARING:
        {
            if (m_timer)
            {
                if (m_targets.getUnitTarget() && m_targets.getUnitTarget()->isAlive() && !m_targets.getUnitTarget()->isVisibleForOrDetect(m_caster, m_caster, false) && !m_IsTriggeredSpell )
                    cancel();

                if (difftime >= m_timer)
                    m_timer = 0;
                else
                    m_timer -= difftime;
            }

            if (m_timer == 0 && !IsNextMeleeSwingSpell() && !IsAutoRepeat())
                cast();
        } break;
        case SPELL_STATE_CASTING:
        {
            if (m_timer > 0)
            {
                if ( m_caster->GetTypeId() == TYPEID_PLAYER )
                {
                    // check if player has jumped before the channeling finished
                    if(((Player*)m_caster)->m_movementInfo.HasMovementFlag(MOVEFLAG_FALLING))
                        cancel();

                    // check for incapacitating player states
                    if ( m_caster->hasUnitState(UNIT_STAT_CAN_NOT_REACT))
                        cancel();

                    // check if player has turned if flag is set
                    if ((m_spellInfo->ChannelInterruptFlags & CHANNEL_FLAG_TURNING) && m_castOrientation != m_caster->GetOrientation())
                        cancel();
                }

                // check if all targets away range
                if (!m_IsTriggeredSpell && (difftime >= m_timer))
                {
                    SpellCastResult result = CheckRange(false, m_targets.getUnitTarget());
                    bool checkFailed = false;
                    switch (result)
                    {
                        case SPELL_CAST_OK:
                            break;
                        case SPELL_FAILED_TOO_CLOSE:
                        case SPELL_FAILED_UNIT_NOT_INFRONT:
                            if (m_spellInfo->HasAttribute(SPELL_ATTR_EX7_HAS_CHARGE_EFFECT))
                                break;
                            checkFailed = true;
                            break;
                        case SPELL_FAILED_OUT_OF_RANGE:
                        default:
                            checkFailed = true;
                            break;
                    }

                    if (checkFailed)
                    {
                        DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell::update  spell %u caster %s  target %s cancelled by CheckRange wile cast process continued (code %u)",
                            m_spellInfo->Id, m_caster ? m_caster->GetObjectGuid().GetString().c_str() : "<none>",
                            m_targets.getUnitTarget() ? m_targets.getUnitTarget()->GetObjectGuid().GetString().c_str() : "<none>",
                            result);
                        SendCastResult(result);
                        cancel(true);
                        return;
                    }
                }

                // check if there are alive targets left
                if (!IsAliveUnitPresentInTargetList())
                {
                    SendChannelUpdate(0);
                    finish();
                }

                if (difftime >= m_timer)
                    m_timer = 0;
                else
                    m_timer -= difftime;
            }

            if (m_timer == 0)
            {
                SendChannelUpdate(0);

                // channeled spell processed independently for quest targeting
                // cast at creature (or GO) quest objectives update at successful cast channel finished
                // ignore autorepeat/melee casts for speed (not exist quest for spells (hm... )
                if ( !IsAutoRepeat() && !IsNextMeleeSwingSpell() )
                {
                    if ( Player* p = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself() )
                    {
                        for(TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
                        {
                            TargetInfo const& target = *ihit;
                            if (!target.targetGUID.IsCreatureOrVehicle())
                                continue;

                            Unit* unit = m_caster->GetObjectGuid() == target.targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, target.targetGUID);
                            if (unit == NULL)
                                continue;

                            p->RewardPlayerAndGroupAtCast(unit, m_spellInfo->Id);
                        }

                        for(GOTargetList::const_iterator ihit = m_UniqueGOTargetInfo.begin(); ihit != m_UniqueGOTargetInfo.end(); ++ihit)
                        {
                            GOTargetInfo const& target = *ihit;

                            GameObject* go = m_caster->GetMap()->GetGameObject(target.targetGUID);
                            if(!go)
                                continue;

                            p->RewardPlayerAndGroupAtCast(go, m_spellInfo->Id);
                        }
                    }
                }

                finish();
            }
        } break;
        default:
        {
        }break;
    }
}

void Spell::finish(bool ok)
{
    if (!m_caster)
        return;

    if (m_spellState == SPELL_STATE_FINISHED)
        return;

    m_spellState = SPELL_STATE_FINISHED;

    // other code related only to successfully finished spells
    if (!ok)
        return;

    // handle SPELL_AURA_ADD_TARGET_TRIGGER auras
    if (Unit* caster = GetAffectiveCaster())
    {
        Unit::AuraList const& targetTriggers = caster->GetAurasByType(SPELL_AURA_ADD_TARGET_TRIGGER);
        for(Unit::AuraList::const_iterator i = targetTriggers.begin(); i != targetTriggers.end(); ++i)
        {
            if (!(*i)->isAffectedOnSpell(m_spellInfo))
                continue;
            for(TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
            {
                if (ihit->missCondition == SPELL_MISS_NONE)
                {
                    // check caster->GetObjectGuid() let load auras at login and speedup most often case
                    Unit* unit = caster->GetObjectGuid() == ihit->targetGUID ? caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);
                    if (unit && unit->isAlive())
                    {
                        SpellEntry const* auraSpellInfo = (*i)->GetSpellProto();
                        SpellEffectIndex auraSpellIdx = (*i)->GetEffIndex();
                        // Calculate chance at that moment (can be depend for example from combo points)
                        int32 auraBasePoints = (*i)->GetBasePoints();
                        int32 chance = caster->CalculateSpellDamage(unit, auraSpellInfo, auraSpellIdx, &auraBasePoints);
                        if (roll_chance_i(chance))
                            caster->CastSpell(unit, auraSpellInfo->EffectTriggerSpell[auraSpellIdx], true, NULL, (*i)());
                    }
                }
            }
        }
    }

    // Heal caster for all health leech from all targets
    if (m_healthLeech)
    {
        uint32 absorb = 0;
        m_caster->CalculateHealAbsorb(uint32(m_healthLeech), &absorb);
        m_caster->DealHeal(m_caster, uint32(m_healthLeech) - absorb, m_spellInfo, false, absorb);
    }

    if (IsMeleeAttackResetSpell())
    {
        m_caster->resetAttackTimer(BASE_ATTACK);
        if (m_caster->haveOffhandWeapon())
            m_caster->resetAttackTimer(OFF_ATTACK);
    }

    /*if (IsRangedAttackResetSpell())
        m_caster->resetAttackTimer(RANGED_ATTACK);*/

    // Clear combo at finish state
    if (NeedsComboPoints(m_spellInfo))
    {
        // Not drop combopoints if negative spell and if any miss on enemy exist
        bool needDrop = true;
        if (!IsPositiveSpell(m_spellInfo->Id))
        {
            for(TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
            {
                if (ihit->missCondition != SPELL_MISS_NONE && ihit->targetGUID != m_caster->GetObjectGuid())
                {
                    needDrop = false;
                    break;
                }
            }
        }
        if (needDrop)
            m_caster->ClearComboPoints();
    }

    // potions disabled by client, send event "not in combat" if need
    if (m_caster->GetTypeId() == TYPEID_PLAYER)
        ((Player*)m_caster)->UpdatePotionCooldown(this);

    // call triggered spell only at successful cast (after clear combo points -> for add some if need)
    if (!m_TriggerSpells.empty())
        CastTriggerSpells();

    // call not triggered spell only at successful cast
    if (!m_NotTriggerSpells.empty())
        CastNotTriggerSpells();

    // Stop Attack for some spells
    if (m_spellInfo->HasAttribute(SPELL_ATTR_STOP_ATTACK_TARGET))
        m_caster->AttackStop();

    // update encounter state if needed
    Map* map = m_caster->GetMap();
    if (map && map->IsDungeon())
    {
        if (DungeonPersistentState* state = ((DungeonMap*)map)->GetPersistanceState())
            state->UpdateEncounterState(ENCOUNTER_CREDIT_CAST_SPELL, m_spellInfo->Id);
    }
}

void Spell::SendCastResult(SpellCastResult result)
{
    if (result == SPELL_CAST_OK)
        return;

    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    if(((Player*)m_caster)->GetSession()->PlayerLoading())  // don't send cast results at loading time
        return;

    SendCastResult((Player*)m_caster, m_spellInfo, m_cast_count, result);
}

void Spell::SendCastResult(Player* caster, SpellEntry const* spellInfo, uint8 cast_count, SpellCastResult result, bool isPetCastResult /*=false*/)
{
    if (result == SPELL_CAST_OK)
        return;

    WorldPacket data(isPetCastResult ? SMSG_PET_CAST_FAILED : SMSG_CAST_FAILED, (4 + 1 + 2));
    data << uint8(cast_count);                              // single cast or multi 2.3 (0/1)
    data << uint32(spellInfo->Id);
    data << uint8(!IsPassiveSpell(spellInfo) ? result : SPELL_FAILED_DONT_REPORT); // do not report failed passive spells
    switch (result)
    {
        case SPELL_FAILED_NOT_READY:
            data << uint32(0);                              // unknown, value 1 seen for 14177 (update cooldowns on client flag)
            break;
        case SPELL_FAILED_REQUIRES_SPELL_FOCUS:
            data << uint32(spellInfo->RequiresSpellFocus);  // SpellFocusObject.dbc id
            break;
        case SPELL_FAILED_REQUIRES_AREA:                    // AreaTable.dbc id
            // hardcode areas limitation case
            switch(spellInfo->Id)
            {
                case 41617:                                 // Cenarion Mana Salve
                case 41619:                                 // Cenarion Healing Salve
                    data << uint32(3905);
                    break;
                case 41618:                                 // Bottled Nethergon Energy
                case 41620:                                 // Bottled Nethergon Vapor
                    data << uint32(3842);
                    break;
                case 45373:                                 // Bloodberry Elixir
                    data << uint32(4075);
                    break;
                default:                                    // default case (don't must be)
                    data << uint32(0);
                    break;
            }
            break;
        case SPELL_FAILED_TOTEMS:
            for(int i = 0; i < MAX_SPELL_TOTEMS; ++i)
                if(spellInfo->Totem[i])
                    data << uint32(spellInfo->Totem[i]);    // client needs only one id, not 2...
            break;
        case SPELL_FAILED_TOTEM_CATEGORY:
            for(int i = 0; i < MAX_SPELL_TOTEM_CATEGORIES; ++i)
                if(spellInfo->TotemCategory[i])
                    data << uint32(spellInfo->TotemCategory[i]);// client needs only one id, not 2...
            break;
        case SPELL_FAILED_EQUIPPED_ITEM_CLASS:
        case SPELL_FAILED_EQUIPPED_ITEM_CLASS_MAINHAND:
        case SPELL_FAILED_EQUIPPED_ITEM_CLASS_OFFHAND:
            data << uint32(spellInfo->EquippedItemClass);
            data << uint32(spellInfo->EquippedItemSubClassMask);
            break;
        case SPELL_FAILED_PREVENTED_BY_MECHANIC:
            data << uint32(0);                              // SpellMechanic.dbc id
            break;
        case SPELL_FAILED_CUSTOM_ERROR:
            data << uint32(0);                              // custom error id (see enum SpellCastResultCustom)
            break;
        case SPELL_FAILED_NEED_EXOTIC_AMMO:
            data << uint32(spellInfo->EquippedItemSubClassMask);// seems correct...
            break;
        case SPELL_FAILED_REAGENTS:
            // normally client checks reagents, just some script effects here
            if (spellInfo->Id == 46584)                      // Raise Dead
                data << uint32(37201);                      // Corpse Dust
            else
                data << uint32(0);                              // item id
            break;
        case SPELL_FAILED_NEED_MORE_ITEMS:
            data << uint32(0);                              // item id
            data << uint32(0);                              // item count?
            break;
        case SPELL_FAILED_MIN_SKILL:
            data << uint32(0);                              // SkillLine.dbc id
            data << uint32(0);                              // required skill value
            break;
        case SPELL_FAILED_TOO_MANY_OF_ITEM:
            data << uint32(0);                              // ItemLimitCategory.dbc id
            break;
        case SPELL_FAILED_FISHING_TOO_LOW:
            data << uint32(0);                              // required fishing skill
            break;
        default:
            break;
    }
    caster->GetSession()->SendPacket(&data);
}

void Spell::SendSpellStart()
{
    if (!IsNeedSendToClient())
        return;

    DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Sending SMSG_SPELL_START id=%u", m_spellInfo->Id);

    uint32 castFlags = CAST_FLAG_UNKNOWN2;
    if (IsRangedSpell())
        castFlags |= CAST_FLAG_AMMO;

    if (m_spellInfo->runeCostID)
        castFlags |= CAST_FLAG_UNKNOWN19;

    Unit *caster = m_triggeredByAuraSpell && IsChanneledSpell(m_triggeredByAuraSpell) ? GetAffectiveCaster() : m_caster;

    WorldPacket data(SMSG_SPELL_START, 9 + caster->GetPackGUID().size() + 1 + 4 + 4 + 4 + 4 + 1 + 1 + 9 + 4 + 4 + 4);
    if (m_CastItem)
        data << m_CastItem->GetPackGUID();
    else
        data << caster->GetPackGUID();

    data << m_caster->GetPackGUID();
    data << uint8(m_cast_count);                            // pending spell cast
    data << uint32(m_spellInfo->Id);                        // spellId
    data << uint32(castFlags);                              // cast flags
    data << uint32(m_timer);                                // delay?

    data << m_targets;

    if (castFlags & CAST_FLAG_PREDICTED_POWER)              // predicted power
        data << uint32(0);

    if (castFlags & CAST_FLAG_PREDICTED_RUNES)              // predicted runes
    {
        uint8 v1 = 0;//m_runesState;
        uint8 v2 = 0;//((Player*)m_caster)->GetRunesState();
        data << uint8(v1);                                  // runes state before
        data << uint8(v2);                                  // runes state after
        for(uint8 i = 0; i < MAX_RUNES; ++i)
        {
            uint8 m = (1 << i);
            if (m & v1)                                      // usable before...
                if(!(m & v2))                               // ...but on cooldown now...
                    data << uint8(0);                       // some unknown byte (time?)
        }
    }

    if (castFlags & CAST_FLAG_AMMO)                         // projectile info
        WriteAmmoToPacket(&data);

    if (castFlags & CAST_FLAG_IMMUNITY)                     // cast immunity
    {
        data << uint32(0);                                  // used for SetCastSchoolImmunities
        data << uint32(0);                                  // used for SetCastImmunities
    }

    m_caster->SendMessageToSet(&data, true);
}

void Spell::SendSpellGo()
{
    // not send invisible spell casting
    if (!IsNeedSendToClient())
        return;

    DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Sending SMSG_SPELL_GO id=%u", m_spellInfo->Id);

    uint32 castFlags = CAST_FLAG_UNKNOWN9;
    if (IsRangedSpell())
        castFlags |= CAST_FLAG_AMMO;                        // arrows/bullets visual

    if ((m_caster->GetTypeId() == TYPEID_PLAYER) && (m_caster->getClass() == CLASS_DEATH_KNIGHT) && m_spellInfo->runeCostID)
    {
        castFlags |= CAST_FLAG_UNKNOWN19;                   // same as in SMSG_SPELL_START
        castFlags |= CAST_FLAG_PREDICTED_POWER;             // makes cooldowns visible
        castFlags |= CAST_FLAG_PREDICTED_RUNES;             // rune cooldowns list
    }

    if ((m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION) && (m_targets.GetSpeed() > M_NULL_F || m_targets.GetElevation() > M_NULL_F))
        castFlags |= CAST_FLAG_ADJUST_MISSILE;             // spell has trajectory (guess parameters)

    Unit *caster = m_triggeredByAuraSpell && IsChanneledSpell(m_triggeredByAuraSpell) ? GetAffectiveCaster() : m_caster;

    if (!caster)
        caster = m_caster;                                  // temporary. TODO - need find source of problem.

    WorldPacket data(SMSG_SPELL_GO, 50);                    // guess size

    if (m_CastItem)
        data << m_CastItem->GetPackGUID();
    else
        data << caster->GetPackGUID();

    data << caster->GetPackGUID();
    data << uint8(m_cast_count);                            // pending spell cast?
    data << uint32(m_spellInfo->Id);                        // spellId
    data << uint32(castFlags);                              // cast flags
    data << uint32(WorldTimer::getMSTime());                // timestamp

    WriteSpellGoTargets(&data);

    data << m_targets;

    if (castFlags & CAST_FLAG_PREDICTED_POWER)              // predicted power
        data << uint32(m_caster->GetPower(m_caster->getPowerType())); // Yes, it is really predicted power.

    if (castFlags & CAST_FLAG_PREDICTED_RUNES)              // predicted runes
    {
        uint8 v1 = m_runesState;
        uint8 v2 =  m_caster->getClass() == CLASS_DEATH_KNIGHT ? ((Player*)m_caster)->GetRunesState() : 0;
        data << uint8(v1);                                  // runes state before
        data << uint8(v2);                                  // runes state after
        for(uint8 i = 0; i < MAX_RUNES; ++i)
        {
            uint8 m = (1 << i);
            if (m & v1)                                      // usable before...
                if(!(m & v2))                               // ...but on cooldown now...
                    data << uint8(0);                       // some unknown byte (time?)
        }
    }

    if (castFlags & CAST_FLAG_ADJUST_MISSILE)               // adjust missile trajectory duration
    {
        data << float(m_targets.GetElevation());            // Elevation of missile
        data << uint32(m_delayMoment);                      // Calculated trajectory delay time.
    }

    if (castFlags & CAST_FLAG_AMMO)                         // projectile info
        WriteAmmoToPacket(&data);

    if (castFlags & CAST_FLAG_VISUAL_CHAIN)                 // spell visual chain effect
    {
        data << uint32(0);                                  // SpellVisual.dbc id?
        data << uint32(0);                                  // overrides previous field if > 0 and violencelevel client cvar < 2
    }

    if (m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION)
    {
        data << uint8(0);                                   // The value increase for each time, can remind of a cast count for the spell
    }

    if (m_targets.m_targetMask & TARGET_FLAG_VISUAL_CHAIN)  // probably used (or can be used) with CAST_FLAG_VISUAL_CHAIN flag
    {
        data << uint32(0);                                  // count

        //for(int = 0; i < count; ++i)
        //{
        //    // position and guid?
        //    data << float(0) << float(0) << float(0) << uint64(0);
        //}
    }

    m_caster->SendMessageToSet(&data, true);

    DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST,"Spell::SendSpellGo: %s cast spell %u on %s, targets count (mask %u) "SIZEFMTD" "SIZEFMTD" "SIZEFMTD,
                m_caster->GetObjectGuid().GetString().c_str(),
                m_spellInfo->Id,
                m_targets.getUnitTarget() ? m_targets.getUnitTarget()->GetObjectGuid().GetString().c_str() : "<none>",
                m_targets.m_targetMask,
                m_UniqueTargetInfo.size(),
                m_UniqueGOTargetInfo.size(),
                m_UniqueItemInfo.size()
                );
}

void Spell::WriteAmmoToPacket(WorldPacket* data)
{
    uint32 ammoInventoryType = 0;
    uint32 ammoDisplayID = 0;

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        Item *pItem = ((Player*)m_caster)->GetWeaponForAttack( RANGED_ATTACK );
        if (pItem)
        {
            ammoInventoryType = pItem->GetProto()->InventoryType;
            if ( ammoInventoryType == INVTYPE_THROWN )
                ammoDisplayID = pItem->GetProto()->DisplayInfoID;
            else
            {
                uint32 ammoID = ((Player*)m_caster)->GetUInt32Value(PLAYER_AMMO_ID);
                if (ammoID)
                {
                    ItemPrototype const *pProto = ObjectMgr::GetItemPrototype( ammoID );
                    if (pProto)
                    {
                        ammoDisplayID = pProto->DisplayInfoID;
                        ammoInventoryType = pProto->InventoryType;
                    }
                }
                else if (m_caster->GetDummyAura(46699))      // Requires No Ammo
                {
                    ammoDisplayID = 5996;                   // normal arrow
                    ammoInventoryType = INVTYPE_AMMO;
                }
            }
        }
    }
    else
    {
        for (uint8 i = 0; i < MAX_VIRTUAL_ITEM_SLOT; ++i)
        {
            if (uint32 item_id = m_caster->GetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID + i))
            {
                if (ItemEntry const * itemEntry = sItemStore.LookupEntry(item_id))
                {
                    if (itemEntry->Class == ITEM_CLASS_WEAPON)
                    {
                        switch(itemEntry->SubClass)
                        {
                            case ITEM_SUBCLASS_WEAPON_THROWN:
                                ammoDisplayID = itemEntry->DisplayId;
                                ammoInventoryType = itemEntry->InventoryType;
                                break;
                            case ITEM_SUBCLASS_WEAPON_BOW:
                            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                                ammoDisplayID = 5996;       // is this need fixing?
                                ammoInventoryType = INVTYPE_AMMO;
                                break;
                            case ITEM_SUBCLASS_WEAPON_GUN:
                                ammoDisplayID = 5998;       // is this need fixing?
                                ammoInventoryType = INVTYPE_AMMO;
                                break;
                        }

                        if (ammoDisplayID)
                            break;
                    }
                }
            }
        }
    }

    *data << uint32(ammoDisplayID);
    *data << uint32(ammoInventoryType);
}

void Spell::WriteSpellGoTargets(WorldPacket* data)
{
    size_t count_pos = data->wpos();
    *data << uint8(0);                                      // placeholder

    // This function also fill data for channeled spells:
    // m_needAliveTargetMask req for stop channeling if one target die
    uint32 hit  = m_UniqueGOTargetInfo.size();              // Always hits on GO
    uint32 miss = 0;

    for(TargetList::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (ihit->effectMask == 0)                          // No effect apply - all immuned add state
        {
            // possibly SPELL_MISS_IMMUNE2 for this??
            ihit->missCondition = SPELL_MISS_IMMUNE2;
            ++miss;
        }
        else if (ihit->missCondition == SPELL_MISS_NONE)    // Add only hits
        {
            ++hit;
            *data << ihit->targetGUID;
            m_needAliveTargetMask |= ihit->effectMask;
        }
        else
            ++miss;
    }

    for(GOTargetList::const_iterator ighit = m_UniqueGOTargetInfo.begin(); ighit != m_UniqueGOTargetInfo.end(); ++ighit)
        *data << ighit->targetGUID;                         // Always hits

    data->put<uint8>(count_pos, hit);

    *data << (uint8)miss;
    for(TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (ihit->missCondition != SPELL_MISS_NONE)         // Add only miss
        {
            *data << ihit->targetGUID;
            *data << uint8(ihit->missCondition);
            if (ihit->missCondition == SPELL_MISS_REFLECT)
                *data << uint8(ihit->reflectResult);
        }
    }
    // Reset m_needAliveTargetMask for non channeled spell
    if(!IsChanneledSpell(m_spellInfo))
        m_needAliveTargetMask = 0;
}

void Spell::SendLogExecute()
{
    uint32 count1 = 0;
    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (!m_effectExecuteData[i].empty())
            ++count1;
    }

    if (!count1)
        return;

    Unit *target = m_targets.getUnitTarget() ? m_targets.getUnitTarget() : m_caster;

    WorldPacket data(SMSG_SPELLLOGEXECUTE, 9 + 4 + 4 + 4 + 4 + 8);

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
        data << m_caster->GetPackGUID();
    else
        data << target->GetPackGUID();

    data << uint32(m_spellInfo->Id);
    data << uint32(count1);                                 // count1 (effects count)

    DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell::SendLogExecute() sended spelllog to %s (spell %u, effects count %u)", m_caster->GetObjectGuid().GetString().c_str(), m_spellInfo->Id, count1);

    for (uint32 i = 0; i < count1; ++i)
    {
        if (m_effectExecuteData[i].empty())
            continue;

        data << uint32(m_spellInfo->Effect[i]);             // spell effect
        data.append(m_effectExecuteData[i]);

        m_effectExecuteData[i].clear();
    }
    m_caster->SendMessageToSet(&data, true);
}

void Spell::SendEffectLogExecute(SpellEffectIndex eff, ObjectGuid targetGuid, uint32 data1, uint32 data2, float data3)
{
    if (eff >= MAX_EFFECT_INDEX || m_spellInfo->Effect[eff] == SPELL_EFFECT_NONE)
        return;

    ByteBuffer& data = m_effectExecuteData[eff];
    bool isFirst = data.empty();

    switch(m_spellInfo->Effect[eff])
    {
        // target guid and some data logged
        case SPELL_EFFECT_POWER_DRAIN:
        {
            // reserve one uint32 for counter, if need
            if (isFirst)
                data << uint32(0);
            data << targetGuid.WriteAsPacked();
            data << data1;
            data << data2;
            data << data3;
            break;
        }
        // target guid and some data logged
        case SPELL_EFFECT_DURABILITY_DAMAGE:
        {
            // reserve one uint32 for counter, if need
            if (isFirst)
                data << uint32(0);
            data << targetGuid.WriteAsPacked();
            data << data1;
            data << data2;
            break;
        }
        // target guid and some data logged
        case SPELL_EFFECT_ADD_EXTRA_ATTACKS:
        case SPELL_EFFECT_INTERRUPT_CAST:
        {
            // reserve one uint32 for counter, if need
            if (isFirst)
                data << uint32(0);
            data << targetGuid.WriteAsPacked();
            data << data1;
            break;
        }
        // only entry of item used/created
        case SPELL_EFFECT_CREATE_ITEM:
        case SPELL_EFFECT_CREATE_ITEM_2:
        case SPELL_EFFECT_FEED_PET:
        {
            // reserve one uint32 for counter, if need
            if (isFirst)
                data << uint32(0);
            data <<    data1;
            break;
        }
        // only ObjectGuid of target logged
        case SPELL_EFFECT_OPEN_LOCK:
        case SPELL_EFFECT_SUMMON:
        case SPELL_EFFECT_TRANS_DOOR:
        case SPELL_EFFECT_SUMMON_PET:
        case SPELL_EFFECT_SUMMON_OBJECT_WILD:
        case SPELL_EFFECT_CREATE_HOUSE:
        case SPELL_EFFECT_DUEL:
        case SPELL_EFFECT_SUMMON_OBJECT_SLOT1:
        case SPELL_EFFECT_SUMMON_OBJECT_SLOT2:
        case SPELL_EFFECT_SUMMON_OBJECT_SLOT3:
        case SPELL_EFFECT_SUMMON_OBJECT_SLOT4:
        case SPELL_EFFECT_DISMISS_PET:
        case SPELL_EFFECT_RESURRECT:
        case SPELL_EFFECT_RESURRECT_NEW:
        {
            // reserve one uint32 for counter, if need
            if (isFirst)
                data << uint32(0);
            data << targetGuid.WriteAsPacked();
            break;
        }
        // nothing logged, no log data for effect
        default:
        {
            // nothing need
            return;
        }
    }
    uint32 count = m_effectExecuteData[eff].read<uint32>(0);
    m_effectExecuteData[eff].put<uint32>(0, ++count);
}

void Spell::SendInterrupted(uint8 result)
{
    if (!m_caster || !m_caster->IsInWorld())
        return;

    WorldPacket data(SMSG_SPELL_FAILURE, m_caster->GetPackGUID().size() + 1 + 4 + 1);
    data << m_caster->GetPackGUID();
    data << uint8(m_cast_count);
    data << uint32(m_spellInfo->Id);
    data << uint8(result);
    m_caster->SendMessageToSet(&data, true);

    data.Initialize(SMSG_SPELL_FAILED_OTHER, 8 + 1 + 4 + 1);
    data << m_caster->GetObjectGuid();
    data << uint8(m_cast_count);
    data << uint32(m_spellInfo->Id);
    data << uint8(result);
    m_caster->SendMessageToSet(&data, true);
}

void Spell::SendChannelUpdate(uint32 time)
{
    if (time == 0)
    {
        // Temp solution (ha-a-ack...) from cmangos - need rewrite to UnitStates
        // Reset farsight for some possessing auras of possessed summoned (as they might work with different aura types)
        if (m_spellInfo->HasAttribute(SPELL_ATTR_EX_FARSIGHT) && m_caster->GetTypeId() == TYPEID_PLAYER && m_caster->GetCharmGuid()
                && !IsSpellHaveAura(m_spellInfo, SPELL_AURA_MOD_POSSESS) && !IsSpellHaveAura(m_spellInfo, SPELL_AURA_MOD_POSSESS_PET))
        {
            Player* player = (Player*)m_caster;
            // These Auras are applied to self, so get the possessed first
            Unit* possessed = player->GetCharm();

            player->SetCharm(NULL);
            if (possessed)
                player->SetClientControl(possessed, 0);
            player->SetMover(NULL);
            player->SetViewPoint(NULL);
            player->RemovePetActionBar();

            if (possessed)
            {
                possessed->clearUnitState(UNIT_STAT_CONTROLLED);
                possessed->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);
                possessed->SetCharmerGuid(ObjectGuid());
                // TODO - Requires more specials for target?

                // Some possessed might want to despawn?
                if (possessed->GetUInt32Value(UNIT_CREATED_BY_SPELL) == m_spellInfo->Id && possessed->GetTypeId() == TYPEID_UNIT)
                    ((Creature*)possessed)->ForcedDespawn();
            }
        }
        else if (m_spellInfo->HasAttribute(SPELL_ATTR_EX_FARSIGHT) && m_caster->GetTypeId() == TYPEID_PLAYER)
        {
            if (/*DynamicObject* dynObj = */m_caster->GetDynObject(m_triggeredByAuraSpell ? m_triggeredByAuraSpell->Id : m_spellInfo->Id))
                ((Player*)m_caster)->SetViewPoint(NULL);
        }

        m_caster->RemoveAurasByCasterSpell(m_spellInfo->Id, m_caster->GetObjectGuid());

        ObjectGuid target_guid = m_caster->GetChannelObjectGuid();
        if (target_guid != m_caster->GetObjectGuid() && target_guid.IsUnit())
            if (Unit* target = ObjectAccessor::GetUnit(*m_caster, target_guid))
                target->RemoveAurasByCasterSpell(m_spellInfo->Id, m_caster->GetObjectGuid());

        // Only finish channeling when latest channeled spell finishes
        if (m_caster->GetUInt32Value(UNIT_CHANNEL_SPELL) != m_spellInfo->Id)
            return;

        m_caster->SetChannelObjectGuid(ObjectGuid());
        m_caster->SetUInt32Value(UNIT_CHANNEL_SPELL, 0);
    }

    WorldPacket data(MSG_CHANNEL_UPDATE, m_caster->GetPackGUID().size() + 4);
    data << m_caster->GetPackGUID();
    data << uint32(time);
    m_caster->SendMessageToSet(&data, true);
}

void Spell::SendChannelStart(uint32 duration)
{
    WorldObject* target = NULL;

    // select dynobject created by first effect if any
    if (m_spellInfo->Effect[EFFECT_INDEX_0] == SPELL_EFFECT_PERSISTENT_AREA_AURA)
        target = m_caster->GetDynObject(m_spellInfo->Id, EFFECT_INDEX_0);
    // select first not resisted target from target list for _0_ effect
    else if (!m_UniqueTargetInfo.empty())
    {
        for(TargetList::const_iterator itr = m_UniqueTargetInfo.begin(); itr != m_UniqueTargetInfo.end(); ++itr)
        {
            if ((itr->effectMask & (1 << EFFECT_INDEX_0)) && itr->reflectResult == SPELL_MISS_NONE &&
                itr->targetGUID != m_caster->GetObjectGuid())
            {
                target = ObjectAccessor::GetUnit(*m_caster, itr->targetGUID);
                break;
            }
        }
    }
    else if(!m_UniqueGOTargetInfo.empty())
    {
        for(GOTargetList::const_iterator itr = m_UniqueGOTargetInfo.begin(); itr != m_UniqueGOTargetInfo.end(); ++itr)
        {
            if (itr->effectMask & (1 << EFFECT_INDEX_0))
            {
                target = m_caster->GetMap()->GetGameObject(itr->targetGUID);
                break;
            }
        }
    }

    WorldPacket data(MSG_CHANNEL_START, m_caster->GetPackGUID().size() + 4 + 4);
    data << m_caster->GetPackGUID();
    data << uint32(m_spellInfo->Id);
    data << uint32(duration);
    m_caster->SendMessageToSet(&data, true);

    m_timer = duration;

    if (target)
        m_caster->SetChannelObjectGuid(target->GetObjectGuid());

    m_caster->SetUInt32Value(UNIT_CHANNEL_SPELL, m_spellInfo->Id);
}

void Spell::SendResurrectRequest(Player* target)
{
    // get ressurector name for creature resurrections, otherwise packet will be not accepted
    // for player resurrections the name is looked up by guid
    char const* resurrectorName = m_caster->GetTypeId() == TYPEID_PLAYER ? "" : m_caster->GetNameForLocaleIdx(target->GetSession()->GetSessionDbLocaleIndex());

    WorldPacket data(SMSG_RESURRECT_REQUEST, (8+4+strlen(resurrectorName)+1+1+1+4));
    // resurrector guid
    data << m_caster->GetObjectGuid();
    data << uint32(strlen(resurrectorName) + 1);
    data << resurrectorName;
    // null terminator
    data << uint8(0);
    data << uint8(m_caster->GetTypeId() == TYPEID_PLAYER ? 0 : 1); // "you'll be afflicted with resurrection sickness"
    // override delay sent with SMSG_CORPSE_RECLAIM_DELAY, set instant resurrection for spells with this attribute
    if (m_spellInfo->HasAttribute(SPELL_ATTR_EX3_IGNORE_RESURRECTION_TIMER))
        data << uint32(0);

    target->GetSession()->SendPacket(&data);
}

void Spell::TakeCastItem()
{
    if(!m_CastItem || m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    // not remove cast item at triggered spell (equipping, weapon damage, etc)
    if (m_IsTriggeredSpell && !(m_targets.m_targetMask & TARGET_FLAG_TRADE_ITEM))
        return;

    ItemPrototype const *proto = m_CastItem->GetProto();

    if(!proto)
    {
        // This code is to avoid a crash
        // I'm not sure, if this is really an error, but I guess every item needs a prototype
        sLog.outError("Cast item (%s) has no item prototype", m_CastItem->GetGuidStr().c_str());
        return;
    }

    bool expendable = false;
    bool withoutCharges = false;

    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (proto->Spells[i].SpellId)
        {
            // item has limited charges
            if (proto->Spells[i].SpellCharges)
            {
                if (proto->Spells[i].SpellCharges < 0 && !(proto->ExtraFlags & ITEM_EXTRA_NON_CONSUMABLE))
                    expendable = true;

                int32 charges = m_CastItem->GetSpellCharges(i);

                // item has charges left
                if (charges)
                {
                    (charges > 0) ? --charges : ++charges;  // abs(charges) less at 1 after use
                    if (proto->Stackable == 1)
                        m_CastItem->SetSpellCharges(i, charges);
                    m_CastItem->SetState(ITEM_CHANGED, (Player*)m_caster);
                }

                // all charges used
                withoutCharges = (charges == 0);
            }
        }
    }

    if (expendable && withoutCharges)
    {
        uint32 count = 1;
        ((Player*)m_caster)->DestroyItemCount(m_CastItem, count, true);

        // prevent crash at access to deleted m_targets.getItemTarget
        ClearCastItem();
    }
}

void Spell::TakePower()
{
    if (m_CastItem || m_triggeredByAuraSpell)
        return;

    // health as power used
    if (m_spellInfo->GetPowerType() == POWER_HEALTH)
    {
        m_caster->ModifyHealth( -(int32)m_powerCost );
        return;
    }

    if (m_spellInfo->GetPowerType() >= MAX_POWERS)
    {
        sLog.outError("Spell::TakePower: Unknown power type '%d'", m_spellInfo->GetPowerType());
        return;
    }

    Powers powerType = Powers(m_spellInfo->GetPowerType());

    if (powerType == POWER_RUNE)
    {
        CheckOrTakeRunePower(true);
        return;
    }

    bool needApplyMod = false;
    for (TargetList::const_iterator itr = m_UniqueTargetInfo.begin(); itr != m_UniqueTargetInfo.end(); ++itr)
    {
        if (itr->missCondition != SPELL_MISS_NONE)
        {
            needApplyMod = true;
            break;
        }
    }

    if (needApplyMod)
        if (Player* modOwner = m_caster->GetSpellModOwner())
            modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_COST_ON_HIT_FAIL, m_powerCost);

    m_caster->ModifyPower(powerType, -(int32)m_powerCost);

    // Set the five second timer
    if (powerType == POWER_MANA && m_powerCost > 0)
    {
        uint32 delay = 0;
        if (m_caster->GetCharmInfo())
            delay = m_caster->GetCharmInfo()->GetGlobalCooldownMgr().GetGlobalCooldown(m_spellInfo);
        else if (m_caster->GetTypeId() == TYPEID_PLAYER)
            delay = ((Player*)m_caster)->GetGlobalCooldownMgr().HasGlobalCooldown(m_spellInfo);

        m_caster->AddEvent(new ManaUseEvent(*m_caster), delay);
    }
}

SpellCastResult Spell::CheckOrTakeRunePower(bool take)
{
    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return SPELL_CAST_OK;

    Player *plr = (Player*)m_caster;

    if (plr->getClass() != CLASS_DEATH_KNIGHT)
        return SPELL_CAST_OK;

    SpellRuneCostEntry const *src = sSpellRuneCostStore.LookupEntry(m_spellInfo->runeCostID);

    if(!src)
        return SPELL_CAST_OK;

    if (src->NoRuneCost() && (!take || src->NoRunicPowerGain()))
        return SPELL_CAST_OK;

    if (take)
        m_runesState = plr->GetRunesState();                // store previous state

    // at this moment for rune cost exist only no cost mods, and no percent mods
    int32 runeCostMod = 10000;
    if (Player* modOwner = plr->GetSpellModOwner())
        modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_COST, runeCostMod);

    if (runeCostMod > 0)
    {
        int32 runeCost[NUM_RUNE_TYPES];                         // blood, frost, unholy, death

        // init cost data and apply mods
        for(uint32 i = 0; i < RUNE_DEATH; ++i)
            runeCost[i] = runeCostMod > 0 ? src->RuneCost[i] : 0;

        runeCost[RUNE_DEATH] = 0;                               // calculated later

        // scan non-death runes (death rune not used explicitly in rune costs)
        for(uint32 i = 0; i < MAX_RUNES; ++i)
        {
            RuneType rune = plr->GetCurrentRune(i);
            if (runeCost[rune] <= 0)
                continue;

            // already used
            if (plr->GetRuneCooldown(i) != 0)
                continue;

            if (take)
                plr->SetRuneCooldown(i, RUNE_COOLDOWN);         // 5*2=10 sec

            --runeCost[rune];
        }

        // collect all not counted rune costs to death runes cost
        for(uint32 i = 0; i < RUNE_DEATH; ++i)
            if (runeCost[i] > 0)
                runeCost[RUNE_DEATH] += runeCost[i];

        // scan death runes
        if (runeCost[RUNE_DEATH] > 0)
        {
            for(uint32 i = 0; i < MAX_RUNES && runeCost[RUNE_DEATH]; ++i)
            {
                RuneType rune = plr->GetCurrentRune(i);
                if (rune != RUNE_DEATH)
                    continue;

                // already used
                if (plr->GetRuneCooldown(i) != 0)
                    continue;

                if (take)
                    plr->SetRuneCooldown(i, RUNE_COOLDOWN); // 5*2=10 sec

                --runeCost[rune];

                if (take)
                {
                    plr->ConvertRune(i, plr->GetBaseRune(i));
                    plr->ClearConvertedBy(i);
                }
            }
        }

        if(!take && runeCost[RUNE_DEATH] > 0)
            return SPELL_FAILED_NO_POWER;                       // not sure if result code is correct
    }

    if (take)
    {
        // you can gain some runic power when use runes
        float rp = float(src->runePowerGain);
        rp *= sWorld.getConfig(CONFIG_FLOAT_RATE_POWER_RUNICPOWER_INCOME);
        plr->ModifyPower(POWER_RUNIC_POWER, (int32)rp);
    }

    return SPELL_CAST_OK;
}

void Spell::TakeAmmo()
{
    if (m_attackType == RANGED_ATTACK && m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        Item* pItem = ((Player*)m_caster)->GetWeaponForAttack(RANGED_ATTACK, true, false);

        // wands don't have ammo
        if (!pItem || pItem->GetProto()->SubClass == ITEM_SUBCLASS_WEAPON_WAND)
            return;

        if (pItem->GetProto()->InventoryType == INVTYPE_THROWN)
        {
            if (pItem->GetMaxStackCount() == 1)
            {
                // decrease durability for non-stackable throw weapon
                ((Player*)m_caster)->DurabilityPointLossForEquipSlot(EQUIPMENT_SLOT_RANGED);
            }
            else
            {
                // decrease items amount for stackable throw weapon
                uint32 count = 1;
                ((Player*)m_caster)->DestroyItemCount(pItem, count, true);
            }
        }
        else if (uint32 ammo = ((Player*)m_caster)->GetUInt32Value(PLAYER_AMMO_ID))
            ((Player*)m_caster)->DestroyItemCount(ammo, 1, true);
    }
}


void Spell::TakeReagents()
{
    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    if (IgnoreItemRequirements())                           // reagents used in triggered spell removed by original spell or don't must be removed.
        return;

    Player* p_caster = (Player*)m_caster;
    if (p_caster->CanNoReagentCast(m_spellInfo))
        return;

    for(uint32 x = 0; x < MAX_SPELL_REAGENTS; ++x)
    {
        if (m_spellInfo->Reagent[x] <= 0)
            continue;

        uint32 itemid = m_spellInfo->Reagent[x];
        uint32 itemcount = m_spellInfo->ReagentCount[x];

        // if CastItem is also spell reagent
        if (m_CastItem)
        {
            ItemPrototype const *proto = m_CastItem->GetProto();
            if ( proto && proto->ItemId == itemid )
            {
                for(int s = 0; s < MAX_ITEM_PROTO_SPELLS; ++s)
                {
                    // CastItem will be used up and does not count as reagent
                    int32 charges = m_CastItem->GetSpellCharges(s);
                    if (proto->Spells[s].SpellCharges < 0 && abs(charges) < 2)
                    {
                        ++itemcount;
                        break;
                    }
                }

                m_CastItem = NULL;
            }
        }

        // if getItemTarget is also spell reagent
        if (m_targets.getItemTargetEntry() == itemid)
            m_targets.setItemTarget(NULL);

        p_caster->DestroyItemCount(itemid, itemcount, true);
    }
}

void Spell::HandleThreatSpells()
{
    if (m_UniqueTargetInfo.empty())
        return;

    SpellThreatEntry const* threatEntry = sSpellMgr.GetSpellThreatEntry(m_spellInfo->Id);

    if (!threatEntry || (!threatEntry->threat && fabs(threatEntry->ap_bonus) < M_NULL_F))
        return;

    float threat = threatEntry->threat;
    if (fabs(threatEntry->ap_bonus) > M_NULL_F)
        threat += threatEntry->ap_bonus * m_caster->GetTotalAttackPowerValue(GetWeaponAttackType(m_spellInfo));

    bool positive = true;
    uint8 effectMask = 0;
    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
        if (m_spellInfo->Effect[i])
            effectMask |= (1<<i);

    if (m_negativeEffectMask & effectMask)
    {
        // can only handle spells with clearly defined positive/negative effect, check at spell_threat loading probably not perfect
        // so abort when only some effects are negative.
        if ((m_negativeEffectMask & effectMask) != effectMask)
        {
            DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell %u, rank %u, is not clearly positive or negative, ignoring bonus threat", m_spellInfo->Id, sSpellMgr.GetSpellRank(m_spellInfo->Id));
            return;
        }
        positive = false;
    }

    // since 2.0.1 threat from positive effects also is distributed among all targets, so the overall caused threat is at most the defined bonus
    threat /= m_UniqueTargetInfo.size();

    for (TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (ihit->missCondition != SPELL_MISS_NONE)
            continue;

        Unit* target = m_caster->GetObjectGuid() == ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);
        if (!target)
            continue;

        // positive spells distribute threat among all units that are in combat with target, like healing
        if (positive)
        {
            target->getHostileRefManager().threatAssist(m_caster /*real_caster ??*/, threat, m_spellInfo);
        }
        // for negative spells threat gets distributed among affected targets
        else
        {
            if (!target->CanHaveThreatList())
                continue;

            target->AddThreat(m_caster, threat, false, GetSpellSchoolMask(m_spellInfo), m_spellInfo);
        }
    }

    DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell %u added an additional %f threat for %s %u target(s)", m_spellInfo->Id, threat, positive ? "assisting" : "harming", uint32(m_UniqueTargetInfo.size()));
}

void Spell::HandleEffects(Unit *pUnitTarget, Item *pItemTarget, GameObject *pGOTarget, SpellEffectIndex i)
{
    unitTarget = pUnitTarget;
    itemTarget = pItemTarget;
    gameObjTarget = pGOTarget;

    uint8 eff = m_spellInfo->Effect[i];

    damage = CalculateDamage(i, unitTarget);

    DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell %u Effect%d : %u Targets: %s, %s, %s",
        m_spellInfo->Id, i, eff,
        unitTarget ? unitTarget->GetGuidStr().c_str() : "-",
        itemTarget ? itemTarget->GetGuidStr().c_str() : "-",
        gameObjTarget ? gameObjTarget->GetGuidStr().c_str() : "-");

    if (eff < TOTAL_SPELL_EFFECTS)
    {
        (*this.*SpellEffects[eff])(i);
    }
    else
    {
        sLog.outError("WORLD: Spell FX %d > TOTAL_SPELL_EFFECTS ", eff);
    }

    // store effect index who did the damage (for picking correct damage multiplier)
    if (m_damage > 0)
        m_damageIndex = i;
}

void Spell::AddPrecastSpell(uint32 spellId)
{
    SpellEntry const* spellInfo = sSpellStore.LookupEntry(spellId);
    if (!spellInfo)
    {
        sLog.outError("Spell::AddPrecastSpell: unknown spell id %u used as pre-cast spell for spell %u)", spellId, m_spellInfo->Id);
        return;
    }
    m_preCastSpells.push_back(spellInfo);
}

void Spell::AddTriggeredSpell(uint32 spellId)
{
    SpellEntry const* spellInfo = sSpellStore.LookupEntry(spellId);
    if (!spellInfo)
    {
        sLog.outError("Spell::AddTriggeredSpell: unknown spell id %u used as triggred spell for spell %u)", spellId, m_spellInfo->Id);
        return;
    }
    m_TriggerSpells.push_back(spellInfo);
}

void Spell::AddNotTriggeredSpell(uint32 spellId)
{
    SpellEntry const* spellInfo = sSpellStore.LookupEntry(spellId);
    if (!spellInfo)
    {
        sLog.outError("Spell::AddNotTriggeredSpell: unknown spell id %u used as not triggred spell for spell %u)", spellId, m_spellInfo->Id);
        return;
    }
    m_NotTriggerSpells.push_back(spellInfo);
}

void Spell::CastPreCastSpells(Unit* target)
{
    for (SpellInfoList::const_iterator si = m_preCastSpells.begin(); si != m_preCastSpells.end(); ++si)
        m_caster->CastSpell(target, (*si), true, m_CastItem);
}

void Spell::CastTriggerSpells()
{
    for (SpellInfoList::const_iterator si = m_TriggerSpells.begin(); si != m_TriggerSpells.end(); ++si)
    {
        Spell* spell = new Spell(m_caster, (*si), true, m_originalCasterGuid);
        spell->prepare(&m_targets);                         // use original spell original targets
    }
}
void Spell::CastNotTriggerSpells()
{
    for (SpellInfoList::const_iterator si = m_NotTriggerSpells.begin(); si != m_NotTriggerSpells.end(); ++si)
    {
        Spell* spell = new Spell(m_caster, (*si), false, m_originalCasterGuid);
        spell->prepare(&m_targets);                         // use original spell original targets
    }
}

Unit* Spell::GetPrefilledUnitTargetOrUnitTarget(SpellEffectIndex effIndex) const
{
    for (TargetList::const_iterator itr = m_UniqueTargetInfo.begin(); itr != m_UniqueTargetInfo.end(); ++itr)
        if (itr->effectMask & (1 << effIndex))
            return m_caster->GetMap()->GetUnit(itr->targetGUID);

    return m_targets.getUnitTarget();
}

SpellCastResult Spell::CheckCast(bool strict)
{
    // Festive Holiday Mount
    if (m_spellInfo->Id == 62061 && m_caster->HasAura(62061, EFFECT_INDEX_0))
        return SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW;

    // check cooldowns to prevent cheating (ignore passive spells, that client side visual only)
    if (!m_spellInfo->HasAttribute(SPELL_ATTR_PASSIVE) && m_caster->HasSpellCooldown(m_spellInfo))
    {
        if (m_triggeredByAuraSpell)
            return SPELL_FAILED_DONT_REPORT;
        else
            return SPELL_FAILED_NOT_READY;
    }

    // check global cooldown
    if (strict && !m_IsTriggeredSpell && HasGlobalCooldown())
        return SPELL_FAILED_NOT_READY;

    // only allow triggered spells if at an ended battleground
    if (!m_IsTriggeredSpell && m_caster->GetTypeId() == TYPEID_PLAYER)
        if (BattleGround * bg = ((Player*)m_caster)->GetBattleGround())
            if (bg->GetStatus() == STATUS_WAIT_LEAVE)
                return SPELL_FAILED_DONT_REPORT;

    if (!m_IsTriggeredSpell && IsNonCombatSpell(m_spellInfo) &&
        m_caster->isInCombat() && !m_caster->IsIgnoreUnitState(m_spellInfo, IGNORE_UNIT_COMBAT_STATE))
        return SPELL_FAILED_AFFECTING_COMBAT;

    if (m_caster->GetTypeId() == TYPEID_PLAYER && !((Player*)m_caster)->isGameMaster() &&
        sWorld.getConfig(CONFIG_BOOL_VMAP_INDOOR_CHECK) &&
        VMAP::VMapFactory::createOrGetVMapManager()->isLineOfSightCalcEnabled())
    {
        if (m_spellInfo->HasAttribute(SPELL_ATTR_OUTDOORS_ONLY) &&
                !m_caster->GetTerrain()->IsOutdoors(m_caster->GetPositionX(), m_caster->GetPositionY(), m_caster->GetPositionZ()))
            return SPELL_FAILED_ONLY_OUTDOORS;

        if(m_spellInfo->HasAttribute(SPELL_ATTR_INDOORS_ONLY) &&
                m_caster->GetTerrain()->IsOutdoors(m_caster->GetPositionX(), m_caster->GetPositionY(), m_caster->GetPositionZ()))
            return SPELL_FAILED_ONLY_INDOORS;
    }
    // only check at first call, Stealth auras are already removed at second call
    // for now, ignore triggered spells
    if (strict && !m_IsTriggeredSpell)
    {
        // Ignore form req aura
        if (!m_caster->HasAffectedAura(SPELL_AURA_MOD_IGNORE_SHAPESHIFT, m_spellInfo))
        {
            // Cannot be used in this stance/form
            SpellCastResult shapeError = GetErrorAtShapeshiftedCast(m_spellInfo, m_caster->GetShapeshiftForm());
            if (shapeError != SPELL_CAST_OK)
                return shapeError;

            if (m_spellInfo->HasAttribute(SPELL_ATTR_ONLY_STEALTHED) && !(m_caster->HasStealthAura()))
                return SPELL_FAILED_ONLY_STEALTHED;
        }
    }

    // caster state requirements
    if (m_spellInfo->CasterAuraState && !m_caster->HasAuraState(AuraState(m_spellInfo->CasterAuraState)))
        return SPELL_FAILED_CASTER_AURASTATE;
    if (m_spellInfo->CasterAuraStateNot && m_caster->HasAuraState(AuraState(m_spellInfo->CasterAuraStateNot)))
        return SPELL_FAILED_CASTER_AURASTATE;

    // Caster aura req check if need
    if (m_spellInfo->casterAuraSpell && !m_caster->HasAura(m_spellInfo->casterAuraSpell))
        return SPELL_FAILED_CASTER_AURASTATE;
    if (m_spellInfo->excludeCasterAuraSpell)
    {
        // Special cases of non existing auras handling
        if (m_spellInfo->excludeCasterAuraSpell == 61988)
        {
            // Avenging Wrath Marker
            if (m_caster->HasAura(61987))
                return SPELL_FAILED_CASTER_AURASTATE;
        }
        else if (m_caster->HasAura(m_spellInfo->excludeCasterAuraSpell))
            return SPELL_FAILED_CASTER_AURASTATE;
    }

    if (m_caster->isCharmedOwnedByPlayerOrPlayer())
    {
        // cancel autorepeat spells if cast start when moving
        // (not wand currently autorepeat cast delayed to moving stop anyway in spell update code)
        if (m_caster->GetTypeId() == TYPEID_PLAYER && ((Player*)m_caster)->isMoving())
        {
            // skip stuck spell to allow use it in falling case and apply spell limitations at movement
            if ((!((Player*)m_caster)->m_movementInfo.HasMovementFlag(MOVEFLAG_FALLINGFAR) || m_spellInfo->Effect[EFFECT_INDEX_0] != SPELL_EFFECT_STUCK) &&
                (IsAutoRepeat() || (m_spellInfo->AuraInterruptFlags & AURA_INTERRUPT_FLAG_NOT_SEATED) != 0))
                return SPELL_FAILED_MOVING;
        }

        if (m_caster->hasUnitState(UNIT_STAT_STUNNED))
        {
            if (m_spellInfo->SpellFamilyName == SPELLFAMILY_PRIEST && m_spellInfo->GetSpellIconID() == 2178)
            {
                // Glyph of Desperation
                if (m_caster->HasAura(63248))
                    return SPELL_CAST_OK;
                else return SPELL_FAILED_STUNNED;
            }
        }

        // Disengage-like spells allow use only in combat
        if (m_spellInfo->HasAttribute(SPELL_ATTR_STOP_ATTACK_TARGET) && m_spellInfo->HasAttribute(SPELL_ATTR_EX2_UNK26) && !m_caster->isInCombat())
            return SPELL_FAILED_CASTER_AURASTATE;

        if (!m_IsTriggeredSpell
            && NeedsComboPoints(m_spellInfo)
            && !m_caster->IsIgnoreUnitState(m_spellInfo, IGNORE_UNIT_TARGET_STATE)
            && (!m_targets.getUnitTarget() || m_targets.getUnitTarget()->GetObjectGuid() != m_caster->GetComboTargetGuid()))
            // warrior not have real combo-points at client side but use this way for mark allow Overpower use (need really recheck this, for pets alsO)
            return (m_caster->GetObjectGuid().IsPet() || m_caster->getClass() == CLASS_WARRIOR) ? SPELL_FAILED_CASTER_AURASTATE : SPELL_FAILED_NO_COMBO_POINTS;
    }

    Unit *target = m_targets.getUnitTarget();
    if (target && target->IsInWorld())
    {
        // target state requirements (not allowed state), apply to self also
        // This check not need - checked in CheckTarget()
        // if (m_spellInfo->TargetAuraStateNot && target->HasAuraState(AuraState(m_spellInfo->TargetAuraStateNot)))
        //    return SPELL_FAILED_TARGET_AURASTATE;

        if (!m_IsTriggeredSpell && IsDeathOnlySpell(m_spellInfo) && target->isAlive())
            return SPELL_FAILED_TARGET_NOT_DEAD;

        // Target aura req check if need
        // This check fully not need - checked in CheckTarget()
        // if (m_spellInfo->targetAuraSpell && !target->HasAura(m_spellInfo->targetAuraSpell))
        //    return SPELL_FAILED_CASTER_AURASTATE;

        if (m_spellInfo->excludeTargetAuraSpell)
        {
            // Special cases of non existing auras handling
            if (m_spellInfo->excludeTargetAuraSpell == 61988)
            {
                // Avenging Wrath Marker
                if (target->HasAura(61987))
                    return SPELL_FAILED_CASTER_AURASTATE;

            }
            else if (target->HasAura(m_spellInfo->excludeTargetAuraSpell))
                return SPELL_FAILED_CASTER_AURASTATE;
        }

        // totem immunity for channeled spells(needs to be before spell cast)
        // spell attribs for player channeled spells
        // (from rsa - very strange attributes set...)
        if ((m_spellInfo->HasAttribute(SPELL_ATTR_EX_CHANNEL_TRACKING_TARGET))
            && (m_spellInfo->HasAttribute(SPELL_ATTR_EX5_AFFECTED_BY_HASTE))
            && target->GetTypeId() == TYPEID_UNIT
            && ((Creature*)target)->IsTotem())
            return SPELL_FAILED_IMMUNE;

        bool non_caster_target = target != m_caster && !IsSpellWithCasterSourceTargetsOnly(m_spellInfo);

        if (non_caster_target)
        {
            // target state requirements (apply to non-self only), to allow cast affects to self like Dirty Deeds
            if (m_spellInfo->TargetAuraState && !target->HasAuraStateForCaster(AuraState(m_spellInfo->TargetAuraState), m_caster->GetObjectGuid()) &&
                !m_caster->IsIgnoreUnitState(m_spellInfo, m_spellInfo->TargetAuraState == AURA_STATE_FROZEN ? IGNORE_UNIT_TARGET_NON_FROZEN : IGNORE_UNIT_TARGET_STATE))
                return SPELL_FAILED_TARGET_AURASTATE;

            // Not allow direct casting on flying player (enable only server-side casts)
            if (target->IsTaxiFlying() && !m_spellInfo->HasAttribute(SPELL_ATTR_HIDE_IN_COMBAT_LOG))
                return SPELL_FAILED_BAD_TARGETS;

            if(!m_IsTriggeredSpell && !target->IsVisibleTargetForSpell(m_caster, m_spellInfo))
                return SPELL_FAILED_LINE_OF_SIGHT;

            // auto selection spell rank implemented in WorldSession::HandleCastSpellOpcode
            // this case can be triggered if rank not found (too low-level target for first rank)
            if (m_caster->GetTypeId() == TYPEID_PLAYER && !m_CastItem && !m_IsTriggeredSpell)
            {
                // spell expected to be auto-downranking in cast handle, so must be same
                if (m_spellInfo != sSpellMgr.SelectAuraRankForLevel(m_spellInfo, target->getLevel()))
                    return SPELL_FAILED_LOWLEVEL;
            }

            if (m_caster->GetTypeId() == TYPEID_PLAYER && m_spellInfo->Mechanic == MECHANIC_DISARM)
            {
                if (target->GetTypeId() == TYPEID_PLAYER)
                {
                    Player *player = (Player*)target;
                    if (m_spellInfo->Id == 51722)                           // Dismantle
                    {
                        if (!player->GetWeaponForAttack(BASE_ATTACK) && !player->GetShield() && !player->GetWeaponForAttack(RANGED_ATTACK))
                            return SPELL_FAILED_TARGET_NO_WEAPONS;
                    }
                    else if ((!player->GetWeaponForAttack(BASE_ATTACK) && !player->GetWeaponForAttack(RANGED_ATTACK)) || !player->IsUsingEquippedWeapon(true))
                    {
                        return SPELL_FAILED_TARGET_NO_WEAPONS;
                    }
                }
                else if (!target->GetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID))
                    return SPELL_FAILED_TARGET_NO_WEAPONS;
            }
        }
        else if (m_caster == target)
        {
            if (m_caster->GetTypeId() == TYPEID_PLAYER && m_caster->IsInWorld())
            {
                // Additional check for some spells
                // If 0 spell effect empty - client not send target data (need use selection)
                // TODO: check it on next client version
                if (m_targets.m_targetMask == TARGET_FLAG_SELF &&
                    m_spellInfo->EffectImplicitTargetA[EFFECT_INDEX_1] == TARGET_CHAIN_DAMAGE)
                {
                    target = m_caster->GetMap()->GetUnit(((Player *)m_caster)->GetSelectionGuid());
                    if (!target)
                        return SPELL_FAILED_BAD_TARGETS;

                    m_targets.setUnitTarget(target);
                }
            }

            // Some special spells with non-caster only mode

            // Fire Shield
            if (m_spellInfo->SpellFamilyName == SPELLFAMILY_WARLOCK &&
                m_spellInfo->GetSpellIconID() == 16)
                return SPELL_FAILED_BAD_TARGETS;

            // Focus Magic (main spell)
            if (m_spellInfo->Id == 54646)
                return SPELL_FAILED_BAD_TARGETS;

            // Lay on Hands (self cast)
            if (m_spellInfo->SpellFamilyName == SPELLFAMILY_PALADIN &&
                m_spellInfo->GetSpellFamilyFlags().test<CF_PALADIN_LAY_ON_HANDS>())
            {
                if (target->HasAura(25771))                 // Forbearance
                    return SPELL_FAILED_CASTER_AURASTATE;
                if (target->HasAura(61987))                 // Avenging Wrath Marker
                    return SPELL_FAILED_CASTER_AURASTATE;
            }
        }

        // check pet presents
        for (int j = 0; j < MAX_EFFECT_INDEX; ++j)
        {
            if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_PET)
            {
                Pet *pet = m_caster->GetPet();
                if (!pet || (!pet->isAlive() && !IsSpellAllowDeadTarget(m_spellInfo)))
                {
                    if (m_triggeredByAuraSpell)              // not report pet not existence for triggered spells
                        return SPELL_FAILED_DONT_REPORT;
                    else if (!pet)
                        return SPELL_FAILED_NO_PET;
                    else
                        return SPELL_FAILED_TARGETS_DEAD;
                }
                break;
            }
        }

        //check creature type
        //ignore self casts (including area casts when caster selected as target)
        if (non_caster_target)
        {
            if(!SpellMgr::IsTargetMatchedWithCreatureType(m_spellInfo, target))
            {
                if (target->GetTypeId() == TYPEID_PLAYER)
                    return SPELL_FAILED_TARGET_IS_PLAYER;
                else
                    return SPELL_FAILED_BAD_TARGETS;
            }

            // simple cases
            bool explicit_target_mode = false;
            bool target_hostile = false;
            bool target_hostile_checked = false;
            bool target_friendly = false;
            bool target_friendly_checked = false;
            for(int k = 0; k < MAX_EFFECT_INDEX;  ++k)
            {
                if (IsExplicitPositiveTarget(m_spellInfo->EffectImplicitTargetA[k]))
                {
                    if (!target_hostile_checked)
                    {
                        target_hostile_checked = true;
                        target_hostile = m_caster->IsHostileTo(target);
                    }

                    if (target_hostile)
                        return SPELL_FAILED_BAD_TARGETS;

                    explicit_target_mode = true;
                }
                else if (IsExplicitNegativeTarget(m_spellInfo->EffectImplicitTargetA[k]))
                {
                    if (!target_friendly_checked)
                    {
                        target_friendly_checked = true;
                        target_friendly = m_caster->IsFriendlyTo(target);
                    }

                    if (target_friendly)
                        return SPELL_FAILED_BAD_TARGETS;

                    explicit_target_mode = true;
                }
            }
            // TODO: this check can be applied and for player to prevent cheating when IsPositiveSpell will return always correct result.
            // check target for pet/charmed casts (not self targeted), self targeted cast used for area effects and etc
            if (!explicit_target_mode && m_caster->GetTypeId() == TYPEID_UNIT && !m_caster->GetCharmerOrOwnerGuid().IsEmpty() && !IsDispelSpell(m_spellInfo))
            {
                // check correctness positive/negative cast target (pet cast real check and cheating check)
                if (IsPositiveSpell(m_spellInfo->Id))
                {
                    if (!target_hostile_checked)
                    {
                        target_hostile_checked = true;
                        target_hostile = m_caster->IsHostileTo(target);
                    }

                    if (target_hostile)
                        return SPELL_FAILED_BAD_TARGETS;
                }
                else
                {
                    if (!target_friendly_checked)
                    {
                        target_friendly_checked = true;
                        target_friendly = m_caster->IsFriendlyTo(target);
                    }

                    if (target_friendly)
                        return SPELL_FAILED_BAD_TARGETS;
                }
            }
        }

        if (IsPositiveSpell(m_spellInfo->Id) && m_caster->IsFriendlyTo(target))
        {
            if (!target->IsImmunedToSchool(GetSpellSchoolMask(m_spellInfo)) && target->IsImmuneToSpell(m_spellInfo, true))
                return SPELL_FAILED_TARGET_AURASTATE;

            if (target->IsInWorld() && target->IsInMap(m_caster))
                if (target->HasMorePoweredBuff(m_spellInfo->Id))
                    return m_IsTriggeredSpell ? SPELL_FAILED_DONT_REPORT : SPELL_FAILED_AURA_BOUNCED;
        }

        //Must be behind the target.
        if (m_spellInfo->GetAttributesEx2() == SPELL_ATTR_EX2_UNK20 && m_spellInfo->HasAttribute(SPELL_ATTR_EX_UNK9) && target->HasInArc(M_PI_F, m_caster))
        {
            // Exclusion for Pounce: Facing Limitation was removed in 2.0.1, but it still uses the same, old Ex-Flags
            // Exclusion for Mutilate:Facing Limitation was removed in 2.0.1 and 3.0.3, but they still use the same, old Ex-Flags
            // Exclusion for Throw: Facing limitation was added in 3.2.x, but that shouldn't be
            if (!m_spellInfo->IsFitToFamily<SPELLFAMILY_DRUID, CF_DRUID_POUNCE>() &&
                !m_spellInfo->IsFitToFamily<SPELLFAMILY_ROGUE, CF_ROGUE_MUTILATE>() &&
                m_spellInfo->Id != 2764)
            {
                SendInterrupted(2);
                return SPELL_FAILED_NOT_BEHIND;
            }
        }

        //Target must be facing you.
        if ((m_spellInfo->GetAttributes() == (SPELL_ATTR_ABILITY | SPELL_ATTR_NOT_SHAPESHIFT | SPELL_ATTR_UNK18 | SPELL_ATTR_STOP_ATTACK_TARGET)) && !target->HasInArc(M_PI_F, m_caster))
        {
            SendInterrupted(2);
            return SPELL_FAILED_NOT_INFRONT;
        }

        // check if target is in combat
        if (non_caster_target && m_spellInfo->HasAttribute(SPELL_ATTR_EX_NOT_IN_COMBAT_TARGET) && target->isInCombat())
            return SPELL_FAILED_TARGET_AFFECTING_COMBAT;
    }
    // zone check
    if (!m_caster->GetMap() || !m_caster->GetTerrain())
        return SPELL_FAILED_DONT_REPORT;

    uint32 zone, area;
    m_caster->GetZoneAndAreaId(zone, area);

    SpellCastResult locRes= sSpellMgr.GetSpellAllowedInLocationError(m_spellInfo, m_caster->GetMapId(), zone, area,
        m_caster->GetCharmerOrOwnerPlayerOrPlayerItself());
    if (locRes != SPELL_CAST_OK)
    {
        if (!m_IsTriggeredSpell)
            return locRes;
        else
            return SPELL_FAILED_DONT_REPORT;
    }

    bool castOnVehicleAllowed = false;

    if (VehicleKitPtr vehicle = m_caster->GetVehicle())
    {
        if (m_spellInfo->HasAttribute(SPELL_ATTR_EX6_CASTABLE_ON_VEHICLE))
            castOnVehicleAllowed = true;

        if (VehicleSeatEntry const* seatInfo = vehicle->GetSeatInfo(m_caster))
            if ((seatInfo->m_flags & SEAT_FLAG_CAN_CAST) || (seatInfo->m_flags & SEAT_FLAG_CAN_ATTACK))
                castOnVehicleAllowed = true;
    }


    // not let players cast spells at mount (and let do it to creatures)
    if ((m_caster->IsMounted() || (m_caster->GetVehicle() && !castOnVehicleAllowed)) && m_caster->GetTypeId() == TYPEID_PLAYER && !m_IsTriggeredSpell &&
        !IsPassiveSpell(m_spellInfo) && !m_spellInfo->HasAttribute(SPELL_ATTR_CASTABLE_WHILE_MOUNTED))
    {
        if (m_caster->IsTaxiFlying())
            return SPELL_FAILED_NOT_ON_TAXI;
        else
            return SPELL_FAILED_NOT_MOUNTED;
    }

    // always (except passive spells) check items
    if (!IsPassiveSpell(m_spellInfo))
    {
        SpellCastResult castResult = CheckItems();
        if (castResult != SPELL_CAST_OK)
            return castResult;
    }

    // check spell focus object
    if (m_spellInfo->RequiresSpellFocus)
    {
        GameObject* ok = NULL;
        MaNGOS::GameObjectFocusCheck go_check(m_caster, m_spellInfo->RequiresSpellFocus);
        MaNGOS::GameObjectSearcher<MaNGOS::GameObjectFocusCheck> checker(ok, go_check);
        Cell::VisitGridObjects(m_caster, checker, m_caster->GetMap()->GetVisibilityDistance());

        if (!ok)
            return SPELL_FAILED_REQUIRES_SPELL_FOCUS;

        focusObject = ok;                                   // game object found in range
    }

    // Database based targets from spell_target_script
    if (m_UniqueTargetInfo.empty())                         // skip second CheckCast apply (for delayed spells for example)
    {
        for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
        {
            if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT ||
               (m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT && m_spellInfo->EffectImplicitTargetA[j] != TARGET_SELF) ||
               m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES ||
               m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT_COORDINATES ||
               m_spellInfo->EffectImplicitTargetA[j] == TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT)
            {

                SQLMultiStorage::SQLMSIteratorBounds<SpellTargetEntry> bounds = sSpellScriptTargetStorage.getBounds<SpellTargetEntry>(m_spellInfo->Id);

                WorldLocation const* targetLoc = sSpellMgr.GetSpellTargetPosition(m_spellInfo->Id);

                if (bounds.first == bounds.second)
                {
                    if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT || m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT)
                        sLog.outErrorDb("Spell entry %u, effect %i has EffectImplicitTargetA/EffectImplicitTargetB = TARGET_SCRIPT, but creature are not defined in `spell_script_target`", m_spellInfo->Id, j);

                    if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES || m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT_COORDINATES)
                    {
                        if (!targetLoc)
                            sLog.outErrorDb("Spell entry %u, effect %i has EffectImplicitTargetA/EffectImplicitTargetB = TARGET_SCRIPT_COORDINATES, but gameobject or creature are not defined in `spell_script_target` and target location not defined in `spell_target_position`", m_spellInfo->Id, j);
                        else
                            DEBUG_FILTER_LOG(LOG_FILTER_DB_STRICTED_CHECK,"Spell entry %u, effect %i has EffectImplicitTargetA/EffectImplicitTargetB = TARGET_SCRIPT_COORDINATES, but gameobject or creature are not defined in `spell_script_target` and target location defined in `spell_target_position`", m_spellInfo->Id, j);
                    }

                    if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT)
                        sLog.outErrorDb("Spell entry %u, effect %i has EffectImplicitTargetA/EffectImplicitTargetB = TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT, but gameobject are not defined in `spell_script_target`", m_spellInfo->Id, j);
                }

                SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(m_spellInfo->GetRangeIndex());
                float range = GetSpellMaxRange(srange);

                Creature* targetExplicit = NULL;            // used for cases where a target is provided (by script for example)
                Creature* creatureScriptTarget = NULL;
                GameObject* goScriptTarget = NULL;

                for (SQLMultiStorage::SQLMultiSIterator<SpellTargetEntry> i_spellST = bounds.first; i_spellST != bounds.second; ++i_spellST)
                {
                    if (i_spellST->CanNotHitWithSpellEffect(SpellEffectIndex(j)))
                        continue;

                    switch (i_spellST->type)
                    {
                        case SPELL_TARGET_TYPE_GAMEOBJECT:
                        {
                            GameObject* p_GameObject = NULL;

                            if (i_spellST->targetEntry)
                            {
                                MaNGOS::NearestGameObjectEntryInObjectRangeCheck go_check(*m_caster, i_spellST->targetEntry, range);
                                MaNGOS::GameObjectLastSearcher<MaNGOS::NearestGameObjectEntryInObjectRangeCheck> checker(p_GameObject, go_check);
                                Cell::VisitGridObjects(m_caster, checker, range);

                                if (p_GameObject)
                                {
                                    // remember found target and range, next attempt will find more near target with another entry
                                    creatureScriptTarget = NULL;
                                    goScriptTarget = p_GameObject;
                                    range = go_check.GetLastRange();
                                }
                            }
                            else if (focusObject)           // Focus Object
                            {
                                float frange = m_caster->GetDistance(focusObject);
                                if (range >= frange)
                                {
                                    creatureScriptTarget = NULL;
                                    goScriptTarget = focusObject;
                                    range = frange;
                                }
                            }
                            break;
                        }
                        case SPELL_TARGET_TYPE_CREATURE:
                        case SPELL_TARGET_TYPE_DEAD:
                        default:
                        {
                            Creature* p_Creature = NULL;

                            // check if explicit target is provided and check it up against database valid target entry/state
                            if (Unit* pTarget = m_targets.getUnitTarget())
                            {
                                if (pTarget->GetTypeId() == TYPEID_UNIT && pTarget->GetEntry() == i_spellST->targetEntry)
                                {
                                    if (i_spellST->type == SPELL_TARGET_TYPE_DEAD && ((Creature*)pTarget)->IsCorpse())
                                    {
                                        // always use spellMaxRange, in case GetLastRange returned different in a previous pass
                                        if (pTarget->IsWithinDistInMap(m_caster, GetSpellMaxRange(srange)))
                                            targetExplicit = (Creature*)pTarget;
                                    }
                                    else if (i_spellST->type == SPELL_TARGET_TYPE_CREATURE && pTarget->isAlive())
                                    {
                                        // always use spellMaxRange, in case GetLastRange returned different in a previous pass
                                        if (pTarget->IsWithinDistInMap(m_caster, GetSpellMaxRange(srange)))
                                            targetExplicit = (Creature*)pTarget;
                                    }
                                }
                            }

                            // no target provided or it was not valid, so use closest in range
                            if (!targetExplicit)
                            {
                                MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck u_check(*m_caster, i_spellST->targetEntry, i_spellST->type != SPELL_TARGET_TYPE_DEAD, i_spellST->type == SPELL_TARGET_TYPE_DEAD, range);
                                MaNGOS::CreatureLastSearcher<MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck> searcher(p_Creature, u_check);

                                // Visit all, need to find also Pet* objects
                                Cell::VisitAllObjects(m_caster, searcher, range);

                                range = u_check.GetLastRange();
                            }

                            // always prefer provided target if it's valid
                            if (targetExplicit)
                                creatureScriptTarget = targetExplicit;
                            else if (p_Creature)
                                creatureScriptTarget = p_Creature;

                            if (creatureScriptTarget)
                                goScriptTarget = NULL;

                            break;
                        }
                    }
                }

                if (creatureScriptTarget)
                {
                    // store coordinates for TARGET_SCRIPT_COORDINATES
                    if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES ||
                        m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT_COORDINATES)
                    {
                        m_targets.setDestination(creatureScriptTarget->GetPosition());

                        if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES && m_spellInfo->Effect[j] != SPELL_EFFECT_PERSISTENT_AREA_AURA)
                            AddUnitTarget(creatureScriptTarget, SpellEffectIndex(j));
                    }
                    // store explicit target for TARGET_SCRIPT
                    else
                    {
                        if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT ||
                            m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT)
                            AddUnitTarget(creatureScriptTarget, SpellEffectIndex(j));
                    }
                }
                else if (goScriptTarget)
                {
                    // store coordinates for TARGET_SCRIPT_COORDINATES
                    if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES ||
                        m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT_COORDINATES)
                    {
                        m_targets.setDestination(goScriptTarget->GetPosition());

                        if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES && m_spellInfo->Effect[j] != SPELL_EFFECT_PERSISTENT_AREA_AURA)
                            AddGOTarget(goScriptTarget, SpellEffectIndex(j));
                    }
                    // store explicit target for TARGET_SCRIPT and TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT
                    else
                    {
                        if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT ||
                            m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT ||
                            m_spellInfo->EffectImplicitTargetA[j] == TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT ||
                            m_spellInfo->EffectImplicitTargetB[j] == TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT)
                            AddGOTarget(goScriptTarget, SpellEffectIndex(j));
                    }
                }
                else if (targetLoc)
                {
                    // Coordinates for spell stored in `spell_target_position` table.
                    // store coordinates for TARGET_SCRIPT_COORDINATES
                    if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES ||
                        m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT_COORDINATES)
                    {
                        m_targets.setDestination(*targetLoc);
                        AddUnitTarget(GetAffectiveCaster(), SpellEffectIndex(j));
                    }
                    else
                    {
                        // not report target not existence for triggered spells
                        if (m_triggeredByAuraSpell || m_IsTriggeredSpell)
                            return SPELL_FAILED_DONT_REPORT;
                        else
                            return SPELL_FAILED_BAD_TARGETS;
                    }
                }
                //Missing DB Entry or targets for this spellEffect.
                else
                {
                    /* For TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT makes DB targets optional not required for now
                     * TODO: Makes more research for this target type
                     */
                    if (m_spellInfo->EffectImplicitTargetA[j] != TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT)
                    {
                        // not report target not existence for triggered spells
                        if (m_triggeredByAuraSpell || m_IsTriggeredSpell)
                            return SPELL_FAILED_DONT_REPORT;
                        else
                            return SPELL_FAILED_BAD_TARGETS;
                    }
                }
            }
        }
    }

    if(!m_IsTriggeredSpell)
    {
        SpellCastResult castResult = CheckRange(strict, m_targets.getUnitTarget());
        if (castResult != SPELL_CAST_OK)
            return castResult;
    }

    {
        SpellCastResult castResult = CheckPower();
        if (castResult != SPELL_CAST_OK)
            return castResult;
    }

    if(!m_IsTriggeredSpell)                                 // triggered spell not affected by stun/etc
    {
        SpellCastResult castResult = CheckCasterAuras();
        if (castResult != SPELL_CAST_OK)
            return castResult;
    }

    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        // for effects of spells that have only one target
        switch(m_spellInfo->Effect[i])
        {
            case SPELL_EFFECT_INSTAKILL:
                break;
            case SPELL_EFFECT_DUMMY:
            {
                if (m_spellInfo->Id == 51582)          // Rocket Boots Engaged
                {
                    if (m_caster->IsInWater())
                        return SPELL_FAILED_ONLY_ABOVEWATER;
                }
                else if (m_spellInfo->SpellFamilyName == SPELLFAMILY_DEATHKNIGHT && m_spellInfo->GetSpellFamilyFlags().test<CF_DEATHKNIGHT_DEATH_COIL>()) // Death Coil (DeathKnight)
                {
                    Unit* target = m_targets.getUnitTarget();
                    if (!target || (target->IsFriendlyTo(m_caster) && target->GetCreatureType() != CREATURE_TYPE_UNDEAD))
                        return SPELL_FAILED_BAD_TARGETS;
                }
                else if (m_spellInfo->Id == 51690)          // Killing Spree
                {
                    UnitList targets;

                    float radius = GetSpellMaxRange(sSpellRangeStore.LookupEntry(m_spellInfo->GetRangeIndex()));

                    MaNGOS::AnyUnfriendlyVisibleUnitInObjectRangeCheck unitCheck(m_caster, m_caster, radius);
                    MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyVisibleUnitInObjectRangeCheck> checker(targets, unitCheck);
                    Cell::VisitAllObjects(m_caster, checker, radius);

                    if (targets.empty())
                        return SPELL_FAILED_OUT_OF_RANGE;
                }
                else if (m_spellInfo->GetSpellIconID() == 156)   // Holy Shock
                {
                    // spell different for friends and enemies
                    // hart version required facing
                    if (m_targets.getUnitTarget() && !m_caster->IsFriendlyTo(m_targets.getUnitTarget()) && !m_caster->HasInArc(M_PI_F, m_targets.getUnitTarget()))
                        return SPELL_FAILED_UNIT_NOT_INFRONT;
                }
                else if(m_spellInfo->Id == 49576)           // Death Grip
                {
                    if(m_caster->m_movementInfo.HasMovementFlag(MovementFlags(MOVEFLAG_FALLING | MOVEFLAG_FALLINGFAR)))
                        return SPELL_FAILED_MOVING;
                }
                else if (m_spellInfo->SpellFamilyName == SPELLFAMILY_SHAMAN && m_spellInfo->GetSpellIconID() == 33) // Fire Nova
                {
                    // fire totems slot
                    if (!m_caster->GetTotemGuid(TOTEM_SLOT_FIRE))
                        return SPELL_FAILED_TOTEMS;
                }
                // Voracious Appetite && Cannibalize && Carrion Feeder
                else if (m_UniqueTargetInfo.empty() /*only in first check!*/
                    && (m_spellInfo->Id == 20577 || m_spellInfo->Id == 52749 || m_spellInfo->Id == 54044))
                {
                    m_targets.setUnitTarget(NULL);
                    WorldObject* result = FindCorpseUsing<MaNGOS::CannibalizeObjectCheck>(m_spellInfo->TargetCreatureType);
                    if (result && result->IsInWorld())
                    {
                        switch (result->GetTypeId())
                        {
                            case TYPEID_UNIT:
                            case TYPEID_PLAYER:
                                m_targets.setUnitTarget((Unit*)result);
                                break;
                            case TYPEID_CORPSE:
                                if (Player* owner = ObjectAccessor::FindPlayer(((Corpse*)result)->GetOwnerGuid()))
                                    if (owner->IsInMap(m_caster) && !owner->isAlive())
                                        m_targets.setUnitTarget((Unit*)owner);
                                break;
                            default:
                                return SPELL_FAILED_NO_EDIBLE_CORPSES;
                        }
                    }
                    if (!m_targets.getUnitTarget())
                        return SPELL_FAILED_NO_EDIBLE_CORPSES;
                }
                break;
            }
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            {
                // Hammer of Wrath
                if (m_spellInfo->IsFitToFamily<SPELLFAMILY_PALADIN, CF_PALADIN_HAMMER_OF_WRATH>())
                {
                    if (!m_targets.getUnitTarget())
                        return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                    if (m_targets.getUnitTarget()->GetHealth() > m_targets.getUnitTarget()->GetMaxHealth()*0.2)
                        return SPELL_FAILED_BAD_TARGETS;
                }
                break;
            }
            case SPELL_EFFECT_TAMECREATURE:
            {
                // Spell can be triggered, we need to check original caster prior to caster
                Unit* caster = GetAffectiveCaster();
                if (!caster || caster->GetTypeId() != TYPEID_PLAYER ||
                    !m_targets.getUnitTarget() ||
                    m_targets.getUnitTarget()->GetTypeId() == TYPEID_PLAYER)
                    return SPELL_FAILED_BAD_TARGETS;

                Player* plrCaster = (Player*)caster;

                bool gmmode = m_triggeredBySpellInfo == NULL;

                if (gmmode && !ChatHandler(plrCaster).FindCommand("npc tame"))
                {
                    plrCaster->SendPetTameFailure(PETTAME_UNKNOWNERROR);
                    return SPELL_FAILED_DONT_REPORT;
                }

                if (plrCaster->getClass() != CLASS_HUNTER && !gmmode)
                {
                    plrCaster->SendPetTameFailure(PETTAME_UNITSCANTTAME);
                    return SPELL_FAILED_DONT_REPORT;
                }

                Creature* target = (Creature*)m_targets.getUnitTarget();

                if (target->IsPet() || target->isCharmed())
                {
                    plrCaster->SendPetTameFailure(PETTAME_CREATUREALREADYOWNED);
                    return SPELL_FAILED_DONT_REPORT;
                }

                if (target->getLevel() > plrCaster->getLevel() && !gmmode)
                {
                    plrCaster->SendPetTameFailure(PETTAME_TOOHIGHLEVEL);
                    return SPELL_FAILED_DONT_REPORT;
                }

                if (target->GetCreatureInfo()->IsExotic() && !plrCaster->CanTameExoticPets() && !gmmode)
                {
                    plrCaster->SendPetTameFailure(PETTAME_CANTCONTROLEXOTIC);
                    return SPELL_FAILED_DONT_REPORT;
                }

                if (!target->GetCreatureInfo()->isTameable(plrCaster->CanTameExoticPets()))
                {
                    plrCaster->SendPetTameFailure(PETTAME_NOTTAMEABLE);
                    return SPELL_FAILED_DONT_REPORT;
                }

                if (plrCaster->GetPetGuid() || plrCaster->GetCharmGuid())
                {
                    plrCaster->SendPetTameFailure(PETTAME_ANOTHERSUMMONACTIVE);
                    return SPELL_FAILED_DONT_REPORT;
                }

                break;
            }
            case SPELL_EFFECT_LEARN_SPELL:
            {
                if (m_spellInfo->EffectImplicitTargetA[i] != TARGET_PET)
                    break;

                Pet* pet = m_caster->GetPet();

                if(!pet)
                    return SPELL_FAILED_NO_PET;

                SpellEntry const *learn_spellproto = sSpellStore.LookupEntry(m_spellInfo->EffectTriggerSpell[i]);

                if(!learn_spellproto)
                    return SPELL_FAILED_NOT_KNOWN;

                if (m_spellInfo->spellLevel > pet->getLevel())
                    return SPELL_FAILED_LOWLEVEL;

                break;
            }
            case SPELL_EFFECT_LEARN_PET_SPELL:
            {
                Pet* pet = m_caster->GetPet();

                if(!pet)
                    return SPELL_FAILED_NO_PET;

                SpellEntry const *learn_spellproto = sSpellStore.LookupEntry(m_spellInfo->EffectTriggerSpell[i]);

                if(!learn_spellproto)
                    return SPELL_FAILED_NOT_KNOWN;

                if (m_spellInfo->spellLevel > pet->getLevel())
                    return SPELL_FAILED_LOWLEVEL;

                break;
            }
            case SPELL_EFFECT_APPLY_GLYPH:
            {
                uint32 glyphId = m_spellInfo->EffectMiscValue[i];
                if (GlyphPropertiesEntry const *gp = sGlyphPropertiesStore.LookupEntry(glyphId))
                    if (m_caster->HasAura(gp->SpellId))
                        return SPELL_FAILED_UNIQUE_GLYPH;
                break;
            }
            case SPELL_EFFECT_FEED_PET:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER)
                    return SPELL_FAILED_BAD_TARGETS;

                Item* foodItem = m_targets.getItemTarget();
                if(!foodItem)
                    return SPELL_FAILED_BAD_TARGETS;

                Pet* pet = m_caster->GetPet();

                if(!pet)
                    return SPELL_FAILED_NO_PET;

                if(!pet->HaveInDiet(foodItem->GetProto()))
                    return SPELL_FAILED_WRONG_PET_FOOD;

                if(!pet->GetCurrentFoodBenefitLevel(foodItem->GetProto()->ItemLevel))
                    return SPELL_FAILED_FOOD_LOWLEVEL;

                if (pet->isInCombat())
                    return SPELL_FAILED_AFFECTING_COMBAT;

                break;
            }
            case SPELL_EFFECT_POWER_BURN:
            case SPELL_EFFECT_POWER_DRAIN:
            {
                // Can be area effect, Check only for players and not check if target - caster (spell can have multiply drain/burn effects)
                if (m_caster->GetTypeId() == TYPEID_PLAYER)
                    if (Unit* target = m_targets.getUnitTarget())
                        if (target != m_caster && int32(target->getPowerType()) != m_spellInfo->EffectMiscValue[i])
                            return SPELL_FAILED_BAD_TARGETS;
                break;
            }
            case SPELL_EFFECT_CHARGE:
            {
                if (m_caster->hasUnitState(UNIT_STAT_ROOT))
                {
                    // Intervene with Warbringer talent
                    if (m_spellInfo->Id == 3411 && m_caster->HasAura(57499))
                        m_caster->RemoveAurasAtMechanicImmunity(IMMUNE_TO_ROOT_AND_SNARE_MASK, 0);
                    else
                        return SPELL_FAILED_ROOTED;
                }

                break;
            }
            case SPELL_EFFECT_SKINNING:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER || !m_targets.getUnitTarget() || m_targets.getUnitTarget()->GetTypeId() != TYPEID_UNIT)
                    return SPELL_FAILED_BAD_TARGETS;

                if (!m_targets.getUnitTarget()->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE))
                    return SPELL_FAILED_TARGET_UNSKINNABLE;

                Creature* creature = (Creature*)m_targets.getUnitTarget();
                if ( creature->GetCreatureType() != CREATURE_TYPE_CRITTER && ( !creature->lootForBody || creature->lootForSkin || !creature->loot.empty() ) )
                {
                    return SPELL_FAILED_TARGET_NOT_LOOTED;
                }

                uint32 skill = creature->GetCreatureInfo()->GetRequiredLootSkill();

                int32 skillValue = ((Player*)m_caster)->GetSkillValue(skill);
                int32 TargetLevel = m_targets.getUnitTarget()->getLevel();
                int32 ReqValue = (skillValue < 100 ? (TargetLevel-10) * 10 : TargetLevel * 5);
                if (ReqValue > skillValue)
                    return SPELL_FAILED_LOW_CASTLEVEL;

                // chance for fail at orange skinning attempt
                if ( (m_selfContainer && (*m_selfContainer) == this) &&
                    skillValue < sWorld.GetConfigMaxSkillValue() &&
                    (ReqValue < 0 ? 0 : ReqValue) > irand(skillValue - 25, skillValue + 37) )
                    return SPELL_FAILED_TRY_AGAIN;

                break;
            }
            case SPELL_EFFECT_OPEN_LOCK:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER)  // only players can open locks, gather etc.
                    return SPELL_FAILED_BAD_TARGETS;

                // we need a go target in case of TARGET_GAMEOBJECT (for other targets acceptable GO and items)
                if (m_spellInfo->EffectImplicitTargetA[i] == TARGET_GAMEOBJECT)
                {
                    if (!m_targets.getGOTarget())
                        return SPELL_FAILED_BAD_TARGETS;
                }

                // get the lock entry
                uint32 lockId = 0;
                if (GameObject* go = m_targets.getGOTarget())
                {
                    // In BattleGround players can use only flags and banners
                    if ( ((Player*)m_caster)->InBattleGround() &&
                        !((Player*)m_caster)->CanUseBattleGroundObject() && m_spellInfo->Id!= 1842 ) // Disarm Trap can be used
                        return SPELL_FAILED_TRY_AGAIN;

                    lockId = go->GetGOInfo()->GetLockId();
                    if (!lockId)
                        return SPELL_FAILED_ALREADY_OPEN;
                }
                else if (Item* item = m_targets.getItemTarget())
                {
                    // not own (trade?)
                    if (item->GetOwner() != m_caster)
                        return SPELL_FAILED_ITEM_GONE;

                    lockId = item->GetProto()->LockID;

                    // if already unlocked
                    if (!lockId || item->HasFlag(ITEM_FIELD_FLAGS, ITEM_DYNFLAG_UNLOCKED))
                        return SPELL_FAILED_ALREADY_OPEN;
                }
                else
                    return SPELL_FAILED_BAD_TARGETS;

                SkillType skillId = SKILL_NONE;
                int32 reqSkillValue = 0;
                int32 skillValue = 0;

                // check lock compatibility
                SpellCastResult res = CanOpenLock(SpellEffectIndex(i), lockId, skillId, reqSkillValue, skillValue);
                if (res != SPELL_CAST_OK)
                    return res;

                // chance for fail at orange mining/herb/LockPicking gathering attempt
                // second check prevent fail at rechecks
                if (skillId != SKILL_NONE && (!m_selfContainer || ((*m_selfContainer) != this)))
                {
                    bool canFailAtMax = skillId != SKILL_HERBALISM && skillId != SKILL_MINING;

                    // chance for failure in orange gather / lockpick (gathering skill can't fail at maxskill)
                    if((canFailAtMax || skillValue < sWorld.GetConfigMaxSkillValue()) && reqSkillValue > irand(skillValue - 25, skillValue + 37))
                        return SPELL_FAILED_TRY_AGAIN;
                }
                break;
            }
            case SPELL_EFFECT_SUMMON_DEAD_PET:
            {
                Creature *pet = m_caster->GetPet();
                if(!pet)
                    return SPELL_FAILED_NO_PET;

                if (pet->isAlive())
                    return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                break;
            }
            // This is generic summon effect
            case SPELL_EFFECT_SUMMON:
            {
                if (SummonPropertiesEntry const *summon_prop = sSummonPropertiesStore.LookupEntry(m_spellInfo->EffectMiscValueB[i]))
                {
                    if (summon_prop->Group == SUMMON_PROP_GROUP_PETS)
                    {
                        if (m_caster->GetPetGuid())
                            return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                        if (m_caster->GetCharmGuid())
                            return SPELL_FAILED_ALREADY_HAVE_CHARM;
                    }
                }

                break;
            }
            case SPELL_EFFECT_SUMMON_OBJECT_SLOT1:
            case SPELL_EFFECT_SUMMON_OBJECT_SLOT2:
            case SPELL_EFFECT_SUMMON_OBJECT_SLOT3:
            case SPELL_EFFECT_SUMMON_OBJECT_SLOT4:
            {
                if (m_caster->GetTypeId() == TYPEID_PLAYER)
                    if (((Player*)m_caster)->HasMovementFlag(MOVEFLAG_ONTRANSPORT))
                        return SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW;

                break;
            }
            case SPELL_EFFECT_SUMMON_PET:
            {
                if (m_caster->GetPetGuid())                 // let warlock do a replacement summon
                {

                    Pet* pet = ((Player*)m_caster)->GetPet();

                    if (m_caster->GetTypeId() == TYPEID_PLAYER && m_caster->getClass() == CLASS_WARLOCK)
                    {
                        if (strict)                         //Summoning Disorientation, trigger pet stun (cast by pet so it doesn't attack player)
                            pet->CastSpell(pet, 32752, true, NULL, NULL, pet->GetObjectGuid());
                    }
                    else
                        return SPELL_FAILED_ALREADY_HAVE_SUMMON;
                }

                if (m_caster->GetCharmGuid())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                break;
            }
            case SPELL_EFFECT_SUMMON_PLAYER:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER)
                    return SPELL_FAILED_BAD_TARGETS;

                if(((Player*)m_caster)->GetSelectionGuid().IsEmpty())
                    return SPELL_FAILED_BAD_TARGETS;

                Player* target = sObjectMgr.GetPlayer(((Player*)m_caster)->GetSelectionGuid());

                if ( !target || ((Player*)m_caster) == target)
                    return SPELL_FAILED_BAD_TARGETS;

                if (!target->IsInSameRaidWith((Player*)m_caster) && m_spellInfo->Id != 48955)
                    return SPELL_FAILED_BAD_TARGETS;

                // check if our map is dungeon
                if ( sMapStore.LookupEntry(m_caster->GetMapId())->IsDungeon() )
                {
                    InstanceTemplate const* instance = ObjectMgr::GetInstanceTemplate(m_caster->GetMapId());
                    if(!instance)
                        return SPELL_FAILED_TARGET_NOT_IN_INSTANCE;
                    if ( instance->levelMin > target->getLevel() )
                        return SPELL_FAILED_LOWLEVEL;
                    if ( instance->levelMax && instance->levelMax < target->getLevel() )
                        return SPELL_FAILED_HIGHLEVEL;
                }
                break;
            }
            case SPELL_EFFECT_FRIEND_SUMMON:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER)
                    return SPELL_FAILED_BAD_TARGETS;

                if(((Player*)m_caster)->GetSelectionGuid().IsEmpty())
                    return SPELL_FAILED_BAD_TARGETS;

                Player* target = sObjectMgr.GetPlayer(((Player*)m_caster)->GetSelectionGuid());

                if (!target || !target->IsReferAFriendLinked(((Player*)m_caster)))
                    return SPELL_FAILED_BAD_TARGETS;

                break;
            }
            case SPELL_EFFECT_LEAP_BACK:
            {
                if (m_spellInfo->Id == 781)
                    if(!m_caster->isInCombat())
                        return SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW;
            }
            // no break here!
            case SPELL_EFFECT_TELEPORT_UNITS_FACE_CASTER:
            {
                // Blink has leap first and then removing of auras with root effect
                // need further research with this
                if (m_spellInfo->Effect[i] != SPELL_EFFECT_LEAP)
                {
                    if (m_caster->hasUnitState(UNIT_STAT_ROOT))
                        return SPELL_FAILED_ROOTED;
                }

                // not allow use this effect at battleground until battleground start
                if (m_caster->GetTypeId() == TYPEID_PLAYER)
                {
                    if (BattleGround const *bg = ((Player*)m_caster)->GetBattleGround())
                        if (bg->GetStatus() != STATUS_IN_PROGRESS)
                            return SPELL_FAILED_TRY_AGAIN;

                    if(((Player*)m_caster)->HasMovementFlag(MOVEFLAG_ONTRANSPORT))
                        return SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW;
                }
                break;
            }
            case SPELL_EFFECT_STEAL_BENEFICIAL_BUFF:
            {
                if (m_targets.getUnitTarget() == m_caster)
                    return SPELL_FAILED_BAD_TARGETS;
                break;
            }
            case SPELL_EFFECT_JUMP:
            case SPELL_EFFECT_JUMP2:
            case SPELL_EFFECT_CHARGE2:
            {
                if (m_caster->hasUnitState(UNIT_STAT_ROOT))
                    return SPELL_FAILED_ROOTED;
                break;
            }
            case SPELL_EFFECT_ADD_FARSIGHT:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER || ((Player*)m_caster)->HasExternalViewPoint())
                    return SPELL_FAILED_BAD_TARGETS;
            }
            default:
                break;
        }
    }

    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        // Do not check in case of junk in DBC
        if (!IsAuraApplyEffect(m_spellInfo, SpellEffectIndex(i)))
            continue;

        // Possible Unit-target for the spell
        Unit* expectedTarget = GetPrefilledUnitTargetOrUnitTarget(SpellEffectIndex(i));

        switch (m_spellInfo->EffectApplyAuraName[i])
        {
            case SPELL_AURA_DUMMY:
            {
                //custom check
                switch(m_spellInfo->Id)
                {
                    case 34026:                             // Kill Command
                        if (!m_caster->GetPet())
                            return SPELL_FAILED_NO_PET;
                        break;
                    case 61336:                             // Survival Instincts
                        if (m_caster->GetTypeId() != TYPEID_PLAYER || !((Player*)m_caster)->IsInFeralForm())
                            return SPELL_FAILED_ONLY_SHAPESHIFT;
                        break;
                    default:
                        break;
                }
                break;
            }
            case SPELL_AURA_MOD_POSSESS:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER)
                    return SPELL_FAILED_UNKNOWN;

                if (expectedTarget == m_caster)
                    return SPELL_FAILED_BAD_TARGETS;

                if (m_caster->GetPetGuid())
                    return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                if (m_caster->GetCharmGuid())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                if (m_caster->GetCharmerGuid())
                    return SPELL_FAILED_CHARMED;

                if (!expectedTarget)
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                if (expectedTarget->GetCharmerGuid())
                    return SPELL_FAILED_CHARMED;

                if (int32(expectedTarget->getLevel()) > CalculateDamage(SpellEffectIndex(i), expectedTarget))
                    return SPELL_FAILED_HIGHLEVEL;

                break;
            }
            case SPELL_AURA_MOD_CHARM:
            {
                if (expectedTarget == m_caster)
                    return SPELL_FAILED_BAD_TARGETS;

                if (m_caster->GetPetGuid())
                    return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                if (m_caster->GetCharmGuid())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                if (m_caster->GetCharmerGuid())
                    return SPELL_FAILED_CHARMED;

                if (!expectedTarget)
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                if (expectedTarget->GetCharmerGuid())
                    return SPELL_FAILED_CHARMED;

                if (int32(expectedTarget->getLevel()) > CalculateDamage(SpellEffectIndex(i), expectedTarget))
                    return SPELL_FAILED_HIGHLEVEL;

                break;
            }
            case SPELL_AURA_MOD_POSSESS_PET:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER)
                    return SPELL_FAILED_UNKNOWN;

                if (m_caster->GetCharmGuid())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                if (m_caster->GetCharmerGuid())
                    return SPELL_FAILED_CHARMED;

                Pet* pet = m_caster->GetPet();
                if (!pet)
                    return SPELL_FAILED_NO_PET;

                if (pet->GetCharmerGuid())
                    return SPELL_FAILED_CHARMED;

                break;
            }
            case SPELL_AURA_MOUNTED:
            {
                if (m_caster->IsInWater())
                    return SPELL_FAILED_ONLY_ABOVEWATER;

                // Ignore map check if spell have AreaId. AreaId already checked and this prevent special mount spells
                if (m_caster->GetTypeId() == TYPEID_PLAYER &&
                        (m_caster->GetMap() && !sMapStore.LookupEntry(m_caster->GetMapId())->IsMountAllowed()) &&
                        !m_IsTriggeredSpell &&
                        !m_spellInfo->AreaGroupId)
                    return SPELL_FAILED_NO_MOUNTS_ALLOWED;

                if (m_caster->IsInDisallowedMountForm())
                    return SPELL_FAILED_NOT_SHAPESHIFT;

                break;
            }
            case SPELL_AURA_RANGED_ATTACK_POWER_ATTACKER_BONUS:
            {
                if (!expectedTarget)
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                // can be casted at non-friendly unit or own pet/charm
                if (m_caster->IsFriendlyTo(expectedTarget))
                    return SPELL_FAILED_TARGET_FRIENDLY;

                break;
            }
            case SPELL_AURA_FLY:
            case SPELL_AURA_MOD_FLIGHT_SPEED_MOUNTED:
            {
                // not allow cast fly spells if not have req. skills  (all spells is self target)
                // allow always ghost flight spells
                if (m_caster->GetTypeId() == TYPEID_PLAYER && m_caster->isAlive())
                {
                    if (!((Player*)m_caster)->CanStartFlyInArea(m_caster->GetMapId(), zone, area))
                        return m_IsTriggeredSpell ? SPELL_FAILED_DONT_REPORT : SPELL_FAILED_NOT_HERE;
                }
                break;
            }
            case SPELL_AURA_PERIODIC_MANA_LEECH:
            {
                if (!expectedTarget)
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                if (m_caster->GetTypeId() != TYPEID_PLAYER || m_CastItem)
                    break;

                if (expectedTarget->getPowerType() != POWER_MANA)
                    return SPELL_FAILED_BAD_TARGETS;

                break;
            }
            case SPELL_AURA_MIRROR_IMAGE:
            {
                if (!expectedTarget)
                    return SPELL_FAILED_BAD_TARGETS;

                // Target must be creature. TODO: Check if target can also be player
                if (expectedTarget->GetTypeId() != TYPEID_UNIT)
                    return SPELL_FAILED_BAD_TARGETS;

                if (expectedTarget == m_caster)             // Clone self can't be accepted
                    return SPELL_FAILED_BAD_TARGETS;

                // It is assumed that target can not be cloned if already cloned by same or other clone auras
                if (expectedTarget->HasAuraType(SPELL_AURA_MIRROR_IMAGE))
                    return SPELL_FAILED_BAD_TARGETS;

                break;
            }
            case SPELL_AURA_CONTROL_VEHICLE:
            {
                Unit* pTarget = m_targets.getUnitTarget();

                // In case of TARGET_SCRIPT, we have already added a target. Use it here (and find a better solution)
                if (m_UniqueTargetInfo.size() == 1)
                    pTarget = m_caster->GetMap()->GetAnyTypeCreature(m_UniqueTargetInfo.front().targetGUID);

                if (!pTarget || !pTarget->GetVehicleKit())
                    return SPELL_FAILED_BAD_TARGETS;

                if (m_currentBasePoints[i] < 0 || m_currentBasePoints[i] > MAX_VEHICLE_SEAT)
                    return SPELL_FAILED_BAD_TARGETS;

                // -1 if "first empty seat", seat number (starting from 0) - in other cases
                int32 seat = m_currentBasePoints[i] - 1;

                if (!pTarget->GetVehicleKit()->HasEmptySeat(seat))
                {
                    if (seat == -1)
                        return SPELL_FAILED_BAD_TARGETS;

                    if (!seat && m_caster->GetTypeId() == TYPEID_PLAYER && !pTarget->GetVehicleKit()->HasEmptySeat(-1))
                        return SPELL_FAILED_BAD_TARGETS;

                    if (m_caster->GetTypeId() != TYPEID_PLAYER)
                        return SPELL_FAILED_BAD_TARGETS;
                }
                break;
            }
            case SPELL_AURA_FAR_SIGHT:
            case SPELL_AURA_BIND_SIGHT:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER || ((Player*)m_caster)->HasExternalViewPoint())
                    return SPELL_FAILED_BAD_TARGETS;
            }
            default:
                break;
        }
    }

    // and some hacks, as always
    switch(m_spellInfo->Id)
    {
        // spells that dont have direct effects listed above
        // maybe should check triggered/linked spells?
        // but not all are implemented in this way
        case 36554: // Shadowstep
        case 51690: // Killing Spree
            if (m_caster->hasUnitState(UNIT_STAT_ROOT))
                return SPELL_FAILED_ROOTED;
            break;
    }

    // check trade slot case (last, for allow catch any another cast problems)
    if (m_targets.m_targetMask & TARGET_FLAG_TRADE_ITEM)
    {
        if (m_caster->GetTypeId() != TYPEID_PLAYER)
            return SPELL_FAILED_NOT_TRADING;

        Player *pCaster = ((Player*)m_caster);
        TradeData* my_trade = pCaster->GetTradeData();

        if (!my_trade)
            return SPELL_FAILED_NOT_TRADING;

        TradeSlots slot = TradeSlots(m_targets.getItemTargetGuid().GetRawValue());
        if (slot != TRADE_SLOT_NONTRADED)
            return SPELL_FAILED_ITEM_NOT_READY;

        // if trade not complete then remember it in trade data
        if (!my_trade->IsInAcceptProcess())
        {
            // Spell will be casted at completing the trade. Silently ignore at this place
            my_trade->SetSpell(m_spellInfo->Id, m_CastItem);
            return SPELL_FAILED_DONT_REPORT;
        }
    }

    // check LOS for ground targeted spells
    if (!m_spellInfo->HasAttribute(SPELL_ATTR_EX2_IGNORE_LOS) && !m_targets.getUnitTarget() && !m_targets.getGOTarget() && !m_targets.getItemTarget())
    {
        if (!m_targets.getDestination().IsEmpty() && !m_caster->IsWithinLOS(m_targets.getDestination().x, m_targets.getDestination().y, m_targets.getDestination().z))
            return SPELL_FAILED_LINE_OF_SIGHT;
    }

    // all ok
    return SPELL_CAST_OK;
}

SpellCastResult Spell::CheckPetCast(Unit* target)
{
    if(!m_caster->isAlive() && !m_spellInfo->HasAttribute(SPELL_ATTR_CASTABLE_WHILE_DEAD))
        return SPELL_FAILED_CASTER_DEAD;

    if (m_caster->IsNonMeleeSpellCasted(false))              //prevent spellcast interruption by another spellcast
        return SPELL_FAILED_SPELL_IN_PROGRESS;
    if (m_caster->isInCombat() && IsNonCombatSpell(m_spellInfo))
        return SPELL_FAILED_AFFECTING_COMBAT;

    if (m_caster->GetTypeId()==TYPEID_UNIT && (((Creature*)m_caster)->IsPet() || m_caster->isCharmed()))
    {
                                                            //dead owner (pets still alive when owners ressed?)
        if (m_caster->GetCharmerOrOwner() && (!m_caster->GetCharmerOrOwner()->isAlive() && !(m_caster->GetCharmerOrOwner()->getDeathState() == GHOULED)))
            return SPELL_FAILED_CASTER_DEAD;

        if(!target && m_targets.getUnitTarget())
            target = m_targets.getUnitTarget();

        bool need = false;
        for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (m_spellInfo->EffectImplicitTargetA[i] == TARGET_CHAIN_DAMAGE ||
                m_spellInfo->EffectImplicitTargetA[i] == TARGET_SINGLE_FRIEND ||
                m_spellInfo->EffectImplicitTargetA[i] == TARGET_SINGLE_FRIEND_2 ||
                m_spellInfo->EffectImplicitTargetA[i] == TARGET_DUELVSPLAYER ||
                m_spellInfo->EffectImplicitTargetA[i] == TARGET_SINGLE_PARTY ||
                m_spellInfo->EffectImplicitTargetA[i] == TARGET_CURRENT_ENEMY_COORDINATES)
            {
                need = true;
                if(!target)
                {
                    DEBUG_LOG("Spell::CheckPetCast Charmed creature %s attempt to cast spell %u, but no required target",m_caster->GetObjectGuid().GetString().c_str(),m_spellInfo->Id);
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;
                }
                break;
            }
        }
        if (need)
            m_targets.setUnitTarget(target);

        Unit* _target = m_targets.getUnitTarget();

        if (_target)                                         //for target dead/target not valid
        {
            if (IsPositiveSpell(m_spellInfo->Id) && !IsDispelSpell(m_spellInfo))
            {
                if (m_caster->IsHostileTo(_target))
                {
                    DEBUG_LOG("Spell::CheckPetCast Charmed creature %s attempt to cast positive spell %u, but target %s is hostile",m_caster->GetObjectGuid().GetString().c_str(),m_spellInfo->Id, target->GetObjectGuid().GetString().c_str());
                    return SPELL_FAILED_BAD_TARGETS;
                }
            }
            else if (!_target->isTargetableForAttack() ||
            (!m_IsTriggeredSpell &&
            (!_target->isVisibleForOrDetect(m_caster,m_caster,true) && (m_caster->GetCharmerOrOwner() && !target->isVisibleForOrDetect(m_caster->GetCharmerOrOwner(),m_caster->GetCharmerOrOwner(),true)))))
            {
                DEBUG_LOG("Spell::CheckPetCast Charmed creature %s attempt to cast spell %u, but target %s is not targetable or not detectable",m_caster->GetObjectGuid().GetString().c_str(),m_spellInfo->Id,target->GetObjectGuid().GetString().c_str());
                return SPELL_FAILED_BAD_TARGETS;            // guessed error
            }
            else
            {
                bool dualEffect = false;
                for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
                {
                                                            // This effects is positive AND negative. Need for vehicles cast.
                    dualEffect |= (m_spellInfo->EffectImplicitTargetA[j] == TARGET_DUELVSPLAYER
                                   || m_spellInfo->EffectImplicitTargetA[j] == TARGET_IN_FRONT_OF_CASTER_30
                                   || m_spellInfo->EffectImplicitTargetA[j] == TARGET_MASTER
                                   || m_spellInfo->EffectImplicitTargetA[j] == TARGET_SELF2
                                   || m_spellInfo->EffectImplicitTargetA[j] == TARGET_IN_FRONT_OF_CASTER
                                   || m_spellInfo->EffectImplicitTargetA[j] == TARGET_EFFECT_SELECT
                                   || m_spellInfo->EffectImplicitTargetA[j] == TARGET_CASTER_COORDINATES);
                }
                if (!dualEffect && m_caster->getVictim() && (!IsPositiveSpell(m_spellInfo->Id) || IsDispelSpell(m_spellInfo)))
                {
                    if (!m_caster->IsHostileTo(_target) && (m_caster->GetCharmerOrOwner() && m_caster->GetCharmerOrOwner()->IsFriendlyTo(_target)))
                    {
                        DEBUG_LOG("Spell::CheckPetCast Charmed creature %s attempt to cast negative spell %u, but target %s is friendly",m_caster->GetObjectGuid().GetString().c_str(),m_spellInfo->Id, target->GetObjectGuid().GetString().c_str());
                        return SPELL_FAILED_BAD_TARGETS;
                    }
                }
                else if (!m_caster->GetVehicleKit() && m_caster->IsFriendlyTo(_target) && !(!m_caster->GetCharmerOrOwner() || !m_caster->GetCharmerOrOwner()->IsFriendlyTo(_target))
                     && !dualEffect && !IsDispelSpell(m_spellInfo))
                {
                    DEBUG_LOG("Spell::CheckPetCast Charmed creature %s attempt to cast spell %u, but target %s is not valid",m_caster->GetObjectGuid().GetString().c_str(),m_spellInfo->Id,_target->GetObjectGuid().GetString().c_str());
                    return SPELL_FAILED_BAD_TARGETS;
                }

                if (m_caster->GetObjectGuid() == _target->GetObjectGuid() && !dualEffect && !IsPositiveSpell(m_spellInfo->Id))
                {
                    DEBUG_LOG("Spell::CheckPetCast Charmed creature %s attempt to cast negative spell %u on self!",m_caster->GetObjectGuid().GetString().c_str(), m_spellInfo->Id);
                    return SPELL_FAILED_BAD_TARGETS;
                }
            }
        }
                                                            //cooldown
        if(m_caster->HasSpellCooldown(m_spellInfo))
            return SPELL_FAILED_NOT_READY;
    }

    return CheckCast(true);
}

SpellCastResult Spell::CheckCasterAuras() const
{
    // Flag drop spells totally immuned to caster auras
    // FIXME: find more nice check for all totally immuned spells
    // HasAttribute(SPELL_ATTR_EX3_UNK28) ?
    if (m_spellInfo->Id == 23336 ||                         // Alliance Flag Drop
        m_spellInfo->Id == 23334 ||                         // Horde Flag Drop
        m_spellInfo->Id == 34991)                           // Summon Netherstorm Flag
        return SPELL_CAST_OK;

    uint8 school_immune = 0;
    uint32 mechanic_immune = 0;
    uint32 dispel_immune = 0;

    // Check if the spell grants school or mechanic immunity.
    // We use bitmasks so the loop is done only once and not on every aura check below.
    if (m_spellInfo->HasAttribute(SPELL_ATTR_EX_DISPEL_AURAS_ON_IMMUNITY))
    {
        for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_SCHOOL_IMMUNITY)
                school_immune |= uint32(m_spellInfo->EffectMiscValue[i]);
            else if (m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MECHANIC_IMMUNITY)
                mechanic_immune |= 1 << uint32(m_spellInfo->EffectMiscValue[i]-1);
            else if (m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MECHANIC_IMMUNITY_MASK)
                mechanic_immune |= uint32(m_spellInfo->EffectMiscValue[i]);
            else if (m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_DISPEL_IMMUNITY)
                dispel_immune |= GetDispellMask(DispelType(m_spellInfo->EffectMiscValue[i]));
        }

        // immune movement impairment and loss of control (spell data have special structure for mark this case)
        if (IsSpellRemoveAllMovementAndControlLossEffects(m_spellInfo))
            mechanic_immune = IMMUNE_TO_MOVEMENT_IMPAIRMENT_AND_LOSS_CONTROL_MASK;
    }

    // Check whether the cast should be prevented by any state you might have.
    SpellCastResult prevented_reason = SPELL_CAST_OK;
    bool spellUsableWhileStunned = m_spellInfo->HasAttribute(SPELL_ATTR_EX5_USABLE_WHILE_STUNNED);

    // Have to check if there is a stun aura. Otherwise will have problems with ghost aura apply while logging out
    uint32 unitflag = m_caster->GetUInt32Value(UNIT_FIELD_FLAGS);     // Get unit state
    if (unitflag & UNIT_FLAG_STUNNED)
    {
        // Pain Suppression (have SPELL_ATTR_EX5_USABLE_WHILE_STUNNED that must be used only with glyph)
        if (m_spellInfo->SpellFamilyName == SPELLFAMILY_PRIEST && m_spellInfo->GetSpellIconID() == 2178)
        {
            if (!m_caster->HasAura(63248))                  // Glyph of Pain Suppression
                spellUsableWhileStunned = false;
        }

        // spell is usable while stunned, check if caster has only mechanic stun auras, another stun types must prevent cast spell
        if (spellUsableWhileStunned)
        {
            bool is_stun_mechanic = true;
            Unit::AuraList const& stunAuras = m_caster->GetAurasByType(SPELL_AURA_MOD_STUN);
            for (Unit::AuraList::const_iterator itr = stunAuras.begin(); itr != stunAuras.end(); ++itr)
                if (!(*itr)->HasMechanic(MECHANIC_STUN))
                {
                    is_stun_mechanic = false;
                    break;
                }
            if (!is_stun_mechanic)
                prevented_reason = SPELL_FAILED_STUNNED;
        }
        else
            prevented_reason = SPELL_FAILED_STUNNED;
    }
    else if ((unitflag & UNIT_FLAG_CONFUSED) && !m_spellInfo->HasAttribute(SPELL_ATTR_EX5_USABLE_WHILE_CONFUSED))
        prevented_reason = SPELL_FAILED_CONFUSED;
    else if ((unitflag & UNIT_FLAG_FLEEING) && !m_spellInfo->HasAttribute(SPELL_ATTR_EX5_USABLE_WHILE_FEARED))
        prevented_reason = SPELL_FAILED_FLEEING;
    else if ((unitflag & UNIT_FLAG_SILENCED) && m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
        prevented_reason = SPELL_FAILED_SILENCED;
    else if ((unitflag & UNIT_FLAG_PACIFIED) && m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_PACIFY)
        prevented_reason = SPELL_FAILED_PACIFIED;
    else if (m_caster->HasAuraType(SPELL_AURA_ALLOW_ONLY_ABILITY))
    {
        Unit::AuraList const& casingLimit = m_caster->GetAurasByType(SPELL_AURA_ALLOW_ONLY_ABILITY);
        for(Unit::AuraList::const_iterator itr = casingLimit.begin(); itr != casingLimit.end(); ++itr)
        {
            if(!(*itr)->isAffectedOnSpell(m_spellInfo))
            {
                prevented_reason = SPELL_FAILED_CASTER_AURASTATE;
                break;
            }
        }
    }

    // Attr must make flag drop spell totally immune from all effects
    if (prevented_reason != SPELL_CAST_OK)
    {
        if (school_immune || mechanic_immune || dispel_immune)
        {
            //Checking auras is needed now, because you are prevented by some state but the spell grants immunity.
            MAPLOCK_READ(m_caster, MAP_LOCK_TYPE_AURAS);
            Unit::SpellAuraHolderMap const& auras = m_caster->GetSpellAuraHolderMap();
            for(Unit::SpellAuraHolderMap::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
            {
                SpellEntry const * pEntry = itr->second->GetSpellProto();

                if ((GetSpellSchoolMask(pEntry) & school_immune) && !pEntry->HasAttribute(SPELL_ATTR_EX_UNAFFECTED_BY_SCHOOL_IMMUNE))
                    continue;
                if ((1<<(pEntry->Dispel)) & dispel_immune)
                    continue;

                for (int32 i = 0; i < MAX_EFFECT_INDEX; ++i)
                {
                    Aura *aura = itr->second->GetAuraByEffectIndex(SpellEffectIndex(i));
                    if (!aura)
                        continue;

                    if (GetSpellMechanicMask(pEntry, 1 << i) & mechanic_immune)
                        continue;
                    // Make a second check for spell failed so the right SPELL_FAILED message is returned.
                    // That is needed when your casting is prevented by multiple states and you are only immune to some of them.
                    switch(aura->GetModifier()->m_auraname)
                    {
                        case SPELL_AURA_MOD_STUN:
                            if (!spellUsableWhileStunned || !aura->HasMechanic(MECHANIC_STUN))
                                return SPELL_FAILED_STUNNED;
                            break;
                        case SPELL_AURA_MOD_CONFUSE:
                            if (!m_spellInfo->HasAttribute(SPELL_ATTR_EX5_USABLE_WHILE_CONFUSED))
                                return SPELL_FAILED_CONFUSED;
                            break;
                        case SPELL_AURA_MOD_FEAR:
                            if (!m_spellInfo->HasAttribute(SPELL_ATTR_EX5_USABLE_WHILE_FEARED))
                                return SPELL_FAILED_FLEEING;
                            break;
                        case SPELL_AURA_MOD_SILENCE:
                        case SPELL_AURA_MOD_PACIFY:
                        case SPELL_AURA_MOD_PACIFY_SILENCE:
                            if ( m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_PACIFY)
                                return SPELL_FAILED_PACIFIED;
                            else if ( m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
                                return SPELL_FAILED_SILENCED;
                            break;
                        default: break;
                    }
                }
            }
        }
        // You are prevented from casting and the spell casted does not grant immunity. Return a failed error.
        else
            return prevented_reason;
    }
    return SPELL_CAST_OK;
}

SpellCastResult Spell::CheckCastTargets() const
{

    if (!IsSpellRequiresTarget(m_spellInfo) || IsSpellWithCasterSourceTargetsOnly(m_spellInfo))
        return SPELL_CAST_OK;

    // Spell without any target
    if ( !m_targets.HasLocation() &&
        m_UniqueTargetInfo.empty() &&
        m_UniqueGOTargetInfo.empty() &&
        m_UniqueItemInfo.empty())
        return SPELL_FAILED_BAD_TARGETS;

    // recheck by target mask - NY currently
    /*
    if (m_targets.m_targetMask & (TARGET_FLAG_DEST_LOCATION | TARGET_FLAG_SRC_LOCATION))
        if (!m_targets.HasLocation())
            return SPELL_FAILED_BAD_TARGETS;
    if (m_targets.m_targetMask & (TARGET_FLAG_SELF | TARGET_FLAG_UNIT))
        if (m_UniqueTargetInfo.empty())
            return SPELL_FAILED_BAD_TARGETS;
    if (m_targets.m_targetMask & TARGET_FLAG_ITEM)
        if (m_UniqueItemInfo.empty())
            return SPELL_FAILED_BAD_TARGETS;
    if (m_targets.m_targetMask & (TARGET_FLAG_OBJECT | TARGET_FLAG_OBJECT_UNK))
        if (m_UniqueGOTargetInfo.empty())
            return SPELL_FAILED_BAD_TARGETS;
    */

    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        switch (m_spellInfo->Effect[i])
        {
            case SPELL_EFFECT_LEAP:
            {
                // Destination point checked only in third (CheckTargets) check phase, in first/second his not setted
                if (m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION)
                {
                    // spell failed, if distance less then 1.0f
                    if (m_caster->GetDistance(m_targets.getDestination()) < 1.0f)
                        return SPELL_FAILED_TOO_CLOSE;

                    if (fabs(m_caster->GetPositionZ() - m_targets.getDestination().getZ()) > 8.0f)
                        return SPELL_FAILED_NOPATH;

                    float pz  = m_caster->GetMap()->GetHeight(m_caster->GetPhaseMask(), m_targets.getDestination().getX(), m_targets.getDestination().getY(), m_targets.getDestination().getZ());

                    if (fabs(pz - m_targets.getDestination().getZ()) > 8.0f)
                        return SPELL_FAILED_NOPATH;
                }
                else
                    return SPELL_FAILED_TRY_AGAIN;
                break;
            }
            default:
                break;
        }
    }

    return SPELL_CAST_OK;
}

SpellCastResult Spell::CanAutoCast(Unit* target)
{
    ObjectGuid targetguid = target->GetObjectGuid();

    for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
    {
        if (m_spellInfo->Effect[j] == SPELL_EFFECT_APPLY_AURA)
        {
            if ( m_spellInfo->StackAmount <= 1)
            {
                if (target->HasAura(m_spellInfo->Id, SpellEffectIndex(j)))
                    return SPELL_FAILED_TARGET_AURASTATE;
            }
            else
            {
                if (Aura* aura = target->GetAura(m_spellInfo->Id, SpellEffectIndex(j)))
                    if (aura->GetStackAmount() >= m_spellInfo->StackAmount)
                        return SPELL_FAILED_TARGET_AURASTATE;
            }
        }
        else if (IsAreaAuraEffect( m_spellInfo->Effect[j] ))
        {
            if ( target->HasAura(m_spellInfo->Id, SpellEffectIndex(j)) )
                return SPELL_FAILED_TARGET_AURASTATE;
        }
    }

    SpellCastResult result = CheckPetCast(target);

    if (result == SPELL_CAST_OK || result == SPELL_FAILED_UNIT_NOT_INFRONT)
    {
        FillTargetMap();
        //check if among target units, our WANTED target is as well (->only self cast spells return false)
        for (TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
            if (ihit->targetGUID == targetguid)
                return result;
    }
    return result;                                           //target invalid
}

SpellCastResult Spell::CheckRange(bool strict, WorldObject* checkTarget /*=NULL*/)
{
    Unit* pTarget = (checkTarget && checkTarget->GetObjectGuid().IsUnit()) ? (Unit*)checkTarget : m_targets.getUnitTarget();
    GameObject* pGoTarget = (checkTarget && checkTarget->GetObjectGuid().IsGameObject()) ? (GameObject*)checkTarget : m_targets.getGOTarget();

    SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(m_spellInfo->GetRangeIndex());

    bool friendly = pTarget ? pTarget->IsFriendlyTo(m_caster) : false;
    float max_range = GetSpellMaxRange(srange, friendly);
    float min_range = GetSpellMinRange(srange, friendly);
    float add_range = bool(checkTarget) ? checkTarget->GetObjectBoundingRadius() : (strict ? 1.25f : 6.25f);

    // special range cases
    switch (m_spellInfo->GetRangeIndex())
    {
        // self cast doesn't need range checking -- also for Starshards fix
        // spells that can be cast anywhere also need no check
        case SPELL_RANGE_IDX_SELF_ONLY:
        case SPELL_RANGE_IDX_ANYWHERE:
            return SPELL_CAST_OK;
        // combat range spells are treated differently
        case SPELL_RANGE_IDX_COMBAT:
        {
            if (pTarget)
            {
                if (pTarget == m_caster)
                    return SPELL_CAST_OK;

                if (m_caster->GetTypeId() == TYPEID_PLAYER &&
                    (m_spellInfo->FacingCasterFlags & SPELL_FACING_FLAG_INFRONT) && !m_caster->HasInArc(M_PI_F, pTarget))
                    return SPELL_FAILED_UNIT_NOT_INFRONT;

                float combat_range = m_caster->GetCombatDistance(pTarget, true);
                float range_mod = combat_range + add_range;

                if (Player* modOwner = m_caster->GetSpellModOwner())
                    modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_RANGE, range_mod);

                float range_delta = range_mod - combat_range;

                // with additional 5 dist for non stricted case (some melee spells have delay in apply)
                return m_caster->CanReachWithMeleeAttack(pTarget, range_delta) ? SPELL_CAST_OK : SPELL_FAILED_OUT_OF_RANGE;
            }
            break;                                          // let continue in generic way for no target
        }
        default:
            // add radius of caster and ~5 yds "give" for non stricred (landing) check
            max_range += add_range;
            break;
    }

    if (Player* modOwner = m_caster->GetSpellModOwner())
        modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_RANGE, max_range);

    if (pTarget && pTarget != m_caster)
    {
        // distance from target in checks
        float dist = m_caster->GetCombatDistance(pTarget, m_spellInfo->GetRangeIndex() == SPELL_RANGE_IDX_COMBAT);

        if (dist > max_range)
            return SPELL_FAILED_OUT_OF_RANGE;
        if (min_range && dist < min_range)
            return SPELL_FAILED_TOO_CLOSE;
        if (m_caster->GetTypeId() == TYPEID_PLAYER &&
            (m_spellInfo->FacingCasterFlags & SPELL_FACING_FLAG_INFRONT) && !m_caster->HasInArc(M_PI_F, pTarget))
            return SPELL_FAILED_UNIT_NOT_INFRONT;
    }

    if (pGoTarget)
    {
        // distance from target in checks
        float dist = m_caster->GetDistance(pGoTarget);

        if (dist > max_range)
            return SPELL_FAILED_OUT_OF_RANGE;
        if (min_range && dist < min_range)
            return SPELL_FAILED_TOO_CLOSE;
        if (m_caster->GetTypeId() == TYPEID_PLAYER &&
            (m_spellInfo->FacingCasterFlags & SPELL_FACING_FLAG_INFRONT) && !m_caster->HasInArc(M_PI_F, pGoTarget))
            return SPELL_FAILED_NOT_INFRONT;
    }

    // TODO verify that such spells really use bounding radius
    if ((max_range || min_range) && m_targets.HasLocation())
    {
        WorldLocation const& loc = m_targets.GetLocation();

        if (max_range && !m_caster->IsWithinDist3d(loc, max_range))
            return SPELL_FAILED_OUT_OF_RANGE;
        if (min_range && m_caster->IsWithinDist3d(loc, min_range))
            return SPELL_FAILED_TOO_CLOSE;
    }

    return SPELL_CAST_OK;
}

int32 Spell::CalculatePowerCost(SpellEntry const* spellInfo, Unit* caster, Spell const* spell, Item* castItem)
{
    // item cast not used power
    if (castItem)
        return 0;

    // Spell drain all exist power on cast (Only paladin lay of Hands)
    if (spellInfo->HasAttribute(SPELL_ATTR_EX_DRAIN_ALL_POWER))
    {
        // If power type - health drain all
        if (spellInfo->GetPowerType() == POWER_HEALTH)
            return caster->GetHealth();
        // Else drain all power
        if (spellInfo->GetPowerType() < MAX_POWERS)
            return caster->GetPower(Powers(spellInfo->GetPowerType()));
        sLog.outError("Spell::CalculateManaCost: Unknown power type '%d' in spell %d", spellInfo->GetPowerType(), spellInfo->Id);
        return 0;
    }

    // Base powerCost
    int32 powerCost = spellInfo->manaCost;
    // PCT cost from total amount
    if (spellInfo->ManaCostPercentage)
    {
        switch (spellInfo->GetPowerType())
        {
            // health as power used
            case POWER_HEALTH:
                powerCost += spellInfo->ManaCostPercentage * caster->GetCreateHealth() / 100;
                break;
            case POWER_MANA:
                powerCost += spellInfo->ManaCostPercentage * caster->GetCreateMana() / 100;
                break;
            case POWER_RAGE:
            case POWER_FOCUS:
            case POWER_ENERGY:
            case POWER_HAPPINESS:
                powerCost += spellInfo->ManaCostPercentage * caster->GetMaxPower(Powers(spellInfo->GetPowerType())) / 100;
                break;
            case POWER_RUNE:
            case POWER_RUNIC_POWER:
                DEBUG_LOG("Spell::CalculateManaCost: Not implemented yet!");
                break;
            default:
                sLog.outError("Spell::CalculateManaCost: Unknown power type '%d' in spell %d", spellInfo->GetPowerType(), spellInfo->Id);
                return 0;
        }
    }
    SpellSchools school = GetFirstSchoolInMask(spell ? spell->m_spellSchoolMask : GetSpellSchoolMask(spellInfo));
    // Flat mod from caster auras by spell school
    powerCost += caster->GetInt32Value(UNIT_FIELD_POWER_COST_MODIFIER + school);
    // Shiv - costs 20 + weaponSpeed*10 energy (apply only to non-triggered spell with energy cost)
    if (spellInfo->HasAttribute(SPELL_ATTR_EX4_SPELL_VS_EXTEND_COST))
        powerCost += caster->GetAttackTime(OFF_ATTACK) / 100;
    // Apply cost mod by spell
    if (spell)
        if (Player* modOwner = caster->GetSpellModOwner())
            modOwner->ApplySpellMod(spellInfo->Id, SPELLMOD_COST, powerCost);

    if (spellInfo->HasAttribute(SPELL_ATTR_LEVEL_DAMAGE_CALCULATION))
        powerCost = int32(powerCost/ (1.117f * spellInfo->spellLevel / caster->getLevel() -0.1327f));

    // PCT mod from user auras by school
    powerCost = ceil((float)powerCost * (1.0f + caster->GetFloatValue(UNIT_FIELD_POWER_COST_MULTIPLIER + school)));

    return powerCost;
}

SpellCastResult Spell::CheckPower()
{
    // item cast not used power
    if (m_CastItem)
        return SPELL_CAST_OK;

    // Do precise power regen on spell cast
    if (m_powerCost > 0 && m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        Player* playerCaster = (Player*)m_caster;
        uint32 lastRegenInterval = playerCaster->IsUnderLastManaUseEffect() ? REGEN_TIME_PRECISE : REGEN_TIME_FULL;
        int32 diff = lastRegenInterval - m_caster->GetRegenTimer();
        if (diff >= REGEN_TIME_PRECISE)
            playerCaster->RegenerateAll(diff);
    }

    // health as power used - need check health amount
    if (Powers(m_spellInfo->GetPowerType()) == POWER_HEALTH)
    {
        if (m_caster->GetHealth() <= uint32(abs(m_powerCost)))
            return SPELL_FAILED_CASTER_AURASTATE;
        return SPELL_CAST_OK;
    }

    // Check valid power type
    if (m_spellInfo->GetPowerType() >= MAX_POWERS)
    {
        sLog.outError("Spell::CheckMana: Unknown power type '%d'", m_spellInfo->GetPowerType());
        return SPELL_FAILED_UNKNOWN;
    }

    //check rune cost only if a spell has PowerType == POWER_RUNE
    if (m_spellInfo->GetPowerType() == POWER_RUNE)
    {
        SpellCastResult failReason = CheckOrTakeRunePower(false);
        if (failReason != SPELL_CAST_OK)
            return failReason;
    }

    // Check power amount
    if (m_powerCost > 0 && (int32(m_caster->GetPower(Powers(m_spellInfo->GetPowerType()))) < m_powerCost))
        return SPELL_FAILED_NO_POWER;

    return SPELL_CAST_OK;
}

bool Spell::IgnoreItemRequirements() const
{
    /// Check if it's an enchant scroll. These have no required reagents even though their spell does.
    if (m_CastItem && (m_CastItem->GetProto()->Flags & ITEM_FLAG_ENCHANT_SCROLL))
        return true;

    if (m_IsTriggeredSpell)
    {
        /// Not own traded item (in trader trade slot) req. reagents including triggered spell case
        if (Item* targetItem = m_targets.getItemTarget())
            if (targetItem->GetOwnerGuid() != m_caster->GetObjectGuid())
                return false;

        /// Some triggered spells have same reagents that have master spell
        /// expected in test: master spell have reagents in first slot then triggered don't must use own
        if (m_triggeredBySpellInfo && !m_triggeredBySpellInfo->Reagent[0])
            return false;

        return true;
    }

    return false;
}

SpellCastResult Spell::CheckItems()
{
    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return SPELL_CAST_OK;

    Player* p_caster = (Player*)m_caster;
    bool isScrollItem = false;
    bool isVellumTarget = false;

    // cast item checks
    if (m_CastItem)
    {
        if (m_CastItem->IsInTrade())
            return SPELL_FAILED_ITEM_NOT_FOUND;

        uint32 itemid = m_CastItem->GetEntry();
        if ( !p_caster->HasItemCount(itemid, 1) )
            return SPELL_FAILED_ITEM_NOT_FOUND;

        ItemPrototype const *proto = m_CastItem->GetProto();
        if(!proto)
            return SPELL_FAILED_ITEM_NOT_FOUND;

        if (proto->Flags & ITEM_FLAG_ENCHANT_SCROLL)
            isScrollItem = true;

        for (int i = 0; i < 5; ++i)
            if (proto->Spells[i].SpellCharges)
                if (m_CastItem->GetSpellCharges(i) == 0)
                    return SPELL_FAILED_NO_CHARGES_REMAIN;

        // consumable cast item checks
        if (proto->Class == ITEM_CLASS_CONSUMABLE && m_targets.getUnitTarget())
        {
            // such items should only fail if there is no suitable effect at all - see Rejuvenation Potions for example
            SpellCastResult failReason = SPELL_CAST_OK;
            for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                // skip check, pet not required like checks, and for TARGET_PET m_targets.getUnitTarget() is not the real target but the caster
                if (m_spellInfo->EffectImplicitTargetA[i] == TARGET_PET)
                    continue;

                if (m_spellInfo->Effect[i] == SPELL_EFFECT_HEAL)
                {
                    if (m_targets.getUnitTarget()->GetHealth() == m_targets.getUnitTarget()->GetMaxHealth())
                    {
                        failReason = SPELL_FAILED_ALREADY_AT_FULL_HEALTH;
                        continue;
                    }
                    else
                    {
                        failReason = SPELL_CAST_OK;
                        break;
                    }
                }

                // Mana Potion, Rage Potion, Thistle Tea(Rogue), ...
                if (m_spellInfo->Effect[i] == SPELL_EFFECT_ENERGIZE)
                {
                    if (m_spellInfo->EffectMiscValue[i] < 0 || m_spellInfo->EffectMiscValue[i] >= MAX_POWERS)
                    {
                        failReason = SPELL_FAILED_ALREADY_AT_FULL_POWER;
                        continue;
                    }

                    Powers power = Powers(m_spellInfo->EffectMiscValue[i]);
                    uint8 targetClass = m_targets.getUnitTarget()->getClass();

                    if (power == POWER_MANA)
                    {
                        if (targetClass == CLASS_WARRIOR || targetClass == CLASS_ROGUE)
                        {
                            failReason = SPELL_FAILED_BAD_TARGETS;
                            continue;
                        }
                    }
                    else if (power == POWER_RAGE)
                    {
                        if (targetClass != CLASS_WARRIOR && targetClass != CLASS_DRUID)
                        {
                            failReason = SPELL_FAILED_BAD_TARGETS;
                            continue;
                        }
                    }
                    else if (power == POWER_ENERGY)
                    {
                        if (targetClass != CLASS_ROGUE && targetClass != CLASS_DRUID)
                        {
                            failReason = SPELL_FAILED_BAD_TARGETS;
                            continue;
                        }
                    }

                    if (m_targets.getUnitTarget()->GetPower(power) == m_targets.getUnitTarget()->GetMaxPower(power))
                    {
                        failReason = SPELL_FAILED_ALREADY_AT_FULL_POWER;
                        continue;
                    }
                    else
                    {
                        failReason = SPELL_CAST_OK;
                        break;
                    }
                }
            }

            if (failReason != SPELL_CAST_OK)
                return failReason;
        }
    }

    // check target item (for triggered case not report error)
    if (m_targets.getItemTargetGuid())
    {
        if (m_caster->GetTypeId() != TYPEID_PLAYER)
            return m_IsTriggeredSpell && !(m_targets.m_targetMask & TARGET_FLAG_TRADE_ITEM)
                ? SPELL_FAILED_DONT_REPORT : SPELL_FAILED_BAD_TARGETS;

        if (!m_targets.getItemTarget())
            return m_IsTriggeredSpell  && !(m_targets.m_targetMask & TARGET_FLAG_TRADE_ITEM)
                ? SPELL_FAILED_DONT_REPORT : SPELL_FAILED_ITEM_GONE;

        isVellumTarget = m_targets.getItemTarget()->GetProto()->IsVellum();
        if (!m_targets.getItemTarget()->IsFitToSpellRequirements(m_spellInfo))
            return m_IsTriggeredSpell  && !(m_targets.m_targetMask & TARGET_FLAG_TRADE_ITEM)
                ? SPELL_FAILED_DONT_REPORT : SPELL_FAILED_EQUIPPED_ITEM_CLASS;

        // Do not enchant vellum with scroll
        if (isVellumTarget && isScrollItem)
            return m_IsTriggeredSpell  && !(m_targets.m_targetMask & TARGET_FLAG_TRADE_ITEM)
                ? SPELL_FAILED_DONT_REPORT : SPELL_FAILED_BAD_TARGETS;
    }
    // if not item target then required item must be equipped (for triggered case not report error)
    else
    {
        if (m_caster->GetTypeId() == TYPEID_PLAYER && !((Player*)m_caster)->HasItemFitToSpellReqirements(m_spellInfo))
            return m_IsTriggeredSpell ? SPELL_FAILED_DONT_REPORT : SPELL_FAILED_EQUIPPED_ITEM_CLASS;
    }

    // check reagents (ignore triggered spells with reagents processed by original spell) and special reagent ignore case.
    if (!IgnoreItemRequirements())
    {
        if (!p_caster->CanNoReagentCast(m_spellInfo))
        {
            for(uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            {
                if (m_spellInfo->Reagent[i] <= 0)
                    continue;

                uint32 itemid    = m_spellInfo->Reagent[i];
                uint32 itemcount = m_spellInfo->ReagentCount[i];

                // if CastItem is also spell reagent
                if (m_CastItem && m_CastItem->GetEntry() == itemid)
                {
                    ItemPrototype const *proto = m_CastItem->GetProto();
                    if (!proto)
                        return SPELL_FAILED_REAGENTS;
                    for(int s = 0; s < MAX_ITEM_PROTO_SPELLS; ++s)
                    {
                        // CastItem will be used up and does not count as reagent
                        int32 charges = m_CastItem->GetSpellCharges(s);
                        if (proto->Spells[s].SpellCharges < 0 && !(proto->ExtraFlags & ITEM_EXTRA_NON_CONSUMABLE) && abs(charges) < 2)
                        {
                            ++itemcount;
                            break;
                        }
                    }
                }

                if (!p_caster->HasItemCount(itemid, itemcount))
                    return SPELL_FAILED_REAGENTS;
            }
        }

        // check totem-item requirements (items presence in inventory)
        uint32 totems = MAX_SPELL_TOTEMS;
        for(int i = 0; i < MAX_SPELL_TOTEMS ; ++i)
        {
            if (m_spellInfo->Totem[i] != 0)
            {
                if (p_caster->HasItemCount(m_spellInfo->Totem[i], 1))
                {
                    totems -= 1;
                    continue;
                }
            }
            else
                totems -= 1;
        }

        if (totems != 0)
            return SPELL_FAILED_TOTEMS;

        // Check items for TotemCategory  (items presence in inventory)
        uint32 TotemCategory = MAX_SPELL_TOTEM_CATEGORIES;
        for(int i= 0; i < MAX_SPELL_TOTEM_CATEGORIES; ++i)
        {
            if (m_spellInfo->TotemCategory[i] != 0)
            {
                if (p_caster->HasItemTotemCategory(m_spellInfo->TotemCategory[i]))
                {
                    TotemCategory -= 1;
                    continue;
                }
            }
            else
                TotemCategory -= 1;
        }

        if (TotemCategory != 0)
            return SPELL_FAILED_TOTEM_CATEGORY;
    }

    // special checks for spell effects
    for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        switch (m_spellInfo->Effect[i])
        {
            case SPELL_EFFECT_CREATE_ITEM:
            {
                if (!m_IsTriggeredSpell && m_spellInfo->EffectItemType[i])
                {
                    // Conjure Mana Gem (skip same or low level ranks for later recharge)
                    if (i == EFFECT_INDEX_0 && m_spellInfo->Effect[EFFECT_INDEX_1] == SPELL_EFFECT_DUMMY)
                    {
                        if (ItemPrototype const* itemProto = ObjectMgr::GetItemPrototype(m_spellInfo->EffectItemType[i]))
                        {
                            if (Item* item = p_caster->GetItemByLimitedCategory(itemProto->ItemLimitCategory))
                            {
                                if (item->GetProto()->ItemLevel <= itemProto->ItemLevel)
                                {
                                    if (item->HasMaxCharges())
                                        return SPELL_FAILED_ITEM_AT_MAX_CHARGES;

                                    // will recharge in next effect
                                    continue;
                                }
                            }
                        }
                    }

                    ItemPosCountVec dest;
                    InventoryResult msg = p_caster->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, m_spellInfo->EffectItemType[i], 1 );
                    if (msg != EQUIP_ERR_OK )
                    {
                        p_caster->SendEquipError( msg, NULL, NULL, m_spellInfo->EffectItemType[i] );
                        return SPELL_FAILED_DONT_REPORT;
                    }
                }
                break;
            }
            case SPELL_EFFECT_RESTORE_ITEM_CHARGES:
            {
                if (Item* item = p_caster->GetItemByEntry(m_spellInfo->EffectItemType[i]))
                    if (item->HasMaxCharges())
                        return SPELL_FAILED_ITEM_AT_MAX_CHARGES;

                break;
            }
            case SPELL_EFFECT_ENCHANT_ITEM:
            case SPELL_EFFECT_ENCHANT_ITEM_PRISMATIC:
            {
                Item* targetItem = m_targets.getItemTarget();
                if(!targetItem)
                    return SPELL_FAILED_ITEM_NOT_FOUND;

                if ( targetItem->GetProto()->ItemLevel < m_spellInfo->baseLevel )
                    return SPELL_FAILED_LOWLEVEL;
                // Check if we can store a new scroll, enchanting vellum has implicit SPELL_EFFECT_CREATE_ITEM
                if (isVellumTarget && m_spellInfo->EffectItemType[i])
                {
                    ItemPosCountVec dest;
                    InventoryResult msg = p_caster->CanStoreNewItem( NULL_BAG, NULL_SLOT, dest, m_spellInfo->EffectItemType[i], 1 );
                    if (msg != EQUIP_ERR_OK)
                    {
                        p_caster->SendEquipError( msg, NULL, NULL );
                        return SPELL_FAILED_DONT_REPORT;
                    }
                }
                // Not allow enchant in trade slot for some enchant type
                if ( targetItem->GetOwner() != m_caster )
                {
                    uint32 enchant_id = m_spellInfo->EffectMiscValue[i];
                    SpellItemEnchantmentEntry const *pEnchant = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
                    if(!pEnchant)
                        return SPELL_FAILED_ERROR;
                    if (pEnchant->slot & ENCHANTMENT_CAN_SOULBOUND)
                        return SPELL_FAILED_NOT_TRADEABLE;
                    // cannot replace vellum with scroll in trade slot
                    if (isVellumTarget)
                        return SPELL_FAILED_ITEM_ENCHANT_TRADE_WINDOW;
                }
                break;
            }
            case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
            {
                Item *item = m_targets.getItemTarget();
                if(!item)
                    return SPELL_FAILED_ITEM_NOT_FOUND;
                // Not allow enchant in trade slot for some enchant type
                if ( item->GetOwner() != m_caster )
                {
                    uint32 enchant_id = m_spellInfo->EffectMiscValue[i];
                    SpellItemEnchantmentEntry const *pEnchant = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
                    if(!pEnchant)
                        return SPELL_FAILED_ERROR;
                    if (pEnchant->slot & ENCHANTMENT_CAN_SOULBOUND)
                        return SPELL_FAILED_NOT_TRADEABLE;
                }
                break;
            }
            case SPELL_EFFECT_ENCHANT_HELD_ITEM:
                // check item existence in effect code (not output errors at offhand hold item effect to main hand for example
                break;
            case SPELL_EFFECT_DISENCHANT:
            {
                if(!m_targets.getItemTarget())
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;

                // prevent disenchanting in trade slot
                if ( m_targets.getItemTarget()->GetOwnerGuid() != m_caster->GetObjectGuid() )
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;

                ItemPrototype const* itemProto = m_targets.getItemTarget()->GetProto();
                if(!itemProto)
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;

                // must have disenchant loot (other static req. checked at item prototype loading)
                if (!itemProto->DisenchantID)
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;

                // 2.0.x addon: Check player enchanting level against the item disenchanting requirements
                int32 item_disenchantskilllevel = itemProto->RequiredDisenchantSkill;
                if (item_disenchantskilllevel > int32(p_caster->GetSkillValue(SKILL_ENCHANTING)))
                    return SPELL_FAILED_LOW_CASTLEVEL;
                break;
            }
            case SPELL_EFFECT_PROSPECTING:
            {
                if(!m_targets.getItemTarget())
                    return SPELL_FAILED_CANT_BE_PROSPECTED;
                // ensure item is a prospectable ore
                if (!(m_targets.getItemTarget()->GetProto()->Flags & ITEM_FLAG_PROSPECTABLE))
                    return SPELL_FAILED_CANT_BE_PROSPECTED;
                // prevent prospecting in trade slot
                if (m_targets.getItemTarget()->GetOwnerGuid() != m_caster->GetObjectGuid())
                    return SPELL_FAILED_CANT_BE_PROSPECTED;
                // Check for enough skill in jewelcrafting
                uint32 item_prospectingskilllevel = m_targets.getItemTarget()->GetProto()->RequiredSkillRank;
                if (item_prospectingskilllevel >p_caster->GetSkillValue(SKILL_JEWELCRAFTING))
                    return SPELL_FAILED_LOW_CASTLEVEL;
                // make sure the player has the required ores in inventory
                if (int32(m_targets.getItemTarget()->GetCount()) < CalculateDamage(SpellEffectIndex(i), m_caster))
                    return SPELL_FAILED_NEED_MORE_ITEMS;

                if (!LootTemplates_Prospecting.HaveLootFor(m_targets.getItemTargetEntry()))
                    return SPELL_FAILED_CANT_BE_PROSPECTED;

                break;
            }
            case SPELL_EFFECT_MILLING:
            {
                if(!m_targets.getItemTarget())
                    return SPELL_FAILED_CANT_BE_MILLED;
                // ensure item is a millable herb
                if (!(m_targets.getItemTarget()->GetProto()->Flags & ITEM_FLAG_MILLABLE))
                    return SPELL_FAILED_CANT_BE_MILLED;
                // prevent milling in trade slot
                if (m_targets.getItemTarget()->GetOwnerGuid() != m_caster->GetObjectGuid())
                    return SPELL_FAILED_CANT_BE_MILLED;
                // Check for enough skill in inscription
                uint32 item_millingskilllevel = m_targets.getItemTarget()->GetProto()->RequiredSkillRank;
                if (item_millingskilllevel >p_caster->GetSkillValue(SKILL_INSCRIPTION))
                    return SPELL_FAILED_LOW_CASTLEVEL;
                // make sure the player has the required herbs in inventory
                if (int32(m_targets.getItemTarget()->GetCount()) < CalculateDamage(SpellEffectIndex(i), m_caster))
                    return SPELL_FAILED_NEED_MORE_ITEMS;

                if(!LootTemplates_Milling.HaveLootFor(m_targets.getItemTargetEntry()))
                    return SPELL_FAILED_CANT_BE_MILLED;

                break;
            }
            case SPELL_EFFECT_WEAPON_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER) return SPELL_FAILED_TARGET_NOT_PLAYER;
                if ( m_attackType != RANGED_ATTACK )
                    break;
                Item *pItem = ((Player*)m_caster)->GetWeaponForAttack(m_attackType,true,false);
                if (!pItem)
                    return SPELL_FAILED_EQUIPPED_ITEM;

                switch(pItem->GetProto()->SubClass)
                {
                    case ITEM_SUBCLASS_WEAPON_THROWN:
                    {
                        uint32 ammo = pItem->GetEntry();
                        if ( !((Player*)m_caster)->HasItemCount( ammo, 1 ) )
                            return SPELL_FAILED_NO_AMMO;
                    };  break;
                    case ITEM_SUBCLASS_WEAPON_GUN:
                    case ITEM_SUBCLASS_WEAPON_BOW:
                    case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                    {
                        uint32 ammo = ((Player*)m_caster)->GetUInt32Value(PLAYER_AMMO_ID);
                        if(!ammo)
                        {
                            // Requires No Ammo
                            if (m_caster->GetDummyAura(46699))
                                break;                      // skip other checks

                            return SPELL_FAILED_NO_AMMO;
                        }

                        ItemPrototype const *ammoProto = ObjectMgr::GetItemPrototype( ammo );
                        if(!ammoProto)
                            return SPELL_FAILED_NO_AMMO;

                        if (ammoProto->Class != ITEM_CLASS_PROJECTILE)
                            return SPELL_FAILED_NO_AMMO;

                        // check ammo ws. weapon compatibility
                        switch(pItem->GetProto()->SubClass)
                        {
                            case ITEM_SUBCLASS_WEAPON_BOW:
                            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                                if (ammoProto->SubClass != ITEM_SUBCLASS_ARROW)
                                    return SPELL_FAILED_NO_AMMO;
                                break;
                            case ITEM_SUBCLASS_WEAPON_GUN:
                                if (ammoProto->SubClass != ITEM_SUBCLASS_BULLET)
                                    return SPELL_FAILED_NO_AMMO;
                                break;
                            default:
                                return SPELL_FAILED_NO_AMMO;
                        }

                        if ( !((Player*)m_caster)->HasItemCount( ammo, 1 ) )
                            return SPELL_FAILED_NO_AMMO;
                    };  break;
                    case ITEM_SUBCLASS_WEAPON_WAND:
                        break;
                    default:
                        break;
                }
                break;
            }
            default:break;
        }
    }

    return SPELL_CAST_OK;
}

void Spell::Delayed()
{
    if(!m_caster || m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    if (m_spellState == SPELL_STATE_DELAYED)
        return;                                             // spell is active and can't be time-backed

    if (isDelayableNoMore())                                 // Spells may only be delayed twice
        return;

    // spells not loosing casting time ( slam, dynamites, bombs.. )
    if(!(m_spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_DAMAGE))
        return;

    // check pushback reduce
    int32 delaytime = 500;                                  // spellcasting delay is normally 500ms
    int32 delayReduce = 100;                                // must be initialized to 100 for percent modifiers
    ((Player*)m_caster)->ApplySpellMod(m_spellInfo->Id, SPELLMOD_NOT_LOSE_CASTING_TIME, delayReduce);
    delayReduce += m_caster->GetTotalAuraModifier(SPELL_AURA_REDUCE_PUSHBACK) - 100;
    if (delayReduce >= 100)
        return;

    delaytime = delaytime * (100 - delayReduce) / 100;

    if (int32(m_timer) + delaytime > m_casttime)
    {
        delaytime = m_casttime - m_timer;
        m_timer = m_casttime;
    }
    else
        m_timer += delaytime;

    DETAIL_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell %u partially interrupted for (%d) ms at damage", m_spellInfo->Id, delaytime);

    WorldPacket data(SMSG_SPELL_DELAYED, m_caster->GetPackGUID().size() + 4);
    data << m_caster->GetPackGUID();
    data << uint32(delaytime);

    m_caster->SendMessageToSet(&data, true);
}

void Spell::DelayedChannel()
{
    if(!m_caster || m_caster->GetTypeId() != TYPEID_PLAYER || getState() != SPELL_STATE_CASTING)
        return;

    if (isDelayableNoMore())                                 // Spells may only be delayed twice
        return;

    // check pushback reduce
    int32 delaytime = GetSpellDuration(m_spellInfo) * 25 / 100;// channeling delay is normally 25% of its time per hit
    int32 delayReduce = 100;                                // must be initialized to 100 for percent modifiers
    ((Player*)m_caster)->ApplySpellMod(m_spellInfo->Id, SPELLMOD_NOT_LOSE_CASTING_TIME, delayReduce);
    delayReduce += m_caster->GetTotalAuraModifier(SPELL_AURA_REDUCE_PUSHBACK) - 100;
    if (delayReduce >= 100)
        return;

    delaytime = delaytime * (100 - delayReduce) / 100;

    if (int32(m_timer) < delaytime)
    {
        delaytime = m_timer;
        m_timer = 0;
    }
    else
        m_timer -= delaytime;

    DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell %u partially interrupted for %i ms, new duration: %u ms", m_spellInfo->Id, delaytime, m_timer);

    for(TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if ((*ihit).missCondition == SPELL_MISS_NONE)
        {
            if (Unit* unit = m_caster->GetObjectGuid() == ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID))
                unit->DelaySpellAuraHolder(m_spellInfo->Id, delaytime, unit->GetObjectGuid());
        }
    }

    for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
    {
        // partially interrupt persistent area auras
        if (DynamicObject* dynObj = m_caster->GetDynObject(m_spellInfo->Id, SpellEffectIndex(j)))
            dynObj->Delay(delaytime);
    }

    SendChannelUpdate(m_timer);
}

void Spell::UpdateOriginalCasterPointer()
{
    if (m_originalCasterGuid == m_caster->GetObjectGuid())
        m_originalCaster = m_caster;
    else if (m_originalCasterGuid.IsGameObject())
    {
        GameObject* go = m_caster->IsInWorld() ? m_caster->GetMap()->GetGameObject(m_originalCasterGuid) : NULL;
        m_originalCaster = go ? go->GetOwner() : NULL;
    }
    else
    {
        Unit* unit = ObjectAccessor::GetUnit(*m_caster, m_originalCasterGuid);
        m_originalCaster = unit && unit->IsInWorld() ? unit : NULL;
    }
}

void Spell::UpdatePointers()
{
    UpdateOriginalCasterPointer();

    m_targets.Update(m_caster);
}

CurrentSpellTypes Spell::GetCurrentContainer()
{
    if (IsNextMeleeSwingSpell())
        return (CURRENT_MELEE_SPELL);
    else if (IsAutoRepeat())
        return (CURRENT_AUTOREPEAT_SPELL);
    else if (IsChanneledSpell(m_spellInfo))
        return (CURRENT_CHANNELED_SPELL);
    else
        return (CURRENT_GENERIC_SPELL);
}


bool Spell::IsNeedSendToClient() const
{
    return m_spellInfo->GetSpellVisual() || m_spellInfo->SpellVisual[1] || IsChanneledSpell(m_spellInfo) ||
        m_spellInfo->GetSpeed() > 0.0f || (!m_triggeredByAuraSpell && !m_IsTriggeredSpell);
}

bool Spell::IsTriggeredSpellWithRedundentCastTime() const
{
    return m_IsTriggeredSpell && (m_spellInfo->manaCost || m_spellInfo->ManaCostPercentage);
}

bool Spell::HaveTargetsForEffect(SpellEffectIndex effect) const
{
    for(TargetList::const_iterator itr = m_UniqueTargetInfo.begin(); itr != m_UniqueTargetInfo.end(); ++itr)
        if (itr->effectMask & (1 << effect))
            return true;

    for(GOTargetList::const_iterator itr = m_UniqueGOTargetInfo.begin(); itr != m_UniqueGOTargetInfo.end(); ++itr)
        if (itr->effectMask & (1 << effect))
            return true;

    for(ItemTargetList::const_iterator itr = m_UniqueItemInfo.begin(); itr != m_UniqueItemInfo.end(); ++itr)
        if (itr->effectMask & (1 << effect))
            return true;

    return false;
}

SpellCastResult Spell::CanOpenLock(SpellEffectIndex effIndex, uint32 lockId, SkillType& skillId, int32& reqSkillValue, int32& skillValue)
{
    if(!lockId)                                             // possible case for GO and maybe for items.
        return SPELL_CAST_OK;

    // Get LockInfo
    LockEntry const *lockInfo = sLockStore.LookupEntry(lockId);

    if (!lockInfo)
        return SPELL_FAILED_BAD_TARGETS;

    bool reqKey = false;                                    // some locks not have reqs

    for(int j = 0; j < 8; ++j)
    {
        switch(lockInfo->Type[j])
        {
            // check key item (many fit cases can be)
            case LOCK_KEY_ITEM:
                if (lockInfo->Index[j] && m_CastItem && m_CastItem->GetEntry()==lockInfo->Index[j])
                    return SPELL_CAST_OK;
                reqKey = true;
                break;
                // check key skill (only single first fit case can be)
            case LOCK_KEY_SKILL:
            {
                reqKey = true;

                // wrong locktype, skip
                if (uint32(m_spellInfo->EffectMiscValue[effIndex]) != lockInfo->Index[j])
                    continue;

                skillId = SkillByLockType(LockType(lockInfo->Index[j]));

                if ( skillId != SKILL_NONE )
                {
                    // skill bonus provided by casting spell (mostly item spells)
                    // add the damage modifier from the spell casted (cheat lock / skeleton key etc.) (use m_currentBasePoints, CalculateDamage returns wrong value)
                    uint32 spellSkillBonus = uint32(m_currentBasePoints[effIndex]);
                    reqSkillValue = lockInfo->Skill[j];

                    // castitem check: rogue using skeleton keys. the skill values should not be added in this case.
                    skillValue = m_CastItem || m_caster->GetTypeId()!= TYPEID_PLAYER ?
                        0 : ((Player*)m_caster)->GetSkillValue(skillId);

                    skillValue += spellSkillBonus;

                    if (skillValue < reqSkillValue)
                        return SPELL_FAILED_LOW_CASTLEVEL;
                }

                return SPELL_CAST_OK;
            }
        }
    }

    if (reqKey)
        return SPELL_FAILED_BAD_TARGETS;

    return SPELL_CAST_OK;
}

/**
 * Fill target list by units around (x,y) points at radius distance

 * @param targetUnitMap        Reference to target list that filled by function
 * @param x                    X coordinates of center point for target search
 * @param y                    Y coordinates of center point for target search
 * @param radius               Radius around (x,y) for target search
 * @param pushType             Additional rules for target area selection (in front, angle, etc)
 * @param spellTargets         Additional rules for target selection base at hostile/friendly state to original spell caster
 * @param originalCaster       If provided set alternative original caster, if =NULL then used Spell::GetAffectiveObject() return
 */
void Spell::FillAreaTargets(UnitList &targetUnitMap, float radius, SpellNotifyPushType pushType, SpellTargets spellTargets, WorldObject* originalCaster /*=NULL*/)
{
    MaNGOS::SpellNotifierCreatureAndPlayer notifier(*this, targetUnitMap, radius, pushType, spellTargets, originalCaster);
    Cell::VisitAllObjects(notifier.GetCenterX(), notifier.GetCenterY(), m_caster->GetMap(), notifier, radius);
}

void Spell::FillRaidOrPartyTargets(UnitList &targetUnitMap, Unit* member, Unit* center, float radius, bool raid, bool withPets, bool withcaster)
{
    Player *pMember = member->GetCharmerOrOwnerPlayerOrPlayerItself();
    Group *pGroup = pMember ? pMember->GetGroup() : NULL;

    if (pGroup)
    {
        uint8 subgroup = pMember->GetSubGroup();

        for (GroupReference* itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
        {
            Player* Target = itr->getSource();

            if (!Target || !Target->IsInWorld() || !Target->GetMap())
                continue;

            // IsHostileTo check duel and controlled by enemy
            if (Target && (raid || subgroup==Target->GetSubGroup())
                && !m_caster->IsHostileTo(Target))
            {
                if ((Target == center || center->IsWithinDistInMap(Target, radius)) &&
                    (withcaster || Target != m_caster))
                    targetUnitMap.push_back(Target);

                if (withPets)
                {
                    GuidSet const& groupPets = Target->GetPets();
                    if (!groupPets.empty())
                    {
                        for (GuidSet::const_iterator itr = groupPets.begin(); itr != groupPets.end(); ++itr)
                        {
                            if (Pet* pPet = Target->GetMap()->GetPet(*itr))
                            {
                                if ((pPet == center || center->IsWithinDistInMap(pPet, radius)) &&
                                    (withcaster || pPet != m_caster))
                                     targetUnitMap.push_back(pPet);
                            }
                        }
                    }
                }
            }
        }
    }
    else
    {
        Unit* ownerOrSelf = pMember ? pMember : member->GetCharmerOrOwnerOrSelf();
        if ((ownerOrSelf == center || center->IsWithinDistInMap(ownerOrSelf, radius)) &&
            (withcaster || ownerOrSelf != m_caster))
            targetUnitMap.push_back(ownerOrSelf);

        if (withPets)
            if (Pet* pet = ownerOrSelf->GetPet())
                if ((pet == center || center->IsWithinDistInMap(pet, radius)) &&
                    (withcaster || pet != m_caster))
                    targetUnitMap.push_back(pet);
    }
}

void Spell::FillRaidOrPartyManaPriorityTargets(UnitList &targetUnitMap, Unit* member, Unit* center, float radius, uint32 count, bool raid, bool withPets, bool withCaster)
{
    FillRaidOrPartyTargets(targetUnitMap, member, center, radius, raid, withPets, withCaster);

    PrioritizeManaUnitQueue manaUsers;
    for(UnitList::const_iterator itr = targetUnitMap.begin(); itr != targetUnitMap.end(); ++itr)
        if ((*itr)->getPowerType() == POWER_MANA && !(*itr)->isDead())
            manaUsers.push(PrioritizeManaUnitWraper(*itr));

    targetUnitMap.clear();
    while(!manaUsers.empty() && targetUnitMap.size() < count)
    {
        targetUnitMap.push_back(manaUsers.top().getUnit());
        manaUsers.pop();
    }
}

void Spell::FillRaidOrPartyHealthPriorityTargets(UnitList &targetUnitMap, Unit* member, Unit* center, float radius, uint32 count, bool raid, bool withPets, bool withCaster)
{
    FillRaidOrPartyTargets(targetUnitMap, member, center, radius, raid, withPets, withCaster);

    PrioritizeHealthUnitQueue healthQueue;
    for (UnitList::const_iterator itr = targetUnitMap.begin(); itr != targetUnitMap.end(); ++itr)
    {
        Unit* unit = *itr;
        if (!unit)
            continue;
        if (unit->isDead() || !unit->IsInWorld())
            continue;
        healthQueue.push(PrioritizeHealthUnitWraper(unit));
    }

    targetUnitMap.clear();
    while(!healthQueue.empty() && targetUnitMap.size() < count)
    {
        targetUnitMap.push_back(healthQueue.top().getUnit());
        healthQueue.pop();
    }
}

WorldObject* Spell::GetAffectiveCasterObject() const
{
    if (!m_originalCasterGuid)
        return m_caster;

    if (m_originalCasterGuid.IsGameObject() && m_caster->GetMap())
        return m_caster->GetMap()->GetGameObject(m_originalCasterGuid);
    return m_originalCaster;
}

WorldObject* Spell::GetCastingObject() const
{
    if (m_originalCasterGuid.IsGameObject())
        return m_caster->GetMap() ? m_caster->GetMap()->GetGameObject(m_originalCasterGuid) : NULL;
    else
        return m_caster;
}

void Spell::ResetEffectDamageAndHeal()
{
    m_damage = 0;
    m_healing = 0;
    m_damageIndex = -1;
}

void Spell::SelectMountByAreaAndSkill(Unit* target, SpellEntry const* parentSpell, uint32 spellId75, uint32 spellId150, uint32 spellId225, uint32 spellId300, uint32 spellIdSpecial)
{
    if (!target || target->GetTypeId() != TYPEID_PLAYER)
        return;

    // Prevent stacking of mounts
    target->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
    uint16 skillval = ((Player*)target)->GetSkillValue(SKILL_RIDING);
    if (!skillval)
        return;

    if (skillval >= 225 && (spellId300 > 0 || spellId225 > 0))
    {
        uint32 spellid = skillval >= 300 ? spellId300 : spellId225;
        SpellEntry const *pSpell = sSpellStore.LookupEntry(spellid);
        if (!pSpell)
        {
            sLog.outError("SelectMountByAreaAndSkill: unknown spell id %i by caster: %s", spellid, target->GetGuidStr().c_str());
            return;
        }

        // zone check
        uint32 zone, area;
        target->GetZoneAndAreaId(zone, area);

        SpellCastResult locRes= sSpellMgr.GetSpellAllowedInLocationError(pSpell, target->GetMapId(), zone, area, target->GetCharmerOrOwnerPlayerOrPlayerItself());
        if (locRes != SPELL_CAST_OK || !((Player*)target)->CanStartFlyInArea(target->GetMapId(), zone, area))
            target->CastSpell(target, spellId150, true, NULL, NULL, ObjectGuid(), parentSpell);
        else if (spellIdSpecial > 0)
        {
            for (PlayerSpellMap::const_iterator iter = ((Player*)target)->GetSpellMap().begin(); iter != ((Player*)target)->GetSpellMap().end(); ++iter)
            {
                if (iter->second.state != PLAYERSPELL_REMOVED)
                {
                    SpellEntry const *spellInfo = sSpellStore.LookupEntry(iter->first);
                    for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
                    {
                        if (spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MOD_FLIGHT_SPEED_MOUNTED)
                        {
                            int32 mountSpeed = spellInfo->CalculateSimpleValue(SpellEffectIndex(i));

                            // speed higher than 280 replace it
                            if (mountSpeed > 280)
                            {
                                target->CastSpell(target, spellIdSpecial, true, NULL, NULL, ObjectGuid(), parentSpell);
                                return;
                            }
                        }
                    }
                }
            }
            target->CastSpell(target, pSpell, true, NULL, NULL, ObjectGuid(), parentSpell);
        }
        else
            target->CastSpell(target, pSpell, true, NULL, NULL, ObjectGuid(), parentSpell);
    }
    else if (skillval >= 150 && spellId150 > 0)
        target->CastSpell(target, spellId150, true, NULL, NULL, ObjectGuid(), parentSpell);
    else if (spellId75 > 0)
        target->CastSpell(target, spellId75, true, NULL, NULL, ObjectGuid(), parentSpell);

    return;
}

void Spell::ClearCastItem()
{
    if (m_CastItem==m_targets.getItemTarget())
        m_targets.setItemTarget(NULL);

    m_CastItem = NULL;
}

bool Spell::HasGlobalCooldown()
{
    // global cooldown have only player or controlled units
    if (m_caster->GetCharmInfo())
        return m_caster->GetCharmInfo()->GetGlobalCooldownMgr().HasGlobalCooldown(m_spellInfo);
    else if (m_caster->GetTypeId() == TYPEID_PLAYER)
        return ((Player*)m_caster)->GetGlobalCooldownMgr().HasGlobalCooldown(m_spellInfo);
    else
        return false;
}

void Spell::TriggerGlobalCooldown()
{
    int32 gcd = m_spellInfo->StartRecoveryTime;
    if (!gcd)
        return;

    // global cooldown can't leave range 1..1.5 secs (if it it)
    // exist some spells (mostly not player directly casted) that have < 1 sec and > 1.5 sec global cooldowns
    // but its as test show not affected any spell mods.
    if (gcd >= 1000 && gcd <= 1500)
    {
        // gcd modifier auras applied only to self spells and only player have mods for this
        if (m_caster->GetTypeId() == TYPEID_PLAYER)
            ((Player*)m_caster)->ApplySpellMod(m_spellInfo->Id, SPELLMOD_GLOBAL_COOLDOWN, gcd);

        // apply haste rating
        gcd = int32(float(gcd) * m_caster->GetFloatValue(UNIT_MOD_CAST_SPEED));

        if (gcd < 1000)
            gcd = 1000;
        else if (gcd > 1500)
            gcd = 1500;
    }

    // global cooldown have only player or controlled units
    if (m_caster->GetCharmInfo())
        m_caster->GetCharmInfo()->GetGlobalCooldownMgr().AddGlobalCooldown(m_spellInfo, gcd);
    else if (m_caster->GetTypeId() == TYPEID_PLAYER)
        ((Player*)m_caster)->GetGlobalCooldownMgr().AddGlobalCooldown(m_spellInfo, gcd);
}

void Spell::CancelGlobalCooldown()
{
    if (!m_spellInfo->StartRecoveryTime)
        return;

    // cancel global cooldown when interrupting current cast
    if (m_caster->GetCurrentSpell(CURRENT_GENERIC_SPELL) != this)
        return;

    // global cooldown have only player or controlled units
    if (m_caster->GetCharmInfo())
        m_caster->GetCharmInfo()->GetGlobalCooldownMgr().CancelGlobalCooldown(m_spellInfo);
    else if (m_caster->GetTypeId() == TYPEID_PLAYER)
        ((Player*)m_caster)->GetGlobalCooldownMgr().CancelGlobalCooldown(m_spellInfo);
}

float Spell::GetBaseSpellSpeed()
{
    // Speed possible inherited from triggering spell
    float speed_proto = (fabs(m_spellInfo->GetSpeed()) < M_NULL_F && m_triggeredBySpellInfo) ?
                            m_triggeredBySpellInfo->GetSpeed() : m_spellInfo->GetSpeed();

    // spell speed calculation for charge-like spells
    if (m_spellInfo->HasAttribute(SPELL_ATTR_EX7_HAS_CHARGE_EFFECT)
        && (fabs(speed_proto) < M_NULL_F))
        speed_proto = BASE_CHARGE_SPEED;

    return speed_proto;
}

bool Spell::FillCustomTargetMap(SpellEffectIndex i, UnitList& targetUnitMap, uint8& effToIndex)
{
    float radius;
    uint32 unMaxTargets = 0;

    if (m_spellInfo->EffectRadiusIndex[i])
        radius = GetSpellRadius(sSpellRadiusStore.LookupEntry(m_spellInfo->EffectRadiusIndex[i]));
    else
        radius = GetSpellMaxRange(sSpellRangeStore.LookupEntry(m_spellInfo->GetRangeIndex()));

    // Resulting effect depends on spell that we want to cast
    switch (m_spellInfo->Id)
    {
        case 19185: // Entrapment
        case 64803:
        case 64804:
        {
            FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            break;
        }
        // Voracious Appetite && Cannibalize && Carrion Feeder additional check
        case 20577:
        case 52748:
        case 54044:
        {
            if (m_targets.getUnitTarget() && m_targets.getUnitTarget() != m_caster)
                targetUnitMap.push_back(m_targets.getUnitTarget());
            else
                targetUnitMap.clear();
            break;
        }
        case 28374: // Decimate - Gluth encounter
        {
            FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_ALL);
            targetUnitMap.remove(m_caster);
            break;
        }
        case 43263: // Ghoul Taunt (Army of the Dead)
        {           // exclude Player and WorldBoss targets
            FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            for (UnitList::iterator itr = targetUnitMap.begin(); itr != targetUnitMap.end();)
            {
                if (!*itr || (*itr)->GetTypeId() == TYPEID_PLAYER || static_cast<Creature*>(*itr)->IsWorldBoss())
                    itr = targetUnitMap.erase(itr);
                else
                    ++itr;
            }
            if (!targetUnitMap.empty())
                return true;
            break;
        }
        case 46584: // Raise Dead
        {
            Unit* pCorpseTarget = NULL;
            WorldObject* result = FindCorpseUsing<MaNGOS::RaiseDeadObjectCheck>(CREATURE_TYPEMASK_HUMANOID_OR_UNDEAD);
            if (result && result->IsInWorld())
            {
                switch (result->GetTypeId())
                {
                    case TYPEID_UNIT:
                    case TYPEID_PLAYER:
                        pCorpseTarget =(Unit*)result;
                        break;
                    case TYPEID_CORPSE:
                        if (Player* owner = ObjectAccessor::FindPlayer(((Corpse*)result)->GetOwnerGuid()))
                            if (owner->IsInMap(m_caster) && !owner->isAlive())
                                pCorpseTarget =(Unit*)owner;
                        break;
                    default:
                        /* pCorpseTarget == NULL;*/
                        break;
                }
            }
            targetUnitMap.push_back(pCorpseTarget ? pCorpseTarget : (Unit*)m_caster);
            break;
        }
        case 47496: // Ghoul's explode
        case 50444:
        {
            FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            break;
        }
        case 48018: // Demonic Circle: Summon
        case 60854: // Demonic Circle: Clear
        {
            targetUnitMap.push_back(m_caster);
            break;
        }
        case 48743: // Death Pact
        {
            if (i != EFFECT_INDEX_1)
                return false;

            Unit* unitTarget = m_targets.getUnitTarget();
            if (unitTarget && (unitTarget->GetEntry() == 26125 || unitTarget->GetEntry() == 24207) &&
                unitTarget->GetObjectGuid().IsPet() && unitTarget->GetOwnerGuid() == m_caster->GetObjectGuid())
            {
                targetUnitMap.push_back(unitTarget);
            }
            else
            {
                GuidSet const& guardians = m_caster->GetGuardians();
                if (!guardians.empty())
                {
                    for (GuidSet::const_iterator itr = guardians.begin(); itr != guardians.end(); ++itr)
                    {
                        if (Unit* ghoul = m_caster->GetMap()->GetUnit(*itr))
                            if (ghoul->GetEntry() == 24207 || ghoul->GetEntry() == 26125)
                                targetUnitMap.push_back(ghoul);
                    }

                    if (targetUnitMap.size() > 1)
                    {
                        targetUnitMap.sort(TargetDistanceOrderNear(m_caster));
                        targetUnitMap.resize(1);
                    }
                }

                if (targetUnitMap.empty())
                   if (m_caster->GetPet() && m_caster->GetPet()->GetEntry() == 26125)
                       targetUnitMap.push_back((Unit*)m_caster->GetPet());
            }

            if (targetUnitMap.empty())
            {
                // no valid targets, clear cooldown at fail
                m_caster->RemoveSpellCooldown(m_spellInfo->Id, true);
                SendCastResult(SPELL_FAILED_NO_VALID_TARGETS);
                finish(false);
            }
            break;

        }
        case 49158: // Corpse explosion
        case 51325:
        case 51326:
        case 51327:
        case 51328:
        {
            Unit* unitTarget = NULL;

            targetUnitMap.remove(m_caster);
            unitTarget = m_targets.getUnitTarget();

            // Cast on corpses...
            if ((unitTarget &&
                unitTarget->getDeathState() == CORPSE &&
                (unitTarget->GetCreatureTypeMask() & CREATURE_TYPEMASK_MECHANICAL_OR_ELEMENTAL) == 0) ||
                    // ...or own Risen Ghoul pet - self explode effect
                (unitTarget && unitTarget->GetEntry() == 26125 &&
                unitTarget->GetCreatorGuid() == m_caster->GetObjectGuid()))
            {
                targetUnitMap.push_back(unitTarget);
            }
            else
            {
                Unit* pCorpseTarget = NULL;
                MaNGOS::NearestCorpseInObjectRangeCheck u_check(*m_caster, radius);
                MaNGOS::UnitLastSearcher<MaNGOS::NearestCorpseInObjectRangeCheck> searcher(pCorpseTarget, u_check);
                Cell::VisitGridObjects(m_caster, searcher, radius);

                if (pCorpseTarget)
                    targetUnitMap.push_back(pCorpseTarget);
            }

            if (targetUnitMap.empty())
            {
                // no valid targets, clear cooldown at fail
                m_caster->RemoveSpellCooldown(m_spellInfo->Id, true);
                SendCastResult(SPELL_FAILED_NO_VALID_TARGETS);
                finish(false);
            }
            break;
        }
        case 49821: // Mind Sear
        case 53022:
        {
            Unit* unitTarget = m_targets.getUnitTarget();
            FillAreaTargets(targetUnitMap, radius, PUSH_INHERITED_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            if  (unitTarget)
                targetUnitMap.remove(unitTarget);
            return true;
        }
        case 50294: // Starfall triggered in-range damage (all ranks)
        case 53188:
        case 53189:
        case 53190:
        {
            Unit* unitTarget = m_targets.getUnitTarget();
            FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            if  (unitTarget)
            {
                targetUnitMap.remove(unitTarget);
                m_targets.setDestination(unitTarget->GetPosition());
            }
            break;
        }
        case 57143: // Life Burst (Wyrmrest Skytalon)
        {
            // hack - spell is AoE but implicitTargets dont match here :/
            SetTargetMap(SpellEffectIndex(i), TARGET_ALL_FRIENDLY_UNITS_AROUND_CASTER, targetUnitMap);
            targetUnitMap.push_back(m_caster);
            break;
        }
        case 57557: // Pyrobuffet (Sartharion encounter) - don't target Range Markered units
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_HOSTILE);
            if (!tempTargetUnitMap.empty())
            {
                for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
                {
                    if ((*iter) && !(*iter)->HasAura(m_spellInfo->CalculateSimpleValue(EFFECT_INDEX_2)))
                        targetUnitMap.push_back(*iter);
                }
            }

            if (!targetUnitMap.empty())
                return true;

            break;
        }
        case 48278: //Svala - Banshee Paralize
        {
            UnitList tmpUnitMap;
            FillAreaTargets(tmpUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);

            if (tmpUnitMap.empty())
                break;

            for (UnitList::const_iterator itr = tmpUnitMap.begin(); itr != tmpUnitMap.end(); ++itr)
            {
                 if (!*itr) continue;

                 if ((*itr)->HasAura(48267))
                     targetUnitMap.push_back(*itr);
            }
            break;
        }
        case 58912: // Deathstorm
        {
            if (!m_caster->GetObjectGuid().IsVehicle())
                break;

            SetTargetMap(SpellEffectIndex(i), TARGET_RANDOM_ENEMY_CHAIN_IN_AREA, targetUnitMap);
            break;
        }
        case 57496: // Volazj Insanity
        {
            UnitList PlayerList;
            FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);

            if (targetUnitMap.empty())
                break;

            for (UnitList::const_iterator itr = targetUnitMap.begin(); itr != targetUnitMap.end(); ++itr)
            {
                 if (!*itr) continue;

                 if ((*itr)->GetTypeId() == TYPEID_PLAYER)
                     PlayerList.push_back(*itr);
            }

            if (PlayerList.empty() || PlayerList.size() > 5 || PlayerList.size() < 2)
                break;

            uint32 uiPhaseIndex = 0;
            uint32 uiSummonIndex;
            for (UnitList::const_iterator itr = PlayerList.begin(); itr != PlayerList.end(); ++itr)
            {
                if (!*itr) continue;

                (*itr)->CastSpell((*itr), 57508+uiPhaseIndex, true);

                uiSummonIndex = 0;
                for (UnitList::const_iterator iter = PlayerList.begin(); iter != PlayerList.end(); ++iter)
                {
                    if (!*iter) continue;

                    if ((*itr) != (*iter))
                        (*itr)->CastSpell((*itr), 57500+uiSummonIndex, true);
                    uiSummonIndex++;
                }
                uiPhaseIndex++;
            }
            break;
        }
        case 58836:  // Initialize Images
        {
            if (i != EFFECT_INDEX_0)
                return false;

            GuidSet const& guardians = m_caster->GetGuardians();
            if (guardians.empty())
                return true;

            for (GuidSet::const_iterator itr = guardians.begin(); itr != guardians.end(); ++itr)
                if (Pet* pPet = m_caster->GetMap()->GetPet(*itr))
                    targetUnitMap.push_back(pPet);

            break;
        }
        case 59754:                    //Rune Tap triggered from Glyph of Rune Tap
        {
            FillRaidOrPartyTargets(targetUnitMap, m_caster, m_caster, radius, false, true, false);
            break;
        }
        case 61999: // Raise ally
        {
            WorldObject* result = FindCorpseUsing<MaNGOS::RaiseAllyObjectCheck>(CREATURE_TYPEMASK_NONE);
            if (result && result->GetTypeId() == TYPEID_UNIT)
                targetUnitMap.push_back((Unit*)result);
            else
                targetUnitMap.push_back((Unit*)m_caster);
            break;
        }
        case 62343: // Heat (remove all except active iron constructs)
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_ALL);

            for (UnitList::iterator itr = tempTargetUnitMap.begin(),next; itr != tempTargetUnitMap.end(); ++itr)
            {
                if ((*itr) && (*itr)->GetEntry() == 33121 &&
                    !(*itr)->HasAura(62468) && !(*itr)->HasAura(62373) &&
                    !(*itr)->HasAura(62382) && !(*itr)->HasAura(67114)
                    )
                {
                    targetUnitMap.push_back(*itr);
                }
            }
            break;
        }
        case 62488: // Activate Constructs (remove all except inactive constructs)
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_NOT_HOSTILE);
            tempTargetUnitMap.sort(TargetDistanceOrderNear(m_caster));
            for (UnitList::iterator itr = tempTargetUnitMap.begin(),next; itr != tempTargetUnitMap.end(); ++itr)
            {
                if ((*itr) && (*itr)->GetEntry() == 33121 && (*itr)->HasAura(62468)) // check for stun aura
                {
                    targetUnitMap.push_back(*itr);
                    return true;    // only one Construct per Spell
                }
            }
            break;
        }
        case 50811: // Shatter (normal&heroic) (Krystallus in Halls of Stone)
        case 61547:
        case 63025: // Gravity Bomb (XT-002 in Ulduar) - exclude caster from pull and double damage
        case 64233:
        {
            FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_HOSTILE);
            targetUnitMap.remove(m_caster);
            break;
        }
        case 52659: // Static Overload (normal&heroic) (Ionar in Halls of Lightning)
        case 59796:
        case 63023: // Searing Light (normal&heroic) (XT-002 in Ulduar)
        case 65120:
        {
            FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_HOSTILE);
            break;
        }
        case 63278: // Mark of Faceless
        {
            if (i != EFFECT_INDEX_1)
                return false;

            Unit* currentTarget = m_targets.getUnitTarget();
            if (currentTarget)
            {
                m_targets.setDestination(currentTarget->GetPosition());
                FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
                targetUnitMap.remove(currentTarget);
            }
            break;
        }
        case 65044: // Flames
        case 65045: // Flame of demolisher
        {
            FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            break;
        }
        case 65919: // Anub'arak Cast Check Ice Spell (Trial of the Crusader - Anub'arak)
        case 67858:
        case 67859:
        case 67860:
        {
            m_caster->CastSpell(m_caster, 66181, true);
            FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            break;
        }
        case 67470: // Pursuing Spikes (Check Aura and Summon Spikes) (Trial Of The Crusader - Anub'arak)
        {
            UnitList tmpUnitMap;
            bool m_bOneTargetHaveAura = false;

            FillAreaTargets(tmpUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            if (!tmpUnitMap.empty())
            {
                for (UnitList::const_iterator itr = tmpUnitMap.begin(); itr != tmpUnitMap.end(); ++itr)
                {
                    if ((*itr)->HasAura(67574))
                    {
                        m_bOneTargetHaveAura = true;
                        break;
                    }
                    else
                    {
                        if ((*itr)->GetTypeId() == TYPEID_PLAYER)
                            targetUnitMap.push_back(*itr);
                    }
                }
                if (!m_bOneTargetHaveAura && !targetUnitMap.empty())
                {
                    uint32 t = 0;
                    UnitList::iterator iter = targetUnitMap.begin();
                    while (iter != targetUnitMap.end() && (*iter)->IsWithinDist(m_caster, radius))
                    {
                        ++t;
                        ++iter;
                    }

                    iter = targetUnitMap.begin();
                    std::advance(iter, urand(0, t-1));
                    if (*iter)
                        (*iter)->CastSpell((*iter), 67574, true);
                }
            }
            break;
        }
        case 68508: // Crushing Leap (IoC bosses)
        {
            UnitList tmpUnitMap;
            FillAreaTargets(tmpUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            for (UnitList::const_iterator itr = tmpUnitMap.begin(); itr != tmpUnitMap.end(); ++itr)
            {
                if (*itr && (*itr)->GetTypeId() == TYPEID_PLAYER && !(*itr)->m_movementInfo.HasMovementFlag(MovementFlags(MOVEFLAG_FALLING)))
                    targetUnitMap.push_back(*itr);
            }
            break;
        }
        case 66862: // Radiance (Trial of the Champion - Eadric the Pure)
        case 67681:
        {
            UnitList tmpUnitMap;
            FillAreaTargets(tmpUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            for (UnitList::const_iterator itr = tmpUnitMap.begin(); itr != tmpUnitMap.end(); ++itr)
            {
                if (*itr && (*itr)->isInFrontInMap(m_caster, DEFAULT_VISIBILITY_DISTANCE) && (*itr)->IsWithinLOSInMap(m_caster))
                    targetUnitMap.push_back(*itr);
            }
            break;
        }
        case 69099: // Ice Pulse (Lich King)
        case 73776:
        case 73777:
        case 73778:
        {
            FillAreaTargets(targetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_AOE_DAMAGE, GetAffectiveCaster());
            break;
        }
        case 69298: // Cancel Resistant To Blight (Festergut)
        {
            UnitList tmpUnitMap;
            FillAreaTargets(tmpUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            for (UnitList::const_iterator itr = tmpUnitMap.begin(); itr != tmpUnitMap.end(); ++itr)
            {
                if ((*itr) && (*itr)->GetTypeId() == TYPEID_PLAYER)
                    targetUnitMap.push_back(*itr);
            }
            break;
        }
        case 69782: // Ooze Flood (Rotface)
        {
            UnitList tempTargetUnitMap;
            UnitList oozesMap;

            targetUnitMap.clear();
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_ALL);
            // stalkers in valves
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                // Puddle Stalker
                if ((*iter)->GetEntry() == 37013 && (*iter)->GetPositionZ() < m_caster->GetPositionZ() + 3.0f)
                    oozesMap.push_back((*iter));
            }

            UnitList::iterator itr;

            // 2 random targets
            if (!oozesMap.empty())
            {
                itr = oozesMap.begin();
                std::advance(itr, urand(0, oozesMap.size() - 1));

                // first Ooze Flood
                targetUnitMap.push_back(*itr);
                oozesMap.remove(*itr);

                // now find the second - closest one
                if (!oozesMap.empty())
                {
                    oozesMap.sort(TargetDistanceOrderNear(*itr));
                    itr = oozesMap.begin();
                    targetUnitMap.push_back(*itr);
                }

                return true;
            }

            return false;
        }
        case 69538: // Small Ooze Combine (Rotface)
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_ALL);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if ((*iter)->GetEntry() == 36897)
                    targetUnitMap.push_back((*iter));
            }

            targetUnitMap.remove(m_caster);

            if (!targetUnitMap.empty())
                targetUnitMap.resize(1);

            break;
        }
        case 69553: // Large Ooze Combine (Rotface)
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_ALL);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if ((*iter)->GetEntry() == 36899)
                    targetUnitMap.push_back((*iter));
            }

            targetUnitMap.remove(m_caster);

            if (!targetUnitMap.empty())
                targetUnitMap.resize(1);

            break;
        }
        case 69610: // Large Ooze Buff Combine (Rotface)
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_ALL);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if ((*iter)->GetEntry() == 36897)
                    targetUnitMap.push_back((*iter));
            }

            targetUnitMap.remove(m_caster);

            if (!targetUnitMap.empty())
                targetUnitMap.resize(1);

            break;
        }
        case 69674:                                 // Mutated Infection (Rotface)
        case 71224:                                 // Mutated Infection (Rotface)
        case 73023:                                 // Mutated Infection (heroic)
        case 73022:                                 // Mutated Infection (heroic)
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if ((*iter)->GetTypeId() == TYPEID_PLAYER &&
                    (*iter) != m_caster->getVictim())
                    targetUnitMap.push_back(*iter);
            }

            unMaxTargets = 1;
            break;
        }
        case 69762: // Unchained Magic (Sindragosa)
        {
            unMaxTargets = 2;

            if (m_caster->GetMap()->GetDifficulty() == RAID_DIFFICULTY_25MAN_NORMAL ||
                m_caster->GetMap()->GetDifficulty() == RAID_DIFFICULTY_25MAN_HEROIC)
            {
                unMaxTargets = 6;
            }

            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if ((*iter)->getPowerType() == POWER_MANA && (*iter)->GetCharmerOrOwnerPlayerOrPlayerItself())
                    targetUnitMap.push_back(*iter);
            }
            break;
        }
        case 69832: // Unstable Ooze Explosion (Rotface)
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_ALL);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if ((*iter)->GetEntry() == 38107)
                    targetUnitMap.push_back((*iter));
            }
            targetUnitMap.push_back(m_caster);
            break;
        }
        case 69839: // Unstable Ooze Explosion (Rotface)
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_ALL);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if ((*iter)->GetCharmerOrOwnerPlayerOrPlayerItself() || (*iter)->GetEntry() == 36899)
                    targetUnitMap.push_back((*iter));
            }

            unMaxTargets = 3;
            break;
        }
        case 70117: // Icy grip (Sindragosa encounter)
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if (!(*iter)->GetCharmerOrOwnerPlayerOrPlayerItself())
                    continue;

                targetUnitMap.push_back((*iter));
            }
            break;
        }
        case 70127: // Mystic Buffet (Sindragosa)
        case 72528:
        case 72529:
        case 72530:
        {
            FillAreaTargets(targetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            targetUnitMap.remove(m_caster);
            break;
        }
        case 70338: // Necrotic Plague
        case 73785:
        case 73786:
        case 73787:
        {
            FillAreaTargets(targetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_ALL);
            targetUnitMap.remove(m_caster);
            for (UnitList::iterator itr = targetUnitMap.begin(); itr != targetUnitMap.end();)
            {
                // Lich King has Plague Avoidance aura
                if (!(*itr) || (*itr)->GetDummyAura(72846))
                    itr = targetUnitMap.erase(itr);
                else
                    ++itr;
            }
            if (!targetUnitMap.empty() && targetUnitMap.size() > 1)
            {
                targetUnitMap.sort(TargetDistanceOrderNear(m_caster));
                targetUnitMap.resize(1);
            }
            break;
        }
        case 70402: // Mutated Transformation (Putricide)
        case 72511:
        case 72512:
        case 72513:
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_FRIENDLY);
            for (UnitList::iterator itr = tempTargetUnitMap.begin(); itr != tempTargetUnitMap.end(); ++itr)
            {
                if ((*itr) && !(*itr)->GetObjectGuid().IsVehicle())
                    targetUnitMap.push_back(*itr);
            }
            break;
        }
        case 70447: // Volatile Ooze Adhesive (Putricide)
        case 72836:
        case 72837:
        case 72838:
        case 70672: // Gaseous Bloat (Putricide)
        case 72455:
        case 72832:
        case 72833:
        {
            targetUnitMap.push_back(m_targets.getUnitTarget());
            break;
        }
        case 70499: // Vile Spirits Visual Effect (Lich King)
        {
            radius = 100.0f;
            FillAreaTargets(targetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_ALL);
            for (UnitList::iterator itr = targetUnitMap.begin(); itr != targetUnitMap.end();)
            {
                if (!(*itr) || (*itr)->GetEntry() != 37799)
                    itr = targetUnitMap.erase(itr);
                else
                    ++itr;
            }

            unMaxTargets = 1;
            break;
        }
        case 70572: // Grip of Agony
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_ALL);
            for (UnitList::iterator itr = tempTargetUnitMap.begin(); itr != tempTargetUnitMap.end(); ++itr)
            {
                if (*itr && (*itr)->GetTypeId() == TYPEID_UNIT)
                {
                    switch ((*itr)->GetEntry())
                    {
                        case 37187: // Overlord Saurfang
                        case 37920: // Korkron Reaver
                        case 37200: // Muradin
                        case 37902: // Alliance Mason
                            targetUnitMap.push_back(*itr);
                            break;
                    }
                }
            }
            break;
        }
        case 70701: // Expunged Gas (Putricide)
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_FRIENDLY);
            for (UnitList::iterator itr = tempTargetUnitMap.begin(); itr != tempTargetUnitMap.end(); ++itr)
            {
                if ((*itr) && !(*itr)->GetObjectGuid().IsVehicle())
                    targetUnitMap.push_back(*itr);
            }
            break;
        }
        case 70728: // Exploit Weakness triggered
        case 70840: // Devious Minds triggered
        {
            targetUnitMap.push_back(m_caster);
            if (m_caster->GetObjectGuid().IsPet())
            {
                if (Unit* owner = m_caster->GetOwner())
                    targetUnitMap.push_back(owner);
            }
            else
            {
               GuidSet const& groupPets = m_caster->GetPets();
               if (!groupPets.empty())
               {
                   for (GuidSet::const_iterator itr = groupPets.begin(); itr != groupPets.end(); ++itr)
                   {
                       if (Pet* pPet = m_caster->GetMap()->GetPet(*itr))
                           targetUnitMap.push_back((Unit*)pPet);
                   }
               }
            }
            break;
        }
        case 70911: // Unbound Plague (Putricide)
        case 72854:
        case 72855:
        case 72856:
        {
            if (m_targets.getUnitTarget())
                targetUnitMap.push_back(m_targets.getUnitTarget());

            break;
        }
        case 70920: // Unbound Plague Search Effect (Putricide)
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_ALL);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                // target only other players which dont have Mutated Transformation aura on self
                if ((*iter) && (*iter)->GetTypeId() == TYPEID_PLAYER &&
                    !(*iter)->GetDummyAura(70308))
                {
                    targetUnitMap.push_back(*iter);
                }
            }

            targetUnitMap.remove(m_caster);
            unMaxTargets = 1;
            break;
        }
        case 70961: // Shattered Bones
        {
            if (i != EFFECT_INDEX_1)
                return false;

            effToIndex = EFFECT_INDEX_0;
            break;
        }
        case 71075: // Invocation of Blood (V) Move
        case 71079: // Invocation of Blood (K) Move
        case 71082: // Invocation of Blood (T) Move
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_ALL);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                // target the one with Invocation of Blood aura
                if ((*iter)->HasAura(70952) ||
                    (*iter)->HasAura(70981) ||
                    (*iter)->HasAura(70982))
                {
                    targetUnitMap.push_back(*iter);
                    break;
                }
            }

            break;
        }
        case 71307: // Vile Gas (Festergut)
        case 71908:
        case 72270:
        case 72271:
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_ALL);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if ((*iter)->GetEntry() == 38548)
                    targetUnitMap.push_back(*iter);
            }

            unMaxTargets = 1;
            break;
        }
        case 72873: // Malleable Goo (Putricide)
        case 72874:
        case 72550:
        case 72458:
        case 72549:
        case 70853:
        case 72297:
        case 72548:
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_ALL);
            for (UnitList::iterator itr = tempTargetUnitMap.begin(); itr != tempTargetUnitMap.end(); ++itr)
            {
                if ((*itr) && !(*itr)->GetObjectGuid().IsVehicle())
                    targetUnitMap.push_back(*itr);
            }
            targetUnitMap.remove(m_caster);
            break;
        }
        case 72038: // Empowered Shock Vortex (Blood Council)
        case 72815:
        case 72816:
        case 72817:
        {
            FillAreaTargets(targetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            targetUnitMap.remove(m_caster);
        }
        case 72905: // Frostbolt Volley (Lady Deathwhisper)
        case 72906:
        case 72907:
        case 72908:
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if (!(*iter)->GetCharmerOrOwnerPlayerOrPlayerItself())
                    continue;

                targetUnitMap.push_back((*iter));
            }

            break;
        }
        case 72378: // Blood Nova (Saurfang)
        case 73058:
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if (!(*iter)->GetObjectGuid().IsPlayer())
                    continue;

                if (m_caster->GetDistance(*iter) < 10.0f) // stored in 72379 radius?
                    continue;

                targetUnitMap.push_back((*iter));
            }

            unMaxTargets = 1;
            break;
        }
        case 72385:                                     // Boiling Blood
        case 72441:
        case 72442:
        case 72443:
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if (!(*iter)->GetObjectGuid().IsPlayer())
                    continue;

                if ((*iter) == m_caster->getVictim())
                    continue;

                targetUnitMap.push_back((*iter));
            }

            unMaxTargets = 1;
            break;
        }
        case 71336:                                     // Pact of the Darkfallen
        {
            UnitList tempTargetUnitMap;
            radius = 100.0f;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if (!(*iter)->GetObjectGuid().IsPlayer() ||
                    m_caster->getVictim() == (*iter) ||         // don't target the tank
                    (*iter)->HasAuraOfDifficulty(70451))        // don't target offtank linked with Blood Mirror
                {
                    continue;
                }

                targetUnitMap.push_back((*iter));
            }

            if (m_caster->GetMap()->GetDifficulty() == RAID_DIFFICULTY_25MAN_NORMAL ||
                m_caster->GetMap()->GetDifficulty() == RAID_DIFFICULTY_25MAN_HEROIC)
            {
                unMaxTargets = 3;
            }
            else
                unMaxTargets = 2;

            break;
        }
        case 71341:                                     // Pact of the Darkfallen (dmg part)
        case 71390:                                     // Pact of the Darkfallen (visual link part)
        {
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, 100.0f, PUSH_SELF_CENTER, SPELL_TARGETS_FRIENDLY);
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if ((*iter)->HasAura(71340))
                    targetUnitMap.push_back((*iter));
            }
            break;
        }
        case 71445:                                 // Twilight Bloodbolt (Lana'thel)
        case 71471:
        {
            radius = DEFAULT_VISIBILITY_INSTANCE;
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_ALL);
            for (UnitList::iterator itr = tempTargetUnitMap.begin(); itr != tempTargetUnitMap.end();++itr)
            {
                if ((*itr) && (*itr)->GetTypeId() == TYPEID_PLAYER &&
                    !(*itr)->HasAuraOfDifficulty(70445) && !(*itr)->HasAuraOfDifficulty(70451)) // Blood Mirror
                {
                    targetUnitMap.push_back(*itr);
                }
            }

            unMaxTargets = 2;
            break;
        }
        case 71447:                                 // Bloodbolt Splash 10N
        case 71481:                                 // Bloodbolt Splash 25N
        case 71482:                                 // Bloodbolt Splash 10H
        case 71483:                                 // Bloodbolt Splash 25H
        {
            FillAreaTargets(targetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_FRIENDLY);
            targetUnitMap.remove(m_caster);
            break;
        }
        case 72133:                                 // Pain and Suffering (Lich King)
        case 73788:
        case 73789:
        case 73790:
        {
            // keep standard chain targeting for dummy effect
            if (i == EFFECT_INDEX_0)
                return false;

            FillAreaTargets(targetUnitMap, radius, PUSH_IN_FRONT_15, SPELL_TARGETS_AOE_DAMAGE);

            break;
        }
        case 72376:                                 // Raise Dead (Lich King)
        case 72429:                                 // Mass Resurrection (Lich King)
        {
            radius = DEFAULT_VISIBILITY_INSTANCE;
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_ALL);
            for (UnitList::iterator itr = tempTargetUnitMap.begin(); itr != tempTargetUnitMap.end();++itr)
            {
                if ((*itr) && (*itr)->GetTypeId() == TYPEID_PLAYER && !(*itr)->isAlive())
                    targetUnitMap.push_back(*itr);
            }
            break;
        }
        case 72454:                                 // Mutated Plague (Putricide)
        case 72464:
        case 72506:
        case 72507:
        {
            radius = DEFAULT_VISIBILITY_INSTANCE;
            UnitList tempTargetUnitMap;
            FillAreaTargets(tempTargetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_AOE_DAMAGE, GetAffectiveCaster());
            for (UnitList::iterator itr = tempTargetUnitMap.begin(); itr != tempTargetUnitMap.end(); ++itr)
            {
                if ((*itr) && !(*itr)->GetObjectGuid().IsVehicle())
                    targetUnitMap.push_back(*itr);
            }
            break;
        }
        case 72754:                                     // Defile (Lich King)
        case 73708:
        case 73709:
        case 73710:
        {
            // base radius
            radius = 10.0f;

            // 10man normal - 10% growth
            if (SpellAuraHolderPtr holder = m_caster->GetSpellAuraHolder(72756))
                radius += holder->GetStackAmount() * 1.0f;
            // 25man normal - 5% growth
            else if (SpellAuraHolderPtr holder = m_caster->GetSpellAuraHolder(74162))
                radius += holder->GetStackAmount() * 0.5f;
            // 10man heroic - 10% growth
            else if (SpellAuraHolderPtr holder = m_caster->GetSpellAuraHolder(74163))
                radius += holder->GetStackAmount() * 1.0f;
            // 25man heroic - 5% growth
            else if (SpellAuraHolderPtr holder = m_caster->GetSpellAuraHolder(74164))
                radius += holder->GetStackAmount() * 0.5f;

            FillAreaTargets(targetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            break;
        }
        case 73529:                                     // Shadow Trap triggered knockback (Lich King)
        {
            radius = 10.0f;
            FillAreaTargets(targetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            break;
        }
        case 74282:                                     // Shadow Trap (search target) (Lich King)
        {
            radius = 5.0f;
            FillAreaTargets(targetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            break;
        }
        case 74960:                                     // Infrigidate
        {
            FillAreaTargets(targetUnitMap, 20.0f, PUSH_SELF_CENTER, SPELL_TARGETS_FRIENDLY);
            targetUnitMap.remove(m_caster);

            if (!targetUnitMap.empty())
                unMaxTargets = 1;
            else
                return false;

            break;
        }
        default:
            return false;
    }

    if (targetUnitMap.empty())
        return true;

    // remove NULL targets (weird, but...)
    for (UnitList::iterator itr = targetUnitMap.begin(); itr != targetUnitMap.end();)
    {
        if (!*itr)
            itr = targetUnitMap.erase(itr);
        else
            ++itr;
    }

    if (!unMaxTargets)
        return true;

    // random targets
    while (targetUnitMap.size() > unMaxTargets)
    {
        UnitList::iterator itr = targetUnitMap.begin();
        std::advance(itr, urand(0, targetUnitMap.size() - 1));
        targetUnitMap.erase(itr);
    }

    return true;
}
