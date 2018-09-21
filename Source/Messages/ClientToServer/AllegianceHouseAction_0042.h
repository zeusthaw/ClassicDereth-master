#pragma once
#include "BinaryReader.h"
#include "Messages\IClientMessage.h"
#include "Player.h"

// Allegiance_DoAllegianceHouseAction | 0042
// Performs an AllegianceHouseAction (Open/Close to guests, Open/Close storage)
class MAllegianceHouseAction_0042 : public IClientMessage
{
public:
	MAllegianceHouseAction_0042(CPlayerWeenie * player);
	void Parse(BinaryReader *reader);

private:
	void Process();
	CPlayerWeenie* m_pPlayer;
	DWORD m_dwHouseAction;
};