
#include "StdAfx.h"
#include "Gem.h"
#include "UseManager.h"
#include "Player.h"
#include "SpellcastingManager.h"
#include "World.h"

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

	if (InqDIDQuality(SPELL_DID, 0) && _nextUse > Timer::cur_time) // If this gem casts a spell and the _nextUse has not elapsed yet then cancel the use.
	{
		player->NotifyUseDone(WERROR_NONE);
		player->SendText("You can't do that yet!", LTT_ERROR);
		return WERROR_NONE;
	}

	if (InqBoolQuality(RARE_USES_TIMER_BOOL, 0) && player->_nextRareUse > Timer::cur_time) // If this is a rare gem and the _nextRareUse has not elapsed yet then cancel the use.
	{
		player->NotifyUseDone(WERROR_NONE);
		player->SendText("You can't do that yet!", LTT_ERROR); // TODO: is this the correct text? Was it sent to your window? Did it notify you how much time you had left?
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
		MakeSpellcastingManager()->CastSpellInstant(player->GetID(), spell_did);

		// If this is a rare gem then set _nextRareUse on the player and broadcast the use locally, otherwise set the _nextUse on the gem.
		if (InqBoolQuality(RARE_USES_TIMER_BOOL, 0))
		{
			player->AsPlayer()->_nextRareUse = Timer::cur_time + 180.0;
			std::string text = csprintf("%s used the rare item %s", player->GetName().c_str(), GetName().c_str());
			if (!text.empty())
			{
				g_pWorld->BroadcastLocal(player->GetLandcell(), text);
			}
		}
		else
			_nextUse = Timer::cur_time + InqFloatQuality(COOLDOWN_DURATION_FLOAT, 0, FALSE);
	}

	DecrementStackOrStructureNum();

	return WERROR_NONE;
}
