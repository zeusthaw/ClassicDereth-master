
#include "StdAfx.h"

#include "Client.h"
#include "ClientCommands.h"
#include "ClientEvents.h"
#include "World.h"
#include <chrono>

#include "Database.h"
#include "DatabaseIO.h"
#include "Database2.h"

#include "ChatMsgs.h"
#include "ObjectMsgs.h"

#include "WeenieObject.h"
#include "Monster.h"
#include "Player.h"
#include "ChatMsgs.h"
#include "Movement.h"
#include "MovementManager.h"
#include "Vendor.h"
#include "AllegianceManager.h"
#include "House.h"
#include "SpellcastingManager.h"
#include "TradeManager.h"
#include <chrono>

#include "Config.h"

CClientEvents::CClientEvents(CClient *parent)
{
	m_pClient = parent;
	m_pPlayer = NULL;
}

CClientEvents::~CClientEvents()
{
	if (m_pPlayer)
	{
		m_pPlayer->BeginLogout();
		m_pPlayer->DetachClient();
		m_pPlayer = NULL;
	}
}

void CClientEvents::DetachPlayer()
{
	m_pPlayer = NULL;
}

DWORD CClientEvents::GetPlayerID()
{
	if (!m_pPlayer)
		return 0;

	return m_pPlayer->GetID();
}

CPlayerWeenie *CClientEvents::GetPlayer()
{
	return m_pPlayer;
}

void CClientEvents::ExitWorld()
{
	DetachPlayer();
	m_pClient->ExitWorld();
}

void CClientEvents::Think()
{
	if (m_pPlayer)
	{
		if (m_bSendAllegianceUpdates)
		{
			if (m_fNextAllegianceUpdate <= g_pGlobals->Time())
			{
				SendAllegianceUpdate();
				m_fNextAllegianceUpdate = g_pGlobals->Time() + 5.0f;

				if (!m_bSentFirstAllegianceUpdate)
				{
					m_bSendAllegianceUpdates = FALSE;
					m_bSentFirstAllegianceUpdate = TRUE;
				}
			}
		}
	}
}

void CClientEvents::BeginLogout()
{
	if (m_pPlayer && !m_pPlayer->IsLoggingOut())
	{
		if (m_pPlayer->IsBusyOrInAction())
		{
			m_pPlayer->NotifyWeenieError(WERROR_ACTIONS_LOCKED);
			return;
		}

		m_pPlayer->BeginLogout();
	}
}

// Merged from GDLE2 Team https://gitlab.com/Scribble/gdlenhanced/commit/3cf018646aa989931b63beb0781d4e1a62339746 //
void CClientEvents::ForceLogout()
{
	if (m_pPlayer && !m_pPlayer->IsLoggingOut())
	{
		m_pPlayer->BeginLogout();
	}
}


void CClientEvents::OnLogoutCompleted()
{
	ExitWorld();
}

void CClientEvents::LoginError(int iError)
{
	DWORD ErrorPackage[2];

	ErrorPackage[0] = 0xF659;
	ErrorPackage[1] = iError;

	m_pClient->SendNetMessage(ErrorPackage, sizeof(ErrorPackage), PRIVATE_MSG);
}

void CClientEvents::LoginCharacter(DWORD char_weenie_id, const char *szAccount)
{
	if (!m_pClient->HasCharacter(char_weenie_id))
	{
		LoginError(13); // update error codes
		SERVER_WARN << szAccount << "Logging in with a character that doesn't belong to this account!\n";
		return;
	}

	if (m_pPlayer || g_pWorld->FindPlayer(char_weenie_id))
	{
		// LOG(Temp, Normal, "Character already logged in!\n");
		LoginError(13); // update error codes
		SERVER_WARN << szAccount << "Login request, but character already logged in!\n";
		return;
	}

	/*
	if (_stricmp(szAccount, m_pClient->GetAccountInfo().username))
	{
		LoginError(15);
		LOG(Client, Warning, "Bad account for login: \"%s\" \"%s\"\n", szAccount, m_pClient->GetAccountInfo().username);
		return;
	}
	*/
	
	m_pPlayer = new CPlayerWeenie(m_pClient, char_weenie_id, m_pClient->IncCharacterInstanceTS(char_weenie_id));
	
	if (!m_pPlayer->Load())
	{
		LoginError(13); // update error codes
		SERVER_WARN << szAccount << "Login request, but character failed to load!\n";

		delete m_pPlayer;

		return;
	}

	m_pPlayer->SetLoginPlayerQualities(); // overrides
	m_pPlayer->RecalculateEncumbrance();
	m_pPlayer->LoginCharacter();
	
	last_age_update = chrono::system_clock::to_time_t(chrono::system_clock::now());

	// give characters created before creation timestamp was being set a timestamp and DOB from their DB date_created
	if (!m_pPlayer->m_Qualities.GetInt(CREATION_TIMESTAMP_INT, 0))
	{
		CharacterDesc_t char_info = g_pDBIO->GetCharacterInfo(m_pPlayer->GetID());
		if (char_info.date_created) // check that the query got info, will be 0 if it didn't
		{
			time_t t = char_info.date_created;
			m_pPlayer->m_Qualities.SetInt(CREATION_TIMESTAMP_INT, t);
			m_pPlayer->NotifyIntStatUpdated(CREATION_TIMESTAMP_INT);

			std::stringstream ss;
			ss << "You were born on " << std::put_time(std::localtime(&t), "%m/%d/%y %I:%M:%S %p."); // convert time to a string of format '01/01/18 11:59:59 AM.'
			m_pPlayer->m_Qualities.SetString(DATE_OF_BIRTH_STRING, ss.str());
			m_pPlayer->NotifyStringStatUpdated(DATE_OF_BIRTH_STRING);
		}
	}

	if (!m_pPlayer->m_Qualities.GetInt(CREATION_TIMESTAMP_INT, 0))
	{
		m_pPlayer->m_Qualities.SetFloat(GLOBAL_XP_MOD_FLOAT, 1.0);
		m_pPlayer->NotifyFloatStatUpdated(GLOBAL_XP_MOD_FLOAT);
	}


	last_age_update = chrono::system_clock::to_time_t(chrono::system_clock::now());

	//temporarily send a purge all enchantments packet on login to wipe stacked characters.
	if (g_pConfig->SpellPurgeOnLogin())
	{
		if (m_pPlayer->m_Qualities._enchantment_reg)
		{
			PackableList<DWORD> removed;

			if (m_pPlayer->m_Qualities._enchantment_reg->_add_list)
			{
				for (auto it = m_pPlayer->m_Qualities._enchantment_reg->_add_list->begin(); it != m_pPlayer->m_Qualities._enchantment_reg->_add_list->end();)
				{
					if (it->_duration == -1.0)
					{
						removed.push_back(it->_id);
						it = m_pPlayer->m_Qualities._enchantment_reg->_add_list->erase(it);
					}
					else
					{
						it++;
					}
				}
			}

			if (m_pPlayer->m_Qualities._enchantment_reg->_mult_list)
			{
				for (auto it = m_pPlayer->m_Qualities._enchantment_reg->_mult_list->begin(); it != m_pPlayer->m_Qualities._enchantment_reg->_mult_list->end();)
				{
					if (it->_duration == -1.0)
					{
						removed.push_back(it->_id);
						it = m_pPlayer->m_Qualities._enchantment_reg->_mult_list->erase(it);
					}
					else
					{
						it++;
					}
				}
			}

			if (removed.size())
			{
				// m_Qualities._enchantment_reg->PurgeEnchantments();

				BinaryWriter expireMessage;
				expireMessage.Write<DWORD>(0x2C8);
				removed.Pack(&expireMessage);

				m_pClient->SendNetMessage(&expireMessage, PRIVATE_MSG, TRUE, FALSE);
			}
		}
	}

	for (auto wielded : m_pPlayer->m_Wielded) 
	{

		// Updates shields to have SHIELD_VALUE_INT
		if (wielded->InqIntQuality(ITEM_TYPE_INT, 0) == TYPE_ARMOR && wielded->InqIntQuality(LOCATIONS_INT, 0) == SHIELD_LOC)
		{
			wielded->m_Qualities.SetInt(SHIELD_VALUE_INT, wielded->InqIntQuality(ARMOR_LEVEL_INT, 0));
		}

		// Weeping wand nerf
		if (wielded->m_Qualities.m_WeenieType == 35 && wielded->InqStringQuality(NAME_STRING, "") == "Weeping Wand"
			&& (wielded->InqDIDQuality(SPELL_DID, 0) > 0 || wielded->InqFloatQuality(SLAYER_DAMAGE_BONUS_FLOAT, 0) != 1.4))
		{
			wielded->m_Qualities.RemoveDataID(SPELL_DID);
			wielded->m_Qualities.SetFloat(SLAYER_DAMAGE_BONUS_FLOAT, 1.4);
		}





		//Elemental Yumi Bow Fix
		DWORD yumiSlash;
		DWORD yumiPierce;
		DWORD yumiBludge;
		DWORD yumiAcid;
		DWORD yumiElectric;
		DWORD yumiFrost;
		DWORD yumiFlame;
		if (wielded->m_Qualities.InqDataID(SETUP_DID, yumiSlash) && yumiSlash == 33559028)
		{
			//wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060AC);//Compound Bow Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Slicing Yumi");//Re-Write the name to match Item//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_SLASHING);//Elemental UI Effect//
			wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, SLASH_DAMAGE_TYPE);


		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, yumiPierce) && yumiPierce == 33559027)
		{
			//wielded->m_Qualities.SetDataID(ICON_DID, 100677125);//Compound Bow Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Piercing Yumi");//Re-Write the name to match Item//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_PIERCING);//Elemental UI Effect//
			wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, PIERCE_DAMAGE_TYPE);


		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, yumiBludge) && yumiBludge == 33559030)
		{
			//wielded->m_Qualities.SetDataID(ICON_DID, 100677123);//Compound Bow Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Smashing Yumi");//Re-Write the name to match Item//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_BLUDGEONING);//Elemental UI Effect//
			wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, BLUDGEON_DAMAGE_TYPE);

		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, yumiAcid) && yumiAcid == 33559029)
		{
			//wielded->m_Qualities.SetDataID(ICON_DID, 100677121);//Compound Bow Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Stinging Yumi");//Re-Write the name to match Item//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
			wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, ACID_DAMAGE_TYPE);

		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, yumiElectric) && yumiElectric == 33559031)
		{
			//wielded->m_Qualities.SetDataID(ICON_DID, 100677118);//Compound Bow Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Sparking Yumi");//Re-Write the name to match Item//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Elemental UI Effect//
			wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, ELECTRIC_DAMAGE_TYPE);

		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, yumiFrost) && yumiFrost == 33559026)
		{
			//wielded->m_Qualities.SetDataID(ICON_DID, 100677119);//Compound Bow Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Freezing Yumi");//Re-Write the name to match Item//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
			wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, COLD_DAMAGE_TYPE);

		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, yumiFlame) && yumiFlame == 33559025)
		{
			//wielded->m_Qualities.SetDataID(ICON_DID, 100677122);//Compound Bow Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Smoldering Yumi");//Re-Write the name to match Item//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
			wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, FIRE_DAMAGE_TYPE);

		}

		//Compound Bow Fix
		DWORD compoundBow;
		DWORD compoundBowSlash;
		DWORD compoundBowPierce;
		DWORD compoundBowBludge;
		DWORD compoundBowAcid;
		DWORD compoundBowElectric;
		DWORD compoundBowFrost;
		DWORD compoundBowFlame;
		if (wielded->m_Qualities.InqDataID(SETUP_DID, compoundBow) && compoundBow == 0x0200144E)
		{
			wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060AC);//Compound Bow Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Compound Bow");//Re-Write the name to match Item//

		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, compoundBowSlash) && compoundBowSlash == 0x02001488)
		{
			wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060AC);//Compound Bow Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Slicing Compound Bow");//Re-Write the name to match Item//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_SLASHING);//Elemental UI Effect//
			wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, SLASH_DAMAGE_TYPE);


		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, compoundBowPierce) && compoundBowPierce == 0x0200148A)
		{
			wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060AD);//Compound Bow Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Piercing Compound Bow");//Re-Write the name to match Item//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_PIERCING);//Elemental UI Effect//
			wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, PIERCE_DAMAGE_TYPE);


		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, compoundBowBludge) && compoundBowBludge == 0x02001489)
		{
			wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060B1);//Compound Bow Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Smashing Compound Bow");//Re-Write the name to match Item//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_BLUDGEONING);//Elemental UI Effect//
			wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, BLUDGEON_DAMAGE_TYPE);

		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, compoundBowAcid) && compoundBowAcid == 0x02001475)
		{
			wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060AE);//Compound Bow Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Corrosive Compound Bow");//Re-Write the name to match Item//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
			wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, ACID_DAMAGE_TYPE);

		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, compoundBowElectric) && compoundBowElectric == 0x02001472)
		{
			wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060AF);//Compound Bow Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Sparking Compound Bow");//Re-Write the name to match Item//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Elemental UI Effect//
			wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, ELECTRIC_DAMAGE_TYPE);

		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, compoundBowFrost) && compoundBowFrost == 0x02001473)
		{
			wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060AA);//Compound Bow Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Chilling Compound Bow");//Re-Write the name to match Item//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
			wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, COLD_DAMAGE_TYPE);

		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, compoundBowFlame) && compoundBowFlame == 0x02001474)
		{
			wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060B0);//Compound Bow Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Searing Compound Bow");//Re-Write the name to match Item//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
			wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, FIRE_DAMAGE_TYPE);

		}


		//Spadone Fix
		DWORD spadonePhysical;
		DWORD spadoneElectric;
		DWORD spadoneAcid;
		DWORD spadoneFrost;
		DWORD spadoneFlame;
		if (wielded->m_Qualities.InqDataID(SETUP_DID, spadonePhysical) && spadonePhysical == 0x0200130B)
		{
			wielded->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//Spadone Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Spadone");//Re-Write the name to match Item//

		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, spadoneElectric) && spadoneElectric == 0x02001892)
		{
			wielded->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//Spadone Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Spadone Pallete//
			wielded->m_Qualities.SetString(NAME_STRING, "Electric Spadone");//Re-Write the name to match Item//
		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, spadoneAcid) && spadoneAcid == 0x02001891)
		{
			wielded->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//Spadone Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
			wielded->m_Qualities.SetString(NAME_STRING, "Acid Spadone");//Re-Write the name to match Item//
		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, spadoneFrost) && spadoneFrost == 0x02001890)
		{
			wielded->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//Spadone Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
			wielded->m_Qualities.SetString(NAME_STRING, "Frost Spadone");//Re-Write the name to match Item//
		}
		if (wielded->m_Qualities.InqDataID(SETUP_DID, spadoneFlame) && spadoneFlame == 0x0200188F)
		{
			wielded->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//Spadone Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
			wielded->m_Qualities.SetString(NAME_STRING, "Flame Spadone");//Re-Write the name to match Item//
		}

		//  Elemental Staff Fix  //
		int staffSlash;
		int staffPierce;
		int staffBludge;
		int staffAcid;
		int staffElectric;
		int staffFrost;
		int staffFlame;

		if (wielded->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffSlash) && staffSlash == SLASH_DAMAGE_TYPE && wielded->InqStringQuality(NAME_STRING, "") == "Staff")
		{
			//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_SLASHING);//Elemental UI Effect//
			wielded->m_Qualities.SetString(NAME_STRING, "Slashing Staff");//Re-Write the name to match Item//
			wielded->m_Qualities.SetDataID(SETUP_DID, 0x02001850);

		}
		if (wielded->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffPierce) && staffPierce == PIERCE_DAMAGE_TYPE && wielded->InqStringQuality(NAME_STRING, "") == "Staff")
		{
			//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_PIERCING);//Elemental UI Effect//
			wielded->m_Qualities.SetString(NAME_STRING, "Piercing Staff");//Re-Write the name to match Item//
			wielded->m_Qualities.SetDataID(SETUP_DID, 0x0200184F);
		}
		if (wielded->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffBludge) && staffBludge == BLUDGEON_DAMAGE_TYPE && wielded->InqStringQuality(NAME_STRING, "") == "Staff")
		{
			//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_BLUDGEONING);//Elemental UI Effect//
			wielded->m_Qualities.SetString(NAME_STRING, "Bludgeoning Staff");//Re-Write the name to match Item//
			wielded->m_Qualities.SetDataID(SETUP_DID, 0x0200184B);
		}
		if (wielded->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffAcid) && staffAcid == ACID_DAMAGE_TYPE && wielded->InqStringQuality(NAME_STRING, "") == "Staff")
		{
			//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
			wielded->m_Qualities.SetString(NAME_STRING, "Acid Staff");//Re-Write the name to match Item//
			wielded->m_Qualities.SetDataID(SETUP_DID, 0x0200184A);
		}
		if (wielded->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffElectric) && staffElectric == ELECTRIC_DAMAGE_TYPE && wielded->InqStringQuality(NAME_STRING, "") == "Staff")
		{
			//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Elemental UI Effect//
			wielded->m_Qualities.SetString(NAME_STRING, "Electric Staff");//Re-Write the name to match Item//
			wielded->m_Qualities.SetDataID(SETUP_DID, 0x0200184C);
		}
		if (wielded->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffFrost) && staffFrost == COLD_DAMAGE_TYPE && wielded->InqStringQuality(NAME_STRING, "") == "Staff")
		{
			//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
			wielded->m_Qualities.SetString(NAME_STRING, "Frost Staff");//Re-Write the name to match Item//
			wielded->m_Qualities.SetDataID(SETUP_DID, 0x0200184E);
		}
		if (wielded->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffFlame) && staffFlame == FIRE_DAMAGE_TYPE && wielded->InqStringQuality(NAME_STRING, "") == "Staff")
		{
			//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
			wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
			wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
			wielded->m_Qualities.SetString(NAME_STRING, "Fire Staff");//Re-Write the name to match Item//
			wielded->m_Qualities.SetDataID(SETUP_DID, 0x0200184D);
		}

	}

	for (auto item : m_pPlayer->m_Items)
	{
		// Updates shields to have SHIELD_VALUE_INT
		if (item->InqIntQuality(ITEM_TYPE_INT, 0) == TYPE_ARMOR && item->InqIntQuality(LOCATIONS_INT, 0) == SHIELD_LOC)
		{
			item->m_Qualities.SetInt(SHIELD_VALUE_INT, item->InqIntQuality(ARMOR_LEVEL_INT, 0));
		}

		// Weeping wand nerf
		if (item->m_Qualities.m_WeenieType == 35 && item->InqStringQuality(NAME_STRING, "") == "Weeping Wand"
			&& (item->InqDIDQuality(SPELL_DID, 0) > 0 || item->InqFloatQuality(SLAYER_DAMAGE_BONUS_FLOAT, 0) != 1.4))
		{
			item->m_Qualities.RemoveDataID(SPELL_DID);
			item->m_Qualities.SetFloat(SLAYER_DAMAGE_BONUS_FLOAT, 1.4);
		}


		//Elemental Yumi Bow Fix
		DWORD yumiSlash;
		DWORD yumiPierce;
		DWORD yumiBludge;
		DWORD yumiAcid;
		DWORD yumiElectric;
		DWORD yumiFrost;
		DWORD yumiFlame;
		if (item->m_Qualities.InqDataID(SETUP_DID, yumiSlash) && yumiSlash == 33559028)
		{
			item->m_Qualities.SetDataID(ICON_DID, 0x0060060AC);//Compound Bow Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Slicing Yumi");//Re-Write the name to match Item//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_SLASHING);//Elemental UI Effect//
			item->m_Qualities.SetInt(DAMAGE_TYPE_INT, SLASH_DAMAGE_TYPE);


		}
		if (item->m_Qualities.InqDataID(SETUP_DID, yumiPierce) && yumiPierce == 33559027)
		{
			item->m_Qualities.SetDataID(ICON_DID, 100677125);//Compound Bow Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Piercing Yumi");//Re-Write the name to match Item//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_PIERCING);//Elemental UI Effect//
			item->m_Qualities.SetInt(DAMAGE_TYPE_INT, PIERCE_DAMAGE_TYPE);


		}
		if (item->m_Qualities.InqDataID(SETUP_DID, yumiBludge) && yumiBludge == 33559030)
		{
			item->m_Qualities.SetDataID(ICON_DID, 100677123);//Compound Bow Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Smashing Yumi");//Re-Write the name to match Item//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_BLUDGEONING);//Elemental UI Effect//
			item->m_Qualities.SetInt(DAMAGE_TYPE_INT, BLUDGEON_DAMAGE_TYPE);

		}
		if (item->m_Qualities.InqDataID(SETUP_DID, yumiAcid) && yumiAcid == 33559029)
		{
			item->m_Qualities.SetDataID(ICON_DID, 100677121);//Compound Bow Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Stinging Yumi");//Re-Write the name to match Item//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
			item->m_Qualities.SetInt(DAMAGE_TYPE_INT, ACID_DAMAGE_TYPE);

		}
		if (item->m_Qualities.InqDataID(SETUP_DID, yumiElectric) && yumiElectric == 33559031)
		{
			item->m_Qualities.SetDataID(ICON_DID, 100677118);//Compound Bow Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Sparking Yumi");//Re-Write the name to match Item//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Elemental UI Effect//
			item->m_Qualities.SetInt(DAMAGE_TYPE_INT, ELECTRIC_DAMAGE_TYPE);

		}
		if (item->m_Qualities.InqDataID(SETUP_DID, yumiFrost) && yumiFrost == 33559026)
		{
			item->m_Qualities.SetDataID(ICON_DID, 100677119);//Compound Bow Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Freezing Yumi");//Re-Write the name to match Item//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
			item->m_Qualities.SetInt(DAMAGE_TYPE_INT, COLD_DAMAGE_TYPE);

		}
		if (item->m_Qualities.InqDataID(SETUP_DID, yumiFlame) && yumiFlame == 33559025)
		{
			item->m_Qualities.SetDataID(ICON_DID, 100677122);//Compound Bow Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Smoldering Yumi");//Re-Write the name to match Item//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
			item->m_Qualities.SetInt(DAMAGE_TYPE_INT, FIRE_DAMAGE_TYPE);

		}

		//Compound Bow Fix
		DWORD compoundBow;
		DWORD compoundBowSlash;
		DWORD compoundBowPierce;
		DWORD compoundBowBludge;
		DWORD compoundBowAcid;
		DWORD compoundBowElectric;
		DWORD compoundBowFrost;
		DWORD compoundBowFlame;
		if (item->m_Qualities.InqDataID(SETUP_DID, compoundBow) && compoundBow == 0x0200144E)
		{
			item->m_Qualities.SetDataID(ICON_DID, 0x0060060AC);//Compound Bow Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Compound Bow");//Re-Write the name to match Item//

		}
		if (item->m_Qualities.InqDataID(SETUP_DID, compoundBowSlash) && compoundBowSlash == 0x02001488)
		{
			item->m_Qualities.SetDataID(ICON_DID, 0x0060060AC);//Compound Bow Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Slicing Compound Bow");//Re-Write the name to match Item//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_SLASHING);//Elemental UI Effect//
			item->m_Qualities.SetInt(DAMAGE_TYPE_INT, SLASH_DAMAGE_TYPE);


		}
		if (item->m_Qualities.InqDataID(SETUP_DID, compoundBowPierce) && compoundBowPierce == 0x0200148A)
		{
			item->m_Qualities.SetDataID(ICON_DID, 0x0060060AD);//Compound Bow Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Piercing Compound Bow");//Re-Write the name to match Item//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_PIERCING);//Elemental UI Effect//
			item->m_Qualities.SetInt(DAMAGE_TYPE_INT, PIERCE_DAMAGE_TYPE);


		}
		if (item->m_Qualities.InqDataID(SETUP_DID, compoundBowBludge) && compoundBowBludge == 0x02001489)
		{
			item->m_Qualities.SetDataID(ICON_DID, 0x0060060B1);//Compound Bow Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Smashing Compound Bow");//Re-Write the name to match Item//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_BLUDGEONING);//Elemental UI Effect//
			item->m_Qualities.SetInt(DAMAGE_TYPE_INT, BLUDGEON_DAMAGE_TYPE);

		}
		if (item->m_Qualities.InqDataID(SETUP_DID, compoundBowAcid) && compoundBowAcid == 0x02001475)
		{
			item->m_Qualities.SetDataID(ICON_DID, 0x0060060AE);//Compound Bow Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Corrosive Compound Bow");//Re-Write the name to match Item//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
			item->m_Qualities.SetInt(DAMAGE_TYPE_INT, ACID_DAMAGE_TYPE);

		}
		if (item->m_Qualities.InqDataID(SETUP_DID, compoundBowElectric) && compoundBowElectric == 0x02001472)
		{
			item->m_Qualities.SetDataID(ICON_DID, 0x0060060AF);//Compound Bow Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Sparking Compound Bow");//Re-Write the name to match Item//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Elemental UI Effect//
			item->m_Qualities.SetInt(DAMAGE_TYPE_INT, ELECTRIC_DAMAGE_TYPE);

		}
		if (item->m_Qualities.InqDataID(SETUP_DID, compoundBowFrost) && compoundBowFrost == 0x02001473)
		{
			item->m_Qualities.SetDataID(ICON_DID, 0x0060060AA);//Compound Bow Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Chilling Compound Bow");//Re-Write the name to match Item//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
			item->m_Qualities.SetInt(DAMAGE_TYPE_INT, COLD_DAMAGE_TYPE);

		}
		if (item->m_Qualities.InqDataID(SETUP_DID, compoundBowFlame) && compoundBowFlame == 0x02001474)
		{
			item->m_Qualities.SetDataID(ICON_DID, 0x0060060B0);//Compound Bow Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Searing Compound Bow");//Re-Write the name to match Item//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
			item->m_Qualities.SetInt(DAMAGE_TYPE_INT, FIRE_DAMAGE_TYPE);

		}

		//Spadone Fix//
		DWORD spadonePhysical; 
		DWORD spadoneElectric; 
		DWORD spadoneAcid;  
		DWORD spadoneFrost;
		DWORD spadoneFlame;
		if (item->m_Qualities.InqDataID(SETUP_DID, spadonePhysical) && spadonePhysical == 0x0200130B)
		{
			item->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//Spadone Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Spadone");//Re-Write the name to match Item//

		}
		if (item->m_Qualities.InqDataID(SETUP_DID, spadoneElectric) && spadoneElectric == 0x02001892)
		{
			item->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//Spadone Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Spadone Pallete//
			item->m_Qualities.SetString(NAME_STRING, "Electric Spadone");//Re-Write the name to match Item//
		}
		if (item->m_Qualities.InqDataID(SETUP_DID, spadoneAcid) && spadoneAcid == 0x02001891)
		{
			item->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//Spadone Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
			item->m_Qualities.SetString(NAME_STRING, "Acid Spadone");//Re-Write the name to match Item//
		}
		if (item->m_Qualities.InqDataID(SETUP_DID, spadoneFrost) && spadoneFrost == 0x02001890)
		{
			item->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//Spadone Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
			item->m_Qualities.SetString(NAME_STRING, "Frost Spadone");//Re-Write the name to match Item//
		}
		if (item->m_Qualities.InqDataID(SETUP_DID, spadoneFlame) && spadoneFlame == 0x0200188F)
		{
			item->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//Spadone Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
			item->m_Qualities.SetString(NAME_STRING, "Flame Spadone");//Re-Write the name to match Item//
		}

		//  Elemental Staff Fix  //
		int staffSlash;
		int staffPierce;
		int staffBludge;
		int staffAcid;
		int staffElectric;
		int staffFrost;
		int staffFlame;

		if (item->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffSlash) && staffSlash == SLASH_DAMAGE_TYPE && item->InqStringQuality(NAME_STRING, "") == "Staff")
		{
			//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_SLASHING);//Elemental UI Effect//
			item->m_Qualities.SetString(NAME_STRING, "Slashing Staff");//Re-Write the name to match Item//
			item->m_Qualities.SetDataID(SETUP_DID, 0x02001850);

		}
		if (item->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffPierce) && staffPierce == PIERCE_DAMAGE_TYPE && item->InqStringQuality(NAME_STRING, "") == "Staff")
		{
			//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_PIERCING);//Elemental UI Effect//
			item->m_Qualities.SetString(NAME_STRING, "Piercing Staff");//Re-Write the name to match Item//
			item->m_Qualities.SetDataID(SETUP_DID, 0x0200184F);
		}
		if (item->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffBludge) && staffBludge == BLUDGEON_DAMAGE_TYPE && item->InqStringQuality(NAME_STRING, "") == "Staff")
		{
			//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_BLUDGEONING);//Elemental UI Effect//
			item->m_Qualities.SetString(NAME_STRING, "Bludgeoning Staff");//Re-Write the name to match Item//
			item->m_Qualities.SetDataID(SETUP_DID, 0x0200184B);
		}
		if (item->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffAcid) && staffAcid == ACID_DAMAGE_TYPE && item->InqStringQuality(NAME_STRING, "") == "Staff")
		{
			//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
			item->m_Qualities.SetString(NAME_STRING, "Acid Staff");//Re-Write the name to match Item//
			item->m_Qualities.SetDataID(SETUP_DID, 0x0200184A);
		}
		if (item->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffElectric) && staffElectric == ELECTRIC_DAMAGE_TYPE && item->InqStringQuality(NAME_STRING, "") == "Staff")
		{
			//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Elemental UI Effect//
			item->m_Qualities.SetString(NAME_STRING, "Electric Staff");//Re-Write the name to match Item//
			item->m_Qualities.SetDataID(SETUP_DID, 0x0200184C);
		}
		if (item->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffFrost) && staffFrost == COLD_DAMAGE_TYPE && item->InqStringQuality(NAME_STRING, "") == "Staff")
		{
			//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
			item->m_Qualities.SetString(NAME_STRING, "Frost Staff");//Re-Write the name to match Item//
			item->m_Qualities.SetDataID(SETUP_DID, 0x0200184E);
		}
		if (item->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffFlame) && staffFlame == FIRE_DAMAGE_TYPE && item->InqStringQuality(NAME_STRING, "") == "Staff")
		{
			//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
			item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
			item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
			item->m_Qualities.SetString(NAME_STRING, "Fire Staff");//Re-Write the name to match Item//
			item->m_Qualities.SetDataID(SETUP_DID, 0x0200184D);
		}

	}

	for (auto pack : m_pPlayer->m_Packs)
	{

		if (pack->m_Qualities.id != W_PACKCREATUREESSENCE_CLASS && pack->m_Qualities.id != W_PACKITEMESSENCE_CLASS && pack->m_Qualities.id != W_PACKLIFEESSENCE_CLASS &&
			pack->m_Qualities.id != W_PACKWARESSENCE_CLASS)
		{

			for (auto item : pack->AsContainer()->m_Items)
			{
				// Updates shields to have SHIELD_VALUE_INT
				if (item->InqIntQuality(ITEM_TYPE_INT, 0) == TYPE_ARMOR && item->InqIntQuality(LOCATIONS_INT, 0) == SHIELD_LOC)
				{
					item->m_Qualities.SetInt(SHIELD_VALUE_INT, item->InqIntQuality(ARMOR_LEVEL_INT, 0));
				}

				// Weeping wand nerf
				if (item->m_Qualities.m_WeenieType == 35 && item->InqStringQuality(NAME_STRING, "") == "Weeping Wand"
					&& (item->InqDIDQuality(SPELL_DID, 0) > 0 || item->InqFloatQuality(SLAYER_DAMAGE_BONUS_FLOAT, 0) != 1.4))
				{
					item->m_Qualities.RemoveDataID(SPELL_DID);
					item->m_Qualities.SetFloat(SLAYER_DAMAGE_BONUS_FLOAT, 1.4);
				}

				//Elemental Yumi XBow Fix
				DWORD yumiXSlash;
				DWORD yumiXPierce;
				DWORD yumiXBludge;
				DWORD yumiXAcid;
				DWORD yumiXElectric;
				DWORD yumiXFrost;
				DWORD yumiXFlame;
				if (item->m_Qualities.InqDataID(SETUP_DID, yumiXSlash) && yumiXSlash == 33559028)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x0060060AC);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Slicing Yumi");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_SLASHING);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, SLASH_DAMAGE_TYPE);


				}
				if (item->m_Qualities.InqDataID(SETUP_DID, yumiXPierce) && yumiXPierce == 33559027)
				{
					item->m_Qualities.SetDataID(ICON_DID, 100677125);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Piercing Yumi");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_PIERCING);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, PIERCE_DAMAGE_TYPE);


				}
				if (item->m_Qualities.InqDataID(SETUP_DID, yumiXBludge) && yumiXBludge == 33559030)
				{
					item->m_Qualities.SetDataID(ICON_DID, 100677123);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Smashing Yumi");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_BLUDGEONING);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, BLUDGEON_DAMAGE_TYPE);

				}
				if (item->m_Qualities.InqDataID(SETUP_DID, yumiXAcid) && yumiXAcid == 33559029)
				{
					item->m_Qualities.SetDataID(ICON_DID, 100677121);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Stinging Yumi");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, ACID_DAMAGE_TYPE);

				}
				if (item->m_Qualities.InqDataID(SETUP_DID, yumiXElectric) && yumiXElectric == 33559031)
				{
					item->m_Qualities.SetDataID(ICON_DID, 100677118);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Sparking Yumi");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, ELECTRIC_DAMAGE_TYPE);

				}
				if (item->m_Qualities.InqDataID(SETUP_DID, yumiXFrost) && yumiXFrost == 33559026)
				{
					item->m_Qualities.SetDataID(ICON_DID, 100677119);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Freezing Yumi");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, COLD_DAMAGE_TYPE);

				}
				if (item->m_Qualities.InqDataID(SETUP_DID, yumiXFlame) && yumiXFlame == 33559025)
				{
					item->m_Qualities.SetDataID(ICON_DID, 100677122);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Smoldering Yumi");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, FIRE_DAMAGE_TYPE);

				}

				//Elemental Yumi Bow Fix
				DWORD yumiSlash;
				DWORD yumiPierce;
				DWORD yumiBludge;
				DWORD yumiAcid;
				DWORD yumiElectric;
				DWORD yumiFrost;
				DWORD yumiFlame;
				if (item->m_Qualities.InqDataID(SETUP_DID, yumiSlash) && yumiSlash == 33559028)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x0060060AC);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Slicing Yumi");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_SLASHING);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, SLASH_DAMAGE_TYPE);


				}
				if (item->m_Qualities.InqDataID(SETUP_DID, yumiPierce) && yumiPierce == 33559027)
				{
					item->m_Qualities.SetDataID(ICON_DID, 100677125);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Piercing Yumi");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_PIERCING);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, PIERCE_DAMAGE_TYPE);


				}
				if (item->m_Qualities.InqDataID(SETUP_DID, yumiBludge) && yumiBludge == 33559030)
				{
					item->m_Qualities.SetDataID(ICON_DID, 100677123);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Smashing Yumi");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_BLUDGEONING);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, BLUDGEON_DAMAGE_TYPE);

				}
				if (item->m_Qualities.InqDataID(SETUP_DID, yumiAcid) && yumiAcid == 33559029)
				{
					item->m_Qualities.SetDataID(ICON_DID, 100677121);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Stinging Yumi");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, ACID_DAMAGE_TYPE);

				}
				if (item->m_Qualities.InqDataID(SETUP_DID, yumiElectric) && yumiElectric == 33559031)
				{
					item->m_Qualities.SetDataID(ICON_DID, 100677118);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Sparking Yumi");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, ELECTRIC_DAMAGE_TYPE);

				}
				if (item->m_Qualities.InqDataID(SETUP_DID, yumiFrost) && yumiFrost == 33559026)
				{
					item->m_Qualities.SetDataID(ICON_DID, 100677119);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Freezing Yumi");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, COLD_DAMAGE_TYPE);

				}
				if (item->m_Qualities.InqDataID(SETUP_DID, yumiFlame) && yumiFlame == 33559025)
				{
					item->m_Qualities.SetDataID(ICON_DID, 100677122);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x09898CD);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Smoldering Yumi");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, FIRE_DAMAGE_TYPE);

				}

				//Compound Bow Fix
				DWORD compoundBow;
				DWORD compoundBowSlash;
				DWORD compoundBowPierce;
				DWORD compoundBowBludge;
				DWORD compoundBowAcid;
				DWORD compoundBowElectric;
				DWORD compoundBowFrost;
				DWORD compoundBowFlame;
				if (item->m_Qualities.InqDataID(SETUP_DID, compoundBow) && compoundBow == 0x0200144E)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x0060060AC);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Compound Bow");//Re-Write the name to match Item//

				}
				if (item->m_Qualities.InqDataID(SETUP_DID, compoundBowSlash) && compoundBowSlash == 0x02001488)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x0060060AC);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Slicing Compound Bow");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_SLASHING);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, SLASH_DAMAGE_TYPE);


				}
				if (item->m_Qualities.InqDataID(SETUP_DID, compoundBowPierce) && compoundBowPierce == 0x0200148A)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x0060060AD);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Piercing Compound Bow");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_PIERCING);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, PIERCE_DAMAGE_TYPE);


				}
				if (item->m_Qualities.InqDataID(SETUP_DID, compoundBowBludge) && compoundBowBludge == 0x02001489)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x0060060B1);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Smashing Compound Bow");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_BLUDGEONING);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, BLUDGEON_DAMAGE_TYPE);

				}
				if (item->m_Qualities.InqDataID(SETUP_DID, compoundBowAcid) && compoundBowAcid == 0x02001475)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x0060060AE);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Corrosive Compound Bow");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, ACID_DAMAGE_TYPE);

				}
				if (item->m_Qualities.InqDataID(SETUP_DID, compoundBowElectric) && compoundBowElectric == 0x02001472)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x0060060AF);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Sparking Compound Bow");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, ELECTRIC_DAMAGE_TYPE);

				}
				if (item->m_Qualities.InqDataID(SETUP_DID, compoundBowFrost) && compoundBowFrost == 0x02001473)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x0060060AA);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Chilling Compound Bow");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, COLD_DAMAGE_TYPE);

				}
				if (item->m_Qualities.InqDataID(SETUP_DID, compoundBowFlame) && compoundBowFlame == 0x02001474)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x0060060B0);//Compound Bow Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Searing Compound Bow");//Re-Write the name to match Item//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
					item->m_Qualities.SetInt(DAMAGE_TYPE_INT, FIRE_DAMAGE_TYPE);

				}

				/*Claw Fix
				DWORD clawPhysical;
				DWORD clawElectric;
				DWORD clawAcid;
				DWORD clawFire;
				DWORD clawFrost;
				if (item->m_Qualities.InqDataID(SETUP_DID, clawPhysical) && clawPhysical == 0x02001448)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//TOD Claw Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Claw");//Re-Write the name to match Item//

				}
				if (item->m_Qualities.InqDataID(SETUP_DID, clawElectric) && clawElectric == 0x0200145A)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//TOD Claw Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Spadone Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Lightning Claw");//Re-Write the name to match Item//
				}
				if (item->m_Qualities.InqDataID(SETUP_DID, clawAcid) && clawAcid == 0x02001891)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//TOD Claw Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
					item->m_Qualities.SetString(NAME_STRING, "Acid Claw");//Re-Write the name to match Item//
				}
				if (item->m_Qualities.InqDataID(SETUP_DID, clawFire) && clawFire == 0x0200145C)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//TOD Claw Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
					item->m_Qualities.SetString(NAME_STRING, "Flame Claw");//Re-Write the name to match Item//
				}
				if (item->m_Qualities.InqDataID(SETUP_DID, clawFrost) && clawFrost == 0x0200145B)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//TOD Claw Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
					item->m_Qualities.SetString(NAME_STRING, "Frost Claw");//Re-Write the name to match Item//
				}
				*/

				//Spadone Fix
				DWORD spadonePhysical;
				DWORD spadoneElectric;
				DWORD spadoneAcid;
				DWORD spadoneFrost;
				DWORD spadoneFlame;
				if (item->m_Qualities.InqDataID(SETUP_DID, spadonePhysical) && spadonePhysical == 0x0200130B)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//Spadone Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Spadone");//Re-Write the name to match Item//

				}
				if (item->m_Qualities.InqDataID(SETUP_DID, spadoneElectric) && spadoneElectric == 0x02001892)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//Spadone Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Spadone Pallete//
					item->m_Qualities.SetString(NAME_STRING, "Electric Spadone");//Re-Write the name to match Item//
				}
				if (item->m_Qualities.InqDataID(SETUP_DID, spadoneAcid) && spadoneAcid == 0x02001891)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//Spadone Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
					item->m_Qualities.SetString(NAME_STRING, "Acid Spadone");//Re-Write the name to match Item//
				}
				if (item->m_Qualities.InqDataID(SETUP_DID, spadoneFrost) && spadoneFrost == 0x02001890)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//Spadone Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
					item->m_Qualities.SetString(NAME_STRING, "Frost Spadone");//Re-Write the name to match Item//
				}
				if (item->m_Qualities.InqDataID(SETUP_DID, spadoneFlame) && spadoneFlame == 0x0200188F)
				{
					item->m_Qualities.SetDataID(ICON_DID, 0x06006B77);//Spadone Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04001A25);//Spadone Pallete//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
					item->m_Qualities.SetString(NAME_STRING, "Flame Spadone");//Re-Write the name to match Item//
				}

				//  Elemental Staff Fix  //
				int staffSlash;
				int staffPierce;
				int staffBludge;
				int staffAcid;
				int staffElectric;
				int staffFrost;
				int staffFlame;

				if (item->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffSlash) && staffSlash == SLASH_DAMAGE_TYPE && item->InqStringQuality(NAME_STRING, "") == "Staff")
				{
					//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_SLASHING);//Elemental UI Effect//
					item->m_Qualities.SetString(NAME_STRING, "Slashing Staff");//Re-Write the name to match Item//
					item->m_Qualities.SetDataID(SETUP_DID, 0x02001850);

				}
				if (item->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffPierce) && staffPierce == PIERCE_DAMAGE_TYPE && item->InqStringQuality(NAME_STRING, "") == "Staff")
				{
					//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_PIERCING);//Elemental UI Effect//
					item->m_Qualities.SetString(NAME_STRING, "Piercing Staff");//Re-Write the name to match Item//
					item->m_Qualities.SetDataID(SETUP_DID, 0x0200184F);
				}
				if (item->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffBludge) && staffBludge == BLUDGEON_DAMAGE_TYPE && item->InqStringQuality(NAME_STRING, "") == "Staff")
				{
					//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_BLUDGEONING);//Elemental UI Effect//
					item->m_Qualities.SetString(NAME_STRING, "Bludgeoning Staff");//Re-Write the name to match Item//
					item->m_Qualities.SetDataID(SETUP_DID, 0x0200184B);
				}
				if (item->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffAcid) && staffAcid == ACID_DAMAGE_TYPE && item->InqStringQuality(NAME_STRING, "") == "Staff")
				{
					//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
					item->m_Qualities.SetString(NAME_STRING, "Acid Staff");//Re-Write the name to match Item//
					item->m_Qualities.SetDataID(SETUP_DID, 0x0200184A);
				}
				if (item->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffElectric) && staffElectric == ELECTRIC_DAMAGE_TYPE && item->InqStringQuality(NAME_STRING, "") == "Staff")
				{
					//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Elemental UI Effect//
					item->m_Qualities.SetString(NAME_STRING, "Electric Staff");//Re-Write the name to match Item//
					item->m_Qualities.SetDataID(SETUP_DID, 0x0200184C);
				}
				if (item->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffFrost) && staffFrost == COLD_DAMAGE_TYPE && item->InqStringQuality(NAME_STRING, "") == "Staff")
				{
					//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
					item->m_Qualities.SetString(NAME_STRING, "Frost Staff");//Re-Write the name to match Item//
					item->m_Qualities.SetDataID(SETUP_DID, 0x0200184E);
				}
				if (item->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffFlame) && staffFlame == FIRE_DAMAGE_TYPE && item->InqStringQuality(NAME_STRING, "") == "Staff")
				{
					//item->m_Qualities.SetDataID(ICON_DID, 0x6006852);//Elemental Staff Icon//
					item->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
					item->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
					item->m_Qualities.SetString(NAME_STRING, "Fire Staff");//Re-Write the name to match Item//
					item->m_Qualities.SetDataID(SETUP_DID, 0x0200184D);
				}

			}
		}
	}

	// give characters created before creation timestamp was being set a timestamp and DOB from their DB date_created
	if (!m_pPlayer->m_Qualities.GetInt(CREATION_TIMESTAMP_INT, 0))
	{
		CharacterDesc_t char_info = g_pDBIO->GetCharacterInfo(m_pPlayer->GetID());
		if (char_info.date_created) // check that the query got info, will be 0 if it didn't
		{
			time_t t = char_info.date_created;
			m_pPlayer->m_Qualities.SetInt(CREATION_TIMESTAMP_INT, t);
			m_pPlayer->NotifyIntStatUpdated(CREATION_TIMESTAMP_INT);

			std::stringstream ss;
			ss << std::put_time(std::localtime(&t), "%m/%d/%y %I:%M:%S %p."); // convert time to a string of format '01/01/18 11:59:59 AM.'
			m_pPlayer->m_Qualities.SetString(DATE_OF_BIRTH_STRING, ss.str());
			m_pPlayer->NotifyStringStatUpdated(DATE_OF_BIRTH_STRING);
		}
	}

	if (m_pPlayer->m_Qualities.GetInt(HERITAGE_GROUP_INT, 0) == Lugian_HeritageGroup)
		m_pPlayer->m_Qualities.SetDataID(MOTION_TABLE_DID, 0x9000216);

	if (m_pPlayer->m_Qualities.GetInt(HERITAGE_GROUP_INT, 0) == Empyrean_HeritageGroup && m_pPlayer->m_Qualities.GetDID(MOTION_TABLE_DID, 0x9000001) == 0x9000001)
		m_pPlayer->m_Qualities.SetDataID(MOTION_TABLE_DID, 0x9000207);

	m_pPlayer->SendText("Classic Dereth:E Now Enhanced with Source Edits provided by the GDLE Team!" SERVER_VERSION_NUMBER_STRING " " SERVER_VERSION_STRING, LTT_DEFAULT);
	m_pPlayer->SendText("GDLE is Maintained by, ChosenOne, LikeableLime, Scribble and the GDLE Dev Team. Contact them at https://discord.gg/WzGX348", LTT_DEFAULT);
	m_pPlayer->SendText("Powered by GamesDeadLol(GDL). Not an official Asheron's Call server.", LTT_DEFAULT);
	SendAllegianceMOTD();

	/*
	if (*g_pConfig->WelcomeMessage() != 0)
	{
		m_pPlayer->SendText(g_pConfig->WelcomeMessage(), LTT_DEFAULT);
	}
	*/

	g_pWorld->CreateEntity(m_pPlayer);

	//temporarily add all enchantments back from the character's wielded items
	if (g_pConfig->SpellPurgeOnLogin())
	{
		for (auto item : m_pPlayer->m_Wielded)
		{
			if (item->m_Qualities._spell_book)
			{
				bool bShouldCast = true;

				std::string name;
				if (item->m_Qualities.InqString(CRAFTSMAN_NAME_STRING, name))
				{
					if (!name.empty() && name != item->InqStringQuality(NAME_STRING, ""))
					{
						bShouldCast = false;

						m_pPlayer->NotifyWeenieErrorWithString(WERROR_ACTIVATION_NOT_CRAFTSMAN, name.c_str());
					}
				}

				int difficulty;
				difficulty = 0;
				if (item->m_Qualities.InqInt(ITEM_DIFFICULTY_INT, difficulty, TRUE, FALSE))
				{
					DWORD skillLevel = 0;
					if (!m_pPlayer->m_Qualities.InqSkill(ARCANE_LORE_SKILL, skillLevel, FALSE) || (int)skillLevel < difficulty)
					{
						bShouldCast = false;

						m_pPlayer->NotifyWeenieError(WERROR_ACTIVATION_ARCANE_LORE_TOO_LOW);
					}
				}

				if (bShouldCast)
				{
					difficulty = 0;
					DWORD skillActivationTypeDID = 0;



					if (item->m_Qualities.InqInt(ITEM_SKILL_LEVEL_LIMIT_INT, difficulty, TRUE, FALSE) && item->m_Qualities.InqDataID(ITEM_SKILL_LIMIT_DID, skillActivationTypeDID))
					{
						STypeSkill skillActivationType = SkillTable::OldToNewSkill((STypeSkill)skillActivationTypeDID);


						DWORD skillLevel = 0;
						if (!m_pPlayer->m_Qualities.InqSkill(skillActivationType, skillLevel, FALSE) || (int)skillLevel < difficulty)
						{
							bShouldCast = false;

							m_pPlayer->NotifyWeenieErrorWithString(WERROR_ACTIVATION_SKILL_TOO_LOW, CachedSkillTable->GetSkillName(skillActivationType).c_str());
						}
					}
				}

				if (bShouldCast && item->InqIntQuality(ITEM_ALLEGIANCE_RANK_LIMIT_INT, 0) > item->InqIntQuality(ALLEGIANCE_RANK_INT, 0))
				{
					bShouldCast = false;
					m_pPlayer->NotifyInventoryFailedEvent(item->GetID(), WERROR_ACTIVATION_RANK_TOO_LOW);
				}

				if (bShouldCast)
				{
					int heritageRequirement = item->InqIntQuality(HERITAGE_GROUP_INT, -1);
					if (heritageRequirement != -1 && heritageRequirement != item->InqIntQuality(HERITAGE_GROUP_INT, 0))
					{
						bShouldCast = false;
						std::string heritageString = item->InqStringQuality(ITEM_HERITAGE_GROUP_RESTRICTION_STRING, "of the correct heritage");
						m_pPlayer->NotifyWeenieErrorWithString(WERROR_ACTIVATION_WRONG_RACE, heritageString.c_str());
					}
				}

				int currentMana = 0;
				if (bShouldCast && item->m_Qualities.InqInt(ITEM_CUR_MANA_INT, currentMana, TRUE, FALSE))
				{
					if (currentMana == 0)
					{
						bShouldCast = false;
						m_pPlayer->NotifyWeenieError(WERROR_ACTIVATION_NOT_ENOUGH_MANA);
					}
					else
						item->_nextManaUse = Timer::cur_time;
				}

				if (bShouldCast)
				{
					DWORD serial = 0;
					serial |= ((DWORD)m_pPlayer->GetEnchantmentSerialByteForMask(item->InqIntQuality(LOCATIONS_INT, 0, TRUE)) << (DWORD)0);
					serial |= ((DWORD)m_pPlayer->GetEnchantmentSerialByteForMask(item->InqIntQuality(CLOTHING_PRIORITY_INT, 0, TRUE)) << (DWORD)8);

					for (auto &spellPage : item->m_Qualities._spell_book->_spellbook)
					{
						item->MakeSpellcastingManager()->CastSpellEquipped(m_pPlayer->GetID(), spellPage.first, (WORD)serial);
					}
				}
			}
		}
	}

	m_pPlayer->DebugValidate();

	return;
}

void CClientEvents::SendText(const char *szText, long lColor)
{
	m_pClient->SendNetMessage(ServerText(szText, lColor), PRIVATE_MSG, FALSE, TRUE);
}

void CClientEvents::Attack(DWORD target, DWORD height, float power)
{
	if (height <= 0 || height >= ATTACK_HEIGHT::NUM_ATTACK_HEIGHTS)
	{
		SERVER_WARN << "Bad melee attack height %u sent by player 0x%08X\n", height, m_pPlayer->GetID();
		return;
	}

	if (power < 0.0f || power > 1.0f)
	{
		SERVER_WARN << "Bad melee attack power %f sent by player 0x%08X\n", power, m_pPlayer->GetID();
		return;
	}

	m_pPlayer->TryMeleeAttack(target, (ATTACK_HEIGHT) height, power);
}

void CClientEvents::MissileAttack(DWORD target, DWORD height, float power)
{
	if (height <= 0 || height >= ATTACK_HEIGHT::NUM_ATTACK_HEIGHTS)
	{
		SERVER_WARN << "Bad missile attack height %u sent by player 0x%08X\n", height, m_pPlayer->GetID();
		return;
	}

	if (power < 0.0f || power > 1.0f)
	{
		SERVER_WARN << "Bad missile attack power %f sent by player 0x%08X\n", power, m_pPlayer->GetID();
		return;
	}

	m_pPlayer->TryMissileAttack(target, (ATTACK_HEIGHT)height, power);
}

void CClientEvents::SendTellByGUID(const char* szText, DWORD dwGUID)
{
	if (strlen(szText) > 300)
		return;

	//should really check for invalid characters and such ;]

	while (szText[0] == ' ') //Skip leading spaces.
		szText++;
	if (szText[0] == '\0') //Make sure the text isn't blank
		return;

	/*
	if (dwGUID == m_pPlayer->GetID())
	{
		m_pPlayer->SendNetMessage(ServerText("You really need some new friends..", 1), PRIVATE_MSG, FALSE);
		return;
	}
	*/

	CPlayerWeenie *pTarget;

	if (!(pTarget = g_pWorld->FindPlayer(dwGUID)))
		return;

	if (pTarget->GetID() != m_pPlayer->GetID())
	{
		char szResponse[300];
		_snprintf(szResponse, 300, "You tell %s, \"%s\"", pTarget->GetName().c_str(), szText);
		m_pPlayer->SendNetMessage(ServerText(szResponse, 4), PRIVATE_MSG, FALSE, TRUE);
	}

	pTarget->SendNetMessage(DirectChat(szText, m_pPlayer->GetName().c_str(), m_pPlayer->GetID(), pTarget->GetID(), 3), PRIVATE_MSG, TRUE);
}

void CClientEvents::SendTellByName(const char* szText, const char* szName)
{
	if (strlen(szName) > 300)
		return;
	if (strlen(szText) > 300)
		return;

	//should really check for invalid characters and such ;]

	while (szText[0] == ' ') //Skip leading spaces.
		szText++;
	if (szText[0] == '\0') //Make sure the text isn't blank
		return;

	CPlayerWeenie *pTarget;

	if (!(pTarget = g_pWorld->FindPlayer(szName)))
		return;

	if (pTarget->GetID() != m_pPlayer->GetID())
	{
		char szResponse[300];
		_snprintf(szResponse, 300, "You tell %s, \"%s\"", pTarget->GetName().c_str(), szText);
		m_pPlayer->SendNetMessage(ServerText(szResponse, 4), PRIVATE_MSG, FALSE, TRUE);
	}

	pTarget->SendNetMessage(DirectChat(szText, m_pPlayer->GetName().c_str(), m_pPlayer->GetID(), pTarget->GetID(), 3), PRIVATE_MSG, TRUE);
}

void CClientEvents::ClientText(const char *szText)
{
	if (strlen(szText) > 500)
		return;

	//should really check for invalid characters and such ;]

	while (szText[0] == ' ') //Skip leading spaces.
		szText++;
	if (szText[0] == '\0') //Make sure the text isn't blank
		return;

	std::string filteredText = m_pClient->GetAccessLevel() >= SENTINEL_ACCESS ? szText : FilterBadChatCharacters(szText);
	szText = filteredText.c_str(); // hacky

	if (szText[0] == '!' || szText[0] == '@' || szText[0] == '/')
	{
		CommandBase::Execute((char *) (++szText), m_pClient);
	}
	else
	{
		if (CheckForChatSpam())
		{
			m_pPlayer->SpeakLocal(szText);
		}
	}
}

void CClientEvents::EmoteText(const char* szText)
{
	if (strlen(szText) > 300)
		return;

	//TODO: Check for invalid characters and such ;)

	while (szText[0] == ' ') //Skip leading spaces.
		szText++;
	if (szText[0] == '\0') //Make sure the text isn't blank
		return;

	m_pPlayer->EmoteLocal(szText);
}

void CClientEvents::ActionText(const char *text)
{
	if (strlen(text) > 300)
		return;
	
	while (text[0] == ' ') //Skip leading spaces.
		text++;
	if (text[0] == '\0') //Make sure the text isn't blank
		return;

	m_pPlayer->ActionLocal(text);
}

void CClientEvents::ChannelText(DWORD channel_id, const char *text)
{
	if (strlen(text) > 300)
		return;

	// TODO: check for invalid characters and such
	while (text[0] == ' ')
		text++;
	if (text[0] == '\0')
		return;

	switch (channel_id)
	{
	case Fellow_ChannelID:
		{
			std::string fellowName;
			if (!m_pPlayer->m_Qualities.InqString(FELLOWSHIP_STRING, fellowName))
				return;

			g_pFellowshipManager->Chat(fellowName, m_pPlayer->GetID(), text);
			CHAT_LOG << m_pPlayer->GetName().c_str() << "says (fellowship)," << text;
			break;
		}

	case Patron_ChannelID:
		g_pAllegianceManager->ChatPatron(m_pPlayer->GetID(), text);
		CHAT_LOG << m_pPlayer->GetName().c_str() << "says (patron)," << text;
		break;

	case Vassals_ChannelID:
		g_pAllegianceManager->ChatVassals(m_pPlayer->GetID(), text);
		CHAT_LOG << m_pPlayer->GetName().c_str() << "says (vassals)," << text;
		break;

	case Covassals_ChannelID:
		g_pAllegianceManager->ChatCovassals(m_pPlayer->GetID(), text);
		CHAT_LOG << m_pPlayer->GetName().c_str() << "says (covassals)," << text;
		break;

	case Monarch_ChannelID:
		g_pAllegianceManager->ChatMonarch(m_pPlayer->GetID(), text);
		CHAT_LOG << m_pPlayer->GetName().c_str() << "says (monarch)," << text;
		break;
	}
}

void CClientEvents::RequestHealthUpdate(DWORD dwGUID)
{
	CWeenieObject *pEntity = g_pWorld->FindWithinPVS(m_pPlayer, dwGUID);

	if (pEntity)
	{
		if (pEntity->IsCreature())
		{
			m_pPlayer->SetLastHealthRequest(pEntity->GetID());
			m_pClient->SendNetMessage(HealthUpdate((CMonsterWeenie *)pEntity), PRIVATE_MSG, TRUE, TRUE);
		}
	}
}

void CClientEvents::ChangeCombatStance(COMBAT_MODE mode)
{
	m_pPlayer->ChangeCombatMode(mode, true);
	// ActionComplete();
}

void CClientEvents::ExitPortal()
{
	m_pPlayer->ExitPortal();
}

void CClientEvents::Ping()
{
	// Pong!
	DWORD Pong = 0x1EA;
	m_pClient->SendNetMessage(&Pong, sizeof(Pong), PRIVATE_MSG, TRUE);
}

void CClientEvents::UseItemEx(DWORD dwSourceID, DWORD dwDestID)
{
	CWeenieObject *pSource = g_pWorld->FindWithinPVS(m_pPlayer, dwSourceID);
	CWeenieObject *pDest = g_pWorld->FindWithinPVS(m_pPlayer, dwDestID);

	if (pSource && pSource->AsCaster())
		m_pPlayer->ExecuteUseEvent(new CWandSpellUseEvent(dwSourceID, pDest ? dwDestID : dwSourceID));
	else if (pSource && pDest)
		pSource->UseWith(m_pPlayer, pDest);
	else
		ActionComplete();
}

void CClientEvents::UseObject(DWORD dwEID)
{
	if (m_pPlayer->IsBusyOrInAction())
	{
		ActionComplete(WERROR_ACTIONS_LOCKED);
		return;
	}

	CWeenieObject *pTarget = g_pWorld->FindWithinPVS(m_pPlayer, dwEID);

	if (pTarget)
	{
		if (pTarget->AsContainer() && pTarget->AsContainer()->_openedById == m_pPlayer->GetID())
		{
			//we're closing a chest
			pTarget->AsContainer()->OnContainerClosed(m_pPlayer);
			ActionComplete();
			return;
		}

		int error = pTarget->UseChecked(m_pPlayer);

		if (error != WERROR_NONE)
			ActionComplete(error);
	}
	else
	{
		ActionComplete(WERROR_OBJECT_GONE);
	}
}

void CClientEvents::ActionComplete(int error)
{
	if (!m_pPlayer)
		return;

	m_pPlayer->NotifyUseDone(error);
}

void CClientEvents::Identify(DWORD target_id)
{
	if (_next_allowed_identify > Timer::cur_time)
	{
		// do not allow to ID too fast
		return;
	}

	/*
	CWeenieObject *pTarget = g_pWorld->FindWithinPVS(m_pPlayer, target_id);

	if (!pTarget)
	{
		// used to check for vendor items, temporary, should be changed
		pTarget = g_pWorld->FindObject(target_id);
	}
	*/

	CWeenieObject *pTarget = g_pWorld->FindObject(target_id);

#ifndef PUBLIC_BUILD
	if (pTarget)
#else
	int vis = 0;
	if (pTarget && !pTarget->m_Qualities.InqBool(VISIBILITY_BOOL, vis))
#endif
	{
		pTarget->TryIdentify(m_pPlayer);
		m_pPlayer->SetLastAssessed(pTarget->GetID());
	}	

	_next_allowed_identify = Timer::cur_time + 0.5;
}

void CClientEvents::SpendAttributeXP(STypeAttribute key, DWORD amount)
{
	// TODO use attribute map
	if (key < 1 || key > 6)
		return;

	// TODO verify they are trying to spend the correct amount of XP

	__int64 unassignedExp = 0;
	m_pPlayer->m_Qualities.InqInt64(AVAILABLE_EXPERIENCE_INT64, unassignedExp);
	if ((unsigned __int64)unassignedExp < (unsigned __int64)amount)
	{
		// Not enough experience
		return;
	}

	m_pPlayer->GiveAttributeXP(key, amount);
	m_pPlayer->m_Qualities.SetInt64(AVAILABLE_EXPERIENCE_INT64, (unsigned __int64)unassignedExp - (unsigned __int64)amount);

	m_pPlayer->NotifyAttributeStatUpdated(key);
	m_pPlayer->NotifyInt64StatUpdated(AVAILABLE_EXPERIENCE_INT64);
}

void CClientEvents::SpendAttribute2ndXP(STypeAttribute2nd key, DWORD amount)
{
	// TODO use vital map
	if (key != 1 && key != 3 && key != 5)
		return;

	// TODO verify they are trying to spend the correct amount of XP

	__int64 unassignedExp = 0;
	m_pPlayer->m_Qualities.InqInt64(AVAILABLE_EXPERIENCE_INT64, unassignedExp);
	if ((unsigned __int64)unassignedExp < (unsigned __int64)amount)
	{
		// Not enough experience
		return;
	}

	m_pPlayer->GiveAttribute2ndXP(key, amount);

	m_pPlayer->m_Qualities.SetInt64(AVAILABLE_EXPERIENCE_INT64, (unsigned __int64)unassignedExp - (unsigned __int64)amount);
	m_pPlayer->NotifyInt64StatUpdated(AVAILABLE_EXPERIENCE_INT64);
}

void CClientEvents::SpendSkillXP(STypeSkill key, DWORD exp)
{
	// TODO verify they are trying to spend the correct amount of XP

	__int64 unassignedExp = 0;
	m_pPlayer->m_Qualities.InqInt64(AVAILABLE_EXPERIENCE_INT64, unassignedExp);
	if ((unsigned __int64)unassignedExp < (unsigned __int64)exp)
	{
		// Not enough experience
		return;
	}

	Skill skill;
	if (!m_pPlayer->m_Qualities.InqSkill(key, skill) || skill._sac < TRAINED_SKILL_ADVANCEMENT_CLASS)
	{
		// Skill doesn't exist or isn't trained
		return;
	}

	m_pPlayer->GiveSkillXP(key, exp);

	m_pPlayer->m_Qualities.SetInt64(AVAILABLE_EXPERIENCE_INT64, (unsigned __int64)unassignedExp - (unsigned __int64)exp);
	m_pPlayer->NotifyInt64StatUpdated(AVAILABLE_EXPERIENCE_INT64);
}

void CClientEvents::SpendSkillCredits(STypeSkill key, DWORD credits)
{
	// TODO verify they are trying to spend the correct amount of XP

	DWORD unassignedCredits = 0;
	m_pPlayer->m_Qualities.InqInt(AVAILABLE_SKILL_CREDITS_INT, *(int *)&unassignedCredits);
	if (unassignedCredits < credits)
	{
		// Not enough experience
		return;
	}

	Skill skill;
	if (!m_pPlayer->m_Qualities.InqSkill(key, skill) || skill._sac >= TRAINED_SKILL_ADVANCEMENT_CLASS)
	{
		// Skill doesn't exist or already trained
		return;
	}

	DWORD costToRaise = m_pPlayer->GetCostToRaiseSkill(key);

	if (m_pPlayer->GetCostToRaiseSkill(key) != credits)
	{
		SERVER_WARN << m_pPlayer->GetName() << "- Credit cost to raise skill does not match what player is trying to spend.";
		return;
	}

	m_pPlayer->GiveSkillAdvancementClass(key, TRAINED_SKILL_ADVANCEMENT_CLASS);

	m_pPlayer->m_Qualities.SetInt(AVAILABLE_SKILL_CREDITS_INT, unassignedCredits - costToRaise);
	m_pPlayer->NotifyIntStatUpdated(AVAILABLE_SKILL_CREDITS_INT);
}

void CClientEvents::LifestoneRecall()
{
	Position lifestone;
	if (m_pPlayer->m_Qualities.InqPosition(SANCTUARY_POSITION, lifestone) && lifestone.objcell_id)
	{
		if (!m_pPlayer->IsBusyOrInAction())
		{
			m_pPlayer->ExecuteUseEvent(new CLifestoneRecallUseEvent());
		}
	}
	else
	{
		m_pClient->SendNetMessage(ServerText("You are not bound to a Lifestone!", 7), PRIVATE_MSG);
	}
}

void CClientEvents::MarketplaceRecall()
{
	if (!m_pPlayer->IsBusyOrInAction())
	{
		m_pPlayer->ExecuteUseEvent(new CMarketplaceRecallUseEvent());
	}
}

void CClientEvents::TryInscribeItem(DWORD object_id, const std::string &text)
{
	CWeenieObject *weenie = m_pPlayer->FindContainedItem(object_id);

	if (!weenie)
	{
		return;
	}

	if (!weenie->IsInscribable())
	{
		return;
	}

	weenie->m_Qualities.SetString(INSCRIPTION_STRING, text.length() <= 800 ? text : text.substr(0, 800));
	weenie->m_Qualities.SetString(SCRIBE_NAME_STRING, m_pPlayer->GetName());
}

void CClientEvents::TryBuyItems(DWORD vendor_id, std::list<class ItemProfile *> &items)
{
	CWeenieObject *weenie = g_pWorld->FindWithinPVS(m_pPlayer, vendor_id);

	if (!weenie)
	{
		ActionComplete(WERROR_NO_OBJECT);
		return;
	}

	int error = WERROR_NO_OBJECT;
	if (weenie->IsVendor())
	{
		error = ((CVendor *)weenie)->TrySellItemsToPlayer(m_pPlayer, items);
		((CVendor *)weenie)->SendVendorInventory(m_pPlayer);
	}

	ActionComplete(error);
}


void CClientEvents::TrySellItems(DWORD vendor_id, std::list<class ItemProfile *> &items)
{
	CWeenieObject *weenie = g_pWorld->FindWithinPVS(m_pPlayer, vendor_id);

	if (!weenie)
	{
		ActionComplete(WERROR_NO_OBJECT);
		return;
	}

	int error = WERROR_NO_OBJECT;
	if (weenie->IsVendor())
	{
		error = ((CVendor *)weenie)->TryBuyItemsFromPlayer(m_pPlayer, items);
		((CVendor *)weenie)->SendVendorInventory(m_pPlayer);
	}

	ActionComplete(error);
}

void CClientEvents::TryFellowshipCreate(const std::string name, int shareXP)
{
	if (m_pPlayer->HasFellowship())
		return;

	int error = g_pFellowshipManager->Create(name, m_pPlayer->GetID(), shareXP);

	if (error)
		m_pPlayer->NotifyWeenieError(error);
}

void CClientEvents::TryFellowshipQuit(int disband)
{
	std::string fellowshipName;
	if (!m_pPlayer->m_Qualities.InqString(FELLOWSHIP_STRING, fellowshipName))
		return;

	int error = 0;
	if (disband)
		error = g_pFellowshipManager->Disband(fellowshipName, m_pPlayer->GetID());
	else
		error = g_pFellowshipManager->Quit(fellowshipName, m_pPlayer->GetID());

	if (error)
		m_pPlayer->NotifyWeenieError(error);
}

void CClientEvents::TryFellowshipDismiss(DWORD dismissed)
{
	std::string fellowshipName;
	if (!m_pPlayer->m_Qualities.InqString(FELLOWSHIP_STRING, fellowshipName))
		return;

	int error = g_pFellowshipManager->Dismiss(fellowshipName, m_pPlayer->GetID(), dismissed);

	if (error)
		m_pPlayer->NotifyWeenieError(error);
}

void CClientEvents::TryFellowshipRecruit(DWORD target)
{
	std::string fellowshipName;
	if (!m_pPlayer->m_Qualities.InqString(FELLOWSHIP_STRING, fellowshipName))
		return;

	int error = g_pFellowshipManager->Recruit(fellowshipName, m_pPlayer->GetID(), target);

	if (error)
		m_pPlayer->NotifyWeenieError(error);
}

void CClientEvents::TryFellowshipUpdate(int on)
{
	std::string fellowshipName;
	if (!m_pPlayer->m_Qualities.InqString(FELLOWSHIP_STRING, fellowshipName))
		return;

	int error = g_pFellowshipManager->RequestUpdates(fellowshipName, m_pPlayer->GetID(), on);

	if (error)
		m_pPlayer->NotifyWeenieError(error);
}

void CClientEvents::TryFellowshipAssignNewLeader(DWORD target)
{
	std::string fellowshipName;
	if (!m_pPlayer->m_Qualities.InqString(FELLOWSHIP_STRING, fellowshipName))
		return;

	int error = g_pFellowshipManager->AssignNewLeader(fellowshipName, m_pPlayer->GetID(), target);

	if (error)
		m_pPlayer->NotifyWeenieError(error);
}

void CClientEvents::TryFellowshipChangeOpenness(int open)
{
	std::string fellowshipName;
	if (!m_pPlayer->m_Qualities.InqString(FELLOWSHIP_STRING, fellowshipName))
		return;

	int error = g_pFellowshipManager->ChangeOpen(fellowshipName, m_pPlayer->GetID(), open);

	if (error)
		m_pPlayer->NotifyWeenieError(error);
}

void CClientEvents::SendAllegianceUpdate()
{
	if (!m_pPlayer)
		return;

	g_pAllegianceManager->SendAllegianceProfile(m_pPlayer);
}

void CClientEvents::SendAllegianceMOTD()
{
	AllegianceTreeNode *self = g_pAllegianceManager->GetTreeNode(m_pPlayer->GetID());
	if (!self)
		return;

	AllegianceInfo *info = g_pAllegianceManager->GetInfo(self->_monarchID);
	if (!info)
		return;

	m_pPlayer->SendText(csprintf("\"%s\" -- %s", info->_info.m_motd.c_str(), info->_info.m_motdSetBy.c_str()), LTT_DEFAULT);
}

void CClientEvents::SetRequestAllegianceUpdate(int on)
{
	m_bSendAllegianceUpdates = on;
}

void CClientEvents::TryBreakAllegiance(DWORD target)
{
	int error = g_pAllegianceManager->TryBreakAllegiance(m_pPlayer, target);
	m_pPlayer->NotifyWeenieError(error);

	if (error == WERROR_NONE)
	{
		SendAllegianceUpdate();
	}
}

void CClientEvents::TrySwearAllegiance(DWORD target)
{
	CPlayerWeenie *targetWeenie = g_pWorld->FindPlayer(target);
	if (!targetWeenie)
	{
		m_pPlayer->NotifyWeenieError(WERROR_NO_OBJECT);
		return;
	}
	
	int error = g_pAllegianceManager->TrySwearAllegiance(m_pPlayer, targetWeenie);	
	m_pPlayer->NotifyWeenieError(error);

	if (error == WERROR_NONE)
	{
		SendAllegianceUpdate();
	}
}

void CClientEvents::AllegianceInfoRequest(const std::string &target)
{
	AllegianceTreeNode *myNode = g_pAllegianceManager->GetTreeNode(m_pPlayer->GetID());
	if (!myNode)
	{
		m_pPlayer->SendText("You are not in an allegiance.", LTT_DEFAULT);
		return;
	}

	AllegianceTreeNode *myMonarchNode = g_pAllegianceManager->GetTreeNode(myNode->_monarchID);
	if (!myMonarchNode)
	{
		m_pPlayer->SendText("There was an error processing your request.", LTT_DEFAULT);
		return;
	}

	AllegianceTreeNode *myTargetNode = myMonarchNode->FindCharByNameRecursivelySlow(target);
	if (!myTargetNode)
	{
		m_pPlayer->SendText("Could not find allegiance member.", LTT_DEFAULT);
		return;
	}

	unsigned int rank = 0;
	if (AllegianceProfile *profile = g_pAllegianceManager->CreateAllegianceProfile(myTargetNode->_charID, &rank))
	{
		BinaryWriter allegianceUpdate;
		allegianceUpdate.Write<DWORD>(0x27C);
		allegianceUpdate.Write<DWORD>(myTargetNode->_charID);
		profile->Pack(&allegianceUpdate);
		m_pPlayer->SendNetMessage(&allegianceUpdate, PRIVATE_MSG, TRUE, FALSE);

		delete profile;
	}
	else
	{
		m_pPlayer->SendText("Error retrieving allegiance member information.", LTT_DEFAULT);
	}
}

void CClientEvents::TrySetAllegianceMOTD(const std::string &text)
{
	AllegianceTreeNode *self = g_pAllegianceManager->GetTreeNode(m_pPlayer->GetID());
	if (!self)
		return;

	if (self->_charID != self->_monarchID)
	{
		m_pPlayer->SendText("Only the monarch can set the Message of the Day.", LTT_DEFAULT);
		return;
	}

	AllegianceInfo *info = g_pAllegianceManager->GetInfo(self->_monarchID);
	if (!info)
		return;

	info->_info.m_motd = text;
	info->_info.m_motdSetBy = m_pPlayer->GetName();
	m_pPlayer->SendText(csprintf("MOTD changed to: \"%s\" -- %s", info->_info.m_motd.c_str(), info->_info.m_motdSetBy.c_str()), LTT_DEFAULT);
}

void CClientEvents::AllegianceHometownRecall()
{
	AllegianceTreeNode *allegianceNode = g_pAllegianceManager->GetTreeNode(m_pPlayer->GetID());

	if (!allegianceNode)
	{
		m_pPlayer->NotifyWeenieError(WERROR_ALLEGIANCE_NONEXISTENT);
		return;
	}

	AllegianceInfo *allegianceInfo = g_pAllegianceManager->GetInfo(allegianceNode->_monarchID);

	if (allegianceInfo && allegianceInfo->_info.m_BindPoint.objcell_id)
	{
		if (!m_pPlayer->IsBusyOrInAction())
			m_pPlayer->ExecuteUseEvent(new CAllegianceHometownRecallUseEvent());
	}
	else
		m_pPlayer->NotifyWeenieError(WERROR_ALLEGIANCE_HOMETOWN_NOT_SET);
}

void CClientEvents::HouseBuy(DWORD slumlord, const PackableList<DWORD> &items)
{
	if (CWeenieObject *slumlordObj = g_pWorld->FindObject(slumlord))
	{
		if (m_pPlayer->DistanceTo(slumlordObj, true) > 10.0)
			return;

		if (CSlumLordWeenie *slumlord = slumlordObj->AsSlumLord())
		{
			slumlord->BuyHouse(m_pPlayer, items);
		}
	}
}

void CClientEvents::HouseRent(DWORD slumlord, const PackableList<DWORD> &items)
{
	if (CWeenieObject *slumlordObj = g_pWorld->FindObject(slumlord))
	{
		if (m_pPlayer->DistanceTo(slumlordObj, true) > 10.0)
			return;

		if (CSlumLordWeenie *slumlord = slumlordObj->AsSlumLord())
		{
			slumlord->RentHouse(m_pPlayer, items);
		}
	}
}

void CClientEvents::HouseAbandon()
{
	DWORD houseId = m_pPlayer->InqDIDQuality(HOUSEID_DID, 0);
	if (houseId)
	{
		CHouseData *houseData = g_pHouseManager->GetHouseData(houseId);
		if (houseData && houseData->_ownerId == m_pPlayer->GetID())
		{
			houseData->AbandonHouse();
			m_pPlayer->SendText("You've abandoned your house.", LTT_DEFAULT); //todo: made up message.
		}
		else
			m_pPlayer->SendText("That house does not belong to you.", LTT_DEFAULT); //todo: made up message.
	}
	else
		m_pClient->SendNetMessage(ServerText("You do not have a house!", 7), PRIVATE_MSG);
}

void CClientEvents::HouseRecall()
{
	DWORD houseId = m_pPlayer->GetAccountHouseId();
	if (houseId)
	{
		CHouseData *houseData = g_pHouseManager->GetHouseData(houseId);
		if (houseData->_ownerAccount == m_pPlayer->GetClient()->GetAccountInfo().id)
		{
			if (!m_pPlayer->IsBusyOrInAction())
				m_pPlayer->ExecuteUseEvent(new CHouseRecallUseEvent());
		}
		else
			m_pPlayer->SendText("That house does not belong to you.", LTT_DEFAULT); //todo: made up message.
	}
	else
		m_pClient->SendNetMessage(ServerText("You do not have a house!", 7), PRIVATE_MSG);
}

void CClientEvents::HouseMansionRecall()
{
	AllegianceTreeNode *allegianceNode = g_pAllegianceManager->GetTreeNode(m_pPlayer->GetID());

	if (!allegianceNode)
	{
		m_pPlayer->NotifyWeenieError(WERROR_ALLEGIANCE_NONEXISTENT);
		return;
	}

	DWORD allegianceHouseId;

	CWeenieObject *monarch = g_pWorld->FindObject(allegianceNode->_monarchID);
	if (!monarch)
	{
		monarch = CWeenieObject::Load(allegianceNode->_monarchID);
		if (!monarch)
			return;
		allegianceHouseId = monarch->InqDIDQuality(HOUSEID_DID, 0);
		delete monarch;
	}
	else
		allegianceHouseId = monarch->InqDIDQuality(HOUSEID_DID, 0);

	if (allegianceHouseId)
	{
		CHouseData *houseData = g_pHouseManager->GetHouseData(allegianceHouseId);
		if (houseData && houseData->_ownerId == allegianceNode->_monarchID && (houseData->_houseType == 2 || houseData->_houseType == 3)) //2 = villa, 3 = mansion
		{
			if (!m_pPlayer->IsBusyOrInAction())
				m_pPlayer->ExecuteUseEvent(new CMansionRecallUseEvent());
			return;
		}
	}

	if(allegianceNode->_patronID)
		m_pClient->SendNetMessage(ServerText("Your monarch does not own a mansion or villa!", 7), PRIVATE_MSG);
	else
		m_pClient->SendNetMessage(ServerText("You do not own a mansion or villa!", 7), PRIVATE_MSG);
}

void CClientEvents::HouseRequestData()
{
	DWORD houseId = m_pPlayer->InqDIDQuality(HOUSEID_DID, 0);
	if (houseId)
		g_pHouseManager->SendHouseData(m_pPlayer, houseId);
	else
	{
		//if we can't get the data send the "no house" packet
		BinaryWriter noHouseData;
		noHouseData.Write<DWORD>(0x0226);
		noHouseData.Write<DWORD>(0);
		m_pPlayer->SendNetMessage(&noHouseData, PRIVATE_MSG, TRUE, FALSE);
	}
}

void CClientEvents::HouseToggleHooks(bool newSetting)
{
	DWORD houseId = m_pPlayer->InqDIDQuality(HOUSEID_DID, 0);
	if (houseId)
	{
		CHouseData *houseData = g_pHouseManager->GetHouseData(houseId);
		if (houseData->_ownerId == m_pPlayer->GetID())
		{
			houseData->SetHookVisibility(newSetting);
			if (newSetting)
				m_pPlayer->SendText("Your dwelling's hooks are now visible.", LTT_DEFAULT); //todo: made up message.
			else
				m_pPlayer->SendText("Your dwelling's hooks are now hidden.", LTT_DEFAULT); //todo: made up message.
		}
		else
			m_pPlayer->SendText("Only the character who owns the house may use this command.", LTT_DEFAULT);
	}
	else
		m_pPlayer->SendText("You do not own a house.", LTT_DEFAULT);
}

void CClientEvents::HouseRequestAccessList()
{
	DWORD houseId = m_pPlayer->InqDIDQuality(HOUSEID_DID, 0);
	if (houseId)
	{
		CHouseData *houseData = g_pHouseManager->GetHouseData(houseId);
		if (houseData->_ownerId == m_pPlayer->GetID())
		{
			m_pPlayer->SendText("Access:", LTT_DEFAULT);
			m_pPlayer->SendText(csprintf("   Public: %s", houseData->_everyoneAccess ? "Allow" : "Deny"), LTT_DEFAULT);
			m_pPlayer->SendText(csprintf("   Allegiance: %s", houseData->_allegianceAccess ? "Allow" : "Deny"), LTT_DEFAULT);

			m_pPlayer->SendText("Storage:", LTT_DEFAULT);
			m_pPlayer->SendText(csprintf("   Public: %s", houseData->_everyoneStorageAccess ? "Allow" : "Deny"), LTT_DEFAULT);
			m_pPlayer->SendText(csprintf("   Allegiance: %s", houseData->_allegianceStorageAccess ? "Allow" : "Deny"), LTT_DEFAULT);

			if (houseData->_accessList.empty())
				m_pPlayer->SendText("Your dwelling's acess list is empty.", LTT_DEFAULT);
			else
			{
				m_pPlayer->SendText("Access List:", LTT_DEFAULT);
				std::list<DWORD>::iterator i = houseData->_accessList.begin();
				while (i != houseData->_accessList.end())
				{
					std::string name = g_pWorld->GetPlayerName(*(i), true);
					if (!name.empty())
					{
						m_pPlayer->SendText(csprintf("   %s", name.c_str()), LTT_DEFAULT);
						i++;
					}
					else
						i = houseData->_accessList.erase(i); //no longer exists.
				}
			}

			if (houseData->_storageAccessList.empty())
				m_pPlayer->SendText("Your dwelling's storage access list is empty.", LTT_DEFAULT);
			else
			{
				m_pPlayer->SendText("Storage Access list:", LTT_DEFAULT);
				std::list<DWORD>::iterator i = houseData->_storageAccessList.begin();
				while (i != houseData->_storageAccessList.end())
				{
					std::string name = g_pWorld->GetPlayerName(*(i), true);
					if (!name.empty())
					{
						m_pPlayer->SendText(csprintf("   %s", name.c_str()), LTT_DEFAULT);
						i++;
					}
					else
						i = houseData->_storageAccessList.erase(i); //no longer exists.
				}
			}
		}
		else
			m_pPlayer->SendText("Only the character who owns the house may use this command.", LTT_DEFAULT);
	}
	else
		m_pPlayer->SendText("You do not own a house.", LTT_DEFAULT);
}

void CClientEvents::HouseAddPersonToAccessList(std::string name)
{
	DWORD targetId = g_pWorld->GetPlayerId(name.c_str(), true);
	if (!target)
	{
		m_pPlayer->SendText("Can't find a player by that name.", LTT_DEFAULT); //todo: made up message.
		return;
	}

	DWORD houseId = m_pPlayer->InqDIDQuality(HOUSEID_DID, 0);
	if (houseId)
	{
		CHouseData *houseData = g_pHouseManager->GetHouseData(houseId);
		if (houseData->_ownerId == m_pPlayer->GetID())
		{
			if (houseData->_accessList.size() > 128)
			{
				m_pPlayer->SendText("The access list is full", LTT_DEFAULT); //todo: made up message.
				return;
			}
			if (std::find(houseData->_accessList.begin(), houseData->_accessList.end(), targetId) != houseData->_accessList.end())
			{
				m_pPlayer->SendText(csprintf("%s is already in the access list", name.c_str()), LTT_DEFAULT); //todo: made up message.
			}
			else
			{
				m_pPlayer->SendText(csprintf("You add %s to your dwelling's access list", name.c_str()), LTT_DEFAULT); //todo: made up message.
				houseData->_accessList.push_back(targetId);
			}
		}
		else
			m_pPlayer->SendText("Only the character who owns the house may use this command.", LTT_DEFAULT);
	}
	else
		m_pPlayer->SendText("You do not own a house.", LTT_DEFAULT);
}

void CClientEvents::HouseRemovePersonFromAccessList(std::string name)
{
	DWORD targetId = g_pWorld->GetPlayerId(name.c_str(), true);
	if (!target)
	{
		m_pPlayer->SendText("Can't find a player by that name.", LTT_DEFAULT); //todo: made up message.
		return;
	}

	DWORD houseId = m_pPlayer->InqDIDQuality(HOUSEID_DID, 0);
	if (houseId)
	{
		CHouseData *houseData = g_pHouseManager->GetHouseData(houseId);
		if (houseData->_ownerId == m_pPlayer->GetID())
		{
			auto iter = std::find(houseData->_accessList.begin(), houseData->_accessList.end(), targetId);
			if (iter != houseData->_accessList.end())
			{
				m_pPlayer->SendText(csprintf("You remove %s from your dwelling's access list", name.c_str()), LTT_DEFAULT); //todo: made up message.
				houseData->_accessList.erase(iter);
			}
			else
			{
				m_pPlayer->SendText(csprintf("%s is not in the access list", name.c_str()), LTT_DEFAULT); //todo: made up message.
			}
		}
		else
			m_pPlayer->SendText("Only the character who owns the house may use this command.", LTT_DEFAULT);
	}
	else
		m_pPlayer->SendText("You do not own a house.", LTT_DEFAULT);
}

void CClientEvents::HouseToggleOpenAccess(bool newSetting)
{
	DWORD houseId = m_pPlayer->InqDIDQuality(HOUSEID_DID, 0);
	if (houseId)
	{
		CHouseData *houseData = g_pHouseManager->GetHouseData(houseId);
		if (houseData->_ownerId == m_pPlayer->GetID())
		{
			if (houseData->_everyoneAccess != newSetting)
			{
				houseData->_everyoneAccess = newSetting;
				if (newSetting)
					m_pPlayer->SendText("Your dwelling is now open to the public.", LTT_DEFAULT); //todo: made up message.
				else
					m_pPlayer->SendText("Your dwelling is now private.", LTT_DEFAULT); //todo: made up message.
			}
			else
			{
				if (newSetting)
					m_pPlayer->SendText("Your dwelling is already open to the public.", LTT_DEFAULT); //todo: made up message.
				else
					m_pPlayer->SendText("Your dwelling is already private.", LTT_DEFAULT); //todo: made up message.
			}
		}
		else
			m_pPlayer->SendText("Only the character who owns the house may use this command.", LTT_DEFAULT);
	}
	else
		m_pPlayer->SendText("You do not own a house.", LTT_DEFAULT);
}

void CClientEvents::HouseToggleOpenStorageAccess()
{
	//not sure how this worked? Is this a toggle? If not which command was used to disable it?
	DWORD houseId = m_pPlayer->InqDIDQuality(HOUSEID_DID, 0);
	if (houseId)
	{
		CHouseData *houseData = g_pHouseManager->GetHouseData(houseId);
		if (houseData->_ownerId == m_pPlayer->GetID())
		{
			if (!houseData->_everyoneStorageAccess)
			{
				m_pPlayer->SendText("Your dwelling's storage is now open to the public.", LTT_DEFAULT); //todo: made up message.
				houseData->_everyoneStorageAccess = true;
			}
			else
			{
				m_pPlayer->SendText("Your dwelling's storage is now private.", LTT_DEFAULT); //todo: made up message.
				houseData->_everyoneStorageAccess = false;
			}
		}
		else
			m_pPlayer->SendText("Only the character who owns the house may use this command.", LTT_DEFAULT);
	}
	else
		m_pPlayer->SendText("You do not own a house.", LTT_DEFAULT);
}

void CClientEvents::HouseAddOrRemovePersonToStorageList(std::string name, bool isAdd)
{
	DWORD targetId = g_pWorld->GetPlayerId(name.c_str(), true);
	if (!target)
	{
		m_pPlayer->SendText("Can't find a player by that name.", LTT_DEFAULT); //todo: made up message.
		return;
	}

	DWORD houseId = m_pPlayer->InqDIDQuality(HOUSEID_DID, 0);
	if (houseId)
	{
		CHouseData *houseData = g_pHouseManager->GetHouseData(houseId);
		if (houseData->_ownerId == m_pPlayer->GetID())
		{
			if (isAdd)
			{
				if (houseData->_accessList.size() > 128)
				{
					m_pPlayer->SendText("The storage access list is full", LTT_DEFAULT); //todo: made up message.
					return;
				}
				if (std::find(houseData->_storageAccessList.begin(), houseData->_storageAccessList.end(), targetId) != houseData->_storageAccessList.end())
				{
					m_pPlayer->SendText(csprintf("%s is already in the storage access list", name.c_str()), LTT_DEFAULT); //todo: made up message.
				}
				else
				{
					m_pPlayer->SendText(csprintf("You add %s to your dwelling's storage access list", name.c_str()), LTT_DEFAULT); //todo: made up message.
					houseData->_storageAccessList.push_back(targetId);
				}
			}
			else
			{
				auto iter = std::find(houseData->_storageAccessList.begin(), houseData->_storageAccessList.end(), targetId);
				if (iter != houseData->_storageAccessList.end())
				{
					m_pPlayer->SendText(csprintf("You remove %s from your dwelling's storage access list", name.c_str()), LTT_DEFAULT); //todo: made up message.
					houseData->_storageAccessList.erase(iter);
				}
				else
				{
					m_pPlayer->SendText(csprintf("%s is not in the storage access list", name.c_str()), LTT_DEFAULT); //todo: made up message.
				}
			}
		}
		else
			m_pPlayer->SendText("Only the character who owns the house may use this command.", LTT_DEFAULT);
	}
	else
		m_pPlayer->SendText("You do not own a house.", LTT_DEFAULT);
}

void CClientEvents::HouseAddOrRemoveAllegianceToAccessList(bool isAdd)
{
	DWORD houseId = m_pPlayer->InqDIDQuality(HOUSEID_DID, 0);
	if (houseId)
	{
		CHouseData *houseData = g_pHouseManager->GetHouseData(houseId);
		if (houseData->_ownerId == m_pPlayer->GetID())
		{
			if (houseData->_allegianceAccess != isAdd)
			{
				houseData->_allegianceAccess = isAdd;
				if (isAdd)
					m_pPlayer->SendText("You have granted your monarchy access to your dwelling.", LTT_DEFAULT);
				else
					m_pPlayer->SendText("You have revoked access to your dwelling to your monarchy.", LTT_DEFAULT);
			}
			else
			{
				if (isAdd)
					m_pPlayer->SendText("The monarchy already has access to your dwelling.", LTT_DEFAULT);
				else
					m_pPlayer->SendText("The monarchy did not have access to your dwelling.", LTT_DEFAULT);
			}
		}
		else
			m_pPlayer->SendText("Only the character who owns the house may use this command.", LTT_DEFAULT);
	}
	else
		m_pPlayer->SendText("You do not own a house.", LTT_DEFAULT);
}

void CClientEvents::HouseAddOrRemoveAllegianceToStorageList(bool isAdd)
{
	DWORD houseId = m_pPlayer->InqDIDQuality(HOUSEID_DID, 0);
	if (houseId)
	{
		CHouseData *houseData = g_pHouseManager->GetHouseData(houseId);
		if (houseData->_ownerId == m_pPlayer->GetID())
		{
			if (houseData->_allegianceStorageAccess != isAdd)
			{
				houseData->_allegianceStorageAccess = isAdd;
				if (isAdd)
					m_pPlayer->SendText("You have granted your monarchy access to your storage.", LTT_DEFAULT);
				else
					m_pPlayer->SendText("You have revoked storage access to your monarchy.", LTT_DEFAULT);
			}
			else
			{
				if (isAdd)
					m_pPlayer->SendText("The monarchy already has storage access in your dwelling.", LTT_DEFAULT);
				else
					m_pPlayer->SendText("The monarchy did not have storage access in your dwelling.", LTT_DEFAULT);
			}
		}
		else
			m_pPlayer->SendText("Only the character who owns the house may use this command.", LTT_DEFAULT);
	}
	else
		m_pPlayer->SendText("You do not own a house.", LTT_DEFAULT);
}

void CClientEvents::HouseClearAccessList()
{
	DWORD houseId = m_pPlayer->InqDIDQuality(HOUSEID_DID, 0);
	if (houseId)
	{
		CHouseData *houseData = g_pHouseManager->GetHouseData(houseId);
		if (houseData->_ownerId == m_pPlayer->GetID())
		{
			houseData->_everyoneAccess = false;
			houseData->_allegianceAccess = false;
			if (houseData->_accessList.empty())
			{
				houseData->_accessList.clear();
				m_pPlayer->SendText("Your clear the dwelling's access list.", LTT_DEFAULT); //todo: made up message.
			}
			else
				m_pPlayer->SendText("There's no one in the dwelling's access list.", LTT_DEFAULT); //todo: made up message.
		}
		else
			m_pPlayer->SendText("Only the character who owns the house may use this command.", LTT_DEFAULT);
	}
	else
		m_pPlayer->SendText("You do not own a house.", LTT_DEFAULT);
}

void CClientEvents::HouseClearStorageAccess()
{
	DWORD houseId = m_pPlayer->InqDIDQuality(HOUSEID_DID, 0);
	if (houseId)
	{
		CHouseData *houseData = g_pHouseManager->GetHouseData(houseId);
		if (houseData->_ownerId == m_pPlayer->GetID())
		{
			houseData->_everyoneStorageAccess = false;
			houseData->_allegianceStorageAccess = false;
			if (houseData->_storageAccessList.empty())
			{
				houseData->_storageAccessList.clear();
				m_pPlayer->SendText("Your clear the dwelling's storage list.", LTT_DEFAULT); //todo: made up message.
			}
			else
				m_pPlayer->SendText("There's no one in the dwelling's storage list.", LTT_DEFAULT); //todo: made up message.
		}
		else
			m_pPlayer->SendText("Only the character who owns the house may use this command.", LTT_DEFAULT);
	}
	else
		m_pPlayer->SendText("You do not own a house.", LTT_DEFAULT);
}

void CClientEvents::NoLongerViewingContents(DWORD container_id)
{	
	if (CWeenieObject *remoteContainerObj = g_pWorld->FindObject(container_id))
	{
		if (CContainerWeenie *remoteContainer = remoteContainerObj->AsContainer())
		{
			remoteContainer->HandleNoLongerViewing(m_pPlayer);
		}
	}
}

void CClientEvents::ChangePlayerOption(PlayerOptions option, bool value)
{
	auto changeCharOption = [&](DWORD optionBit)
		{
			m_pPlayer->_playerModule.options_ &= ~optionBit;
			if (value)
				m_pPlayer->_playerModule.options_ |= optionBit;
		};

	auto changeCharOption2 = [&](DWORD optionBit)
	{
		m_pPlayer->_playerModule.options2_ &= ~optionBit;
		if (value)
			m_pPlayer->_playerModule.options2_ |= optionBit;
	};

	switch (option)
	{
	case AutoRepeatAttack_PlayerOption:
		changeCharOption(AutoRepeatAttack_CharacterOption);
		break;

	case IgnoreAllegianceRequests_PlayerOption:
		changeCharOption(IgnoreAllegianceRequests_CharacterOption);
		break;

	case IgnoreFellowshipRequests_PlayerOption:
		changeCharOption(IgnoreFellowshipRequests_CharacterOption);
		break;

	case IgnoreTradeRequests_PlayerOption:
		changeCharOption(IgnoreAllegianceRequests_CharacterOption);
		break;

	case DisableMostWeatherEffects_PlayerOption:
		changeCharOption(DisableMostWeatherEffects_CharacterOption);
		break;
		 
	case PersistentAtDay_PlayerOption:
		changeCharOption2(PersistentAtDay_CharacterOptions2);
		break;

	case AllowGive_PlayerOption:
		changeCharOption(AllowGive_CharacterOption);
		break;

	case ViewCombatTarget_PlayerOption:
		changeCharOption(ViewCombatTarget_CharacterOption);
		break;

	case ShowTooltips_PlayerOption:
		changeCharOption(ShowTooltips_CharacterOption);
		break;

	case UseDeception_PlayerOption:
		changeCharOption(UseDeception_CharacterOption);
		break;

	case ToggleRun_PlayerOption:
		changeCharOption(ToggleRun_CharacterOption);
		break;

	case StayInChatMode_PlayerOption:
		changeCharOption(StayInChatMode_CharacterOption);
		break;

	case AdvancedCombatUI_PlayerOption:
		changeCharOption(AdvancedCombatUI_CharacterOption);
		break;

	case AutoTarget_PlayerOption:
		changeCharOption(AutoTarget_CharacterOption);
		break;

	case VividTargetingIndicator_PlayerOption:
		changeCharOption(VividTargetingIndicator_CharacterOption);
		break;

	case FellowshipShareXP_PlayerOption:
		changeCharOption(FellowshipShareXP_CharacterOption);
		break;

	case AcceptLootPermits_PlayerOption:
		changeCharOption(AcceptLootPermits_PlayerOption);
		break;

	case FellowshipShareLoot_PlayerOption:
		changeCharOption(FellowshipShareLoot_CharacterOption);
		break;

	case FellowshipAutoAcceptRequests_PlayerOption:
		changeCharOption(FellowshipAutoAcceptRequests_PlayerOption);
		break;

	case SideBySideVitals_PlayerOption:
		changeCharOption(SideBySideVitals_CharacterOption);
		break;

	case CoordinatesOnRadar_PlayerOption:
		changeCharOption(CoordinatesOnRadar_CharacterOption);
		break;

	case SpellDuration_PlayerOption:
		changeCharOption(SpellDuration_CharacterOption);
		break;

	case DisableHouseRestrictionEffects_PlayerOption:
		changeCharOption(DisableHouseRestrictionEffects_PlayerOption);
		break;

	case DragItemOnPlayerOpensSecureTrade_PlayerOption:
		changeCharOption(DragItemOnPlayerOpensSecureTrade_CharacterOption);
		break;

	case DisplayAllegianceLogonNotifications_PlayerOption:
		changeCharOption(DisplayAllegianceLogonNotifications_CharacterOption);
		break;

	case UseChargeAttack_PlayerOption:
		changeCharOption(UseChargeAttack_CharacterOption);
		break;

	case UseCraftSuccessDialog_PlayerOption:
		changeCharOption(UseCraftSuccessDialog_CharacterOption);
		break;

	case HearAllegianceChat_PlayerOption:
		changeCharOption(HearAllegianceChat_CharacterOption);

		if (value)
			SendText("Joined Allegiance Channel!", LTT_CHANNEL_SEND);
		else
			SendText("Left Allegiance Channel!", LTT_CHANNEL_SEND);
		break;

	case DisplayDateOfBirth_PlayerOption:
		changeCharOption2(DisplayDateOfBirth_CharacterOptions2);
		break;

	case DisplayAge_PlayerOption:
		changeCharOption2(DisplayAge_CharacterOptions2);
		break;

	case DisplayChessRank_PlayerOption:
		changeCharOption2(DisplayChessRank_CharacterOptions2);
		break;

	case DisplayFishingSkill_PlayerOption:
		changeCharOption2(DisplayFishingSkill_CharacterOptions2);
		break;

	case DisplayNumberDeaths_PlayerOption:
		changeCharOption2(DisplayNumberDeaths_CharacterOptions2);
		break;

	case DisplayTimeStamps_PlayerOption:
		changeCharOption2(TimeStamp_CharacterOptions2);
		break;

	case SalvageMultiple_PlayerOption:
		changeCharOption(SalvageMultiple_CharacterOptions2);
		break;

	case HearGeneralChat_PlayerOption:
		changeCharOption2(HearGeneralChat_CharacterOptions2);

		if (value)
			SendText("Joined General Channel!", LTT_CHANNEL_SEND);
		else
			SendText("Left General Channel!", LTT_CHANNEL_SEND);
		break;

	case HearTradeChat_PlayerOption:
		changeCharOption2(HearTradeChat_CharacterOptions2);

		if (value)
			SendText("Joined Trade Channel!", LTT_CHANNEL_SEND);
		else
			SendText("Left Trade Channel!", LTT_CHANNEL_SEND);

		break;

	case HearLFGChat_PlayerOption:
		changeCharOption2(HearLFGChat_CharacterOptions2);

		if (value)
			SendText("Joined LFG Channel!", LTT_CHANNEL_SEND);
		else
			SendText("Left LFG Channel!", LTT_CHANNEL_SEND);
		break;

	case HearRoleplayChat_PlayerOption:
		changeCharOption2(HearRoleplayChat_CharacterOptions2);

		if (value)
			SendText("Joined Roleplay Channel!", LTT_CHANNEL_SEND);
		else
			SendText("Left Roleplay Channel!", LTT_CHANNEL_SEND);
		break;

	case AppearOffline_PlayerOption:
		changeCharOption2(AppearOffline_CharacterOptions2);
		break;

	case DisplayNumberCharacterTitles_PlayerOption:
		changeCharOption2(DisplayNumberCharacterTitles_CharacterOptions2);
		break;

	case MainPackPreferred_PlayerOption:
		changeCharOption2(MainPackPreferred_CharacterOptions2);
		break;

	case LeadMissileTargets_PlayerOption:
		changeCharOption2(LeadMissileTargets_CharacterOptions2);
		break;

	case UseFastMissiles_PlayerOption:
		changeCharOption2(UseFastMissiles_CharacterOptions2);
		break;

	case FilterLanguage_PlayerOption:
		changeCharOption2(FilterLanguage_CharacterOptions2);
		break;

	case ConfirmVolatileRareUse_PlayerOption:
		changeCharOption2(ConfirmVolatileRareUse_CharacterOptions2);
		break;

	case HearSocietyChat_PlayerOption:
		changeCharOption2(HearSocietyChat_CharacterOptions2);
		break;

	case ShowHelm_PlayerOption:
		changeCharOption2(ShowHelm_CharacterOptions2);
		break;

	case DisableDistanceFog_PlayerOption:
		changeCharOption2(DisableDistanceFog_CharacterOptions2);
		break;

	case UseMouseTurning_PlayerOption:
		changeCharOption2(UseMouseTurning_CharacterOptions2);
		break;

	case ShowCloak_PlayerOption:
		changeCharOption2(ShowCloak_CharacterOptions2);
		break;

	case LockUI_PlayerOption:
		changeCharOption2(LockUI_CharacterOptions2);
		break;

	case TotalNumberOfPlayerOptions_PlayerOption:
		changeCharOption2(TotalNumberOfPlayerOptions_PlayerOption);
		break;
	}
}

bool CClientEvents::CheckForChatSpam()
{
	if (_next_chat_allowed > Timer::cur_time)
	{
		return false;
	}

	if (_next_chat_interval < Timer::cur_time)
	{
		_next_chat_interval = Timer::cur_time + 8.0;
		_num_chat_sent = 0;
	}

	_num_chat_sent++;
	if (_num_chat_sent > 8)
	{
		_next_chat_allowed = Timer::cur_time + 10.0;
		m_pPlayer->SendText("You are sending too many messages and have been temporarily muted.", LTT_DEFAULT);
		return false;
	}

	return true;
}

// This is it!
void CClientEvents::ProcessEvent(BinaryReader *pReader)
{
	if (!m_pPlayer)
	{
		return;
	}

	DWORD dwSequence = pReader->ReadDWORD();
	DWORD dwEvent = pReader->ReadDWORD();
	if (pReader->GetLastError()) return;

#ifdef _DEBUG
	LOG(Client, Verbose, "Processing event: 0x%X\n", dwEvent);
#endif

	switch (dwEvent)
	{
		case 0x0005: // Change player option
			{
				DWORD option = pReader->ReadDWORD();
				DWORD value = pReader->ReadDWORD();
				if (pReader->GetLastError())
					break;

				ChangePlayerOption((PlayerOptions)option, value ? true : false);
				break;
			}
		case 0x0008: // Melee Attack
			{
				DWORD dwTarget = pReader->ReadDWORD();
				DWORD dwHeight = pReader->ReadDWORD();
				float flPower = pReader->ReadFloat();
				if (pReader->GetLastError()) break;

				Attack(dwTarget, dwHeight, flPower);
				break;
			}
		case 0x000A: // Missile Attack
			{
				DWORD target = pReader->ReadDWORD();
				DWORD height = pReader->ReadDWORD();
				float power = pReader->ReadFloat();
				if (pReader->GetLastError()) break;

				MissileAttack(target, height, power);
				break;
			}
		case 0x0015: //Client Text
			{
				char *szText = pReader->ReadString();
				if (pReader->GetLastError()) break;

				ClientText(szText);
				break;
			}
		case 0x0019: //Store Item
			{
				DWORD dwItemID = pReader->ReadDWORD();
				DWORD dwContainer = pReader->ReadDWORD();
				DWORD dwSlot = pReader->ReadDWORD();
				if (pReader->GetLastError()) break;

				m_pPlayer->MoveItemToContainer(dwItemID, dwContainer, (char)dwSlot);
				break;
			}
		case 0x001A: //Equip Item
			{
				DWORD dwItemID = pReader->ReadDWORD();
				DWORD dwCoverage = pReader->ReadDWORD();
				if (pReader->GetLastError()) break;

				m_pPlayer->MoveItemToWield(dwItemID, dwCoverage);
				break;
			}
		case 0x001B: //Drop Item
			{
				DWORD dwItemID = pReader->ReadDWORD();
				if (pReader->GetLastError()) break;

				m_pPlayer->MoveItemTo3D(dwItemID);
				break;
			}
		case 0x001D: // Swear Allegiance request
			{
				DWORD target = pReader->Read<DWORD>();
				if (pReader->GetLastError()) break;

				TrySwearAllegiance(target);
				break;
			}
		case 0x001E: // Break Allegiance request
			{
				DWORD target = pReader->Read<DWORD>();
				if (pReader->GetLastError())
					break;

				TryBreakAllegiance(target);
				break;
			}
		case 0x001F: // Allegiance Update request
			{
				int on = pReader->Read<int>();
				if (pReader->GetLastError()) break;

				SetRequestAllegianceUpdate(on);
				break;
			}
		case 0x0275: // confirmation response		cas
			{
				DWORD confirmType = pReader->ReadDWORD();
				int context = pReader->ReadInt32();
				bool accepted = pReader->ReadInt32();
				
					switch (confirmType)
					{
					case 0x05: // crafting
						if (accepted)
							{
							m_pPlayer->UseEx(true);
							}
						break;
						}
				
					break;
				}
		case 0x027D: // ust salvage request
		{
			DWORD toolId = pReader->ReadDWORD();

			PackableList<DWORD> items;
			items.UnPack(pReader);

			if (pReader->GetLastError())
				break;

			if (items.size() <= 300) //just some sanity checking: 102 items in main pack + (24 * 7) items in subpacks = 270 items. 300 just to be safe.
				m_pPlayer->PerformSalvaging(toolId, items);
			break;
		}
		case 0x0032: //Send Tell by GUID
			{
				char *text = pReader->ReadString();
				DWORD GUID = pReader->ReadDWORD();

				if (pReader->GetLastError())
					break;

				if (CheckForChatSpam())
				{
					std::string filteredText = FilterBadChatCharacters(text);
					SendTellByGUID(filteredText.c_str(), GUID);
				}

				break;
			}
		case 0x0035: //Use Item Ex
			{
				DWORD dwSourceID = pReader->ReadDWORD();
				DWORD dwDestID = pReader->ReadDWORD();
				if (pReader->GetLastError()) break;

				UseItemEx(dwSourceID, dwDestID);
				break;
			}
		case 0x0036: //Use Object
			{
				DWORD dwEID = pReader->ReadDWORD();
				if (pReader->GetLastError()) break;

				UseObject(dwEID);
				break;
			}
		case 0x0044: // spend XP on vitals
			{
				DWORD dwAttribute2nd = pReader->ReadDWORD();
				DWORD dwXP = pReader->ReadDWORD();
				if (pReader->GetLastError()) break;

				SpendAttribute2ndXP((STypeAttribute2nd)dwAttribute2nd, dwXP);
				break;
			}
		case 0x0045: // spend XP on attributes
			{
				DWORD dwAttribute = pReader->ReadDWORD();
				DWORD dwXP = pReader->ReadDWORD();
				if (pReader->GetLastError()) break;

				SpendAttributeXP((STypeAttribute)dwAttribute, dwXP);
				break;
			}
		case 0x0046: // spend XP on skills
			{
				DWORD dwSkill = pReader->ReadDWORD();
				DWORD dwXP = pReader->ReadDWORD();
				if (pReader->GetLastError()) break;

				SpendSkillXP((STypeSkill)dwSkill, dwXP);
				break;
			}
		case 0x0047: // spend credits to train skill
			{
				DWORD dwSkill = pReader->ReadDWORD();
				DWORD dwCredits = pReader->ReadDWORD();
				if (pReader->GetLastError()) break;

				SpendSkillCredits((STypeSkill)dwSkill, dwCredits);
				break;
			}
		case 0x0048: // cast untargeted spell
			{
				DWORD spell_id = pReader->ReadDWORD();
				if (pReader->GetLastError()) break;

				m_pPlayer->TryCastSpell(m_pPlayer->GetID() /*0*/, spell_id);
				break;
			}
		case 0x004A: // cast targeted spell
			{
				DWORD target = pReader->ReadDWORD();
				DWORD spell_id = pReader->ReadDWORD();
				if (pReader->GetLastError()) break;

				m_pPlayer->TryCastSpell(target, spell_id);
				break;
			}
		case 0x0053: // Evt_Combat__ChangeCombatMode_ID "Change Combat Mode"
			{
				DWORD mode = pReader->ReadDWORD();
				if (pReader->GetLastError()) break;

				ChangeCombatStance((COMBAT_MODE)mode);
				break;
			}
		case 0x0054: // Evt_Inventory__StackableMerge
			{
				DWORD merge_from_id = pReader->Read<DWORD>();
				DWORD merge_to_id = pReader->Read<DWORD>();
				DWORD amount = pReader->Read<DWORD>();

				if (pReader->GetLastError())
					break;

				m_pPlayer->MergeItem(merge_from_id, merge_to_id, amount);
				break;
			}
		case 0x0055: // Evt_Inventory__StackableSplitToContainer
			{
				DWORD stack_id = pReader->Read<DWORD>();
				DWORD container_id = pReader->Read<DWORD>();
				DWORD place = pReader->Read<DWORD>();
				DWORD amount = pReader->Read<DWORD>();

				if (pReader->GetLastError())
					break;

				m_pPlayer->SplitItemToContainer(stack_id, container_id, place, amount);
				break;
			}
		case 0x0056: // Evt_Inventory__StackableSplitTo3D
			{
				DWORD stack_id = pReader->Read<DWORD>();
				DWORD amount = pReader->Read<DWORD>();

				if (pReader->GetLastError())
					break;

				m_pPlayer->SplitItemto3D(stack_id, amount);
				break;
			}
		case 0x019B: // Evt_Inventory__StackableSplitToWield
			{				
				DWORD stack_id = pReader->Read<DWORD>();
				DWORD loc = pReader->Read<DWORD>();
				DWORD amount = pReader->Read<DWORD>();

				if (pReader->GetLastError())
					break;

				m_pPlayer->SplitItemToWield(stack_id, loc, amount);
				break;
			}
		case 0x005D: //Send Tell by Name
		{
			char* szText = pReader->ReadString();
			char* szName = pReader->ReadString();
			if (pReader->GetLastError()) break;

			if (CheckForChatSpam())
			{
				std::string filteredText = FilterBadChatCharacters(szText);
				SendTellByName(filteredText.c_str(), szName);
			}

			break;
		}
		case 0x005F: // Buy from Vendor
			{
				DWORD vendorID = pReader->Read<DWORD>();
				DWORD numItems = pReader->Read<DWORD>();
				if (numItems >= 300)
					break;

				bool error = false;
				std::list<ItemProfile *> items;
				
				for (DWORD i = 0; i < numItems; i++)
				{
					ItemProfile *item = new ItemProfile();
					error = item->UnPack(pReader);
					items.push_back(item);

					if (error || pReader->GetLastError())
						break;
				}

				if (!error && !pReader->GetLastError())
				{
					TryBuyItems(vendorID, items);
				}

				for (auto item : items)
				{
					delete item;
				}

				break;
			}
		case 0x0060: // Buy from Vendor
			{
				DWORD vendorID = pReader->Read<DWORD>();
				DWORD numItems = pReader->Read<DWORD>();
				if (numItems >= 300)
					break;

				bool error = false;
				std::list<ItemProfile *> items;

				for (DWORD i = 0; i < numItems; i++)
				{
					ItemProfile *item = new ItemProfile();
					error = item->UnPack(pReader);
					items.push_back(item);

					if (error || pReader->GetLastError())
						break;
				}

				if (!error && !pReader->GetLastError())
				{
					TrySellItems(vendorID, items);
				}

				for (auto item : items)
				{
					delete item;
					
				}

				break;
			}
		case 0x0063: // Lifestone Recall
		{
			LifestoneRecall();
			break;
		}
		case 0x00A1: // "Login Complete"
		{
			ExitPortal();
			break;
		}
		case 0x00A2: // "Create Fellowship"
		{
			std::string name = pReader->ReadString();
			int shareXP = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			TryFellowshipCreate(name, shareXP);
			break;
		}
		case 0x00A3: // "Quit Fellowship"
		{
			int disband = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			TryFellowshipQuit(disband);
			break;
		}
		case 0x00A4: // "Fellowship Dismiss"
		{
			DWORD dismissed = pReader->Read<DWORD>();
			if (pReader->GetLastError()) break;

			TryFellowshipDismiss(dismissed);
			break;
		}
		case 0x00A5: // "Fellowship Recruit"
		{
			DWORD target = pReader->Read<DWORD>();

			if (pReader->GetLastError())
				break;

			TryFellowshipRecruit(target);
			break;
		}
		case 0x00A6: // "Fellowship Update"
		{
			int on = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			TryFellowshipUpdate(on);
			break;
		}
		case 0x00CD: // Put object in container
			{
				DWORD target_id = pReader->Read<DWORD>();
				DWORD object_id = pReader->Read<DWORD>();
				DWORD amount = pReader->Read<DWORD>();

				if (pReader->GetLastError())
					break;

				m_pPlayer->GiveItem(target_id, object_id, amount);
				break;
			}		
		case 0x00BF: // "Inscribe"
			{
				DWORD target_id = pReader->Read<DWORD>();
				std::string msg = pReader->ReadString();

				if (pReader->GetLastError())
					break;

				TryInscribeItem(target_id, msg);
				break;
			}
		case 0x00C8: // Identify
		{
			DWORD target_id = pReader->ReadDWORD();

			if (pReader->GetLastError())
				break;

			Identify(target_id);
			break;
		}
		case 0x00D6: // Advocate teleport (triggered by having an admin flag set, clicking the mini-map)
		{
			if (m_pPlayer->GetAccessLevel() < ADVOCATE_ACCESS)
				break;

			// Starts with string (was empty when I tested)
			pReader->ReadString();

			// Then position (target)
			Position position;
			position.UnPack(pReader);

			if (pReader->GetLastError())
				break;

			m_pPlayer->Movement_Teleport(position);
			break;
		}
		case 0x0147: // Channel Text
		{
			DWORD channel_id = pReader->ReadDWORD();
			char *msg = pReader->ReadString();

			if (pReader->GetLastError())
				break;

			if (CheckForChatSpam())
			{
				std::string filteredText = FilterBadChatCharacters(msg);
				ChannelText(channel_id, filteredText.c_str());
			}

			break;
		}
		case 0x0195: // No longer viewing contents
			{
				DWORD container_id = pReader->Read<DWORD>();
				if (pReader->GetLastError())
					break;

				NoLongerViewingContents(container_id);
				break;
			}
		case 0x019C: // Add item to shortcut bar
			{
				ShortCutData data;
				data.UnPack(pReader);
				if (pReader->GetLastError())
					break;

				m_pPlayer->_playerModule.AddShortCut(data);
				break;
			}
		case 0x019D: // Add item to shortcut bar
			{
				int index = pReader->Read<int>();
				if (pReader->GetLastError())
					break;

				m_pPlayer->_playerModule.RemoveShortCut(index);
				break;
			}
		case 0x01A1:
			{
				PlayerModule module;
				if (!module.UnPack(pReader) || pReader->GetLastError())
					break;

				m_pPlayer->UpdateModuleFromClient(module);
				break;
			}
		case 0x01B7: // Cancel attack
		{
			// TODO
			m_pPlayer->TryCancelAttack();
			break;
		}
		case 0x01BF: // Request health update
		{
			DWORD target_id = pReader->ReadDWORD();

			if (pReader->GetLastError())
				break;

			RequestHealthUpdate(target_id);
			break;
		}
		case 0x01DF: // Indirect Text (@me)
		{
			char *msg = pReader->ReadString();

			if (pReader->GetLastError())
				break;

			if (CheckForChatSpam())
			{
				std::string filteredText = FilterBadChatCharacters(msg);
				EmoteText(filteredText.c_str());
			}

			break;
		}
		case 0x01E1: // Emote Text (*laugh* sends 'laughs')
		{
			char *msg = pReader->ReadString();

			if (pReader->GetLastError())
				break;

			if (CheckForChatSpam())
			{
				std::string filteredText = FilterBadChatCharacters(msg);
				ActionText(filteredText.c_str());
			}

			break;
		}
		case 0x01E3: // Add item to spell bar
			{
				DWORD spellID = pReader->Read<DWORD>();
				int index = pReader->Read<int>();
				int spellBar = pReader->Read<int>();
				if (pReader->GetLastError())
					break;

				m_pPlayer->_playerModule.AddSpellFavorite(spellID, index, spellBar);
				break;
			}
		case 0x01E4: // Remove item from spell bar
			{
				DWORD spellID = pReader->Read<DWORD>();
				int spellBar = pReader->Read<int>();
				if (pReader->GetLastError())
					break;

				m_pPlayer->_playerModule.RemoveSpellFavorite(spellID, spellBar);
				break;
			}
		case 0x01E9: // Ping
		{
			Ping();
			break;
		}
		case 0x1F6: // Open Trade Negotiations
		{
			if (m_pPlayer->GetTradeManager() != NULL)
			{
				//already trading
				return;
			}

			CPlayerWeenie *pOther = g_pWorld->FindWithinPVS(m_pPlayer, pReader->Read<DWORD>())->AsPlayer();

			if (!pOther)
			{
				// cannot open trade
				m_pPlayer->SendText("Unable to open trade.", LTT_ERROR);
			}
			else if (pOther->_playerModule.options_ & 0x20000)
			{
				SendText((pOther->GetName() + " has disabled trading.").c_str(), LTT_ERROR);
			}
			else if (pOther->IsBusyOrInAction())
			{
				SendText((pOther->GetName() + " is busy.").c_str(), LTT_ERROR);
			}
			else if (pOther->GetTradeManager() != NULL)
			{
				SendText((pOther->GetName() + " is already trading with someone else!").c_str(), LTT_ERROR);
			}
			else if (m_pPlayer->DistanceTo(pOther, true) > 1)
			{
				SendText((pOther->GetName() + " is too far away!").c_str(), LTT_ERROR);
			}
			else
			{
				TradeManager *tm = TradeManager::RegisterTrade(m_pPlayer, pOther);

				m_pPlayer->SetTradeManager(tm);
				pOther->SetTradeManager(tm);
			}
			break;
		}
		case 0x1F7: // Close Trade Negotiations
		{
			TradeManager* tm = m_pPlayer->GetTradeManager();
			if (tm)
			{
				tm->CloseTrade(m_pPlayer);
				return;
			}
			break;
		}
		case 0x1F8: // AddToTrade
		{
			TradeManager* tm = m_pPlayer->GetTradeManager();
			if (tm)
			{
				DWORD item = pReader->Read<DWORD>();

				tm->AddToTrade(m_pPlayer, item);
			}
			break;
		}
		case 0x1FA: // Accept trade
		{
			TradeManager* tm = m_pPlayer->GetTradeManager();
			if (tm)
			{
				tm->AcceptTrade(m_pPlayer);
			}
			break;
		}
		case 0x1FB: // Decline trade
		{
			TradeManager* tm = m_pPlayer->GetTradeManager();
			if (tm)
			{
				tm->DeclineTrade(m_pPlayer);
			}
			break;
		}
		case 0x204: // Reset trade
		{
			TradeManager* tm = m_pPlayer->GetTradeManager();
			if (tm)
			{
				tm->ResetTrade(m_pPlayer);
			}
			break;
		}

		case 0x021C: //House_BuyHouse 
			{
				DWORD slumlord = pReader->Read<DWORD>();
			
				// TODO sanity check on the number of items here
				PackableList<DWORD> items;
				items.UnPack(pReader);

				if (pReader->GetLastError())
					break;

				HouseBuy(slumlord, items);
				break;
			}
		case 0x021F: //House_AbandonHouse 
		{
			HouseAbandon();
			break;
		}
		case 0x21E: //House_QueryHouse 
		{
			HouseRequestData();
			break;
		}
		case 0x0221: //House_RentHouse 
			{
				DWORD slumlord = pReader->Read<DWORD>();

				// TODO sanity check on the number of items here
				PackableList<DWORD> items;
				items.UnPack(pReader);

				if (pReader->GetLastError())
					break;

				//HouseRent(slumlord, items);
				break;
			}
		case 0x0245: //House_AddPermanentGuest 
		{
			std::string name = pReader->ReadString();

			if (pReader->GetLastError())
				break;

			HouseAddPersonToAccessList(name);
			break;
		}
		case 0x0246: //House_RemovePermanentGuest
		{
			std::string name = pReader->ReadString();

			if (pReader->GetLastError())
				break;

			HouseRemovePersonFromAccessList(name);
			break;
		}
		case 0x0247: //House_SetOpenHouseStatus
		{
			int newSetting = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			HouseToggleOpenAccess(newSetting > 0);
			break;
		}
		case 0x0249: //House_ChangeStoragePermission
		{
			std::string name = pReader->ReadString();
			int isAdd = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			HouseAddOrRemovePersonToStorageList(name, isAdd > 0);
			break;
		}
		case 0x024C: //House_RemoveAllStoragePermission 
		{
			HouseClearStorageAccess();
			break;
		}
		case 0x024D: //House_RequestFullGuestList
		{
			HouseRequestAccessList();
			break;
		}
		case 0x0255: //Request allegiance MOTD
		{
			SendAllegianceMOTD();
			break;
		}
		case 0x025C: //House_AddAllStoragePermission
		{
			HouseToggleOpenStorageAccess();
			break;
		}
		case 0x025E: //House_RemoveAllPermanentGuests
		{
			HouseClearAccessList();
			break;
		}
		case 0x0262: // House Recall
		{
			HouseRecall();
			break;
		}		
		case 0x0263: // Request Item Mana
		{
			DWORD itemId = pReader->ReadDWORD();

			if (pReader->GetLastError())
				break;

			m_pPlayer->HandleItemManaRequest(itemId);
			break;
		}
		case 0x0266: // House_SetHooksVisibility 
		{
			int newSetting = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			HouseToggleHooks(newSetting > 0);
			break;
		}
		case 0x0267: //House_ModifyAllegianceGuestPermission 
		{
			int newSetting = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			HouseAddOrRemoveAllegianceToAccessList(newSetting > 0);
			break;
		}
		case 0x0268: //House_ModifyAllegianceStoragePermission
		{
			int newSetting = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			HouseAddOrRemoveAllegianceToStorageList(newSetting > 0);
			break;
		}
		case 0x0278: // House_TeleToMansion
		{
			HouseMansionRecall();
			break;
		}
		case 0x0279: // "/die" command
		{
			if (!m_pPlayer->IsDead() && !m_pPlayer->IsInPortalSpace() && !m_pPlayer->IsBusyOrInAction())
			{
				// this is a bad way of doing this...
				m_pPlayer->SetHealth(0, true);
				m_pPlayer->OnDeath(m_pPlayer->GetID());
			}

			break;
		}
		case 0x027B: // allegiance info request
			{
				std::string target = pReader->ReadString();
				if (target.empty() || pReader->GetLastError())
					break;

				AllegianceInfoRequest(target);
				break;
			}
		case 0x0286: // "/die" command
			{
				DWORD filters = pReader->Read<DWORD>();
				if (pReader->GetLastError())
					break;

				m_pPlayer->_playerModule.spell_filters_ = filters;
				break;
			}
		case 0x028D: // Marketplace Recall
		{
			MarketplaceRecall();
			break;
		}
		case 0x0290: // "Fellowship Assign New Leader"
			{
				DWORD target_id = pReader->Read<DWORD>();

				if (pReader->GetLastError())
					break;

				TryFellowshipAssignNewLeader(target_id);
				break;
			}
		case 0x0291: // "Fellowship Change Openness"
			{
				int open = pReader->Read<int>();

				if (pReader->GetLastError())
					break;

				TryFellowshipChangeOpenness(open);
				break;
			}
		case 0x02AB: //Allegiance_RecallAllegianceHometown (bindstone)
		{
			AllegianceHometownRecall();
			break;
		}
		case 0xF61B: // Jump Movement
		{
			float extent = pReader->Read<float>(); // extent

			Vector jumpVelocity;
			jumpVelocity.UnPack(pReader);

			Position position;
			position.UnPack(pReader);

			if (pReader->GetLastError())
				break;

			// CTransition *transition = m_pPlayer->transition(&m_pPlayer->m_Position, &position, 0);

			/*
			CTransition *transit = m_pPlayer->transition(&m_pPlayer->m_Position, &position, 0);
			if (transit)
			{
				m_pPlayer->SetPositionInternal(transit);
			}
			*/
			
			/*
			double dist = m_pPlayer->m_Position.distance(position);
			if (dist >= 5)
			{
				m_pPlayer->_force_position_timestamp++;
				m_pPlayer->Movement_UpdatePos();

				m_pPlayer->SendText(csprintf("Correcting position due to jump %f", dist), LTT_DEFAULT);
			}
			*/

			double dist = m_pPlayer->m_Position.distance(position);
			if (dist >= 10)
			{
				// Snap them back to their previous position
				m_pPlayer->_force_position_timestamp++;
			}
			else
			{
				m_pPlayer->SetPositionSimple(&position, TRUE);
				
				/*
				CTransition *transit = m_pPlayer->transition(&m_pPlayer->m_Position, &position, 0);
				if (transit)
				{
					m_pPlayer->SetPositionInternal(transit);
				}
				*/
			}

			// m_pPlayer->m_Position = position;

			/*
			long stamina = 0;
			m_pPlayer->get_minterp()->jump(extent, stamina);
			if (stamina > 0)
			{
				unsigned int curStamina = m_pPlayer->GetStamina();

				if (stamina > curStamina)
				{
					// should maybe change the extent? idk
					m_pPlayer->SetStamina(0);
				}
				else
					m_pPlayer->SetStamina(curStamina - stamina);
			}

			m_pPlayer->Animation_Jump(extent, jumpVelocity);
			m_pPlayer->m_bAnimUpdate = TRUE;
			*/
			long stamina = 0;
			if (m_pPlayer->JumpStaminaCost(extent, stamina) && stamina > 0)
			{
				unsigned int curStamina = m_pPlayer->GetStamina();

				if (stamina > curStamina)
				{
					// should maybe change the extent? idk
					m_pPlayer->SetStamina(0);
				}
				else
					m_pPlayer->SetStamina(curStamina - stamina);
			}
			
			m_pPlayer->transient_state &= ~((DWORD)TransientState::CONTACT_TS);
			m_pPlayer->transient_state &= ~((DWORD)WATER_CONTACT_TS);
			m_pPlayer->calc_acceleration();
			m_pPlayer->set_on_walkable(FALSE);
			m_pPlayer->set_local_velocity(jumpVelocity, 0);

			/*
			Vector localVel = m_pPlayer->get_local_physics_velocity();
			Vector vel = m_pPlayer->m_velocityVector;
			m_pPlayer->EmoteLocal(csprintf("Received jump with velocity %.1f %.1f %.1f (my lv: %.1f %.1f %.1f my v: %.1f %.1f %.1f)",
				jumpVelocity.x, jumpVelocity.y, jumpVelocity.z,
				localVel.x, localVel.y, localVel.z,
				vel.x, vel.y, vel.z));
				*/

			m_pPlayer->Movement_UpdateVector();

			break;
		}
		case 0xF61C: // CM_Movement__Event_MoveToState (update vector movement?)
		{
			// TODO: Cancel attack
			
			MoveToStatePack moveToState;
			moveToState.UnPack(pReader);

			if (pReader->GetLastError())
			{
				LOG_PRIVATE(Animation, Warning, "Bad animation message!\n");
				LOG_PRIVATE_BYTES(Animation, Verbose, pReader->GetDataStart(), pReader->GetDataLen());
				break;
			}

			//if (is_newer_event_stamp(moveToState.server_control_timestamp, m_pPlayer->_server_control_timestamp))
			//{
				// LOG(Temp, Normal, "Old server control timestamp on 0xF61C. Ignoring.\n");
			//	break;
			//}

			if (is_newer_event_stamp(moveToState.teleport_timestamp, m_pPlayer->_teleport_timestamp))
			{
				LOG_PRIVATE(Temp, Normal, "Old teleport timestamp on 0xF61C. Ignoring.\n");
				break;
			}
			if (is_newer_event_stamp(moveToState.force_position_ts, m_pPlayer->_force_position_timestamp))
			{
				LOG_PRIVATE(Temp, Normal, "Old force position timestamp on 0xF61C. Ignoring.\n");
				break;
			}

			if (m_pPlayer->IsDead())
			{
				LOG_PRIVATE(Temp, Normal, "Dead players can't move. Ignoring.\n");
				break;
			}

			/*
			CTransition *transit = m_pPlayer->transition(&m_pPlayer->m_Position, &moveToState.position, 0);
			if (transit)
			{
				m_pPlayer->SetPositionInternal(transit);
			}
			*/

			/*
			double dist = m_pPlayer->m_Position.distance(moveToState.position);
			if (dist >= 5)
			{
				m_pPlayer->_force_position_timestamp++;
				m_pPlayer->Movement_UpdatePos();

				m_pPlayer->SendText(csprintf("Correcting position due to state %f", dist), LTT_DEFAULT);
			}
			*/

			/*
			bool bHasCell = m_pPlayer->cell ? true : false;
			m_pPlayer->SetPositionSimple(&moveToState.position, TRUE);
			if (!m_pPlayer->cell && bHasCell)
			{
				m_pPlayer->SendText("Damnet...", LTT_DEFAULT);
			}
			*/

			double dist = m_pPlayer->m_Position.distance(moveToState.position);
			if (dist >= 10)
			{
				// Snap them back to their previous position
				m_pPlayer->_force_position_timestamp++;
			}
			else
			{
				m_pPlayer->SetPositionSimple(&moveToState.position, TRUE);

				/*
				CTransition *transit = m_pPlayer->transition(&m_pPlayer->m_Position, &moveToState.position, 0);
				if (transit)
				{
					m_pPlayer->SetPositionInternal(transit);
				}
				*/
			}

			// m_pPlayer->m_Position = moveToState.position; // should interpolate to this, but oh well

			/*
			if (moveToState.contact)
			{
				m_pPlayer->transient_state |= ((DWORD)TransientState::CONTACT_TS);
			}
			else
			{
				m_pPlayer->transient_state &= ~((DWORD)TransientState::CONTACT_TS);
				m_pPlayer->transient_state &= ~((DWORD)WATER_CONTACT_TS);
			}
			m_pPlayer->calc_acceleration();
			m_pPlayer->set_on_walkable(moveToState.contact);

			m_pPlayer->get_minterp()->standing_longjump = moveToState.longjump_mode ? TRUE : FALSE;
			*/

			m_pPlayer->last_move_was_autonomous = true;
			m_pPlayer->cancel_moveto();

			if (!(moveToState.raw_motion_state.current_style & CM_Style) && moveToState.raw_motion_state.current_style)
			{
				LOG_PRIVATE(Client, Warning, "Bad style received %08X\n", moveToState.raw_motion_state.current_style);
				break;
			}

			if (moveToState.raw_motion_state.forward_command & CM_Action)
			{
				LOG_PRIVATE(Client, Warning, "Bad forward command received %08X\n", moveToState.raw_motion_state.forward_command);
				break;
			}

			if (moveToState.raw_motion_state.sidestep_command & CM_Action)
			{
				LOG_PRIVATE(Client, Warning, "Bad sidestep command received %08X\n", moveToState.raw_motion_state.sidestep_command);
				break;
			}

			if (moveToState.raw_motion_state.turn_command & CM_Action)
			{
				LOG_PRIVATE(Client, Warning, "Bad turn command received %08X\n", moveToState.raw_motion_state.turn_command);
				break;
			}

			CMotionInterp *minterp = m_pPlayer->get_minterp();
			minterp->raw_state = moveToState.raw_motion_state;
			minterp->apply_raw_movement(TRUE, minterp->motion_allows_jump(minterp->interpreted_state.forward_command != 0));

			WORD newestActionStamp = m_MoveActionStamp;

			for (const auto &actionNew : moveToState.raw_motion_state.actions)
			{
				if (m_pPlayer->get_minterp()->interpreted_state.GetNumActions() >= MAX_EMOTE_QUEUE)
					break;

				if (is_newer_event_stamp(newestActionStamp, actionNew.stamp))
				{
					DWORD commandID = GetCommandID(actionNew.action);

					if (!(commandID & CM_Action) || !(commandID & CM_ChatEmote))
					{
						LOG_PRIVATE(Client, Warning, "Bad action received %08X\n", commandID);
						continue;
					}

					MovementParameters params;
					params.action_stamp = ++m_pPlayer->m_wAnimSequence;
					params.autonomous = 1;
					params.speed = actionNew.speed;
					m_pPlayer->get_minterp()->DoMotion(commandID, &params);

					// minterp->interpreted_state.AddAction(ActionNode(actionNew.action, actionNew.speed, ++m_pPlayer->m_wAnimSequence, TRUE));

					// newestActionStamp = actionNew.stamp;
					// m_pPlayer->Animation_PlayEmote(actionNew.action, actionNew.speed);
				}
			}

			m_MoveActionStamp = newestActionStamp;

			// m_pPlayer->Movement_UpdatePos();
			m_pPlayer->Animation_Update();
			// m_pPlayer->m_bAnimUpdate = TRUE;

			// m_pPlayer->Movement_UpdatePos();
			break;
		}
		case 0xF753: // Update Exact Position
		{
			Position position;
			position.UnPack(pReader);

			WORD instance = pReader->ReadWORD();

			if (pReader->GetLastError())
				break;

			if (instance != m_pPlayer->_instance_timestamp)
			{
				LOG_PRIVATE(Temp, Normal, "Bad instance.\n");
				break;
			}

			WORD server_control_timestamp = pReader->ReadWORD();
			if (pReader->GetLastError())
				break;
			//if (is_newer_event_stamp(server_control_timestamp, m_pPlayer->_server_control_timestamp))
			//{
			//	LOG(Temp, Normal, "Old server control timestamp. Ignoring.\n");
			//	break;
			//}

			WORD teleport_timestamp = pReader->ReadWORD();
			if (pReader->GetLastError())
				break;
			if (is_newer_event_stamp(teleport_timestamp, m_pPlayer->_teleport_timestamp))
			{
				LOG_PRIVATE(Temp, Normal, "Old teleport timestamp. Ignoring.\n");
				break;
			}

			WORD force_position_ts = pReader->ReadWORD();
			if (pReader->GetLastError())
				break;
			if (is_newer_event_stamp(force_position_ts, m_pPlayer->_force_position_timestamp))
			{
				LOG_PRIVATE(Temp, Normal, "Old force position timestamp. Ignoring.\n");
				break;
			}

			BOOL bHasContact = pReader->ReadBYTE() ? TRUE : FALSE;
			if (pReader->GetLastError())
				break;
			
			double dist =m_pPlayer->m_Position.distance(position);
			if (dist >= 10)
			{
				// Snap them back to their previous position
				m_pPlayer->_force_position_timestamp++;
				// m_pPlayer->SendText(csprintf("Correcting position due to position update %f", dist), LTT_DEFAULT);
			}
			else
			{
				/*
				CTransition *transit = m_pPlayer->transition(&m_pPlayer->m_Position, &position, 0);
				if (transit)
				{
					m_pPlayer->SetPositionInternal(transit);
					*/
					/*
					double distFromClient = m_pPlayer->m_Position.distance(position);

					if (distFromClient >= 3.0)
					{
						m_pPlayer->_force_position_timestamp++;
					}
				}
				*/

				m_pPlayer->SetPositionSimple(&position, TRUE);

				/*
				if (!m_pPlayer->cell && bHasCell)
				{
					m_pPlayer->SendText("Damnet...", LTT_DEFAULT);
				}
				*/
				// m_pPlayer->m_Position = position; // should interpolate this, not set this directly, but oh well
			
			}

			if (bHasContact)
			{
				m_pPlayer->transient_state |= ((DWORD)TransientState::CONTACT_TS);
			}
			else
			{
				m_pPlayer->transient_state &= ~((DWORD)TransientState::CONTACT_TS);
				m_pPlayer->transient_state &= ~((DWORD)WATER_CONTACT_TS);
			}
			m_pPlayer->calc_acceleration();
			m_pPlayer->set_on_walkable(bHasContact);

			m_pPlayer->Movement_UpdatePos();
			break;
		}
		default:
		{
			//Unknown Event
#ifdef _DEBUG
			LOG(Temp, Normal, "Unhandled client event 0x%X:\n", dwEvent);
#endif
			// LOG_BYTES(Temp, Verbose, in->GetDataPtr(), in->GetDataEnd() - in->GetDataPtr() );
		}
	}
}








