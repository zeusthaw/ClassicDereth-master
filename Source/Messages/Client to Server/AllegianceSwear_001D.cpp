#include "stdafx.h"
#include "AllegianceSwear_001D.h"
#include "AllegianceManager.h"
#include "World.h"

MAllegianceSwear_001D::MAllegianceSwear_001D(CPlayerWeenie * player)
{
	m_pPlayer = player;
}

void MAllegianceSwear_001D::Parse(BinaryReader * reader)
{
	m_dwTarget = reader->ReadDWORD();

	if (reader->GetLastError())
	{
		SERVER_ERROR << "Error parsing a swear allegiance message (0x001D) from the client.";
		return;
	}

	Process();
}

void MAllegianceSwear_001D::Process()
{
	if (CWeenieObject* target = g_pWorld->FindObject(m_dwTarget))
	{
		g_pAllegianceManager->TrySwearAllegiance(m_pPlayer, target);
		g_pAllegianceManager->SendAllegianceProfile(m_pPlayer);
	}
}
