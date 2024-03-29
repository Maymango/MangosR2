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

#include "Common.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Player.h"
#include "World.h"
#include "AccountMgr.h"
#include "ObjectMgr.h"
#include "ObjectGuid.h"
#include "Group.h"
#include "Formulas.h"
#include "ObjectAccessor.h"
#include "BattleGround/BattleGround.h"
#include "BattleGround/BattleGroundMgr.h"
#include "MapManager.h"
#include "MapPersistentStateMgr.h"
#include "Util.h"
#include "LootMgr.h"
#include "LFGMgr.h"
#include "UpdateFieldFlags.h"

// Playerbot
#include "playerbot/PlayerbotMgr.h"

#define LOOT_ROLL_TIMEOUT  (1*MINUTE*IN_MILLISECONDS)

//===================================================
//============== Roll ===============================
//===================================================

void Roll::targetObjectBuildLink()
{
    // called from link()
    getTarget()->addLootValidatorRef(this);
}

void Roll::CalculateCommonVoteMask(uint32 max_enchanting_skill)
{
    m_commonVoteMask = ROLL_VOTE_MASK_ALL;

    ItemPrototype const* itemProto = ObjectMgr::GetItemPrototype(itemid);

    if (itemProto->Flags2 & ITEM_FLAG2_NEED_ROLL_DISABLED)
        m_commonVoteMask = RollVoteMask(m_commonVoteMask & ~ROLL_VOTE_MASK_NEED);

    if (!itemProto->DisenchantID || uint32(itemProto->RequiredDisenchantSkill) > max_enchanting_skill)
        m_commonVoteMask = RollVoteMask(m_commonVoteMask & ~ROLL_VOTE_MASK_DISENCHANT);
}

RollVoteMask Roll::GetVoteMaskFor(Player* player) const
{
    ItemPrototype const* itemProto = ObjectMgr::GetItemPrototype(itemid);

    // In NEED_BEFORE_GREED need disabled for non-usable item for player
    if (m_method != NEED_BEFORE_GREED || player->CanUseItem(itemProto) == EQUIP_ERR_OK)
        return m_commonVoteMask;
    else
        return RollVoteMask(m_commonVoteMask & ~ROLL_VOTE_MASK_NEED);
}

//===================================================
//============== Group ==============================
//===================================================

Group::Group(GroupType type) :
    m_Guid(ObjectGuid()),
    m_groupType(type),
    m_Difficulty(0),
    m_bgGroup(NULL),
    m_lootMethod(FREE_FOR_ALL),
    m_lootThreshold(ITEM_QUALITY_UNCOMMON),
    m_looterGuid(ObjectGuid()),
    m_rollIds(NULL),
    m_subGroupsCounts(NULL),
    m_waitLeaderTimer(0)
{
    m_rollIds = new TRolls();
}

Group::~Group()
{
    if (m_bgGroup)
    {
        if (m_bgGroup->GetBgRaid(ALLIANCE) == this)
            m_bgGroup->SetBgRaid(ALLIANCE, NULL);
        else if (m_bgGroup->GetBgRaid(HORDE) == this)
            m_bgGroup->SetBgRaid(HORDE, NULL);
        else
            sLog.outError("Group::~Group: battleground group is not linked to the correct battleground.");
    }

    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (ObjectGuid const& lootGuid = pMember->GetLootGuid())
                pMember->SendLootRelease(lootGuid);
        }
    }

    if (m_rollIds)
    {
        if (!m_rollIds->empty())
        {
            for (TRolls::const_reverse_iterator itr = m_rollIds->rbegin(); itr != m_rollIds->rend(); ++itr)
                delete *itr;
        }
        delete m_rollIds;
    }

    if (GetObjectGuid())
    {
        DEBUG_LOG("Group::~Group: %s type %u has ben deleted.", GetObjectGuid().GetString().c_str(), m_groupType);
        // it is undefined whether objectmgr (which stores the groups) or instancesavemgr
        // will be unloaded first so we must be prepared for both cases
        // this may unload some dungeon persistent state
        sMapPersistentStateMgr.AddToUnbindQueue(GetObjectGuid());

        if (isLFDGroup())
            sLFGMgr.RemoveLFGState(GetObjectGuid());

        // recheck deletion in ObjectMgr (must be deleted while disband, but additional check not be bad)
        sObjectMgr.RemoveGroup(this);
    }
    else sLog.outError("Group::~Group: not fully created group type %u has ben deleted.", m_groupType);

    // Sub group counters clean up
    if (m_subGroupsCounts)
        delete[] m_subGroupsCounts;
}

bool Group::Create(ObjectGuid guid, const char* name)
{
    m_leaderGuid = guid;
    m_leaderName = name;
    m_groupType  = isBGGroup() ? GROUPTYPE_BGRAID : GROUPTYPE_NORMAL;

    if (m_groupType & GROUPTYPE_RAID)
        _initRaidSubGroupsCounter();

    m_lootMethod = GROUP_LOOT;
    m_lootThreshold = ITEM_QUALITY_UNCOMMON;
    m_looterGuid = guid;

    SetDungeonDifficulty(DUNGEON_DIFFICULTY_NORMAL);
    SetRaidDifficulty(RAID_DIFFICULTY_10MAN_NORMAL);

    if (!GetObjectGuid())
        m_Guid = ObjectGuid(HIGHGUID_GROUP, sObjectMgr.GenerateGroupLowGuid());

    if (!isBGGroup())
    {
        Player* leader = sObjectMgr.GetPlayer(guid);
        if (leader)
        {
            SetDungeonDifficulty(leader->GetDungeonDifficulty());
            SetRaidDifficulty(leader->GetRaidDifficulty());
        }

        Player::ConvertInstancesToGroup(leader, this, guid);
    }

    if (IsNeedSave())
    {
        // store group in database
        CharacterDatabase.BeginTransaction();

        static SqlStatementID delGroup;
        CharacterDatabase.CreateStatement(delGroup, "DELETE FROM groups WHERE groupId = ?")
            .PExecute(m_Guid.GetCounter());

        static SqlStatementID delGroupMemb;
        CharacterDatabase.CreateStatement(delGroupMemb, "DELETE FROM group_member WHERE groupId = ?")
            .PExecute(m_Guid.GetCounter());

        static SqlStatementID insGroup;
        SqlStatement stmt = CharacterDatabase.CreateStatement(insGroup,
            "INSERT INTO groups "
            "(groupId,leaderGuid,lootMethod,looterGuid,lootThreshold,icon1,icon2,icon3,icon4,icon5,icon6,icon7,icon8,groupType,difficulty,raiddifficulty) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");

        stmt.addUInt32(m_Guid.GetCounter());
        stmt.addUInt32(m_leaderGuid.GetCounter());
        stmt.addUInt8(uint8(m_lootMethod));
        stmt.addUInt32(m_looterGuid.GetCounter());
        stmt.addUInt8(uint8(m_lootThreshold));

        for (uint8 i = 0; i < TARGET_ICON_COUNT; ++i)
            stmt.addUInt64(m_targetIcons[i].GetRawValue());

        stmt.addUInt8(uint8(m_groupType));
        stmt.addUInt8(uint8(GetDungeonDifficulty()));
        stmt.addUInt32(uint32(GetRaidDifficulty()));
        stmt.Execute();
    }

    bool retResult = AddMember(guid, name);

    if (IsNeedSave())
        CharacterDatabase.CommitTransaction();

    return retResult;
}

bool Group::LoadGroupFromDB(Field* fields)
{
    //                                          0           1           2              3      4      5      6      7      8      9      10     11         12          13              14          15              16          17
    // result = CharacterDatabase.Query("SELECT lootMethod, looterGuid, lootThreshold, icon1, icon2, icon3, icon4, icon5, icon6, icon7, icon8, groupType, difficulty, raiddifficulty, leaderGuid, groupId FROM groups");

    m_Guid = ObjectGuid(HIGHGUID_GROUP,fields[15].GetUInt32());
    m_leaderGuid = ObjectGuid(HIGHGUID_PLAYER, fields[14].GetUInt32());

    // group leader not exist
    if (!sAccountMgr.GetPlayerNameByGUID(m_leaderGuid, m_leaderName))
        return false;

    m_groupType  = GroupType(fields[11].GetUInt8());

    if (m_groupType & GROUPTYPE_RAID)
        _initRaidSubGroupsCounter();

    uint32 diff = fields[12].GetUInt8();
    if (diff >= MAX_DUNGEON_DIFFICULTY)
        diff = DUNGEON_DIFFICULTY_NORMAL;
    SetDungeonDifficulty(Difficulty(diff));

    uint32 r_diff = fields[13].GetUInt8();
    if (r_diff >= MAX_RAID_DIFFICULTY)
        r_diff = RAID_DIFFICULTY_10MAN_NORMAL;
    SetRaidDifficulty(Difficulty(r_diff));

    m_lootMethod = LootMethod(fields[0].GetUInt8());
    m_looterGuid = ObjectGuid(HIGHGUID_PLAYER, fields[1].GetUInt32());
    m_lootThreshold = ItemQualities(fields[2].GetUInt16());

    for (int i = 0; i < TARGET_ICON_COUNT; ++i)
        m_targetIcons[i] = ObjectGuid(fields[3+i].GetUInt64());

    if (isLFDGroup())
        sLFGMgr.CreateLFGState(GetObjectGuid());

    return true;
}

bool Group::LoadMemberFromDB(uint32 guidLow, uint8 subgroup, GroupFlagMask flags, LFGRoleMask roles)
{
    MemberSlot member;
    member.guid = ObjectGuid(HIGHGUID_PLAYER, guidLow);

    // skip nonexistent member
    if (!sAccountMgr.GetPlayerNameByGUID(member.guid, member.name))
        return false;

    member.group = subgroup;
    member.flags = flags;
    member.roles = roles;
    m_memberSlots.push_back(member);

    SubGroupCounterIncrease(subgroup);

    if (Player* player = sObjectMgr.GetPlayer(member.guid))
    {
        if (player->IsInWorld())
            sLFGMgr.GetLFGPlayerState(player->GetObjectGuid())->SetRoles(roles);
    }

    return true;
}

void Group::ConvertToRaid()
{
    m_groupType = GroupType(m_groupType | GROUPTYPE_RAID);

    _initRaidSubGroupsCounter();

    if (IsNeedSave())
    {
        static SqlStatementID updGroup;
        CharacterDatabase.CreateStatement(updGroup, "UPDATE groups SET groupType = ? WHERE groupId = ?")
            .PExecute(uint8(GetGroupType()), m_Guid.GetCounter());
    }

    SendUpdate();

    // update quest related GO states (quest activity dependent from raid membership)
    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
    {
        if (Player* player = sObjectMgr.GetPlayer(citr->guid))
            player->UpdateForQuestWorldObjects();
    }
}

bool Group::AddInvite(Player* player)
{
    if (!player || player->GetGroupInvite())
        return false;

    Group* group = player->GetGroup();
    if (group && group->isBGGroup())
        group = player->GetOriginalGroup();

    if (group)
        return false;

    RemoveInvite(player);

    m_invitees.insert(player);

    player->SetGroupInvite(GetObjectGuid());

    return true;
}

bool Group::AddLeaderInvite(Player* player)
{
    if (!AddInvite(player))
        return false;

    // Group may be added without Create() call - need make ObjectGuid manually
    if (!GetObjectGuid())
        m_Guid = ObjectGuid(HIGHGUID_GROUP, sObjectMgr.GenerateGroupLowGuid());

    m_leaderGuid = player->GetObjectGuid();
    m_leaderName = player->GetName();
    return true;
}

uint32 Group::RemoveInvite(Player *player)
{
    m_invitees.erase(player);

    player->SetGroupInvite(ObjectGuid());
    return GetMembersCount();
}

void Group::RemoveAllInvites()
{
    for (InvitesList::iterator itr = m_invitees.begin(); itr!=m_invitees.end(); ++itr)
        (*itr)->SetGroupInvite(ObjectGuid());

    m_invitees.clear();
}

Player* Group::GetInvited(ObjectGuid guid) const
{
    for (InvitesList::const_iterator itr = m_invitees.begin(); itr != m_invitees.end(); ++itr)
    {
        if ((*itr)->GetObjectGuid() == guid)
            return (*itr);
    }

    return NULL;
}

Player* Group::GetInvited(const std::string& name) const
{
    for (InvitesList::const_iterator itr = m_invitees.begin(); itr != m_invitees.end(); ++itr)
    {
        if ((*itr)->GetName() == name)
            return (*itr);
    }
    return NULL;
}

bool Group::AddMember(ObjectGuid guid, const char* name)
{
    // not add again if player already in this group
    if (IsMember(guid))
    {
        sLog.outError("Group::AddMember: attempt to add %s into %s. Player already in this group.",
            guid.GetString().c_str(), GetObjectGuid().GetString().c_str());
        return false;
    }

    if (!_addMember(guid, name))
        return false;

    SendUpdate();

    if (isLFDGroup())
        sLFGMgr.AddMemberToLFDGroup(guid);

    if (Player* player = sObjectMgr.GetPlayer(guid))
    {
        if (!IsLeader(guid) && !isBGGroup())
        {
            // reset the new member's instances, unless he is currently in one of them
            // including raid/heroic instances that they are not permanently bound to!
            player->ResetInstances(INSTANCE_RESET_GROUP_JOIN, false);
            player->ResetInstances(INSTANCE_RESET_GROUP_JOIN, true);

            if (player->getLevel() >= LEVELREQUIREMENT_HEROIC)
            {
                if (player->GetDungeonDifficulty() != GetDungeonDifficulty())
                {
                    player->SetDungeonDifficulty(GetDungeonDifficulty());
                    player->SendDungeonDifficulty(true);
                }
                if (player->GetRaidDifficulty() != GetRaidDifficulty())
                {
                    player->SetRaidDifficulty(GetRaidDifficulty());
                    player->SendRaidDifficulty(true);
                }
            }
        }

        player->SetGroupUpdateFlag(GROUP_UPDATE_FULL);
        UpdatePlayerOutOfRange(player);

        // quest related GO state dependent from raid membership
        if (isRaidGroup())
            player->UpdateForQuestWorldObjects();

        // Broadcast new player group member fields to rest of the group
        player->SetFieldNotifyFlag(UF_FLAG_PARTY_MEMBER);

        UpdateData data;

        // Broadcast group members' fields to player
        for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
        {
//            if (itr->getSource() == player)
//                continue;

            if (Player* pMember = itr->getSource())
            {
                if (player->HaveAtClient(pMember->GetObjectGuid()))
                {
                    pMember->SetFieldNotifyFlag(UF_FLAG_PARTY_MEMBER);
                    pMember->BuildValuesUpdateBlockForPlayer(&data, player);
                    pMember->RemoveFieldNotifyFlag(UF_FLAG_PARTY_MEMBER);
                }

                if (pMember->HaveAtClient(player->GetObjectGuid()))
                {
                    UpdateData membData;
                    player->BuildValuesUpdateBlockForPlayer(&membData, pMember);

                    if (membData.HasData())
                    {
                        WorldPacket packet;
                        membData.BuildPacket(&packet);
                        pMember->SendDirectMessage(&packet);
                    }
                }
            }
        }

        if (data.HasData())
        {
            WorldPacket packet;
            data.BuildPacket(&packet);
            player->SendDirectMessage(&packet);
        }

        player->RemoveFieldNotifyFlag(UF_FLAG_PARTY_MEMBER);
    }

    return true;
}

uint32 Group::RemoveMember(ObjectGuid guid, uint8 method, bool logout /*=false*/)
{
    // Frozen Mod
    BroadcastGroupUpdate();
    // Frozen Mod

    RemoveGroupBuffsOnMemberRemove(guid);

    if (!sWorld.getConfig(CONFIG_BOOL_PLAYERBOT_DISABLE))
    {
        Player* const player = sObjectMgr.GetPlayer(guid);
        if (player && player->GetPlayerbotMgr())
            player->GetPlayerbotMgr()->RemoveAllBotsFromGroup();
    }

    // wait to leader reconnect
    if (logout && IsLeader(guid))
        return m_memberSlots.size();

    // remove member and change leader (if need) only if strong more 2 members _before_ member remove
    if (GetMembersCount() > uint32(isBGGroup() ? 1 : 2))    // in BG group case allow 1 members group
    {
        bool leaderChanged = _removeMember(guid);

        if (Player* player = sObjectMgr.GetPlayer(guid))
        {
            // quest related GO state dependent from raid membership
            if (isRaidGroup())
                player->UpdateForQuestWorldObjects();

            if (method == 1)
            {
                WorldPacket data(SMSG_GROUP_UNINVITE, 0);
                player->GetSession()->SendPacket(&data);
            }

            // we already removed player from group and in player->GetGroup() is his original group!
            if (Group* group = player->GetGroup())
                group->SendUpdate();
            else
            {
                WorldPacket data(SMSG_GROUP_LIST, 1 + 1 + 1 + 1 + 8 + 4 + 4 + 8);
                data << uint8(0x10) << uint8(0) << uint8(0) << uint8(0);
                data << uint64(0) << uint32(0) << uint32(0) << uint64(0);
                player->GetSession()->SendPacket(&data);
            }

            _homebindIfInstance(player);

            if (isLFDGroup())
                sLFGMgr.RemoveMemberFromLFDGroup(this, guid);
        }

        if (leaderChanged)
        {
            WorldPacket data(SMSG_GROUP_SET_LEADER, m_memberSlots.front().name.size() + 1);
            data << m_memberSlots.front().name;
            BroadcastPacket(&data, true);
        }

        SendUpdate();
    }
    // if group before remove <= 2 disband it
    else
        Disband(true);

    return m_memberSlots.size();
}

void Group::RemoveGroupBuffsOnMemberRemove(ObjectGuid guid)
{
    // Attention! player may be NULL (offline)
    Player* pLeaver = sObjectMgr.GetPlayer(guid);

    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        if (Player* pGroupGuy = itr->getSource())
        {
            // dont remove my auras from myself, also skip offlined
            if (pGroupGuy->GetObjectGuid() == guid)
                continue;

            // remove all buffs cast by me from group members before leaving
            pGroupGuy->RemoveAllGroupBuffsFromCaster(guid);

            // remove from me all buffs cast by group members
            if (pLeaver)
                pLeaver->RemoveAllGroupBuffsFromCaster(pGroupGuy->GetObjectGuid());
        }
    }
}

void Group::ChangeLeader(ObjectGuid guid)
{
    member_citerator slot = _getMemberCSlot(guid);
    if (slot == m_memberSlots.end())
        return;

    sLFGMgr.Leave(this);

    _setLeader(guid);

    WorldPacket data(SMSG_GROUP_SET_LEADER, slot->name.size() + 1);
    data << slot->name;
    BroadcastPacket(&data, true);
    SendUpdate();
}

void Group::CheckLeader(ObjectGuid const& guid, bool logout)
{
    if (IsLeader(guid))
    {
        if (logout)
            m_waitLeaderTimer = sWorld.getConfig(CONFIG_UINT32_GROUPLEADER_RECONNECT_PERIOD) * IN_MILLISECONDS;
        else
            m_waitLeaderTimer = 0;
        return;
    }

    // normal member logins
    if (logout || m_waitLeaderTimer)
        return;

    Player* pLeader = NULL;

    // find the leader from group members
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        if (itr->getSource()->GetObjectGuid() == m_leaderGuid)
        {
            pLeader = itr->getSource();
            break;
        }
    }

    if (!pLeader || !pLeader->IsInWorld())
        m_waitLeaderTimer = sWorld.getConfig(CONFIG_UINT32_GROUPLEADER_RECONNECT_PERIOD) * IN_MILLISECONDS;
}

Player* Group::GetMemberWithRole(GroupFlagMask role)
{
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* player = itr->getSource();
        if (player && player->IsInWorld() &&
            player->GetObjectGuid() != m_leaderGuid &&
            IsGroupRole(player->GetObjectGuid(), role) &&
            !player->GetPlayerbotAI() /* don't give leader to bots */)
        {
            return player;
        }
    }
    return NULL;
}

bool Group::ChangeLeaderToFirstSuitableMember(bool onlySet/*= false*/)
{
    Player* newLeader = NULL;

    // first find asistants
    if (isRaidGroup())
    {
        // Assistant
        newLeader = GetMemberWithRole(GROUP_ASSISTANT);
        // Main Tank
        if (!newLeader)
            newLeader = GetMemberWithRole(GROUP_MAIN_TANK);
        // Main Assistant
        if (!newLeader)
            newLeader = GetMemberWithRole(GROUP_MAIN_ASSISTANT);
    }

    // then any member
    if (!newLeader)
        newLeader = GetMemberWithRole(GROUP_MEMBER);

    if (newLeader)
    {
        if (onlySet)
            _setLeader(newLeader->GetObjectGuid());
        else
            ChangeLeader(newLeader->GetObjectGuid());
        return true;
    }

    return false;
}

void Group::Disband(bool hideDestroy)
{
    Player* player;

    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
    {
        player = sObjectMgr.GetPlayer(citr->guid);
        if (!player)
            continue;

        // we cannot call _removeMember because it would invalidate member iterator
        // if we are removing player from battleground raid
        if (isBGGroup())
            player->RemoveFromBattleGroundRaid();
        else
        {
            // we can remove player who is in battleground from his original group
            if (player->GetOriginalGroupGuid() == GetObjectGuid())
                player->SetOriginalGroup(ObjectGuid());
            else
                player->SetGroup(ObjectGuid());
        }

        // quest related GO state dependent from raid membership
        if (isRaidGroup())
            player->UpdateForQuestWorldObjects();

        if (!player->GetSession())
            continue;

        if (isLFDGroup())
            sLFGMgr.RemoveMemberFromLFDGroup(this, player->GetObjectGuid());

        if (!hideDestroy)
        {
            WorldPacket data(SMSG_GROUP_DESTROYED, 0);
            player->GetSession()->SendPacket(&data);
        }

        // we already removed player from group and in player->GetGroup() is his original group, send update
        if (Group* group = player->GetGroup())
            group->SendUpdate();
        else
        {
            WorldPacket data(SMSG_GROUP_LIST, 1 + 1 + 1 + 1 + 8 + 4 + 4 + 8);
            data << uint8(0x10) << uint8(0) << uint8(0) << uint8(0);
            data << uint64(0) << uint32(0) << uint32(0) << uint64(0);
            player->GetSession()->SendPacket(&data);
        }

        _homebindIfInstance(player);
    }

    m_rollIds->clear();
    m_memberSlots.clear();

    RemoveAllInvites();

    if (IsNeedSave())
    {
        CharacterDatabase.BeginTransaction();

        static SqlStatementID delGroup;
        CharacterDatabase.CreateStatement(delGroup, "DELETE FROM groups WHERE groupId = ?")
            .PExecute(m_Guid.GetCounter());

        static SqlStatementID delMemb;
        CharacterDatabase.CreateStatement(delMemb, "DELETE FROM group_member WHERE groupId = ?")
            .PExecute(m_Guid.GetCounter());

        CharacterDatabase.CommitTransaction();

        ResetInstances(INSTANCE_RESET_GROUP_DISBAND, false, NULL);
        ResetInstances(INSTANCE_RESET_GROUP_DISBAND, true, NULL);
    }

    if (GetObjectGuid())
        sObjectMgr.RemoveGroup(this);

    m_leaderGuid.Clear();
    m_leaderName = "";
}

/*********************************************************/
/***                   LOOT SYSTEM                     ***/
/*********************************************************/

void Group::SendLootStartRoll(uint32 CountDown, uint32 mapid, const Roll &r)
{
    WorldPacket data(SMSG_LOOT_START_ROLL, (8+4+4+4+4+4+4+1));
    data << r.lootedTargetGUID;                             // creature guid what we're looting
    data << uint32(mapid);                                  // 3.3.3 mapid
    data << uint32(r.itemSlot);                             // item slot in loot
    data << uint32(r.itemid);                               // the itemEntryId for the item that shall be rolled for
    data << uint32(r.itemRandomSuffix);                     // randomSuffix
    data << uint32(r.itemRandomPropId);                     // item random property ID
    data << uint32(r.itemCount);                            // items in stack
    data << uint32(CountDown);                              // the countdown time to choose "need" or "greed"

    size_t voteMaskPos = data.wpos();
    data << uint8(0);                                       // roll type mask, allowed choices (placeholder)

    for (Roll::PlayerVote::const_iterator itr = r.playerVote.begin(); itr != r.playerVote.end(); ++itr)
    {
        Player* p = sObjectMgr.GetPlayer(itr->first);
        if (!p || !p->GetSession())
            continue;

        if (itr->second == ROLL_NOT_VALID)
            continue;

        // dependent from player
        RollVoteMask mask = r.GetVoteMaskFor(p);
        data.put<uint8>(voteMaskPos,uint8(mask));

        p->GetSession()->SendPacket(&data);
    }
}

void Group::SendLootRoll(ObjectGuid const& targetGuid, uint8 rollNumber, uint8 rollType, const Roll &r)
{
    WorldPacket data(SMSG_LOOT_ROLL, (8+4+8+4+4+4+1+1+1));
    data << r.lootedTargetGUID;                             // creature guid what we're looting
    data << uint32(r.itemSlot);                             // unknown, maybe amount of players, or item slot in loot
    data << targetGuid;
    data << uint32(r.itemid);                               // the itemEntryId for the item that shall be rolled for
    data << uint32(r.itemRandomSuffix);                     // randomSuffix
    data << uint32(r.itemRandomPropId);                     // Item random property ID
    data << uint8(rollNumber);                              // 0: "Need for: [item name]" > 127: "you passed on: [item name]"      Roll number
    data << uint8(rollType);                                // 0: "Need for: [item name]" 0: "You have selected need for [item name] 1: need roll 2: greed roll
    data << uint8(0);                                       // auto pass on loot

    for (Roll::PlayerVote::const_iterator itr = r.playerVote.begin(); itr != r.playerVote.end(); ++itr)
    {
        Player* p = sObjectMgr.GetPlayer(itr->first);
        if (!p || !p->GetSession())
            continue;

        if (itr->second != ROLL_NOT_VALID)
            p->GetSession()->SendPacket(&data);
    }
}

void Group::SendLootRollWon(ObjectGuid const& targetGuid, uint8 rollNumber, RollVote rollType, const Roll &r)
{
    WorldPacket data(SMSG_LOOT_ROLL_WON, (8+4+4+4+4+8+1+1));
    data << r.lootedTargetGUID;                             // creature guid what we're looting
    data << uint32(r.itemSlot);                             // item slot in loot
    data << uint32(r.itemid);                               // the itemEntryId for the item that shall be rolled for
    data << uint32(r.itemRandomSuffix);                     // randomSuffix
    data << uint32(r.itemRandomPropId);                     // Item random property
    data << targetGuid;                                     // guid of the player who won.
    data << uint8(rollNumber);                              // rollnumber related to SMSG_LOOT_ROLL
    data << uint8(rollType);                                // Rolltype related to SMSG_LOOT_ROLL

    for (Roll::PlayerVote::const_iterator itr = r.playerVote.begin(); itr != r.playerVote.end(); ++itr)
    {
        Player* p = sObjectMgr.GetPlayer(itr->first);
        if (!p || !p->GetSession())
            continue;

        if (itr->second != ROLL_NOT_VALID)
            p->GetSession()->SendPacket(&data);
    }
}

void Group::SendLootAllPassed(Roll const& r)
{
    WorldPacket data(SMSG_LOOT_ALL_PASSED, (8+4+4+4+4));
    data << r.lootedTargetGUID;                             // creature guid what we're looting
    data << uint32(r.itemSlot);                             // item slot in loot
    data << uint32(r.itemid);                               // The itemEntryId for the item that shall be rolled for
    data << uint32(r.itemRandomPropId);                     // Item random property ID
    data << uint32(r.itemRandomSuffix);                     // Item random suffix ID

    for (Roll::PlayerVote::const_iterator itr=r.playerVote.begin(); itr!=r.playerVote.end(); ++itr)
    {
        Player* p = sObjectMgr.GetPlayer(itr->first);
        if (!p || !p->GetSession())
            continue;

        if (itr->second != ROLL_NOT_VALID)
            p->GetSession()->SendPacket(&data);
    }
}

void Group::GroupLoot(WorldObject* pSource, Loot* loot)
{
    uint32 maxEnchantingSkill = GetMaxSkillValueForGroup(SKILL_ENCHANTING);

    for (uint8 itemSlot = 0; itemSlot < loot->items.size(); ++itemSlot)
    {
        LootItem& lootItem = loot->items[itemSlot];
        ItemPrototype const* itemProto = ObjectMgr::GetItemPrototype(lootItem.itemid);
        if (!itemProto)
        {
            DEBUG_LOG("Group::GroupLoot: missing item prototype for item with id: %d", lootItem.itemid);
            continue;
        }

        //roll for over-threshold item if it's one-player loot
        if (itemProto->Quality >= uint32(m_lootThreshold) && !lootItem.freeforall)
            StartLootRoll(pSource, GROUP_LOOT, loot, itemSlot, maxEnchantingSkill);
        else
            lootItem.is_underthreshold = 1;
    }
}

void Group::NeedBeforeGreed(WorldObject* pSource, Loot* loot)
{
    uint32 maxEnchantingSkill = GetMaxSkillValueForGroup(SKILL_ENCHANTING);

    for (uint8 itemSlot = 0; itemSlot < loot->items.size(); ++itemSlot)
    {
        LootItem& lootItem = loot->items[itemSlot];
        ItemPrototype const* itemProto = ObjectMgr::GetItemPrototype(lootItem.itemid);
        if (!itemProto)
        {
            DEBUG_LOG("Group::NeedBeforeGreed: missing item prototype for item with id: %d", lootItem.itemid);
            continue;
        }

        // only roll for one-player items, not for ones everyone can get
        if (itemProto->Quality >= uint32(m_lootThreshold) && !lootItem.freeforall)
            StartLootRoll(pSource, NEED_BEFORE_GREED, loot, itemSlot, maxEnchantingSkill);
        else
            lootItem.is_underthreshold = 1;
    }
}

void Group::MasterLoot(WorldObject* pSource, Loot* loot)
{
    for (LootItemList::iterator i=loot->items.begin(); i != loot->items.end(); ++i)
    {
        ItemPrototype const *item = ObjectMgr::GetItemPrototype(i->itemid);
        if (!item)
            continue;

        if (item->Quality < uint32(m_lootThreshold))
            i->is_underthreshold = 1;
    }

    uint32 real_count = 0;

    WorldPacket data(SMSG_LOOT_MASTER_LIST, 330);
    data << uint8(GetMembersCount());

    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* looter = itr->getSource();
        if (!looter->IsInWorld())
            continue;

        if (looter->IsWithinDist(pSource, sWorld.getConfig(CONFIG_FLOAT_GROUP_XP_DISTANCE), false))
        {
            data << looter->GetObjectGuid();
            ++real_count;
        }
    }

    data.put<uint8>(0, real_count);

    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* looter = itr->getSource();
        if (looter->IsWithinDist(pSource, sWorld.getConfig(CONFIG_FLOAT_GROUP_XP_DISTANCE), false))
            looter->GetSession()->SendPacket(&data);
    }
}

bool Group::CountRollVote(Player* player, ObjectGuid const& lootedTarget, uint32 itemSlot, RollVote vote)
{
    TRolls::iterator rollI = m_rollIds->begin();
    for (; rollI != m_rollIds->end(); ++rollI)
    {
        if ((*rollI)->isValid() && (*rollI)->lootedTargetGUID == lootedTarget && (*rollI)->itemSlot == itemSlot)
            break;
    }

    if (rollI == m_rollIds->end())
        return false;

    // possible cheating
    RollVoteMask voteMask = (*rollI)->GetVoteMaskFor(player);
    if ((voteMask & (1 << vote)) == 0)
        return false;

    CountRollVote(player->GetObjectGuid(), rollI, vote);    // result not related this function result meaning, ignore
    return true;
}

bool Group::CountRollVote(ObjectGuid const& playerGUID, TRolls::iterator& rollI, RollVote vote)
{
    Roll* roll = *rollI;

    Roll::PlayerVote::iterator itr = roll->playerVote.find(playerGUID);
    // this condition means that player joins to the party after roll begins
    if (itr == roll->playerVote.end())
        return true;                                        // result used for need iterator ++, so avoid for end of list

    if (roll->getLoot())
    {
        if (roll->getLoot()->items.empty())
            return false;
    }

    switch (vote)
    {
        case ROLL_PASS:                                     // Player choose pass
        {
            SendLootRoll(playerGUID, 128, 128, *roll);
            ++roll->totalPass;
            itr->second = ROLL_PASS;
            break;
        }
        case ROLL_NEED:                                     // player choose Need
        {
            SendLootRoll(playerGUID, 0, 0, *roll);
            ++roll->totalNeed;
            itr->second = ROLL_NEED;
            break;
        }
        case ROLL_GREED:                                    // player choose Greed
        {
            SendLootRoll(playerGUID, 128, ROLL_GREED, *roll);
            ++roll->totalGreed;
            itr->second = ROLL_GREED;
            break;
        }
        case ROLL_DISENCHANT:                               // player choose Disenchant
        {
            SendLootRoll(playerGUID, 128, ROLL_DISENCHANT, *roll);
            ++roll->totalGreed;
            itr->second = ROLL_DISENCHANT;
            break;
        }
        default:                                            // Roll removed case
            break;
    }

    if (roll->totalPass + roll->totalNeed + roll->totalGreed >= roll->totalPlayersRolling)
    {
        CountTheRoll(rollI);
        return true;
    }

    return false;
}

void Group::StartLootRoll(WorldObject* lootTarget, LootMethod method, Loot* loot, uint8 itemSlot, uint32 maxEnchantingSkill)
{
    if (itemSlot >= loot->items.size())
        return;

    LootItem const& lootItem = loot->items[itemSlot];

    Roll* r = new Roll(lootTarget->GetObjectGuid(), method, lootItem);

    // a vector is filled with only near party members
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* playerToRoll = itr->getSource();
        if (!playerToRoll || !playerToRoll->GetSession())
            continue;

        if (lootItem.AllowedForPlayer(playerToRoll, lootTarget))
        {
            if (playerToRoll->IsWithinDistInMap(lootTarget, sWorld.getConfig(CONFIG_FLOAT_GROUP_XP_DISTANCE), false))
            {
                r->playerVote[playerToRoll->GetObjectGuid()] = ROLL_NOT_EMITED_YET;
                ++r->totalPlayersRolling;
            }
        }
    }

    if (r->totalPlayersRolling > 0)                         // has looters
    {
        r->setLoot(loot);
        r->itemSlot = itemSlot;

        if (r->totalPlayersRolling == 1)                    // single looter
            r->playerVote.begin()->second = ROLL_NEED;
        else
        {
            // Only GO-group looting and NPC-group looting possible
            MANGOS_ASSERT(lootTarget->isType(TYPEMASK_CREATURE_OR_GAMEOBJECT));

            r->CalculateCommonVoteMask(maxEnchantingSkill); // dependent from item and possible skill

            SendLootStartRoll(LOOT_ROLL_TIMEOUT, lootTarget->GetMapId(), *r);
            loot->items[itemSlot].is_blocked = true;

            lootTarget->StartGroupLoot(this, LOOT_ROLL_TIMEOUT);
        }

        m_rollIds->push_back(r);
    }
    else                                            // no looters??
        delete r;
}

// called when roll timer expires
void Group::EndRoll()
{
    while (!m_rollIds->empty())
    {
        // need more testing here, if rolls disappear
        TRolls::iterator itr = m_rollIds->begin();
        CountTheRoll(itr);                                  //i don't have to edit player votes, who didn't vote ... he will pass
    }
}

void Group::CountTheRoll(TRolls::iterator& rollI)
{
    Roll* roll = *rollI;
    if (!roll->isValid())                                    // is loot already deleted ?
    {
        rollI = m_rollIds->erase(rollI);
        delete roll;
        return;
    }

    // end of the roll
    if (roll->totalNeed > 0)
    {
        if (!roll->playerVote.empty())
        {
            uint8 maxresul = 0;
            ObjectGuid maxguid  = (*roll->playerVote.begin()).first;
            Player* player;

            for (Roll::PlayerVote::const_iterator itr = roll->playerVote.begin(); itr != roll->playerVote.end(); ++itr)
            {
                if (itr->second != ROLL_NEED)
                    continue;

                uint8 randomN = urand(1, 100);
                SendLootRoll(itr->first, randomN, ROLL_NEED, *roll);
                if (maxresul < randomN)
                {
                    maxguid  = itr->first;
                    maxresul = randomN;
                }
            }
            SendLootRollWon(maxguid, maxresul, ROLL_NEED, *roll);
            player = sObjectMgr.GetPlayer(maxguid);

            if (player && player->GetSession())
            {
                player->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_ROLL_NEED_ON_LOOT, roll->itemid, maxresul);

                ItemPosCountVec dest;
                LootItem* item = &(roll->getLoot()->items[roll->itemSlot]);
                InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, roll->itemid, item->count);
                if (msg == EQUIP_ERR_OK)
                {
                    item->is_looted = true;
                    roll->getLoot()->NotifyItemRemoved(roll->itemSlot);
                    --roll->getLoot()->unlootedCount;

                    player->StoreNewItem(dest, roll->itemid, true, item->randomPropertyId, item->GetAllowedLooters());
                    player->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_ITEM, roll->itemid, item->count);
                    player->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_TYPE, roll->getLoot()->loot_type, item->count);
                    player->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_EPIC_ITEM, roll->itemid, item->count);
                }
                else
                {
                    item->is_blocked = false;
                    player->SendEquipError(msg, NULL, NULL, roll->itemid);
                }
            }
        }
    }
    else if (roll->totalGreed > 0)
    {
        if (!roll->playerVote.empty())
        {
            uint8 maxresul = 0;
            ObjectGuid maxguid = (*roll->playerVote.begin()).first;
            Player* player;
            RollVote rollvote = ROLL_PASS;                  //Fixed: Using uninitialized memory 'rollvote'

            Roll::PlayerVote::iterator itr;
            for (itr = roll->playerVote.begin(); itr != roll->playerVote.end(); ++itr)
            {
                if (itr->second != ROLL_GREED && itr->second != ROLL_DISENCHANT)
                    continue;

                uint8 randomN = urand(1, 100);
                SendLootRoll(itr->first, randomN, itr->second, *roll);
                if (maxresul < randomN)
                {
                    maxguid  = itr->first;
                    maxresul = randomN;
                    rollvote = itr->second;
                }
            }
            SendLootRollWon(maxguid, maxresul, rollvote, *roll);
            player = sObjectMgr.GetPlayer(maxguid);

            if (player && player->GetSession())
            {
                player->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_ROLL_GREED_ON_LOOT, roll->itemid, maxresul);

                LootItem* item = &(roll->getLoot()->items[roll->itemSlot]);

                if (rollvote == ROLL_GREED)
                {
                    ItemPosCountVec dest;
                    InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, roll->itemid, item->count);
                    if (msg == EQUIP_ERR_OK)
                    {
                        item->is_looted = true;
                        roll->getLoot()->NotifyItemRemoved(roll->itemSlot);
                        --roll->getLoot()->unlootedCount;

                        player->StoreNewItem(dest, roll->itemid, true, item->randomPropertyId, item->GetAllowedLooters());
                        player->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_ITEM, roll->itemid, item->count);
                        player->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_TYPE, roll->getLoot()->loot_type, item->count);
                        player->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_EPIC_ITEM, roll->itemid, item->count);
                    }
                    else
                    {
                        item->is_blocked = false;
                        player->SendEquipError(msg, NULL, NULL, roll->itemid);
                    }
                }
                else if (rollvote == ROLL_DISENCHANT)
                {
                    item->is_looted = true;
                    roll->getLoot()->NotifyItemRemoved(roll->itemSlot);
                    --roll->getLoot()->unlootedCount;

                    ItemPrototype const* pProto = ObjectMgr::GetItemPrototype(roll->itemid);
                    player->AutoStoreLoot(roll->getLoot()->GetLootTarget(), pProto->DisenchantID, LootTemplates_Disenchant, true);
                }
            }
        }
    }
    else
    {
        SendLootAllPassed(*roll);
        LootItem *item = &(roll->getLoot()->items[roll->itemSlot]);
        if (item) item->is_blocked = false;
    }
    rollI = m_rollIds->erase(rollI);
    delete roll;
}

void Group::SetTargetIcon(uint8 id, ObjectGuid whoGuid, ObjectGuid targetGuid)
{
    if (id >= TARGET_ICON_COUNT)
        return;

    // clean other icons
    if (targetGuid)
    {
        for (int i = 0; i < TARGET_ICON_COUNT; ++i)
        {
            if (m_targetIcons[i] == targetGuid)
                SetTargetIcon(i, ObjectGuid(), ObjectGuid());
        }
    }

    m_targetIcons[id] = targetGuid;

    WorldPacket data(MSG_RAID_TARGET_UPDATE, 1 + 8 + 1 + 8);
    data << uint8(0);                                       // set targets
    data << whoGuid;
    data << uint8(id);
    data << targetGuid;
    BroadcastPacket(&data, true);
}

static void GetDataForXPAtKill_helper(Player* player, Unit const* victim, uint32& sum_level, Player* & member_with_max_level, Player* & not_gray_member_with_max_level)
{
    sum_level += player->getLevel();
    if (!member_with_max_level || member_with_max_level->getLevel() < player->getLevel())
        member_with_max_level = player;

    uint32 gray_level = MaNGOS::XP::GetGrayLevel(player->getLevel());
    if (victim->getLevel() > gray_level && (!not_gray_member_with_max_level
        || not_gray_member_with_max_level->getLevel() < player->getLevel()))
        not_gray_member_with_max_level = player;
}

void Group::GetDataForXPAtKill(Unit const* victim, uint32& count,uint32& sum_level, Player* & member_with_max_level, Player* & not_gray_member_with_max_level, Player* additional)
{
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (!member || !member->isAlive())                  // only for alive
            continue;

        // will proccesed later
        if (member == additional)
            continue;

        if (!member->IsAtGroupRewardDistance(victim))       // at req. distance
            continue;

        ++count;
        GetDataForXPAtKill_helper(member,victim,sum_level,member_with_max_level,not_gray_member_with_max_level);
    }

    if (additional)
    {
        if (additional->IsAtGroupRewardDistance(victim))    // at req. distance
        {
            ++count;
            GetDataForXPAtKill_helper(additional,victim,sum_level,member_with_max_level,not_gray_member_with_max_level);
        }
    }
}

void Group::SendTargetIconList(WorldSession *session)
{
    if (!session)
        return;

    WorldPacket data(MSG_RAID_TARGET_UPDATE, 1 + TARGET_ICON_COUNT * (1 + 8));
    data << uint8(1);                                       // list targets

    for (int i = 0; i < TARGET_ICON_COUNT; ++i)
    {
        if (!m_targetIcons[i])
            continue;

        data << uint8(i);
        data << m_targetIcons[i];
    }

    session->SendPacket(&data);
}

void Group::SendUpdate()
{
    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
    {
        Player* player = sObjectMgr.GetPlayer(citr->guid);
        if (!player || !player->GetSession() || player->GetGroupGuid() != GetObjectGuid())
            continue;
                                                            // guess size
        WorldPacket data(SMSG_GROUP_LIST, (1+1+1+1+8+4+GetMembersCount()*20));
        data << uint8(m_groupType);                         // group type (flags in 3.3)
        data << uint8(citr->group);                         // groupid
        data << uint8(citr->flags);                         // group flags
        data << (isLFGGroup() ? uint8(citr->roles) : uint8(0)); // roles mask
        if (isLFGGroup())
        {
            uint32 dungeonID = sLFGMgr.GetLFGGroupState(GetObjectGuid())->GetDungeonId();
            data << uint8(sLFGMgr.GetLFGGroupState(GetObjectGuid())->GetState() == LFG_STATE_FINISHED_DUNGEON ? 2 : 0);
            data << uint32(dungeonID);
        }
        data << GetObjectGuid();                            // group guid
        data << uint32(0);                                  // 3.3, this value increments every time SMSG_GROUP_LIST is sent
        data << uint32(GetMembersCount() - 1);
        for (member_citerator citr2 = m_memberSlots.begin(); citr2 != m_memberSlots.end(); ++citr2)
        {
            if (citr->guid == citr2->guid)
                continue;

            Player* member = sObjectMgr.GetPlayer(citr2->guid);
            uint8 onlineState = (member && !member->GetSession()->PlayerLogout()) ? MEMBER_STATUS_ONLINE : MEMBER_STATUS_OFFLINE;
            onlineState |= isBGGroup() ? MEMBER_STATUS_PVP : 0;

            data << citr2->name;
            data << citr2->guid;
            data << uint8(onlineState);                     // online-state
            data << uint8(citr2->group);                    // groupid
            data << uint8(citr2->flags);                    // group flags
            data << (isLFGGroup() ? uint8(citr2->roles) : uint8(0));  // 3.3, role?
        }

        data << m_leaderGuid;                               // leader guid
        if (GetMembersCount() - 1)
        {
            data << uint8(m_lootMethod);                    // loot method
            data << m_looterGuid;                           // looter guid
            data << uint8(m_lootThreshold);                 // loot threshold
            data << uint8(GetDungeonDifficulty());          // Dungeon Difficulty
            data << uint8(GetRaidDifficulty());             // Raid Difficulty
            data << uint8(0);                               // 3.3, dynamic difficulty?
        }
        player->GetSession()->SendPacket(&data);

        // when player is loading we need a stats update
        if (player->GetSession()->PlayerLoading())
        {
            player->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_MAX_HP | GROUP_UPDATE_FLAG_MAX_POWER | GROUP_UPDATE_FLAG_LEVEL);
            UpdatePlayerOutOfRange(player);
        }
    }
}

void Group::Update(uint32 diff)
{
    if (!m_waitLeaderTimer)
        return;

    if (m_waitLeaderTimer > diff)
    {
        m_waitLeaderTimer -= diff;
        return;
    }

    m_waitLeaderTimer = 0;

    if (!ChangeLeaderToFirstSuitableMember())
    {
        if (RemoveMember(m_leaderGuid, 0) <= 1)
            delete this;
    }
}

void Group::UpdatePlayerOutOfRange(Player* pPlayer)
{
    if (!pPlayer || !pPlayer->IsInWorld())
        return;

    if (pPlayer->GetGroupUpdateFlag() == GROUP_UPDATE_FLAG_NONE)
        return;

    WorldPacket data;
    pPlayer->GetSession()->BuildPartyMemberStatsChangedPacket(pPlayer, &data);

    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        if (Player* player = itr->getSource())
        {
            if (player != pPlayer && !player->HaveAtClient(pPlayer->GetObjectGuid()))
                player->GetSession()->SendPacket(&data);
        }
    }
}

void Group::BroadcastPacket(WorldPacket* packet, bool ignorePlayersInBGRaid, int group, ObjectGuid ignore)
{
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* pl = itr->getSource();
        if (!pl || (ignore && pl->GetObjectGuid() == ignore) || (ignorePlayersInBGRaid && pl->GetGroup() != this) || !pl->IsInWorld())
            continue;

        if (pl->GetSession() && (group == -1 || itr->getSubGroup() == group))
            pl->GetSession()->SendPacket(packet);
    }
}

void Group::BroadcastReadyCheck(WorldPacket* packet)
{
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* pl = itr->getSource();
        if (pl && pl->GetSession() && pl->IsInWorld())
        {
            if (IsLeader(pl->GetObjectGuid()) || IsAssistant(pl->GetObjectGuid()))
                pl->GetSession()->SendPacket(packet);
        }
    }
}

void Group::OfflineReadyCheck()
{
    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
    {
        Player* pl = sObjectMgr.GetPlayer(citr->guid);
        if (!pl || !pl->GetSession())
        {
            WorldPacket data(MSG_RAID_READY_CHECK_CONFIRM, 8 + 1);
            data << citr->guid;
            data << uint8(0);
            BroadcastReadyCheck(&data);
        }
    }
}

bool Group::_addMember(ObjectGuid guid, const char* name)
{
    // get first not-full group
    uint8 groupid = 0;
    GroupFlagMask flags = GROUP_MEMBER;
    LFGRoleMask roles = LFG_ROLE_MASK_NONE;

    if (isLFGGroup() && sObjectMgr.GetPlayer(guid))
        roles = sLFGMgr.GetLFGPlayerState(guid)->GetRoles();

    if (m_subGroupsCounts)
    {
        bool groupFound = false;
        for (; groupid < MAX_RAID_SUBGROUPS; ++groupid)
        {
            if (m_subGroupsCounts[groupid] < MAX_GROUP_SIZE)
            {
                groupFound = true;
                break;
            }
        }
        // We are raid group and no one slot is free
        if (!groupFound)
            return false;
    }

    return _addMember(guid, name, groupid, flags, roles);
}

bool Group::_addMember(ObjectGuid guid, const char* name, uint8 group, GroupFlagMask flags, LFGRoleMask roles)
{
    if (IsFull())
        return false;

    if (!guid || !guid.IsPlayer())
        return false;

    Player* player = sObjectMgr.GetPlayer(guid, false);

    uint32 lastMap = 0;
    if (player && player->IsInWorld())
        lastMap = player->GetMapId();
    else if (player && player->IsBeingTeleported())
        lastMap = player->GetTeleportDest().GetMapId();

    MemberSlot member;
    member.guid      = guid;
    member.name      = name;
    member.group     = group;
    member.flags     = flags;
    member.roles     = roles;
    member.lastMap   = lastMap;
    m_memberSlots.push_back(member);

    SubGroupCounterIncrease(group);

    if (player)
    {
        player->SetGroupInvite(ObjectGuid());
        //if player is in group and he is being added to BG raid group, then call SetBattleGroundRaid()
        if (player->GetGroup() && isBGGroup())
            player->SetBattleGroundRaid(GetObjectGuid(), group);
        //if player is in bg raid and we are adding him to normal group, then call SetOriginalGroup()
        else if (player->GetGroup())
            player->SetOriginalGroup(GetObjectGuid(), group);
        //if player is not in group, then call set group
        else
            player->SetGroup(GetObjectGuid(), group);

        if (player->IsInWorld())
        {
            // if the same group invites the player back, cancel the homebind timer
            if (InstanceGroupBind* bind = GetBoundInstance(player->GetMapId(), player))
            {
                if (bind->state->GetInstanceId() == player->GetInstanceId())
                    player->m_InstanceValid = true;
            }
        }
    }

    if (!isRaidGroup())                                      // reset targetIcons for non-raid-groups
    {
        for (int i = 0; i < TARGET_ICON_COUNT; ++i)
            m_targetIcons[i].Clear();
    }

    if (IsNeedSave())
    {
        // insert into group table
        static SqlStatementID insMemb;
        SqlStatement stmt = CharacterDatabase.CreateStatement(insMemb, "INSERT INTO group_member (groupId, memberGuid, memberFlags, subgroup, roles) VALUES (?, ?, ?, ?, ?)");

        stmt.addUInt32(m_Guid.GetCounter());
        stmt.addUInt32(member.guid.GetCounter());
        stmt.addUInt8(uint8(member.flags));
        stmt.addUInt8(uint8(member.group));
        stmt.addUInt8(uint8(member.roles));
        stmt.Execute();
    }

    return true;
}

bool Group::_removeMember(ObjectGuid guid)
{
    Player* player = sObjectMgr.GetPlayer(guid);
    if (player)
    {
        // if we are removing player from battleground raid
        if (isBGGroup())
            player->RemoveFromBattleGroundRaid();
        else
        {
            // we can remove player who is in battleground from his original group
            if (player->GetOriginalGroupGuid() == GetObjectGuid())
                player->SetOriginalGroup(ObjectGuid());
            else
                player->SetGroup(ObjectGuid());
        }
    }

    _removeRolls(guid);

    member_witerator slot = _getMemberWSlot(guid);
    if (slot != m_memberSlots.end())
    {
        SubGroupCounterDecrease(slot->group);

        m_memberSlots.erase(slot);
    }

    if (IsNeedSave())
    {
        static SqlStatementID delMemb;
        CharacterDatabase.CreateStatement(delMemb, "DELETE FROM group_member WHERE memberGuid = ?")
            .PExecute(guid.GetCounter());
    }

    if (m_leaderGuid == guid)                               // leader was removed
    {
        if (GetMembersCount() > 0)
            ChangeLeaderToFirstSuitableMember(true);
        return true;
    }

    return false;
}

void Group::_setLeader(ObjectGuid guid)
{
    member_citerator slot = _getMemberCSlot(guid);
    if (slot == m_memberSlots.end())
        return;

    Player* player = sObjectMgr.GetPlayer(slot->guid);

    if (IsNeedSave())
        // TODO: set a time limit to have this function run rarely cause it can be slow
        CharacterDatabase.BeginTransaction();

    if (!isBGGroup())
    {
        // update the group's bound instances when changing leaders
        // remove all permanent binds from the group
        // in the DB also remove solo binds that will be replaced with permbinds
        // from the new leader
        if (IsNeedSave())
        {
            static SqlStatementID delGroupIns;
            CharacterDatabase.CreateStatement(delGroupIns,
                "DELETE FROM group_instance WHERE leaderguid = ? AND (permanent = 1 OR "
                "instance IN (SELECT instance FROM character_instance WHERE guid = ?))")
                .PExecute(m_leaderGuid.GetCounter(), slot->guid.GetCounter());
        }

        if (player)
        {
            for (uint8 i = 0; i < MAX_DIFFICULTY; ++i)
            {
                for (BoundInstancesMap::iterator itr = m_boundInstances[i].begin(); itr != m_boundInstances[i].end();)
                {
                    if (itr->second.perm)
                    {
                        itr->second.state->RemoveFromBindList(GetObjectGuid());
                        itr = m_boundInstances[i].erase(itr);
                    }
                    else
                        ++itr;
                }
            }
        }
    }

    if (IsNeedSave())
    {
        // update the group's solo binds to the new leader
        static SqlStatementID updGroupIns;
        CharacterDatabase.CreateStatement(updGroupIns, "UPDATE group_instance SET leaderGuid = ? WHERE leaderGuid = ?")
            .PExecute(slot->guid.GetCounter(), m_leaderGuid.GetCounter());

        // copy the permanent binds from the new leader to the group
        // overwriting the solo binds with permanent ones if necessary
        // in the DB those have been deleted already
        if (player && !isBGGroup())
            Player::ConvertInstancesToGroup(player, this, slot->guid);

        // update the group leader
        static SqlStatementID updGroup;
        CharacterDatabase.CreateStatement(updGroup, "UPDATE groups SET leaderGuid = ? WHERE groupId = ?")
            .PExecute(slot->guid.GetCounter(), m_Guid.GetCounter());

        CharacterDatabase.CommitTransaction();
    }

    m_leaderGuid = slot->guid;
    m_leaderName = slot->name;
}

void Group::_removeRolls(ObjectGuid guid)
{
    for (TRolls::iterator it = m_rollIds->begin(); it != m_rollIds->end();)
    {
        Roll* roll = *it;
        Roll::PlayerVote::iterator itr2 = roll->playerVote.find(guid);
        if (itr2 == roll->playerVote.end())
        {
            ++it;
            continue;
        }

        if (itr2->second == ROLL_GREED || itr2->second == ROLL_DISENCHANT)
            --roll->totalGreed;
        if (itr2->second == ROLL_NEED)
            --roll->totalNeed;
        if (itr2->second == ROLL_PASS)
            --roll->totalPass;
        if (itr2->second != ROLL_NOT_VALID)
            --roll->totalPlayersRolling;

        roll->playerVote.erase(itr2);

        if (!CountRollVote(guid, it, ROLL_NOT_EMITED_YET))
            ++it;
    }
}

bool Group::_setMembersGroup(ObjectGuid guid, uint8 group)
{
    member_witerator slot = _getMemberWSlot(guid);
    if (slot == m_memberSlots.end())
        return false;

    slot->group = group;

    SubGroupCounterIncrease(group);

    if (IsNeedSave())
    {
        static SqlStatementID updMemb;
        CharacterDatabase.CreateStatement(updMemb, "UPDATE group_member SET subgroup = ? WHERE memberGuid = ?")
            .PExecute(uint8(group), guid.GetCounter());
    }

    return true;
}

void Group::SetGroupUniqueFlag(ObjectGuid guid, GroupFlagsAssignment assignment, uint8 apply)
{
    GroupFlagMask mask        = GROUP_MEMBER;
    GroupFlagMask excludeMask = GROUP_MEMBER;

    switch (assignment)
    {
        case GROUP_ASSIGN_MAINTANK:
            mask = GROUP_MAIN_TANK;
            excludeMask = GROUP_MAIN_ASSISTANT;
            break;
        case GROUP_ASSIGN_MAINASSIST:
            mask = GROUP_MAIN_ASSISTANT;
            excludeMask = GROUP_MAIN_TANK;
            break;
        case GROUP_ASSIGN_ASSISTANT:
            mask = GROUP_ASSISTANT;
            break;
        default:
            sLog.outError("Group::SetGroupUniqueFlag unknown assignment %u on %s", assignment, guid.GetString().c_str());
            return;
    }

    if (guid)
    {
        static SqlStatementID updGroupMemb;
        SqlStatement stmt = CharacterDatabase.CreateStatement(updGroupMemb, "UPDATE group_member SET memberFlags = ? WHERE memberGuid = ?");

        if (apply)
        {
            for (member_witerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
            {
                if (itr->guid != guid)
                {
                    if ((itr->flags & mask) && sWorld.getConfig(CONFIG_BOOL_RAID_FLAGS_UNIQUE))
                    {
                        GroupFlagMask oldMask = itr->flags;
                        itr->flags = GroupFlagMask(oldMask & ~mask);
                        if (itr->flags != oldMask && IsNeedSave())
                            stmt.PExecute(uint8(itr->flags), itr->guid.GetCounter());
                    }
                }
                else
                {
                    GroupFlagMask oldMask = itr->flags;
                    itr->flags = GroupFlagMask((oldMask | mask) & ~excludeMask);
                    if (itr->flags != oldMask && IsNeedSave())
                        stmt.PExecute(uint8(itr->flags), itr->guid.GetCounter());
                }
            }
        }
        else
        {
            member_witerator slot = _getMemberWSlot(guid);
            if (slot != m_memberSlots.end())
            {
                GroupFlagMask oldMask = slot->flags;
                slot->flags = GroupFlagMask(oldMask & ~mask);
                if (slot->flags != oldMask && IsNeedSave())
                    stmt.PExecute(uint8(slot->flags), slot->guid.GetCounter());
            }
        }

        SendUpdate();
    }
}

bool Group::SameSubGroup(Player const* member1, Player const* member2) const
{
    if (!member1 || !member2)
        return false;

    if (member1->GetGroup() != this || member2->GetGroup() != this)
        return false;
    else
        return member1->GetSubGroup() == member2->GetSubGroup();
}

// allows setting subgroup for offline members
void Group::ChangeMembersGroup(ObjectGuid guid, uint8 group)
{
    if (!isRaidGroup())
        return;

    Player* player = sObjectMgr.GetPlayer(guid);
    if (!player)
    {
        uint8 prevSubGroup = GetMemberGroup(guid);
        if (prevSubGroup == group)
            return;

        if (_setMembersGroup(guid, group))
        {
            SubGroupCounterDecrease(prevSubGroup);
            SendUpdate();
        }
    }
    else
        // This methods handles itself groupcounter decrease
        ChangeMembersGroup(player, group);
}

// only for online members
void Group::ChangeMembersGroup(Player* player, uint8 group)
{
    if (!player || !isRaidGroup())
        return;

    uint8 prevSubGroup = player->GetSubGroup();
    if (prevSubGroup == group)
        return;

    if (_setMembersGroup(player->GetObjectGuid(), group))
    {
        if (player->GetGroupGuid() == GetObjectGuid())
            player->GetGroupRef().setSubGroup(group);
        // if player is in BG raid, it is possible that he is also in normal raid - and that normal raid is stored in m_originalGroup reference
        else
        {
            prevSubGroup = player->GetOriginalSubGroup();
            player->GetOriginalGroupRef().setSubGroup(group);
        }
        SubGroupCounterDecrease(prevSubGroup);

        SendUpdate();
    }
}

uint32 Group::GetMaxSkillValueForGroup(SkillType skill)
{
    uint32 maxvalue = 0;

    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (!member)
            continue;

        uint32 value = member->GetSkillValue(skill);
        if (maxvalue < value)
            maxvalue = value;
    }

    return maxvalue;
}

void Group::UpdateLooterGuid(WorldObject* pSource, bool ifneed)
{
    switch (GetLootMethod())
    {
        case MASTER_LOOT:
        case FREE_FOR_ALL:
            return;
        default:
            // round robin style looting applies for all low
            // quality items in each loot method except free for all and master loot
            break;
    }

    member_citerator guid_itr = _getMemberCSlot(GetLooterGuid());
    if (guid_itr != m_memberSlots.end())
    {
        if (ifneed)
        {
            // not update if only update if need and ok
            Player* looter = ObjectAccessor::FindPlayer(guid_itr->guid);
            if (looter && looter->IsWithinDist(pSource, sWorld.getConfig(CONFIG_FLOAT_GROUP_XP_DISTANCE), false))
                return;
        }
        ++guid_itr;
    }

    // search next after current
    if (guid_itr != m_memberSlots.end())
    {
        for (member_citerator itr = guid_itr; itr != m_memberSlots.end(); ++itr)
        {
            if (Player* pl = ObjectAccessor::FindPlayer(itr->guid))
            {
                if (pl->IsWithinDist(pSource, sWorld.getConfig(CONFIG_FLOAT_GROUP_XP_DISTANCE), false))
                {
                    bool refresh = pl->GetLootGuid() == pSource->GetObjectGuid();

                    // if(refresh)                           // update loot for new looter
                    //    pl->GetSession()->DoLootRelease(pl->GetLootGUID());
                    SetLooterGuid(pl->GetObjectGuid());
                    SendUpdate();
                    if (refresh)                            // update loot for new looter
                        pl->SendLoot(pSource->GetObjectGuid(), LOOT_CORPSE);
                    return;
                }
            }
        }
    }

    // search from start
    for (member_citerator itr = m_memberSlots.begin(); itr != guid_itr; ++itr)
    {
        if (Player* pl = ObjectAccessor::FindPlayer(itr->guid))
        {
            if (pl->IsWithinDist(pSource, sWorld.getConfig(CONFIG_FLOAT_GROUP_XP_DISTANCE), false))
            {
                bool refresh = pl->GetLootGuid() == pSource->GetObjectGuid();

                // if(refresh)                               // update loot for new looter
                //    pl->GetSession()->DoLootRelease(pl->GetLootGUID());
                SetLooterGuid(pl->GetObjectGuid());
                SendUpdate();
                if (refresh)                                // update loot for new looter
                    pl->SendLoot(pSource->GetObjectGuid(), LOOT_CORPSE);
                return;
            }
        }
    }

    SetLooterGuid(ObjectGuid());
    SendUpdate();
}

GroupJoinBattlegroundResult Group::CanJoinBattleGroundQueue(BattleGround const* bgOrTemplate, BattleGroundQueueTypeId bgQueueTypeId, uint32 MinPlayerCount, uint32 MaxPlayerCount, bool isRated, uint32 arenaSlot)
{
    BattlemasterListEntry const* bgEntry = sBattlemasterListStore.LookupEntry(bgOrTemplate->GetTypeID());
    if (!bgEntry)
        return ERR_GROUP_JOIN_BATTLEGROUND_FAIL;            // shouldn't happen

    // check for min / max count
    uint32 memberscount = GetMembersCount();

    // only check for MinPlayerCount since MinPlayerCount == MaxPlayerCount for arenas...
    if (bgOrTemplate->isArena() && memberscount != MinPlayerCount)
        return ERR_ARENA_TEAM_PARTY_SIZE;

    if (memberscount > bgEntry->maxGroupSize)                // no MinPlayerCount for battlegrounds
        return ERR_BATTLEGROUND_NONE;                       // ERR_GROUP_JOIN_BATTLEGROUND_TOO_MANY handled on client side

    // get a player as reference, to compare other players' stats to (arena team id, queue id based on level, etc.)
    Player* reference = GetFirstMember()->getSource();
    // no reference found, can't join this way
    if (!reference)
        return ERR_BATTLEGROUND_JOIN_FAILED;

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgOrTemplate->GetMapId(), reference->getLevel());
    if (!bracketEntry)
        return ERR_BATTLEGROUND_JOIN_FAILED;

    uint32 arenaTeamId = reference->GetArenaTeamId(arenaSlot);
    Team team = reference->GetTeam();

    uint32 allowedPlayerCount = 0;

    BattleGroundQueueTypeId bgQueueTypeIdRandom = BattleGroundMgr::BGQueueTypeId(BATTLEGROUND_RB, ARENA_TYPE_NONE);

    // check every member of the group to be able to join
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* member = itr->getSource();
        // offline member? don't let join
        if (!member)
            return ERR_BATTLEGROUND_JOIN_FAILED;
        // don't allow cross-faction join as group
        if (member->GetTeam() != team)
            return ERR_BATTLEGROUND_JOIN_TIMED_OUT;
        // not in the same battleground level bracket, don't let join
        PvPDifficultyEntry const* memberBracketEntry = GetBattlegroundBracketByLevel(bracketEntry->mapId, member->getLevel());
        if (memberBracketEntry != bracketEntry)
            return ERR_BATTLEGROUND_JOIN_RANGE_INDEX;
        // don't let join rated matches if the arena team id doesn't match
        if (isRated && member->GetArenaTeamId(arenaSlot) != arenaTeamId)
            return ERR_BATTLEGROUND_JOIN_FAILED;
        // don't let join if someone from the group is already in that bg queue
        if (member->InBattleGroundQueueForBattleGroundQueueType(bgQueueTypeId))
            return ERR_BATTLEGROUND_JOIN_FAILED;            // not blizz-like
        // don't let join if someone from the group is in bg queue random
        if (member->InBattleGroundQueueForBattleGroundQueueType(bgQueueTypeIdRandom))
            return ERR_IN_RANDOM_BG;
        // don't let join to bg queue random if someone from the group is already in bg queue
        if (bgOrTemplate->GetTypeID() == BATTLEGROUND_RB && member->InBattleGroundQueue())
            return ERR_IN_NON_RANDOM_BG;
        // check for deserter debuff in case not arena queue
        if (bgOrTemplate->GetTypeID() != BATTLEGROUND_AA && !member->CanJoinToBattleground())
            return ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS;
        // check if member can join any more battleground queues
        if (!member->HasFreeBattleGroundQueueId())
            return ERR_BATTLEGROUND_TOO_MANY_QUEUES;        // not blizz-like

        ++allowedPlayerCount;
    }

    if (bgOrTemplate->isArena() && (allowedPlayerCount < MinPlayerCount || allowedPlayerCount > MaxPlayerCount))
        return ERR_ARENA_TEAM_PARTY_SIZE;

    return GroupJoinBattlegroundResult(bgOrTemplate->GetTypeID());
}

void Group::SetDungeonDifficulty(Difficulty difficulty)
{
    m_Difficulty = (m_Difficulty & 0xFF00) | uint32(difficulty);

    if (IsNeedSave())
    {
        static SqlStatementID updDiff;
        CharacterDatabase.CreateStatement(updDiff, "UPDATE groups SET difficulty = ? WHERE groupId = ?")
            .PExecute(uint8(GetDungeonDifficulty()), m_Guid.GetCounter());
    }

    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* player = itr->getSource();
        if (!player->GetSession() || player->getLevel() < LEVELREQUIREMENT_HEROIC)
            continue;

        player->SetDungeonDifficulty(difficulty);
        player->SendDungeonDifficulty(true);
    }
}

void Group::SetRaidDifficulty(Difficulty difficulty)
{
    m_Difficulty = (m_Difficulty & 0x00FF) | (uint32(difficulty) << 8);

    if (IsNeedSave())
    {
        static SqlStatementID updDiff;
        CharacterDatabase.CreateStatement(updDiff, "UPDATE groups SET raiddifficulty = ? WHERE groupId = ?")
            .PExecute(uint32(GetRaidDifficulty()), m_Guid.GetCounter());
    }

    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* player = itr->getSource();
        if (!player->GetSession() || player->getLevel() < LEVELREQUIREMENT_HEROIC)
            continue;

        player->SetRaidDifficulty(difficulty);
        player->SendRaidDifficulty(true);
    }
}

bool Group::InCombatToInstance(uint32 instanceId)
{
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* pPlayer = itr->getSource();
        if (!pPlayer || !pPlayer->IsInWorld())
            continue;

        if (pPlayer->GetMap() && pPlayer->GetInstanceId() == instanceId && pPlayer->IsInCombat())
            return true;
    }
    return false;
}

bool Group::SetPlayerMap(ObjectGuid guid, uint32 mapid)
{
    member_witerator slot = _getMemberWSlot(guid);
    if (slot != m_memberSlots.end())
    {
        slot->lastMap = mapid;
        DEBUG_LOG("Group::SetPlayerMap> map is updated");
        return true;
    }
    return false;
}

void Group::ResetInstances(InstanceResetMethod method, bool isRaid, Player* SendMsgTo)
{
    if (isBGGroup())
        return;

    // method can be INSTANCE_RESET_ALL, INSTANCE_RESET_CHANGE_DIFFICULTY, INSTANCE_RESET_GROUP_DISBAND

    // we assume that when the difficulty changes, all instances that can be reset will be
    Difficulty diff = GetDifficulty(isRaid);

    typedef std::set<uint32> OfflineMapSet;
    OfflineMapSet mapsWithOfflinePlayer;                    // to store map of offline players

    if (method != INSTANCE_RESET_GROUP_DISBAND)
    {
        // Store maps in which are offline members for instance reset check.
        for (member_citerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
        {
            if (!ObjectAccessor::FindPlayer(itr->guid))
                mapsWithOfflinePlayer.insert(itr->lastMap); // add last map from offline player
        }
    }

    for (BoundInstancesMap::iterator itr = m_boundInstances[diff].begin(); itr != m_boundInstances[diff].end();)
    {
        DungeonPersistentState* state = itr->second.state;
        const MapEntry* entry = sMapStore.LookupEntry(itr->first);
        if (!entry || entry->IsRaid() != isRaid || (!state->CanReset() && method != INSTANCE_RESET_GROUP_DISBAND))
        {
            ++itr;
            continue;
        }

        if (method == INSTANCE_RESET_ALL)
        {
            // the "reset all instances" method can only reset normal maps
            if (entry->map_type == MAP_RAID || diff == DUNGEON_DIFFICULTY_HEROIC)
            {
                ++itr;
                continue;
            }
        }

        bool isEmpty = true;

        // check if there are offline members on the map
        if (method != INSTANCE_RESET_GROUP_DISBAND && mapsWithOfflinePlayer.find(state->GetMapId()) != mapsWithOfflinePlayer.end())
            isEmpty = false;

        // if the map is loaded, reset it if can
        if (isEmpty && entry->IsDungeon() && !(method == INSTANCE_RESET_GROUP_DISBAND && !state->CanReset()))
            if (Map* map = sMapMgr.FindMap(state->GetMapId(), state->GetInstanceId()))
                isEmpty = ((DungeonMap*)map)->Reset(method);

        if (SendMsgTo)
        {
            uint32 mapId = state->GetMapId();

            if (!isEmpty)
                SendMsgTo->SendResetInstanceFailed(0, mapId);
            else
            {
                SendMsgTo->SendResetInstanceSuccess(mapId);

                if (sWorld.getConfig(CONFIG_BOOL_INSTANCES_RESET_GROUP_ANNOUNCE))
                {
                    if (Group* group = SendMsgTo->GetGroup())
                    {
                        for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
                        {
                            Player* pMember = itr->getSource();
                            if (pMember && pMember != SendMsgTo &&
                                pMember->GetSession() && pMember->IsInWorld())
                                pMember->SendResetInstanceSuccess(mapId);
                        }
                    }
                }
            }
        }

        // TODO - Adapt here when clear how difficulty changes must be handled
        if (isEmpty || method == INSTANCE_RESET_GROUP_DISBAND || method == INSTANCE_RESET_CHANGE_DIFFICULTY)
        {
            // do not reset the instance, just unbind if others are permanently bound to it
            if (state->CanReset())
                state->DeleteFromDB();
            else
            {
                static SqlStatementID delGroupInst;
                CharacterDatabase.CreateStatement(delGroupInst, "DELETE FROM group_instance WHERE instance = ?")
                    .PExecute(state->GetInstanceId());
            }

            // i don't know for sure if hash_map iterators
            m_boundInstances[diff].erase(itr);
            itr = m_boundInstances[diff].begin();
            // this unloads the instance save unless online players are bound to it
            // (eg. permanent binds or GM solo binds)
            state->RemoveFromBindList(GetObjectGuid());
        }
        else
            ++itr;
    }
}

InstanceGroupBind* Group::GetBoundInstance(uint32 mapid, Player* player)
{
    MapEntry const* mapEntry = sMapStore.LookupEntry(mapid);
    if (!mapEntry)
        return NULL;

    Difficulty difficulty = player->GetDifficulty(mapEntry->IsRaid());

    // some instances only have one difficulty
    MapDifficultyEntry const* mapDiff = GetMapDifficultyData(mapid, difficulty);
    if (!mapDiff)
        difficulty = DUNGEON_DIFFICULTY_NORMAL;

    BoundInstancesMap::iterator itr = m_boundInstances[difficulty].find(mapid);
    if (itr != m_boundInstances[difficulty].end())
        return &itr->second;
    else
        return NULL;
}

InstanceGroupBind* Group::GetBoundInstance(Map* aMap, Difficulty difficulty)
{
    // some instances only have one difficulty
    MapDifficultyEntry const* mapDiff = GetMapDifficultyData(aMap->GetId(),difficulty);
    if (!mapDiff)
        return NULL;

    BoundInstancesMap::iterator itr = m_boundInstances[difficulty].find(aMap->GetId());
    if (itr != m_boundInstances[difficulty].end())
        return &itr->second;
    else
        return NULL;
}

InstanceGroupBind* Group::BindToInstance(DungeonPersistentState *state, bool permanent, bool load)
{
    if (state && IsNeedSave())
    {
        InstanceGroupBind& bind = m_boundInstances[state->GetDifficulty()][state->GetMapId()];
        if (bind.state)
        {
            // when a boss is killed or when copying the players's binds to the group
            if (permanent != bind.perm || state != bind.state)
            {
                if (!load)
                {
                    static SqlStatementID updGroup;
                    CharacterDatabase.CreateStatement(updGroup, "UPDATE group_instance SET instance = ?, permanent = ? WHERE leaderGuid = ? AND instance = ?")
                        .PExecute(state->GetInstanceId(), permanent, GetLeaderGuid().GetCounter(), bind.state->GetInstanceId());
                }
            }
        }
        else if (!load)
        {
            static SqlStatementID insGroup;
            CharacterDatabase.CreateStatement(insGroup, "INSERT INTO group_instance (leaderGuid, instance, permanent) VALUES (?, ?, ?)")
                .PExecute(GetLeaderGuid().GetCounter(), state->GetInstanceId(), permanent);
        }

        if (bind.state != state)
        {
            if (bind.state)
                bind.state->RemoveFromBindList(GetObjectGuid());

            state->AddToBindList(GetObjectGuid());
        }

        bind.state = state;
        bind.perm = permanent;

        if (!load)
        {
            DEBUG_LOG("Group::BindToInstance: Group (Id: %d) is now bound to map %d, instance %d, difficulty %d",
                GetId(), state->GetMapId(), state->GetInstanceId(), state->GetDifficulty());
        }

        return &bind;
    }
    else
        return NULL;
}

void Group::UnbindInstance(uint32 mapid, uint8 difficulty, bool unload)
{
    BoundInstancesMap::iterator itr = m_boundInstances[difficulty].find(mapid);
    if (itr != m_boundInstances[difficulty].end())
    {
        if (!unload && IsNeedSave())
        {
            static SqlStatementID delGroupInst;
            CharacterDatabase.CreateStatement(delGroupInst, "DELETE FROM group_instance WHERE leaderGuid = ? AND instance = ?")
                .PExecute(GetLeaderGuid().GetCounter(), itr->second.state->GetInstanceId());
        }

        itr->second.state->RemoveFromBindList(GetObjectGuid());  // state can become invalid
        m_boundInstances[difficulty].erase(itr);
    }
}

void Group::_homebindIfInstance(Player* player)
{
    if (player && !player->isGameMaster())
    {
        Map* map = player->GetMap();
        if (map && map->IsDungeon())
        {
            // leaving the group in an instance, the homebind timer is started
            // unless the player is permanently saved to the instance
            InstancePlayerBind* playerBind = player->GetBoundInstance(map->GetId(), map->GetDifficulty());
            if (!playerBind || !playerBind->perm)
                player->m_InstanceValid = false;
        }
    }
}
//Frozen Mod
void Group::BroadcastGroupUpdate(void)
{
    for(member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
    {
        Player* pp = sObjectMgr.GetPlayer(citr->guid);
        if (pp && pp->IsInWorld())
        {
            pp->ForceValuesUpdateAtIndex(UNIT_FIELD_BYTES_2);
            pp->ForceValuesUpdateAtIndex(UNIT_FIELD_FACTIONTEMPLATE);
            DEBUG_LOG("-- Forced group value update for '%s'", pp->GetName());

            if (pp->GetPet())
            {
                GuidSet const& groupPets = pp->GetPets();
                if (!groupPets.empty())
                {
                     for (GuidSet::const_iterator itr = groupPets.begin(); itr != groupPets.end(); ++itr)
                     {
                         if (Pet* _pet = pp->GetMap()->GetPet(*itr))
                         {
                             _pet->ForceValuesUpdateAtIndex(UNIT_FIELD_BYTES_2);
                             _pet->ForceValuesUpdateAtIndex(UNIT_FIELD_FACTIONTEMPLATE);
                         }
                     }
                }
                DEBUG_LOG("-- Forced group value update for '%s' pet '%s'", pp->GetName(), pp->GetPet()->GetName());
            }

            for (uint32 i = 0; i < MAX_TOTEM_SLOT; ++i)
            {
                if (Unit* totem = pp->GetMap()->GetUnit(pp->GetTotemGuid(TotemSlot(i))))
                {
                    totem->ForceValuesUpdateAtIndex(UNIT_FIELD_BYTES_2);
                    totem->ForceValuesUpdateAtIndex(UNIT_FIELD_FACTIONTEMPLATE);
                    DEBUG_LOG("-- Forced group value update for '%s' totem #%u", pp->GetName(), i);
                }
            }
        }
    }
}
// Frozen Mod

static void RewardGroupAtKill_helper(Player* pGroupGuy, Unit* pVictim, uint32 count, bool PvP, float group_rate, uint32 sum_level, bool is_dungeon, Player* not_gray_member_with_max_level, Player* member_with_max_level, uint32 xp)
{
    // honor can be in PvP and !PvP (racial leader) cases (for alive)
    if (pGroupGuy->isAlive())
        pGroupGuy->RewardHonor(pVictim,count);

    // xp and reputation only in !PvP case
    if (!PvP)
    {
        float rate = group_rate * float(pGroupGuy->getLevel()) / sum_level;

        // if is in dungeon then all receive full reputation at kill
        // rewarded any alive/dead/near_corpse group member
        pGroupGuy->RewardReputation(pVictim, is_dungeon ? 1.0f : rate);

        // XP updated only for alive group member
        if (pGroupGuy->isAlive() && not_gray_member_with_max_level &&
            pGroupGuy->getLevel() <= not_gray_member_with_max_level->getLevel())
        {
            uint32 itr_xp = (member_with_max_level == not_gray_member_with_max_level) ? uint32(xp*rate) : uint32((xp*rate/2)+1);

            pGroupGuy->GiveXP(itr_xp, pVictim);
            if (Pet* pet = pGroupGuy->GetPet())
                pet->GivePetXP(itr_xp / 2);
        }

        // quest objectives updated only for alive group member or dead but with not released body
        if (pGroupGuy->isAlive()|| !pGroupGuy->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        {
            // normal creature (not pet/etc) can be only in !PvP case
            if (pVictim->GetTypeId() == TYPEID_UNIT)
            {
                if (CreatureInfo const* normalInfo = ObjectMgr::GetCreatureTemplate(pVictim->GetEntry()))
                    pGroupGuy->KilledMonster(normalInfo, pVictim->GetObjectGuid());
            }
        }
    }
}

/** Provide rewards to group members at unit kill
 *
 * @param pVictim       Killed unit
 * @param player_tap    Player who tap unit if online, it can be group member or can be not if leaved after tap but before kill target
 *
 * Rewards received by group members and player_tap
 */
void Group::RewardGroupAtKill(Unit* pVictim, Player* player_tap)
{
    bool PvP = pVictim->isCharmedOwnedByPlayerOrPlayer();

    // prepare data for near group iteration (PvP and !PvP cases)
    uint32 xp = 0;

    uint32 count = 0;
    uint32 sum_level = 0;
    Player* member_with_max_level = NULL;
    Player* not_gray_member_with_max_level = NULL;

    GetDataForXPAtKill(pVictim,count,sum_level,member_with_max_level,not_gray_member_with_max_level,player_tap);

    if (member_with_max_level)
    {
        /// not get Xp in PvP or no not gray players in group
        xp = (PvP || !not_gray_member_with_max_level) ? 0 : MaNGOS::XP::Gain(not_gray_member_with_max_level, pVictim);

        /// skip in check PvP case (for speed, not used)
        bool is_raid = PvP ? false : sMapStore.LookupEntry(pVictim->GetMapId())->IsRaid() && isRaidGroup();
        bool is_dungeon = PvP ? false : sMapStore.LookupEntry(pVictim->GetMapId())->IsDungeon();
        float group_rate = MaNGOS::XP::xp_in_group_rate(count,is_raid);

        for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
        {
            Player* pGroupGuy = itr->getSource();
            if (!pGroupGuy)
                continue;

            // will proccessed later
            if (pGroupGuy == player_tap)
                continue;

            if (!pGroupGuy->IsAtGroupRewardDistance(pVictim))
                continue;                               // member (alive or dead) or his corpse at req. distance

            if (pVictim->GetTypeId() == TYPEID_UNIT)
            {
                if (CreatureInfo const* normalInfo = ObjectMgr::GetCreatureTemplate(pVictim->GetEntry()))
                {
                    if (uint32 normalType = normalInfo->type)
                        pGroupGuy->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_KILL_CREATURE_TYPE, normalType, xp);
                }
            }

            RewardGroupAtKill_helper(pGroupGuy, pVictim, count, PvP, group_rate, sum_level, is_dungeon, not_gray_member_with_max_level, member_with_max_level, xp);
        }

        if (player_tap)
        {
            // member (alive or dead) or his corpse at req. distance
            if (player_tap->IsAtGroupRewardDistance(pVictim))
                RewardGroupAtKill_helper(player_tap, pVictim, count, PvP, group_rate, sum_level, is_dungeon, not_gray_member_with_max_level, member_with_max_level, xp);

            if (pVictim->GetTypeId() == TYPEID_UNIT)
            {
                if (CreatureInfo const* normalInfo = ObjectMgr::GetCreatureTemplate(pVictim->GetEntry()))
                {
                    if (uint32 normalType = normalInfo->type)
                        player_tap->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_KILL_CREATURE_TYPE, normalType, xp);
                }
            }
            player_tap->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_GET_KILLING_BLOWS, 1, 0, pVictim);
        }
    }
}

bool Group::ConvertToLFG(LFGType type)
{
    if (isBGGroup())
        return false;

    switch (type)
    {
        case LFG_TYPE_DUNGEON:
        case LFG_TYPE_QUEST:
        case LFG_TYPE_ZONE:
        case LFG_TYPE_HEROIC_DUNGEON:
            if (isRaidGroup())
                return false;
            m_groupType = GroupType(m_groupType | GROUPTYPE_LFD);
            break;
        case LFG_TYPE_RANDOM_DUNGEON:
            if (isRaidGroup())
                return false;
            m_groupType = GroupType(m_groupType | GROUPTYPE_LFD | GROUPTYPE_UNK1);
            break;
        case LFG_TYPE_RAID:
            if (!isRaidGroup())
                ConvertToRaid();
            m_groupType = GroupType(m_groupType | GROUPTYPE_LFD);
            break;
        default:
            return false;
    }

    m_lootMethod = NEED_BEFORE_GREED;
    SendUpdate();

    if (IsNeedSave())
    {
        static SqlStatementID updGroup;
        CharacterDatabase.CreateStatement(updGroup, "UPDATE groups SET groupType = ? WHERE groupId = ?")
            .PExecute(uint8(m_groupType), GetObjectGuid().GetCounter());
    }
    return true;
}

void Group::SetGroupRoles(ObjectGuid guid, LFGRoleMask roles)
{
    for (member_witerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
    {
        if (itr->guid == guid)
        {
            itr->roles = roles;

            if (IsNeedSave())
            {
                static SqlStatementID updGroupMember;
                CharacterDatabase.CreateStatement(updGroupMember, "UPDATE group_member SET roles = ? WHERE memberGuid = ?")
                    .PExecute(uint8(itr->roles), itr->guid.GetCounter());
            }

            SendUpdate();
            return;
        }
    }
}

LFGRoleMask Group::GetGroupRoles(ObjectGuid guid)
{
    for (member_witerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
    {
        if (itr->guid == guid)
            return itr->roles;
    }
    return LFG_ROLE_MASK_NONE;
}

bool Group::IsNeedSave() const
{
    if (GetGroupType() & GROUPTYPE_BG)
        return false;

    return m_bgGroup == NULL;
}
