#pragma once
#include "BinaryReader.h"
#include "IClientMessage.h"
#include "Player.h"

// Allegiance_SwearAllegiance | 001D
// Player swears allegiance to another character
class MAllegianceSwear_001D : public IClientMessage
{
public:
	MAllegianceSwear_001D(CPlayerWeenie * player);
	void Parse(BinaryReader *reader);

private:
	void Process();
	CPlayerWeenie* m_pPlayer;
	DWORD m_dwTarget;
};