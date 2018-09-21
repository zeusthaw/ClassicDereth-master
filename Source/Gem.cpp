
#include "StdAfx.h"
#include "Gem.h"
#include "UseManager.h"
#include "Player.h"
#include "SpellcastingManager.h"

CGemWeenie::CGemWeenie()
{
}

CGemWeenie::~CGemWeenie()
{
}

int CGemWeenie::Use(CPlayerWeenie *player)
{
	if (!player->FindContainedItem(GetID()))
	{
		player->NotifyUseDone(WERROR_NONE);
		return WERROR_NONE;
	}

	CGenericUseEvent *useEvent = new CGenericUseEvent;
	useEvent->_target_id = GetID();
	player->ExecuteUseEvent(useEvent);

	return WERROR_NONE;
}

int CGemWeenie::DoUseResponse(CWeenieObject *player)
{
	if (InqIntQuality(ITEM_TYPE_INT, 0) == TYPE_FOOD)
	{
		player->DoForcedMotion(Motion_Eat);
		player->DoForcedMotion(Motion_Ready);
	}

	if (DWORD spell_did = InqDIDQuality(SPELL_DID, 0))
	{
		if (cooldown < Timer::cur_time)
		{
			MakeSpellcastingManager()->CastSpellInstant(player->GetID(), spell_did);
			cooldown = Timer::cur_time + InqFloatQuality(COOLDOWN_DURATION_FLOAT, 0, FALSE);
		}
		else
			player->SendText("You can't do that yet!", LTT_ERROR);
	}

	DecrementStackOrStructureNum();

	return WERROR_NONE;
}