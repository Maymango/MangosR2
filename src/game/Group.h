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

#ifndef MANGOSSERVER_GROUP_H
#define MANGOSSERVER_GROUP_H

#include "Common.h"
#include "ObjectGuid.h"
#include "GroupReference.h"
#include "GroupRefManager.h"
#include "BattleGround/BattleGround.h"
#include "LootMgr.h"
#include "DBCEnums.h"
#include "SharedDefines.h"
#include "LFG.h"

#include <map>
#include <vector>

struct ItemPrototype;

class WorldSession;
class Map;
class BattleGround;
class DungeonPersistentState;
class Field;
class Unit;

#define MAX_GROUP_SIZE 5
#define MAX_RAID_SIZE 40
#define MAX_RAID_SUBGROUPS (MAX_RAID_SIZE / MAX_GROUP_SIZE)
#define TARGET_ICON_COUNT 8

enum LootMethod
{
    FREE_FOR_ALL      = 0,
    ROUND_ROBIN       = 1,
    MASTER_LOOT       = 2,
    GROUP_LOOT        = 3,
    NEED_BEFORE_GREED = 4
};

enum RollVote
{
    ROLL_PASS              = 0,
    ROLL_NEED              = 1,
    ROLL_GREED             = 2,
    ROLL_DISENCHANT        = 3,

    // other not send by client
    MAX_ROLL_FROM_CLIENT   = 4,

    ROLL_NOT_EMITED_YET    = 4,                             // send to client
    ROLL_NOT_VALID         = 5                              // not send to client
};

// set what votes allowed
enum RollVoteMask
{
    ROLL_VOTE_MASK_PASS       = 0x01,
    ROLL_VOTE_MASK_NEED       = 0x02,
    ROLL_VOTE_MASK_GREED      = 0x04,
    ROLL_VOTE_MASK_DISENCHANT = 0x08,

    ROLL_VOTE_MASK_ALL        = 0x0F,
};


enum GroupMemberFlags
{
    MEMBER_STATUS_OFFLINE   = 0x0000,
    MEMBER_STATUS_ONLINE    = 0x0001,                       // Lua_UnitIsConnected
    MEMBER_STATUS_PVP       = 0x0002,                       // Lua_UnitIsPVP
    MEMBER_STATUS_DEAD      = 0x0004,                       // Lua_UnitIsDead
    MEMBER_STATUS_GHOST     = 0x0008,                       // Lua_UnitIsGhost
    MEMBER_STATUS_PVP_FFA   = 0x0010,                       // Lua_UnitIsPVPFreeForAll
    MEMBER_STATUS_UNK3      = 0x0020,                       // used in calls from Lua_GetPlayerMapPosition/Lua_GetBattlefieldFlagPosition
    MEMBER_STATUS_AFK       = 0x0040,                       // Lua_UnitIsAFK
    MEMBER_STATUS_DND       = 0x0080,                       // Lua_UnitIsDND
    MEMBER_STATUS_RAF       = 0x0100,                       // RAF status in party/raid
    MEMBER_STATUS_UNK4      = 0x0200,                       // something to do with vehicles
};

enum GroupType                                              // group type flags?
{
    GROUPTYPE_NORMAL = 0x00,
    GROUPTYPE_BG     = 0x01,
    GROUPTYPE_RAID   = 0x02,
    GROUPTYPE_BGRAID = GROUPTYPE_BG | GROUPTYPE_RAID,       // mask
    GROUPTYPE_UNK1   = 0x04,                                // 0x04?
    GROUPTYPE_LFD    = 0x08,
    GROUPTYPE_UNK2   = 0x10,
    // 0x10, leave/change group?, I saw this flag when leaving group and after leaving BG while in group
};

enum GroupFlags
{
    GROUP_FLAG_ASSISTANT      = 0,
    GROUP_FLAG_MAIN_ASSISTANT = 1,
    GROUP_FLAG_MAIN_TANK      = 2,
};

enum GroupFlagMask
{
    GROUP_MEMBER         = 0x00,
    GROUP_ASSISTANT      = (1 << GROUP_FLAG_ASSISTANT),
    GROUP_MAIN_ASSISTANT = (1 << GROUP_FLAG_MAIN_ASSISTANT),
    GROUP_MAIN_TANK      = (1 << GROUP_FLAG_MAIN_TANK),

    // unions
    GROUP_MEMBER_AMT     = (GROUP_ASSISTANT |
                            GROUP_MAIN_ASSISTANT |
                            GROUP_MAIN_TANK),

    GROUP_MEMBER_AT      = (GROUP_ASSISTANT |
                            GROUP_MAIN_TANK),

    GROUP_MEMBER_AM      = (GROUP_ASSISTANT |
                            GROUP_MAIN_ASSISTANT),

    GROUP_MEMBER_MT      = (GROUP_MAIN_ASSISTANT |
                            GROUP_MAIN_TANK),

};

enum GroupFlagsAssignment
{
    GROUP_ASSIGN_MAINASSIST = 0,
    GROUP_ASSIGN_MAINTANK   = 1,
    GROUP_ASSIGN_ASSISTANT  = 2,
};

enum GroupUpdateFlags
{
    GROUP_UPDATE_FLAG_NONE              = 0x00000000,       // nothing
    GROUP_UPDATE_FLAG_STATUS            = 0x00000001,       // uint16, flags
    GROUP_UPDATE_FLAG_CUR_HP            = 0x00000002,       // uint32
    GROUP_UPDATE_FLAG_MAX_HP            = 0x00000004,       // uint32
    GROUP_UPDATE_FLAG_POWER_TYPE        = 0x00000008,       // uint8
    GROUP_UPDATE_FLAG_CUR_POWER         = 0x00000010,       // uint16
    GROUP_UPDATE_FLAG_MAX_POWER         = 0x00000020,       // uint16
    GROUP_UPDATE_FLAG_LEVEL             = 0x00000040,       // uint16
    GROUP_UPDATE_FLAG_ZONE              = 0x00000080,       // uint16
    GROUP_UPDATE_FLAG_POSITION          = 0x00000100,       // uint16, uint16
    GROUP_UPDATE_FLAG_AURAS             = 0x00000200,       // uint64 mask, for each bit set uint32 spellid + uint8 unk
    GROUP_UPDATE_FLAG_PET_GUID          = 0x00000400,       // uint64 pet guid
    GROUP_UPDATE_FLAG_PET_NAME          = 0x00000800,       // pet name, NULL terminated string
    GROUP_UPDATE_FLAG_PET_MODEL_ID      = 0x00001000,       // uint16, model id
    GROUP_UPDATE_FLAG_PET_CUR_HP        = 0x00002000,       // uint32 pet cur health
    GROUP_UPDATE_FLAG_PET_MAX_HP        = 0x00004000,       // uint32 pet max health
    GROUP_UPDATE_FLAG_PET_POWER_TYPE    = 0x00008000,       // uint8 pet power type
    GROUP_UPDATE_FLAG_PET_CUR_POWER     = 0x00010000,       // uint16 pet cur power
    GROUP_UPDATE_FLAG_PET_MAX_POWER     = 0x00020000,       // uint16 pet max power
    GROUP_UPDATE_FLAG_PET_AURAS         = 0x00040000,       // uint64 mask, for each bit set uint32 spellid + uint8 unk, pet auras...
    GROUP_UPDATE_FLAG_VEHICLE_SEAT      = 0x00080000,       // uint32 vehicle_seat_id (index from VehicleSeat.dbc)
    GROUP_UPDATE_PET                    = 0x0007FC00,       // all pet flags
    GROUP_UPDATE_FULL                   = 0x0007FFFF,       // all known flags
};

#define GROUP_UPDATE_FLAGS_COUNT          20
                                                                // 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,11,12,13,14,15,16,17,18,19
static const uint8 GroupUpdateLength[GROUP_UPDATE_FLAGS_COUNT] = { 0, 2, 2, 2, 1, 2, 2, 2, 2, 4, 8, 8, 1, 2, 2, 2, 1, 2, 2, 8};

class Roll : public LootValidatorRef
{
    public:
        Roll(ObjectGuid _lootedTragetGuid, LootMethod method, LootItem const& li)
            : lootedTargetGUID(_lootedTragetGuid), itemid(li.itemid), itemRandomPropId(li.randomPropertyId), itemRandomSuffix(li.randomSuffix),
            itemCount(li.count), totalPlayersRolling(0), totalNeed(0), totalGreed(0), totalPass(0), itemSlot(0),
            m_method(method), m_commonVoteMask(ROLL_VOTE_MASK_ALL) {}
        ~Roll() {}
        void setLoot(Loot *pLoot) { link(pLoot, this); }
        Loot* getLoot() { return getTarget(); }
        void targetObjectBuildLink();

        void CalculateCommonVoteMask(uint32 max_enchanting_skill);
        RollVoteMask GetVoteMaskFor(Player* player) const;

        ObjectGuid lootedTargetGUID;
        uint32 itemid;
        int32  itemRandomPropId;
        uint32 itemRandomSuffix;
        uint8 itemCount;
        typedef UNORDERED_MAP<ObjectGuid, RollVote> PlayerVote;
        PlayerVote playerVote;                              //vote position correspond with player position (in group)
        uint8 totalPlayersRolling;
        uint8 totalNeed;
        uint8 totalGreed;
        uint8 totalPass;
        uint8 itemSlot;

    private:
        LootMethod m_method;
        RollVoteMask m_commonVoteMask;
};

struct InstanceGroupBind
{
    DungeonPersistentState *state;
    bool perm;
    /* permanent InstanceGroupBinds exist iff the leader has a permanent
       PlayerInstanceBind for the same instance. */
    InstanceGroupBind() : state(NULL), perm(false) {}
};

/** request member stats checken **/
/** todo: uninvite people that not accepted invite **/
class MANGOS_DLL_SPEC Group
{
    public:
        struct MemberSlot
        {
            ObjectGuid  guid;
            std::string name;
            uint8       group;
            GroupFlagMask  flags;
            LFGRoleMask roles;
            uint32      lastMap;
        };
        typedef std::list<MemberSlot> MemberSlotList;
        typedef MemberSlotList::const_iterator member_citerator;
        typedef MemberSlotList::iterator       member_witerator;

        typedef UNORDERED_MAP<uint32 /*mapId*/, InstanceGroupBind> BoundInstancesMap;

    protected:
        typedef UNORDERED_SET<Player*> InvitesList;
        typedef std::vector<Roll*> TRolls;

    public:
        Group(GroupType type);
        ~Group();

        // group manipulation methods
        bool   Create(ObjectGuid guid, const char * name);
        bool   LoadGroupFromDB(Field *fields);
        bool   LoadMemberFromDB(uint32 guidLow, uint8 subgroup, GroupFlagMask flags, LFGRoleMask roles);
        bool   AddInvite(Player *player);
        uint32 RemoveInvite(Player *player);
        void   RemoveAllInvites();
        bool   AddLeaderInvite(Player *player);
        bool   AddMember(ObjectGuid guid, const char* name);
        uint32 RemoveMember(ObjectGuid guid, uint8 method, bool logout = false); // method: 0=just remove, 1=kick
        void   RemoveGroupBuffsOnMemberRemove(ObjectGuid guid);
        void   ChangeLeader(ObjectGuid guid);
        void CheckLeader(ObjectGuid const& guid, bool logout);
        bool ChangeLeaderToFirstSuitableMember(bool onlySet = false);
        void   SetLootMethod(LootMethod method) { m_lootMethod = method; }
        void   SetLooterGuid(ObjectGuid guid) { m_looterGuid = guid; }
        void   UpdateLooterGuid(WorldObject* pSource, bool ifneed = false);
        void   SetLootThreshold(ItemQualities threshold) { m_lootThreshold = threshold; }
        void   Disband(bool hideDestroy=false);

        // properties accessories
        ObjectGuid GetObjectGuid() const { return m_Guid; }
        uint32 GetId() const { return m_Guid.GetCounter(); }
        bool IsFull() const { return (m_groupType == GROUPTYPE_NORMAL || isLFGGroup()) ? (m_memberSlots.size() >= MAX_GROUP_SIZE) : (m_memberSlots.size() >= MAX_RAID_SIZE); }
        bool isRaidGroup() const { return m_groupType & GROUPTYPE_RAID; }
        bool isBGGroup()   const { return m_bgGroup != NULL; }
        bool IsCreated()   const { return GetMembersCount() > 0; }
        ObjectGuid GetLeaderGuid() const { return m_leaderGuid; }
        const char * GetLeaderName() const { return m_leaderName.c_str(); }
        LootMethod    GetLootMethod() const { return m_lootMethod; }
        ObjectGuid GetLooterGuid() const { return m_looterGuid; }
        ItemQualities GetLootThreshold() const { return m_lootThreshold; }
        GroupType GetGroupType() const { return m_groupType; };
        bool IsNeedSave() const;

        // member manipulation methods
        bool IsMember(ObjectGuid guid) const { return _getMemberCSlot(guid) != m_memberSlots.end(); }
        bool IsLeader(ObjectGuid guid) const { return GetLeaderGuid() == guid; }
        ObjectGuid GetMemberGuid(const std::string& name)
        {
            for (member_citerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
            {
                if (itr->name == name)
                    return itr->guid;
            }
            return ObjectGuid();
        }
        bool IsGroupRole(ObjectGuid const& guid, GroupFlagMask role) const
        {
            member_citerator mslot = _getMemberCSlot(guid);
            if (mslot == m_memberSlots.end())
                return false;

            if (role == GROUP_MEMBER)
                return true;
            else
                return mslot->flags & role;
        }
        bool IsMainAssistant(ObjectGuid const& guid) const { return IsGroupRole(guid, GROUP_MAIN_ASSISTANT); }
        bool IsMainTank(ObjectGuid const& guid) const { return IsGroupRole(guid, GROUP_MAIN_TANK); }
        bool IsAssistant(ObjectGuid const& guid) const { return IsGroupRole(guid, GROUP_ASSISTANT); }
        Player* GetMemberWithRole(GroupFlagMask role);

        Player* GetInvited(ObjectGuid guid) const;
        Player* GetInvited(const std::string& name) const;

        bool HasFreeSlotSubGroup(uint8 subgroup) const
        {
            return (m_subGroupsCounts && m_subGroupsCounts[subgroup] < MAX_GROUP_SIZE);
        }

        bool SameSubGroup(Player const* member1, Player const* member2) const;

        MemberSlotList const& GetMemberSlots() const { return m_memberSlots; }
        GroupReference* GetFirstMember() { return m_memberMgr.getFirst(); }
        GroupReference const* GetFirstMember() const { return m_memberMgr.getFirst(); }
        uint32 GetMembersCount() const { return m_memberSlots.size(); }
        void GetDataForXPAtKill(Unit const* victim, uint32& count,uint32& sum_level, Player* & member_with_max_level, Player* & not_gray_member_with_max_level, Player* additional = NULL);
        uint8 GetMemberGroup(ObjectGuid guid) const
        {
            member_citerator mslot = _getMemberCSlot(guid);
            if (mslot == m_memberSlots.end())
                return MAX_RAID_SUBGROUPS + 1;

            return mslot->group;
        }

        // some additional raid methods
        void ConvertToRaid();

        void SetBattlegroundGroup(BattleGround *bg) { m_bgGroup = bg; }
        GroupJoinBattlegroundResult CanJoinBattleGroundQueue(BattleGround const* bgOrTemplate, BattleGroundQueueTypeId bgQueueTypeId, uint32 MinPlayerCount, uint32 MaxPlayerCount, bool isRated, uint32 arenaSlot);

        void ChangeMembersGroup(ObjectGuid guid, uint8 group);
        void ChangeMembersGroup(Player *player, uint8 group);

        void SetGroupUniqueFlag(ObjectGuid guid, GroupFlagsAssignment assignment, uint8 apply);

        void SetTargetIcon(uint8 id, ObjectGuid whoGuid, ObjectGuid targetGuid);

        Difficulty GetDifficulty(bool isRaid) const { return isRaid ? GetRaidDifficulty() : GetDungeonDifficulty(); }
        uint32 GetDifficulty() const { return m_Difficulty; }
        Difficulty GetDungeonDifficulty() const { return Difficulty(m_Difficulty & 0x00FF); }
        Difficulty GetRaidDifficulty() const { return Difficulty((m_Difficulty & 0xFF00) >> 8);}
        void SetDungeonDifficulty(Difficulty difficulty);
        void SetRaidDifficulty(Difficulty difficulty);
        uint16 InInstance();
        bool InCombatToInstance(uint32 instanceId);
        void ResetInstances(InstanceResetMethod method, bool isRaid, Player* SendMsgTo);

        void SendTargetIconList(WorldSession *session);
        void SendUpdate();
        void Update(uint32 diff);
        void UpdatePlayerOutOfRange(Player* pPlayer);
                                                            // ignore: GUID of player that will be ignored
        void BroadcastPacket(WorldPacket *packet, bool ignorePlayersInBGRaid, int group=-1, ObjectGuid ignore = ObjectGuid());
        void BroadcastReadyCheck(WorldPacket *packet);
        void OfflineReadyCheck();

        void RewardGroupAtKill(Unit* pVictim, Player* player_tap);

        bool SetPlayerMap(ObjectGuid guid, uint32 mapid);

        /*********************************************************/
        /***                   LOOT SYSTEM                     ***/
        /*********************************************************/

        void SendLootStartRoll(uint32 CountDown, uint32 mapid, const Roll &r);
        void SendLootRoll(ObjectGuid const& targetGuid, uint8 rollNumber, uint8 rollType, const Roll &r);
        void SendLootRollWon(ObjectGuid const& targetGuid, uint8 rollNumber, RollVote rollType, const Roll &r);
        void SendLootAllPassed(const Roll &r);
        void GroupLoot(WorldObject* pSource, Loot* loot);
        void NeedBeforeGreed(WorldObject* pSource, Loot* loot);
        void MasterLoot(WorldObject* pSource, Loot* loot);
        bool CountRollVote(Player* player, ObjectGuid const& lootedTarget, uint32 itemSlot, RollVote vote);
        void StartLootRoll(WorldObject* lootTarget, LootMethod method, Loot* loot, uint8 itemSlot, uint32 maxEnchantingSkill);
        void EndRoll();

        void LinkMember(GroupReference *pRef) { m_memberMgr.insertFirst(pRef); }
        void DelinkMember(GroupReference* /*pRef*/ ) { }

        InstanceGroupBind* BindToInstance(DungeonPersistentState *save, bool permanent, bool load = false);
        void UnbindInstance(uint32 mapid, uint8 difficulty, bool unload = false);
        InstanceGroupBind* GetBoundInstance(uint32 mapId, Player* player);
        InstanceGroupBind* GetBoundInstance(Map* aMap, Difficulty difficulty);
        BoundInstancesMap& GetBoundInstances(Difficulty difficulty) { return m_boundInstances[difficulty]; }

        // LFG
        bool ConvertToLFG(LFGType type);
        bool isLFDGroup()  const { return m_groupType & GROUPTYPE_LFD; }
        bool isLFGGroup()  const { return ((m_groupType & GROUPTYPE_LFD) && !(m_groupType & GROUPTYPE_RAID)) ; }
        bool isLFRGroup()  const { return ((m_groupType & GROUPTYPE_LFD) && (m_groupType & GROUPTYPE_RAID)) ; }
        void SetGroupRoles(ObjectGuid guid, LFGRoleMask roles);
        LFGRoleMask GetGroupRoles(ObjectGuid guid);

        // Frozen Mod
        void BroadcastGroupUpdate(void);
        // Frozen Mod

    protected:
        bool _addMember(ObjectGuid guid, const char* name);
        bool _addMember(ObjectGuid guid, const char* name, uint8 group, GroupFlagMask flags = GROUP_MEMBER, LFGRoleMask roles = LFG_ROLE_MASK_NONE);
        bool _removeMember(ObjectGuid guid);                // returns true if leader has changed
        void _setLeader(ObjectGuid guid);

        void _removeRolls(ObjectGuid guid);

        bool _setMembersGroup(ObjectGuid guid, uint8 group);

        void _homebindIfInstance(Player *player);

        void _initRaidSubGroupsCounter()
        {
            // Sub group counters initialization
            if (!m_subGroupsCounts)
                m_subGroupsCounts = new uint8[MAX_RAID_SUBGROUPS];

            memset((void*)m_subGroupsCounts, 0, MAX_RAID_SUBGROUPS * sizeof(uint8));

            for (member_citerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
                ++m_subGroupsCounts[itr->group];
        }

        member_citerator _getMemberCSlot(ObjectGuid guid) const
        {
            for(member_citerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
                if (itr->guid == guid)
                    return itr;

            return m_memberSlots.end();
        }

        member_witerator _getMemberWSlot(ObjectGuid guid)
        {
            for(member_witerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
                if (itr->guid == guid)
                    return itr;

            return m_memberSlots.end();
        }

        void SubGroupCounterIncrease(uint8 subgroup)
        {
            if (m_subGroupsCounts)
                ++m_subGroupsCounts[subgroup];
        }

        void SubGroupCounterDecrease(uint8 subgroup)
        {
            if (m_subGroupsCounts)
                --m_subGroupsCounts[subgroup];
        }

        uint32 GetMaxSkillValueForGroup(SkillType skill);

        void CountTheRoll(TRolls::iterator& roll);                    // iterator update to next, in CountRollVote if true
        bool CountRollVote(ObjectGuid const& playerGUID, TRolls::iterator& roll, RollVote vote);

        ObjectGuid          m_Guid;
        MemberSlotList      m_memberSlots;
        GroupRefManager     m_memberMgr;
        InvitesList         m_invitees;
        ObjectGuid          m_leaderGuid;
        std::string         m_leaderName;
        GroupType           m_groupType;

        uint32              m_Difficulty;                             // contains both dungeon (first byte) and raid (second byte) difficultyes of player. bytes 2,3 not used.

        BattleGround*       m_bgGroup;
        ObjectGuid          m_targetIcons[TARGET_ICON_COUNT];
        LootMethod          m_lootMethod;
        ItemQualities       m_lootThreshold;
        ObjectGuid          m_looterGuid;
        TRolls*             m_rollIds;
        BoundInstancesMap   m_boundInstances[MAX_DIFFICULTY];
        uint8*              m_subGroupsCounts;
        uint32              m_waitLeaderTimer;

};
#endif
