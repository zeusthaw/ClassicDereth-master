
#include "StdAfx.h"
#include "Corpse.h"
#include "DatabaseIO.h"
#include "WorldLandBlock.h"

#define CORPSE_EXIST_TIME 180

CCorpseWeenie::CCorpseWeenie()
{
	_begin_destroy_at = Timer::cur_time + CORPSE_EXIST_TIME;
}

CCorpseWeenie::~CCorpseWeenie()
{
}

void CCorpseWeenie::ApplyQualityOverrides()
{
	CContainerWeenie::ApplyQualityOverrides();
}

void CCorpseWeenie::SetObjDesc(const ObjDesc &desc)
{
	_objDesc = desc;
}

void CCorpseWeenie::GetObjDesc(ObjDesc &desc)
{
	desc = _objDesc;
}

int CCorpseWeenie::CheckOpenContainer(CWeenieObject *looter)
{
	int error = CContainerWeenie::CheckOpenContainer(looter);

	if (error != WERROR_NONE)
		return error;

	if (_begun_destroy)
		return WERROR_OBJECT_GONE;

	if (!_hasBeenOpened)
	{
		DWORD killerId = InqIIDQuality(KILLER_IID, 0);
		DWORD victimId = InqIIDQuality(VICTIM_IID, 0);
		if (killerId == looter->GetID() || victimId == looter->GetID())
			return WERROR_NONE;

		CPlayerWeenie *corpsePlayer = g_pWorld->FindPlayer(victimId);

		if (!corpsePlayer && _begin_destroy_at - (CORPSE_EXIST_TIME / 2) <= Timer::cur_time) // corpse isn't of a player so allow it to open after a certain time
		{
			return WERROR_NONE;
		}

		CPlayerWeenie *looterAsPlayer = looter->AsPlayer();
		bool killedByPK = m_Qualities.GetBool(PK_KILLER_BOOL, 0);

		if (corpsePlayer && !killedByPK && looterAsPlayer) // Make sure we're both players & don't let corpse permissions work on PK kills
		{
			if (!corpsePlayer->m_umCorpsePermissions.empty()) // if the corpse owner has players on their permissions list
			{
				if (!corpsePlayer->HasPermission(looterAsPlayer))
				{
					looter->SendText("You do not have permission to loot that corpse!", LTT_ERROR);
					return WERROR_FROZEN;
				}
				else
				{
					corpsePlayer->RemoveCorpsePermission(looterAsPlayer);
					looterAsPlayer->RemoveConsent(corpsePlayer);
					return WERROR_NONE;
				}
			}
		}

		if (Fellowship *fellowship = looter->GetFellowship())
		{
			if (!killedByPK)
			{
				if (fellowship->_share_loot)
				{
					for (auto &entry : fellowship->_fellowship_table)
					{
						if (killerId == entry.first)
							return WERROR_NONE;
					}
				}
			}
		}

		if (corpsePlayer != looterAsPlayer)
		{
			looter->SendText("You do not have permission to loot that corpse!", LTT_ERROR);
			return WERROR_FROZEN;
		}

		if (corpsePlayer)
		{
			return WERROR_NONE;
		}

		looter->SendText("You do not have permission to loot that corpse!", LTT_ERROR);
		return WERROR_FROZEN;
	}

	return WERROR_NONE;
}

void CCorpseWeenie::OnContainerOpened(CWeenieObject *other)
{
	CContainerWeenie::OnContainerOpened(other);

	_hasBeenOpened = true;
}

void CCorpseWeenie::OnContainerClosed(CWeenieObject *other)
{
	CContainerWeenie::OnContainerClosed(other);

	if (!m_Items.size() && !m_Packs.size())
	{
		BeginGracefulDestroy();
	}
}

void CCorpseWeenie::SaveEx(class CWeenieSave &save)
{
	m_Qualities.SetFloat(TIME_TO_ROT_FLOAT, _begin_destroy_at - Timer::cur_time);
	CContainerWeenie::SaveEx(save);

	save.m_ObjDesc = _objDesc;

	g_pDBIO->AddOrUpdateWeenieToBlock(GetID(), m_Position.objcell_id >> 16);
}

void CCorpseWeenie::RemoveEx()
{
	g_pDBIO->RemoveWeenieFromBlock(GetID());
}

void CCorpseWeenie::LoadEx(class CWeenieSave &save)
{
	CContainerWeenie::LoadEx(save);

	_objDesc = save.m_ObjDesc;
	_begin_destroy_at = Timer::cur_time + m_Qualities.GetFloat(TIME_TO_ROT_FLOAT, 0.0);
	_shouldSave = true;
	m_bDontClear = true;

	InitPhysicsObj();
	MakeMovementManager(TRUE);
	MovementParameters params;
	params.autonomous = 0;
	last_move_was_autonomous = false;
	DoMotion(GetCommandID(17), &params, 0);
}

bool CCorpseWeenie::ShouldSave()
{
	return _shouldSave;
}

void CCorpseWeenie::Tick()
{
	CContainerWeenie::Tick();

	if (!_begun_destroy)
	{
		if (!_openedById && _begin_destroy_at <= Timer::cur_time)
		{
			BeginGracefulDestroy();
		}
	}
	else
	{
		if (_mark_for_destroy_at <= Timer::cur_time)
		{
			MarkForDestroy();
		}
	}
}

void CCorpseWeenie::BeginGracefulDestroy()
{
	if (_begun_destroy)
	{
		return;
	}

	EmitEffect(PS_Destroy, 1.0f);

	// TODO drop inventory items on the ground

	_shouldSave = false; //we're on our way out, it's no longer necessary to save us to the database.
	RemoveEx(); // and in fact, delete entries in the db

	_mark_for_destroy_at = Timer::cur_time + 2.0;
	_begun_destroy = true;
}


