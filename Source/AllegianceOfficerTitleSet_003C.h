#pragma once
#include "BinaryReader.h"
#include "IClientMessage.h"

// Allegiance_SetAllegianceOfficerTitle | 003C
// Sets a custom officer title for the given permission level
class MAllegianceOfficerTitleSet_003C : public IClientMessage
{
public:
	MAllegianceOfficerTitleSet_003C(CPlayerWeenie * player);
	void Parse(BinaryReader *reader);

private:
	void Process();
	CPlayerWeenie* m_pPlayer;
	DWORD m_OfficerLevel;
	std::string m_OfficerTitle;
};