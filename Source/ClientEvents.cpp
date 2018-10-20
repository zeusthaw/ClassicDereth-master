
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

#include "WeenieDefaults.h"
#include "WeenieObject.h"
#include "Monster.h"
#include "Player.h"
#include "PhatDataBin.h"
#include "ChatMsgs.h"
#include "Movement.h"
#include "EmoteManager.h"
#include "MovementManager.h"
#include "Vendor.h"
#include "AllegianceManager.h"
#include "House.h"
#include "SpellcastingManager.h"
#include "TradeManager.h"
#include "ChessManager.h"
#include <chrono>

#include "Config.h"
#include "Messages.h"

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

std::string GetAgeString(int age)
{
	int mo = age / 2629800; // seconds in a month
	int leftover = age % 2629800;
	int d = leftover / 86400; // seconds in a day
	leftover %= 86400;
	int h = leftover / 3600; // seconds in an hour
	leftover %= 3600;
	int m = leftover / 60; // seconds in a minute
	leftover %= 60;
	int s = leftover;
	std::string out = "You have played for ";
	if (mo)
		out += csprintf("%dmo ", mo);
	if (d)
		out += csprintf("%dd ", d);
	if (h)
		out += csprintf("%dh ", h);
	if (m)
		out += csprintf("%dm ", m);
	out += csprintf("%ds.", s);
	return out;
}

void CClientEvents::ExitWorld()
{
	auto t = chrono::system_clock::to_time_t(chrono::system_clock::now());
	m_pPlayer->m_Qualities.SetInt(LOGOFF_TIMESTAMP_INT, t);

	DetachPlayer();
	m_pClient->ExitWorld();
}

void CClientEvents::Think()
{
	if (m_pPlayer)
	{
		// update in-game age
		int time_now = chrono::system_clock::to_time_t(chrono::system_clock::now());

		if (time_now > (last_age_update + 15))
		{
			int age = m_pPlayer->m_Qualities.GetInt(AGE_INT, 0);
			int time_diff = time_now - last_age_update;
			age += time_diff;
			m_pPlayer->m_Qualities.SetInt(AGE_INT, age);
			m_pPlayer->NotifyIntStatUpdated(AGE_INT);
			last_age_update = time_now;
		}

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
		SERVER_WARN << szAccount << "attempting to log in with a character that doesn't belong to this account";
		return;
	}

	if (m_pPlayer || g_pWorld->FindPlayer(char_weenie_id))
	{
		// LOG(Temp, Normal, "Character already logged in!\n");
		LoginError(13); // update error codes
		SERVER_WARN << szAccount << "Login request, but character already logged in!";
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
		SERVER_WARN << szAccount << "Login request, but character failed to load!";

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

	m_pPlayer->SendText("GDLEnhanced - CLASSIC " SERVER_VERSION_NUMBER_STRING " " SERVER_VERSION_STRING, LTT_DEFAULT);
	m_pPlayer->SendText("Maintained by the GDLE Development Team and Zeus. Contact us at https://discord.gg/WzGX348", LTT_DEFAULT);
	m_pPlayer->SendText("Powered by GamesDeadLol. Not an official Asheron's Call server.", LTT_DEFAULT);
	//SendAllegianceMOTD();

	/*
	if (*g_pConfig->WelcomeMessage() != 0)
	{
	m_pPlayer->SendText(g_pConfig->WelcomeMessage(), LTT_DEFAULT);
	}
	*/

	g_pWorld->CreateEntity(m_pPlayer);

	time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

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

		if (g_pConfig->InventoryPurgeOnLogin())
		{
			if (wielded->m_Qualities.id == g_pConfig->WcidForPurge() || wielded->m_Qualities.id == 33257 || wielded->m_Qualities.id == 41201 || wielded->m_Qualities.id == 41200 || wielded->m_Qualities.id == 41199 || wielded->m_Qualities.id == 41198 || wielded->m_Qualities.id == 41197 || wielded->m_Qualities.id == 48919 || wielded->m_Qualities.id == 32984 || wielded->m_Qualities.id == 33053 || wielded->m_Qualities.id == 33116 || wielded->m_Qualities.id == 41889 || wielded->m_Qualities.id == 35916 || wielded->m_Qualities.id == 35173 || wielded->m_Qualities.id == 43056 || wielded->m_Qualities.id == 27092 || wielded->m_Qualities.id == 41885 || wielded->m_Qualities.id == 41886 || wielded->m_Qualities.id == 37478 || wielded->m_Qualities.id == 34277 || wielded->m_Qualities.id == 27305 || wielded->m_Qualities.id == 38456 || wielded->m_Qualities.id == 49563)
			{
				wielded->_timeToRot = Timer::cur_time; wielded->_beganRot = true;
			}
		}

		// Remove all expired items, or set _timeToRot if not yet expired.
		int lifespan = wielded->m_Qualities.GetInt(LIFESPAN_INT, 0);

		if (lifespan)
		{
			int creationTime = wielded->m_Qualities.GetInt(CREATION_TIMESTAMP_INT, 0);
			int lived = t - creationTime;

			if (creationTime && lived >= lifespan)
			{
				wielded->_timeToRot = Timer::cur_time;
			}
			else
			{
				int lifespanRemaining = lifespan - lived;
				wielded->_timeToRot = Timer::cur_time + lifespanRemaining;
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
				wielded->m_Qualities.SetDataID(ICON_DID, 0x006006852);//Elemental Staff Icon//
				wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
				wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_SLASHING);//Elemental UI Effect//
				wielded->m_Qualities.SetString(NAME_STRING, "Slashing Staff");//Re-Write the name to match Item//
				wielded->m_Qualities.SetDataID(SETUP_DID, 0x02001850);

			}
			if (wielded->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffPierce) && staffPierce == PIERCE_DAMAGE_TYPE && wielded->InqStringQuality(NAME_STRING, "") == "Staff")
			{
				wielded->m_Qualities.SetDataID(ICON_DID, 0x00600685B);//Elemental Staff Icon//
				wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
				wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_PIERCING);//Elemental UI Effect//
				wielded->m_Qualities.SetString(NAME_STRING, "Piercing Staff");//Re-Write the name to match Item//
				wielded->m_Qualities.SetDataID(SETUP_DID, 0x0200184F);
			}
			if (wielded->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffBludge) && staffBludge == BLUDGEON_DAMAGE_TYPE && wielded->InqStringQuality(NAME_STRING, "") == "Staff")
			{
				wielded->m_Qualities.SetDataID(ICON_DID, 0x006006853);//Elemental Staff Icon//
				wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
				wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_BLUDGEONING);//Elemental UI Effect//
				wielded->m_Qualities.SetString(NAME_STRING, "Bludgeoning Staff");//Re-Write the name to match Item//
				wielded->m_Qualities.SetDataID(SETUP_DID, 0x0200184B);
			}
			if (wielded->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffAcid) && staffAcid == ACID_DAMAGE_TYPE && wielded->InqStringQuality(NAME_STRING, "") == "Staff")
			{
				wielded->m_Qualities.SetDataID(ICON_DID, 0x006006855);//Elemental Staff Icon//
				wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
				wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
				wielded->m_Qualities.SetString(NAME_STRING, "Acid Staff");//Re-Write the name to match Item//
				wielded->m_Qualities.SetDataID(SETUP_DID, 0x0200184A);
			}
			if (wielded->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffElectric) && staffElectric == ELECTRIC_DAMAGE_TYPE && wielded->InqStringQuality(NAME_STRING, "") == "Staff")
			{
				wielded->m_Qualities.SetDataID(ICON_DID, 0x006006858);//Elemental Staff Icon//
				wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
				wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Elemental UI Effect//
				wielded->m_Qualities.SetString(NAME_STRING, "Electric Staff");//Re-Write the name to match Item//
				wielded->m_Qualities.SetDataID(SETUP_DID, 0x0200184C);
			}
			if (wielded->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffFrost) && staffFrost == COLD_DAMAGE_TYPE && wielded->InqStringQuality(NAME_STRING, "") == "Staff")
			{
				wielded->m_Qualities.SetDataID(ICON_DID, 0x006006857);//Elemental Staff Icon//
				wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
				wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
				wielded->m_Qualities.SetString(NAME_STRING, "Frost Staff");//Re-Write the name to match Item//
				wielded->m_Qualities.SetDataID(SETUP_DID, 0x0200184E);
			}
			if (wielded->m_Qualities.InqInt(DAMAGE_TYPE_INT, staffFlame) && staffFlame == FIRE_DAMAGE_TYPE && wielded->InqStringQuality(NAME_STRING, "") == "Staff")
			{
				wielded->m_Qualities.SetDataID(ICON_DID, 0x006006854);//Elemental Staff Icon//
				wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x04000BEF);//Elemental Staff Pallete//
				wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
				wielded->m_Qualities.SetString(NAME_STRING, "Fire Staff");//Re-Write the name to match Item//
				wielded->m_Qualities.SetDataID(SETUP_DID, 0x0200184D);
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
				//wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060AC);//Compound Bow Icon//
				wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
				wielded->m_Qualities.SetString(NAME_STRING, "Compound Bow");//Re-Write the name to match Item//

			}
			if (wielded->m_Qualities.InqDataID(SETUP_DID, compoundBowSlash) && compoundBowSlash == 0x02001488)
			{
				//wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060AC);//Compound Bow Icon//
				wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
				wielded->m_Qualities.SetString(NAME_STRING, "Slicing Compound Bow");//Re-Write the name to match Item//
				wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_SLASHING);//Elemental UI Effect//
				wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, SLASH_DAMAGE_TYPE);


			}
			if (wielded->m_Qualities.InqDataID(SETUP_DID, compoundBowPierce) && compoundBowPierce == 0x0200148A)
			{
				//wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060AD);//Compound Bow Icon//
				wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
				wielded->m_Qualities.SetString(NAME_STRING, "Piercing Compound Bow");//Re-Write the name to match Item//
				wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_PIERCING);//Elemental UI Effect//
				wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, PIERCE_DAMAGE_TYPE);


			}
			if (wielded->m_Qualities.InqDataID(SETUP_DID, compoundBowBludge) && compoundBowBludge == 0x02001489)
			{
				//wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060B1);//Compound Bow Icon//
				wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
				wielded->m_Qualities.SetString(NAME_STRING, "Smashing Compound Bow");//Re-Write the name to match Item//
				wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_BLUDGEONING);//Elemental UI Effect//
				wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, BLUDGEON_DAMAGE_TYPE);

			}
			if (wielded->m_Qualities.InqDataID(SETUP_DID, compoundBowAcid) && compoundBowAcid == 0x02001475)
			{
				//wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060AE);//Compound Bow Icon//
				wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
				wielded->m_Qualities.SetString(NAME_STRING, "Corrosive Compound Bow");//Re-Write the name to match Item//
				wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_ACID);//Elemental UI Effect//
				wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, ACID_DAMAGE_TYPE);

			}
			if (wielded->m_Qualities.InqDataID(SETUP_DID, compoundBowElectric) && compoundBowElectric == 0x02001472)
			{
				//wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060AF);//Compound Bow Icon//
				wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
				wielded->m_Qualities.SetString(NAME_STRING, "Sparking Compound Bow");//Re-Write the name to match Item//
				wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_LIGHTNING);//Elemental UI Effect//
				wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, ELECTRIC_DAMAGE_TYPE);

			}
			if (wielded->m_Qualities.InqDataID(SETUP_DID, compoundBowFrost) && compoundBowFrost == 0x02001473)
			{
				//wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060AA);//Compound Bow Icon//
				wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
				wielded->m_Qualities.SetString(NAME_STRING, "Chilling Compound Bow");//Re-Write the name to match Item//
				wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FROST);//Elemental UI Effect//
				wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, COLD_DAMAGE_TYPE);

			}
			if (wielded->m_Qualities.InqDataID(SETUP_DID, compoundBowFlame) && compoundBowFlame == 0x02001474)
			{
				//wielded->m_Qualities.SetDataID(ICON_DID, 0x0060060B0);//Compound Bow Icon//
				wielded->m_Qualities.SetDataID(PALETTE_BASE_DID, 0x01000062D);//Compound Bow Pallete//
				wielded->m_Qualities.SetString(NAME_STRING, "Searing Compound Bow");//Re-Write the name to match Item//
				wielded->m_Qualities.SetInt(UI_EFFECTS_INT, UI_EFFECT_FIRE);//Elemental UI Effect//
				wielded->m_Qualities.SetInt(DAMAGE_TYPE_INT, FIRE_DAMAGE_TYPE);

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

				// Remove all loot items with wcid > g_pConfig->WcidForPurge()
				if (g_pConfig->InventoryPurgeOnLogin())
				{
					if (item->m_Qualities.id == g_pConfig->WcidForPurge() || item->m_Qualities.id == 33257 || item->m_Qualities.id == 41201 || item->m_Qualities.id == 41200 || item->m_Qualities.id == 41199 || item->m_Qualities.id == 41198 || item->m_Qualities.id == 41197 || item->m_Qualities.id == 48919 || item->m_Qualities.id == 32984 || item->m_Qualities.id == 33053 || item->m_Qualities.id == 33116 || item->m_Qualities.id == 41889 || item->m_Qualities.id == 35916 || item->m_Qualities.id == 35173 || item->m_Qualities.id == 43056 || item->m_Qualities.id == 27092 || item->m_Qualities.id == 41885 || item->m_Qualities.id == 41886 || item->m_Qualities.id == 37478 || item->m_Qualities.id == 34277 || item->m_Qualities.id == 27305 || item->m_Qualities.id == 38456 || item->m_Qualities.id == 49563)
					{
						item->_timeToRot = Timer::cur_time; item->_beganRot = true;
					}
				}


				// Remove all expired items, or set _timeToRot if not yet expired.
				int lifespan = item->m_Qualities.GetInt(LIFESPAN_INT, 0);

				if (lifespan)
				{
					int creationTime = item->m_Qualities.GetInt(CREATION_TIMESTAMP_INT, 0);
					int lived = t - creationTime;

					if (creationTime && lived >= lifespan)
					{
						item->_timeToRot = Timer::cur_time;
					}
					else
					{
						int lifespanRemaining = lifespan - lived;
						item->_timeToRot = Timer::cur_time + lifespanRemaining;
					}
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

						if (g_pConfig->InventoryPurgeOnLogin())
						{
							if (item->m_Qualities.id == g_pConfig->WcidForPurge() || item->m_Qualities.id == 33257 || item->m_Qualities.id == 41201 || item->m_Qualities.id == 41200 || item->m_Qualities.id == 41199 || item->m_Qualities.id == 41198 || item->m_Qualities.id == 41197 || item->m_Qualities.id == 48919 || item->m_Qualities.id == 32984 || item->m_Qualities.id == 33053 || item->m_Qualities.id == 33116 || item->m_Qualities.id == 41889 || item->m_Qualities.id == 35916 || item->m_Qualities.id == 35173 || item->m_Qualities.id == 43056 || item->m_Qualities.id == 27092 || item->m_Qualities.id == 41885 || item->m_Qualities.id == 41886 || item->m_Qualities.id == 37478 || item->m_Qualities.id == 34277 || item->m_Qualities.id == 27305 || item->m_Qualities.id == 38456 || item->m_Qualities.id == 49563)
							{
								item->_timeToRot = Timer::cur_time; item->_beganRot = true;
							}
						}


						// Remove all expired items, or set _timeToRot if not yet expired.
						int lifespan = item->m_Qualities.GetInt(LIFESPAN_INT, 0);

						if (lifespan)
						{
							int creationTime = item->m_Qualities.GetInt(CREATION_TIMESTAMP_INT, 0);
							int lived = t - creationTime;

							if (creationTime && lived >= lifespan)
							{
								item->_timeToRot = Timer::cur_time;
							}
							else
							{
								int lifespanRemaining = lifespan - lived;
								item->_timeToRot = Timer::cur_time + lifespanRemaining;
							}

						}
					}
				}


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
		}
	}
}

void CClientEvents::SendText(const char *szText, long lColor)
{
	m_pClient->SendNetMessage(ServerText(szText, lColor), PRIVATE_MSG, FALSE, TRUE);
}

void CClientEvents::Attack(DWORD target, DWORD height, float power)
{

	if (height <= 0 || height >= ATTACK_HEIGHT::NUM_ATTACK_HEIGHTS)
	{
		SERVER_WARN << "Bad melee attack height" << height << "sent by player"<< m_pPlayer->GetID();
		return;
	}

	if (power < 0.0f || power > 1.0f)
	{
		SERVER_WARN << "Bad melee attack power" << power << "sent by player" << m_pPlayer->GetID();
		return;
	}

	m_pPlayer->TryMeleeAttack(target, (ATTACK_HEIGHT) height, power);
}

void CClientEvents::MissileAttack(DWORD target, DWORD height, float power)
{
	if (height <= 0 || height >= ATTACK_HEIGHT::NUM_ATTACK_HEIGHTS)
	{
		SERVER_WARN << "Bad missile attack height" << height << "sent by player" << m_pPlayer->GetID();
		return;
	}

	if (power < 0.0f || power > 1.0f)
	{
		SERVER_WARN << "Bad missile attack power" << power << "sent by player" << m_pPlayer->GetID();
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
	{	
		CWeenieObject *target = g_pWorld->FindObject(dwGUID);

		if (target && !target->m_Qualities._emote_table->_emote_table.empty())
			target->MakeEmoteManager()->ChanceExecuteEmoteSet(ReceiveTalkDirect_EmoteCategory, szText, m_pPlayer->GetID());

		return;
	}

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

	if (szText[0] == '@' || szText[0] == '/')
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

void CClientEvents::SpendAttributeXP(STypeAttribute key, DWORD exp)
{
	// TODO use attribute map
	if (key < 1 || key > 6)
		return;

	// TODO verify they are trying to spend the correct amount of XP

	__int64 unassignedExp = 0;
	m_pPlayer->m_Qualities.InqInt64(AVAILABLE_EXPERIENCE_INT64, unassignedExp);
	if ((unsigned __int64)unassignedExp < (unsigned __int64)exp)
	{
		// Not enough experience
		return;
	}

	Attribute attr;
	if (!m_pPlayer->m_Qualities.InqAttribute(key, attr))
	{
		// Doesn't have the attribute
		return;
	}

	const DWORD amountNeededForMaxXp = attr.GetXpNeededForMaxXp();

	exp = min(exp, amountNeededForMaxXp);

	if (exp > 0)
	{
		m_pPlayer->GiveAttributeXP(key, exp);
		m_pPlayer->m_Qualities.SetInt64(AVAILABLE_EXPERIENCE_INT64, (unsigned __int64)unassignedExp - (unsigned __int64)exp);

		m_pPlayer->NotifyAttributeStatUpdated(key);
		m_pPlayer->NotifyInt64StatUpdated(AVAILABLE_EXPERIENCE_INT64);
	}

}

void CClientEvents::SpendAttribute2ndXP(STypeAttribute2nd key, DWORD exp)
{
	// TODO use vital map
	if (key != 1 && key != 3 && key != 5)
		return;

	// TODO verify they are trying to spend the correct amount of XP

	__int64 unassignedExp = 0;
	m_pPlayer->m_Qualities.InqInt64(AVAILABLE_EXPERIENCE_INT64, unassignedExp);
	if ((unsigned __int64)unassignedExp < (unsigned __int64)exp)
	{
		// Not enough experience
		return;
	}

	SecondaryAttribute attr;
	if (!m_pPlayer->m_Qualities.InqAttribute2nd(key, attr))
	{
		// Doesn't have the secondary attribute
		return;
	}

	const DWORD amountNeededForMaxXp = attr.GetXpNeededForMaxXp();

	// If the exp is more than is needed to reach max, it is limited to the amount needed to reach max
	// This is done as the client may send more than the amount needed if it is desynced
	exp = min(exp, amountNeededForMaxXp);

	if (exp > 0)
	{
		m_pPlayer->GiveAttribute2ndXP(key, exp);
		m_pPlayer->m_Qualities.SetInt64(AVAILABLE_EXPERIENCE_INT64, (unsigned __int64)unassignedExp - (unsigned __int64)exp);
		m_pPlayer->NotifyInt64StatUpdated(AVAILABLE_EXPERIENCE_INT64);
	}

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

	const DWORD amountNeededForMaxXp = skill.GetXpNeededForMaxXp();

	// If the exp is more than is needed to reach max, it is limited to the amount needed to reach max
	// This is done as the client may send more than the amount needed if it is desynced
	exp = min(exp, amountNeededForMaxXp);


	if (exp > 0)
	{
		// Only give the skill exp if it's not maxed
		m_pPlayer->GiveSkillXP(key, exp);

		m_pPlayer->m_Qualities.SetInt64(AVAILABLE_EXPERIENCE_INT64, (unsigned __int64)unassignedExp - (unsigned __int64)exp);
		m_pPlayer->NotifyInt64StatUpdated(AVAILABLE_EXPERIENCE_INT64);
	}

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
	m_pPlayer->m_Qualities.SetSkillLevel(key, 5);
	m_pPlayer->NotifySkillStatUpdated(key);

	m_pPlayer->m_Qualities.SetInt(AVAILABLE_SKILL_CREDITS_INT, unassignedCredits - costToRaise);
	m_pPlayer->NotifyIntStatUpdated(AVAILABLE_SKILL_CREDITS_INT);
}

void CClientEvents::LifestoneRecall()
{
	if ( m_pPlayer->CheckPKActivity())
	{
		m_pPlayer->SendText("You have been involved in Player Killer combat too recently!", LTT_MAGIC);
		return;
	}

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
	if (m_pPlayer->CheckPKActivity())
	{
		m_pPlayer->SendText("You have been involved in Player Killer combat too recently!", LTT_MAGIC);
		return;
	}

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

void CClientEvents::SetRequestAllegianceUpdate(bool on)
{
	m_bSendAllegianceUpdates = on;
}

void CClientEvents::TryBreakAllegiance(DWORD target)
{
	g_pAllegianceManager->TryBreakAllegiance(m_pPlayer, target);
}

void CClientEvents::TrySwearAllegiance(DWORD target)
{
	CPlayerWeenie *targetWeenie = g_pWorld->FindPlayer(target);
	if (!targetWeenie)
	{
		m_pPlayer->NotifyWeenieError(WERROR_NO_OBJECT);
		return;
	}
	
	 g_pAllegianceManager->TrySwearAllegiance(m_pPlayer, targetWeenie);	
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
	if (m_pPlayer->CheckPKActivity())
	{
		m_pPlayer->SendText("You have been involved in Player Killer combat too recently!", LTT_MAGIC);
		return;
	}

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
		{
			m_pPlayer->ExecuteUseEvent(new CAllegianceHometownRecallUseEvent());
		}
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
	if (m_pPlayer->CheckPKActivity())
	{
		m_pPlayer->SendText("You have been involved in Player Killer combat too recently!", LTT_MAGIC);
		return;
	}

	DWORD houseId = m_pPlayer->GetAccountHouseId();
	if (houseId)
	{
		CHouseData *houseData = g_pHouseManager->GetHouseData(houseId);
		if (m_pPlayer->GetClient() && houseData->_ownerAccount == m_pPlayer->GetClient()->GetAccountInfo().id)
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
	if (m_pPlayer->CheckPKActivity())
	{
		m_pPlayer->SendText("You have been involved in Player Killer combat too recently!", LTT_MAGIC);
		return;
	}

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
	DWORD dwSequence = pReader->ReadDWORD();
	DWORD dwEvent = pReader->ReadDWORD();
	if (pReader->GetLastError()) return;

#ifdef _DEBUG
	DEBUG_DATA << "Processing event:" << dwEvent;
#endif

	switch (dwEvent)
	{
		case CHANGE_PLAYER_OPTION: // Change player option
		{
			DWORD option = pReader->ReadDWORD();
			DWORD value = pReader->ReadDWORD();
			if (pReader->GetLastError())
				break;

			ChangePlayerOption((PlayerOptions)option, value ? true : false);
			break;
		}
		case MELEE_ATTACK: // Melee Attack
		{
			DWORD dwTarget = pReader->ReadDWORD();
			DWORD dwHeight = pReader->ReadDWORD();
			float flPower = pReader->ReadFloat();
			if (pReader->GetLastError()) break;

			Attack(dwTarget, dwHeight, flPower);
			break;
		}
		case MISSILE_ATTACK: // Missile Attack
		{
			DWORD target = pReader->ReadDWORD();
			DWORD height = pReader->ReadDWORD();
			float power = pReader->ReadFloat();
			if (pReader->GetLastError()) break;

			MissileAttack(target, height, power);
			break;
		}
		case SET_AFK_MODE:
		{
			// Read: bool afk - Whether user is afk
			break;
		}
		case SET_AFK_MESSAGE:
		{
			// Read: string message
			break;
		}
		case TEXT_CLIENT: //Client Text
		{
			char *szText = pReader->ReadString();
			if (pReader->GetLastError()) break;

			ClientText(szText);
			break;
		}
		case REMOVE_FRIEND:
		{
			// Read: DWORD friendID
			break;
		}
		case ADD_FRIEND:
		{
			// Read: string friendName
			break;
		}
		case STORE_ITEM: //Store Item
		{
			DWORD dwItemID = pReader->ReadDWORD();
			DWORD dwContainer = pReader->ReadDWORD();
			DWORD dwSlot = pReader->ReadDWORD();
			if (pReader->GetLastError()) break;

			m_pPlayer->MoveItemToContainer(dwItemID, dwContainer, (char)dwSlot);
			break;
		}
		case EQUIP_ITEM: //Equip Item
		{
			DWORD dwItemID = pReader->ReadDWORD();
			DWORD dwCoverage = pReader->ReadDWORD();
			if (pReader->GetLastError()) break;

			m_pPlayer->MoveItemToWield(dwItemID, dwCoverage);
			break;
		}
		case DROP_ITEM: //Drop Item
		{
			DWORD dwItemID = pReader->ReadDWORD();
			if (pReader->GetLastError()) break;

			m_pPlayer->MoveItemTo3D(dwItemID);
			break;
		}
		case ALLEGIANCE_SWEAR: // Swear Allegiance request
		{
			MAllegianceSwear_001D msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_BREAK: // Break Allegiance request
		{
			MAllegianceBreak_001E msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_SEND_UPDATES: // Allegiance Update request
		{
			MAllegianceUpdate_001F msg(m_pClient);
			msg.Parse(pReader);
			break;
		}
		case CLEAR_FRIENDS:
		{
			// Nothing to read
			break;
		}
		case RECALL_PKL_ARENA:
		{
			// Nothing to read
			break;
		}
		case RECALL_PK_ARENA:
		{
			// Nothing to read
			break;
		}
		case SET_DISPLAY_TITLE:
		{
			// Read: uint titleID
			break;
		}
		case CONFIRMATION_RESPONSE: // confirmation response
		{
			// ConfirmationTypes: SwearAllegiance 1, AlterSkill 2, AlterAttribute 3, Fellowship 4, Craft 5, Augmentation 6, YesNo 7
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
			case 0x07:
				CWeenieObject *target = g_pWorld->FindObject(context);
					
					if (target)
					{
						target->MakeEmoteManager()->ConfirmationResponse(accepted, m_pPlayer->id);
					}

			}

			break;
		}
		case UST_SALVAGE_REQUEST: // ust salvage request
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
		case ALLEGIANCE_QUERY_NAME:
		{
			MAllegianceNameQuery_0030 msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_CLEAR_NAME:
		{
			MAllegianceNameClear_0031 msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case SEND_TELL_BY_GUID: //Send Tell by GUID
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
		case ALLEGIANCE_SET_NAME:
		{
			MAllegianceNameSet_0033 msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case USE_ITEM_EX: //Use Item Ex
		{
			DWORD dwSourceID = pReader->ReadDWORD();
			DWORD dwDestID = pReader->ReadDWORD();
			if (pReader->GetLastError()) break;
			UseItemEx(dwSourceID, dwDestID);
			break;
		}
		case USE_OBJECT: //Use Object
		{
			DWORD dwEID = pReader->ReadDWORD();
			if (pReader->GetLastError()) break;
			UseObject(dwEID);
			break;
		}
		case ALLEGIANCE_SET_OFFICER:
		{
			MAllegianceOfficerSet_003B msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_SET_OFFICER_TITLE:
		{
			MAllegianceOfficerTitleSet_003C msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_LIST_OFFICER_TITLES:
		{
			MAllegianceOfficerTitlesList_003D msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_CLEAR_OFFICER_TITLES:
		{
			MAllegianceOfficerTitlesClear_003E msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_LOCK_ACTION:
		{
			// Read: AllegianceLockAction action - LockedOff 1, LockedOn 2, ToggleLocked 3, CheckLocked 4, DisplayBypass 5, ClearBypass 6
			break;
		}
		case ALLEGIANCE_APPROVED_VASSAL:
		{
			MAllegianceApprovedVassalSet_0040 msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_CHAT_GAG:
		{
			// Read: string charName, bool on - player being gagged, whether gag is on
			break;
		}
		case ALLEGIANCE_HOUSE_ACTION:
		{
			// Read: AllegianceHouseAction action - Help 1, GuestOpen 2, GuestClosed 3, StorageOpen 4, StorageClosed 5
			break;
		}
		case SPEND_XP_VITALS: // spend XP on vitals
		{
			DWORD dwAttribute2nd = pReader->ReadDWORD();
			DWORD dwXP = pReader->ReadDWORD();
			if (pReader->GetLastError()) break;

					SpendAttribute2ndXP((STypeAttribute2nd)dwAttribute2nd, dwXP);
			break;
		}
		case SPEND_XP_ATTRIBUTES: // spend XP on attributes
		{
			DWORD dwAttribute = pReader->ReadDWORD();
			DWORD dwXP = pReader->ReadDWORD();
			if (pReader->GetLastError()) break;

			SpendAttributeXP((STypeAttribute)dwAttribute, dwXP);
			break;
		}
		case SPEND_XP_SKILLS: // spend XP on skills
		{
			DWORD dwSkill = pReader->ReadDWORD();
			DWORD dwXP = pReader->ReadDWORD();
			if (pReader->GetLastError()) break;

			SpendSkillXP((STypeSkill)dwSkill, dwXP);
			break;
		}
		case SPEND_SKILL_CREDITS: // spend credits to train skill
		{
			DWORD dwSkill = pReader->ReadDWORD();
			DWORD dwCredits = pReader->ReadDWORD();
			if (pReader->GetLastError()) break;

			SpendSkillCredits((STypeSkill)dwSkill, dwCredits);
			break;
		}
		case CAST_UNTARGETED_SPELL: // cast untargeted spell
		{
			DWORD spell_id = pReader->ReadDWORD();
			if (pReader->GetLastError()) break;

			m_pPlayer->TryCastSpell(m_pPlayer->GetID() /*0*/, spell_id);
			break;
		}
		case CAST_TARGETED_SPELL: // cast targeted spell
		{
			DWORD target = pReader->ReadDWORD();
			DWORD spell_id = pReader->ReadDWORD();
			if (pReader->GetLastError()) break;

			m_pPlayer->TryCastSpell(target, spell_id);
			break;
		}
		case CHANGE_COMBAT_STANCE: // Evt_Combat__ChangeCombatMode_ID "Change Combat Mode"
		{
			DWORD mode = pReader->ReadDWORD();
			if (pReader->GetLastError()) break;

			ChangeCombatStance((COMBAT_MODE)mode);
			break;
		}
		case STACKABLE_MERGE: // Evt_Inventory__StackableMerge
		{
			DWORD merge_from_id = pReader->Read<DWORD>();
			DWORD merge_to_id = pReader->Read<DWORD>();
			DWORD amount = pReader->Read<DWORD>();

			if (pReader->GetLastError())
				break;

			m_pPlayer->MergeItem(merge_from_id, merge_to_id, amount);
			break;
		}
		case STACKABLE_SPLIT_TO_CONTAINER: // Evt_Inventory__StackableSplitToContainer
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
		case STACKABLE_SPLIT_TO_3D: // Evt_Inventory__StackableSplitTo3D
		{
			DWORD stack_id = pReader->Read<DWORD>();
			DWORD amount = pReader->Read<DWORD>();

			if (pReader->GetLastError())
				break;

			m_pPlayer->SplitItemto3D(stack_id, amount);
			break;
		}
		case STACKABLE_SPLIT_TO_WIELD: // Evt_Inventory__StackableSplitToWield
		{				
			DWORD stack_id = pReader->Read<DWORD>();
			DWORD loc = pReader->Read<DWORD>();
			DWORD amount = pReader->Read<DWORD>();

			if (pReader->GetLastError())
				break;

			m_pPlayer->SplitItemToWield(stack_id, loc, amount);
			break;
		}
		case SQUELCH_CHARACTER_MODIFY:
		{
			// Read: bool add, DWORD characterID, string characterName, ChatMessageType msgType
			//		0 = unsquelch, 1 = squelch, ChatMessageType = enum LogTextType
			break;
		}
		case SQUELCH_ACCOUNT_MODIFY:
		{
			// Read: bool add, string characterName
			break;
		}
		case SQUELCH_GLOBAL_MODIFY:
		{
			// Read: bool add, ChatMessageType msgType
			break;
		}
		case SEND_TELL_BY_NAME: //Send Tell by Name
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
		case BUY_FROM_VENDOR: // Buy from Vendor
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
		case SELL_TO_VENDOR: // Sell to Vendor
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
		case RECALL_LIFESTONE: // Lifestone Recall
		{
			LifestoneRecall();
			break;
		}
		case LOGIN_COMPLETE: // "Login Complete"
		{
			ExitPortal();
			break;
		}
		case FELLOW_CREATE: // "Create Fellowship"
		{
			std::string name = pReader->ReadString();
			int shareXP = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			TryFellowshipCreate(name, shareXP);
			break;
		}
		case FELLOW_QUIT: // "Quit Fellowship"
		{
			int disband = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			TryFellowshipQuit(disband);
			break;
		}
		case FELLOW_DISMISS: // "Fellowship Dismiss"
		{
			DWORD dismissed = pReader->Read<DWORD>();
			if (pReader->GetLastError()) break;

			TryFellowshipDismiss(dismissed);
			break;
		}
		case FELLOW_RECRUIT: // "Fellowship Recruit"
		{
			DWORD target = pReader->Read<DWORD>();

			if (pReader->GetLastError())
				break;

			TryFellowshipRecruit(target);
			break;
		}
		case FELLOW_UPDATE: // "Fellowship Update"
		{
			int on = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			TryFellowshipUpdate(on);
			break;
		}
		case BOOK_ADD_PAGE:
		{
			// Read: DWORD objectID - ID of book
			break;
		}
		case BOOK_MODIFY_PAGE:
		{
			// Read: DWORD objectID, int pageNum, string pageText
			break;
		}
		case BOOK_DATA: // Request update to book data (seems to be after failed add page)
		{
			// Read: DWORD objectID - ID of book
			break;
		}
		case BOOK_DELETE_PAGE:
		{
			// Read: DWORD objectID, int pageNum
			break;
		}
		case BOOK_PAGE_DATA: // Requests data of a page of a book
		{
			// Read: DWORD objectID, int pageNum
			break;
		}
		case GIVE_OBJECT: // Give an item to someone
		{
			DWORD target_id = pReader->Read<DWORD>();
			DWORD object_id = pReader->Read<DWORD>();
			DWORD amount = pReader->Read<DWORD>();

			if (pReader->GetLastError())
				break;

			m_pPlayer->GiveItem(target_id, object_id, amount);
			break;
		}		
		case INSCRIBE: // "Inscribe"
		{
			DWORD target_id = pReader->Read<DWORD>();
			std::string msg = pReader->ReadString();

			if (pReader->GetLastError())
				break;

			TryInscribeItem(target_id, msg);
			break;
		}
		case APPRAISE: // Identify
		{
			DWORD target_id = pReader->ReadDWORD();

			if (pReader->GetLastError())
				break;

			Identify(target_id);
			break;
		}
		case ADMIN_TELEPORT: // Advocate teleport (triggered by having an admin flag set, clicking the mini-map)
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
		case ABUSE_LOG_REQUEST: // Send abuse report
		{
			// Read: string target, uint status = 1, string complaint
			break;
		}
		case CHANNEL_ADD: // Join chat channel
		{
			// Read: uint channel ID
			break;
		}
		case CHANNEL_REMOVE: // Leave chat channel
		{
			// Read: uint channel ID
			break;
		}
		case CHANNEL_TEXT: // Channel Text
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
		case CHANNEL_LIST: // List of characters listening to a channel
		{
			// Read: PackableList<string> characters
			break;
		}
		case CHANNEL_INDEX: // List of channels available to the player
		{
			// Read: PackableList<string> channels
			break;
		}
		case NO_LONGER_VIEWING_CONTAINER: // No longer viewing contents
		{
			DWORD container_id = pReader->Read<DWORD>();
			if (pReader->GetLastError())
				break;

					NoLongerViewingContents(container_id);
					break;
				}
			case ADD_ITEM_SHORTCUT: // Add item to shortcut bar
				{
					ShortCutData data;
					data.UnPack(pReader);
					if (pReader->GetLastError())
						break;

					m_pPlayer->_playerModule.AddShortCut(data);
					break;
				}
			case REMOVE_ITEM_SHORTCUT: // Add item to shortcut bar
				{
					int index = pReader->Read<int>();
					if (pReader->GetLastError())
						break;

			m_pPlayer->_playerModule.RemoveShortCut(index);
			break;
		}
		case CHARACTER_OPTIONS: // Character Options 
		{
			PlayerModule module;
			if (!module.UnPack(pReader) || pReader->GetLastError())
				break;
			if (m_pPlayer && m_pPlayer->AsPlayer())
			{
				SendText("Updating character configuration.", LTT_SYSTEM_EVENT);
				m_pPlayer->UpdateModuleFromClient(module);
			}
			else
			DEBUG_DATA << "Playermodule call for invalid object";
			break;
		}
		case CANCEL_ATTACK: // Cancel attack
		{
			// TODO
			m_pPlayer->TryCancelAttack();
			break;
		}
		case SPELLBOOK_REMOVE:
		{
			// Read in: LayeredSpellID spellID - Full spell ID combining the (SpellID)id with the spell (ushort)layer.
			break;
		}
		case QUERY_HEALTH: // Request health update
		{
			DWORD target_id = pReader->ReadDWORD();

				if (pReader->GetLastError())
					break;

			RequestHealthUpdate(target_id);
			break;
		}
		case QUERY_AGE:
		{
			DWORD targetID = pReader->ReadDWORD(); // don't need this, query can't target anyone else

			if (pReader->GetLastError())
				break;

			int age = m_pPlayer->m_Qualities.GetInt(AGE_INT, 0);
			SendText(GetAgeString(age).c_str(), LTT_DEFAULT); // You have played for Xm Xd Xh Xm Xs.
			break;
		}
		case QUERY_BIRTH:
		{
			DWORD targetID = pReader->ReadDWORD(); // don't need this, query can't target anyone else

			if (pReader->GetLastError())
				break;

			std::string DOB = m_pPlayer->InqStringQuality(DATE_OF_BIRTH_STRING, "");
			SendText(csprintf("%s", DOB.c_str()), LTT_DEFAULT);
			break;
		}
		case TEXT_INDIRECT: // Indirect Text (@me)
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
		case TEXT_EMOTE: // Emote Text (*laugh* sends 'laughs')
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
		case ADD_TO_SPELLBAR: // Add item to spell bar
		{
			DWORD spellID = pReader->Read<DWORD>();
			int index = pReader->Read<int>();
			int spellBar = pReader->Read<int>();
			if (pReader->GetLastError())
				break;

			m_pPlayer->_playerModule.AddSpellFavorite(spellID, index, spellBar);
			break;
		}
		case REMOVE_FROM_SPELLBAR: // Remove item from spell bar
		{
			DWORD spellID = pReader->Read<DWORD>();
			int spellBar = pReader->Read<int>();
			if (pReader->GetLastError())
				break;

			m_pPlayer->_playerModule.RemoveSpellFavorite(spellID, spellBar);
			break;
		}
		case PING: // Ping
		{
			Ping();
			break;
		}
		case TRADE_OPEN: // Open Trade Negotiations
		{
			if (m_pPlayer->GetTradeManager() != NULL)
			{
				//already trading
				return;
			}

			DWORD target = pReader->Read<DWORD>();
			if (pReader->GetLastError())
				return;

			CWeenieObject *pOther = g_pWorld->FindWithinPVS(m_pPlayer, target);
			CPlayerWeenie *pTarget;
			if (pOther)
				pTarget = pOther->AsPlayer();

			if (!pOther || !pTarget)
			{
				// cannot open trade
				m_pPlayer->SendText("Unable to open trade.", LTT_ERROR);
			}
			else if (pTarget->_playerModule.options_ & 0x20000)
			{
				SendText((pTarget->GetName() + " has disabled trading.").c_str(), LTT_ERROR);
			}
			else if (pTarget->IsBusyOrInAction())
			{
				SendText((pTarget->GetName() + " is busy.").c_str(), LTT_ERROR);
			}
			else if (pTarget->GetTradeManager() != NULL)
			{
				SendText((pTarget->GetName() + " is already trading with someone else!").c_str(), LTT_ERROR);
			}
			else if (m_pPlayer->DistanceTo(pOther, true) > 1)
			{
				SendText((pTarget->GetName() + " is too far away!").c_str(), LTT_ERROR);
			}
			else
			{
				TradeManager *tm = TradeManager::RegisterTrade(m_pPlayer, pTarget);

				m_pPlayer->SetTradeManager(tm);
				pTarget->SetTradeManager(tm);
			}
			break;
		}
		case TRADE_CLOSE: // Close Trade Negotiations
		{
			TradeManager* tm = m_pPlayer->GetTradeManager();
			if (tm)
			{
				tm->CloseTrade(m_pPlayer);
				return;
			}
			break;
		}
		case TRADE_ADD: // AddToTrade
		{
			TradeManager* tm = m_pPlayer->GetTradeManager();
			if (tm)
			{
				DWORD item = pReader->Read<DWORD>();

			tm->AddToTrade(m_pPlayer, item);
			}
			break;
		}
		case TRADE_ACCEPT: // Accept trade
		{
			TradeManager* tm = m_pPlayer->GetTradeManager();
			if (tm)
			{
				tm->AcceptTrade(m_pPlayer);
			}
			break;
		}
		case TRADE_DECLINE: // Decline trade
		{
			TradeManager* tm = m_pPlayer->GetTradeManager();
			if (tm)
			{
				tm->DeclineTrade(m_pPlayer);
			}
			break;
		}
		case TRADE_RESET: // Reset trade
		{
			TradeManager* tm = m_pPlayer->GetTradeManager();
			if (tm)
			{
				tm->ResetTrade(m_pPlayer);
			}
			break;
		}
		case CLEAR_PLAYER_CONSENT_LIST: // Clears the player's corpse looting consent list, /consent clear
		{
			m_pPlayer->ClearConsent(false);
			break;
		}
		case DISPLAY_PLAYER_CONSENT_LIST: // Display the player's corpse looting consent list, /consent who
		{
			m_pPlayer->DisplayConsent();
			break;
		}
		case REMOVE_FROM_PLAYER_CONSENT_LIST: // Remove your corpse looting permission for the given player, / consent remove
		{
			std::string targetName = pReader->ReadString();

			if (pReader->GetLastError())
				break;
			CPlayerWeenie *target = g_pWorld->FindPlayer(targetName.c_str());
			if (target)
			{
				m_pPlayer->RemoveConsent(target);
			}
			break;
		}
		case ADD_PLAYER_PERMISSION: // Grants a player corpse looting permission, /permit add
		{
			std::string targetName = pReader->ReadString();

			if (pReader->GetLastError())
				break;

			CPlayerWeenie *target = g_pWorld->FindPlayer(targetName.c_str());
			if (target)
			{
				m_pPlayer->AddCorpsePermission(target);
			}
			break;
		}
		case REMOVE_PLAYER_PERMISSION:
		{
			std::string targetName = pReader->ReadString();

			if (pReader->GetLastError())
				break;

			CPlayerWeenie *target = g_pWorld->FindPlayer(targetName.c_str());
			if (target)
			{
				m_pPlayer->RemoveCorpsePermission(target);
			}
			break;
		}
		case HOUSE_BUY: //House_BuyHouse 
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
		case HOUSE_ABANDON: //House_AbandonHouse 
		{
			HouseAbandon();
			break;
		}
		case HOUSE_OF_PLAYER_QUERY: //House_QueryHouse 
		{
			HouseRequestData();
			break;
		}
		case HOUSE_RENT: //House_RentHouse 
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
		case HOUSE_ADD_GUEST: //House_AddPermanentGuest 
		{
			std::string name = pReader->ReadString();

			if (pReader->GetLastError())
				break;

			HouseAddPersonToAccessList(name);
			break;
		}
		case HOUSE_REMOVE_GUEST: //House_RemovePermanentGuest
		{
			std::string name = pReader->ReadString();

			if (pReader->GetLastError())
				break;

			HouseRemovePersonFromAccessList(name);
			break;
		}
		case HOUSE_SET_OPEN_ACCESS: //House_SetOpenHouseStatus
		{
			int newSetting = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			HouseToggleOpenAccess(newSetting > 0);
			break;
		}
		case HOUSE_CHANGE_STORAGE_PERMISSIONS: //House_ChangeStoragePermission
		{
			std::string name = pReader->ReadString();
			int isAdd = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			HouseAddOrRemovePersonToStorageList(name, isAdd > 0);
			break;
		}
		case HOUSE_CLEAR_STORAGE_PERMISSIONS: //House_RemoveAllStoragePermission 
		{
			HouseClearStorageAccess();
			break;
		}
		case HOUSE_GUEST_LIST: //House_RequestFullGuestList
		{
			HouseRequestAccessList();
			break;
		}
		case ALLEGIANCE_SET_MOTD:
		{
			MAllegianceMOTDSet_0254 msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_QUERY_MOTD: //Request allegiance MOTD
		{
			MAllegianceMOTDQuery_0255 msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_CLEAR_MOTD:
		{
			MAllegianceMOTDClear_0256 msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case HOUSE_QUERY_SLUMLORD:
		{
			// read in: DWORD lord = pReader->ReadDWORD();
		}
		case HOUSE_SET_OPEN_STORAGE_ACCESS: //House_AddAllStoragePermission
		{
			HouseToggleOpenStorageAccess();
			break;
		}
		case HOUSE_REMOVE_ALL_GUESTS: //House_RemoveAllPermanentGuests
		{
			HouseClearAccessList();
			break;
		}
		case HOUSE_BOOT_ALL:
		{
		break;
		}
		case RECALL_HOUSE: // House Recall
		{
			HouseRecall();
			break;
		}
		case ITEM_MANA_REQUEST: // Request Item Mana
		{
			DWORD itemId = pReader->ReadDWORD();

			if (pReader->GetLastError())
				break;

			m_pPlayer->HandleItemManaRequest(itemId);
			break;
		}
		case HOUSE_SET_HOOKS_VISIBILITY: // House_SetHooksVisibility 
		{
			int newSetting = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			HouseToggleHooks(newSetting > 0);
			break;
		}
		case HOUSE_CHANGE_ALLEGIANCE_GUEST_PERMISSIONS: //House_ModifyAllegianceGuestPermission 
		{
			int newSetting = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			HouseAddOrRemoveAllegianceToAccessList(newSetting > 0);
			break;
		}
		case HOUSE_CHANGE_ALLEGIANCE_STORAGE_PERMISSIONS: //House_ModifyAllegianceStoragePermission
		{
			int newSetting = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			HouseAddOrRemoveAllegianceToStorageList(newSetting > 0);
			break;
		}
		case CHESS_JOIN:
		{
			auto const guid = pReader->Read<uint32_t>();
			pReader->Read<uint32_t>(); // teamId, not used

			CWeenieObject* object = g_pWorld->FindObject(guid);
			if (!object)
				break;

			sChessManager->Join(m_pPlayer, object);
			break;
		}
		case CHESS_QUIT:
			sChessManager->Quit(m_pPlayer);
			break;
		case CHESS_MOVE:
		{
			GDLE::Chess::ChessPieceCoord from, to;
			from.UnPack(pReader);
			to.UnPack(pReader);
			sChessManager->Move(m_pPlayer, from, to);
			break;
		}
		case CHESS_PASS:
			sChessManager->MovePass(m_pPlayer);
			break;
		case CHESS_STALEMATE:
		{
			auto const on = pReader->Read<uint32_t>();
			sChessManager->Stalemate(m_pPlayer, on);
			break;
		}
		case HOUSE_LIST_AVAILABLE:
		{
			// Read: HouseType houseType - type of house to list, cottage 1, villa 2, mansion 3, apartment 4
			// Reply: 0x0271, HouseType houseType, PackableList<uint> houses, int numHouses
			break;
		}
		case ALLEGIANCE_BOOT_PLAYER: // Boots a player from the allegiance, optionally all characters on their account
		{
			MAllegianceBoot_0277 msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case RECALL_HOUSE_MANSION: // House_TeleToMansion
		{
			HouseMansionRecall();
			break;
		}
		case DIE_COMMAND: // "/die" command
		{
			if (!m_pPlayer->IsDead() && !m_pPlayer->IsInPortalSpace() && !m_pPlayer->IsBusyOrInAction())
			{
				// this is a bad way of doing this...
				m_pPlayer->SetHealth(0, true);
				m_pPlayer->OnDeath(m_pPlayer->GetID());
			}

			break;
		}
		case ALLEGIANCE_INFO_REQUEST: // allegiance info request
		{
			MAllegianceInfoRequest_027B msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case SPELLBOOK_FILTERS: // "/die" command
		{
			DWORD filters = pReader->Read<DWORD>();
			if (pReader->GetLastError())
				break;

			m_pPlayer->_playerModule.spell_filters_ = filters;
			break;
		}
		case RECALL_MARKET: // Marketplace Recall
		{
			MarketplaceRecall();
			break;
		}
		case PKLITE:
		{
			// change dot to pink
			// change to attackable by other PKLs
			// change to dropping nothing on death from other PKL
			// do PKL animation
			break;
		}
		case FELLOW_ASSIGN_NEW_LEADER: // "Fellowship Assign New Leader"
		{
			DWORD target_id = pReader->Read<DWORD>();

			if (pReader->GetLastError())
				break;

			TryFellowshipAssignNewLeader(target_id);
			break;
		}
		case FELLOW_CHANGE_OPENNESS: // "Fellowship Change Openness"
		{
			int open = pReader->Read<int>();

			if (pReader->GetLastError())
				break;

			TryFellowshipChangeOpenness(open);
			break;
		}
		case ALLEGIANCE_CHAT_BOOT:
		{
			MAllegianceChatBoot_02A0 msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_ADD_PLAYER_BAN:
		{
			MAllegianceBanAdd_02A1 msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_REMOVE_PLAYER_BAN:
		{
			MAllegianceBanRemove_02A2 msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_LIST_BANS:
		{
			MAllegianceBanList_02A3 msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_REMOVE_OFFICER:
		{
			MAllegianceOfficerRemove_02A5 msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_LIST_OFFICERS:
		{
			MAllegianceOfficersList_02A6 msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case ALLEGIANCE_CLEAR_OFFICERS:
		{
			MAllegianceOfficersClear_02A7 msg(m_pPlayer);
			msg.Parse(pReader);
			break;
		}
		case RECALL_ALLEGIANCE_HOMETOWN: //Allegiance_RecallAllegianceHometown (bindstone)
		{
			AllegianceHometownRecall();
			/*MAllegianceRecallHometown_02AB msg(m_pPlayer);
			msg.Parse(pReader);*/
			break;
		}
		case FINISH_BARBER:
		{
			//Read: DataID basePalette, DataID headObject, DataID headTexture, DataID defaultHeadTexture, DataID eyesTexture
			// DataID defaultEyesTexture, DataID noseTexture, DataID defaultNoseTexture, DataID mouthTexture, DataID defaultMouthTexture
			// DataID skinPalette, DataID hairPalette, DataID eyesPalette, DataID setupID, int option1, int option2
			// option1 = toggle certain race options e.g. empyrean float or flaming head undead, option2 = not used?
			break;
		}
		case MOVEMENT_JUMP: // Jump Movement
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
				//m_pPlayer->SetPositionSimple(&position, TRUE);
				
				
				CTransition *transit = m_pPlayer->transition(&m_pPlayer->m_Position, &position, 0);
				if (transit)
				{
					m_pPlayer->SetPositionInternal(transit);
				}
				
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
		case MOVEMENT_MOVE_TO_STATE: // CM_Movement__Event_MoveToState (update vector movement?)
		{
			// TODO: Cancel attack

			MoveToStatePack moveToState;
			moveToState.UnPack(pReader);

				if (pReader->GetLastError())
				{
					SERVER_WARN << "Bad animation message!";
					SERVER_WARN << pReader->GetDataStart() << pReader->GetDataLen();
					break;
				}

				//if (is_newer_event_stamp(moveToState.server_control_timestamp, m_pPlayer->_server_control_timestamp))
				//{
					// LOG(Temp, Normal, "Old server control timestamp on 0xF61C. Ignoring.\n");
				//	break;
				//}

				if (is_newer_event_stamp(moveToState.teleport_timestamp, m_pPlayer->_teleport_timestamp))
				{
					SERVER_WARN << "Old teleport timestamp on 0xF61C. Ignoring.";
					break;
				}
				if (is_newer_event_stamp(moveToState.force_position_ts, m_pPlayer->_force_position_timestamp))
				{
					SERVER_WARN << "Old force position timestamp on 0xF61C. Ignoring.";
					break;
				}

				if (m_pPlayer->IsDead())
				{
					SERVER_WARN << "Dead players can't move. Ignoring.";
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
					//m_pPlayer->SetPositionSimple(&moveToState.position, TRUE);

				
					CTransition *transit = m_pPlayer->transition(&m_pPlayer->m_Position, &moveToState.position, 0);
					if (transit)
					{
						m_pPlayer->SetPositionInternal(transit);
					}
				
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
					SERVER_WARN << "Bad style received" << moveToState.raw_motion_state.current_style;
					break;
				}

				if (moveToState.raw_motion_state.forward_command & CM_Action)
				{
					SERVER_WARN << "Bad forward command received" << moveToState.raw_motion_state.forward_command;
					break;
				}

				if (moveToState.raw_motion_state.sidestep_command & CM_Action)
				{
					SERVER_WARN << "Bad sidestep command received" << moveToState.raw_motion_state.sidestep_command;
					break;
				}

				if (moveToState.raw_motion_state.turn_command & CM_Action)
				{
					SERVER_WARN << "Bad turn command received" << moveToState.raw_motion_state.turn_command;
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
							SERVER_WARN << "Bad action received" << commandID;
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
		case MOVEMENT_DO_MOVEMENT_COMMAND:
		{
			// Read: uint motion, float speed, uint holdKey
			break;
		}
		case MOVEMENT_TURN_TO:
		{
			// Read: TurnToEventPack ttep - set of turning data, not in client
			break;
		}
		case MOVEMENT_STOP:
		{
			// Read: uint motion, uint holdKey
			break;
		}
		case MOVEMENT_AUTONOMY_LEVEL:
		{
			// Read: uint autonomyLevel - Seems to be 0, 1 or 2. I think 0/1 is server controlled, 2 is client controlled
			// Align to DWORD boundary
			break;
		}
		case MOVEMENT_AUTONOMOUS_POSITION: // Update Exact Position
		{
			Position position;
			position.UnPack(pReader);

				WORD instance = pReader->ReadWORD();

				if (pReader->GetLastError())
					break;

				if (instance != m_pPlayer->_instance_timestamp)
				{
					SERVER_WARN << "Bad instance.";
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
					SERVER_WARN << "Old teleport timestamp. Ignoring.";
					break;
				}

				WORD force_position_ts = pReader->ReadWORD();
				if (pReader->GetLastError())
					break;
				if (is_newer_event_stamp(force_position_ts, m_pPlayer->_force_position_timestamp))
				{
					SERVER_WARN << "Old force position timestamp. Ignoring.";
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
		case MOVEMENT_JUMP_NON_AUTONOMOUS:
		{
			// Read: float extent - Power of jump
			break;
		}
		default:
		{
			//Unknown Event
#ifdef _DEBUG
				SERVER_WARN << "Unhandled client event" << dwEvent;
#endif
				// LOG_BYTES(Temp, Verbose, in->GetDataPtr(), in->GetDataEnd() - in->GetDataPtr() );
			}
		}
}









