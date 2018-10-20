
#include "StdAfx.h"
#include "SkillTable.h"

bool SkillBase::UnPack(BinaryReader *pReader)
{
	_description = pReader->ReadString();
	_name = pReader->ReadString();
	_iconID = pReader->Read<DWORD>();
	_trained_cost = pReader->Read<DWORD>();
	_specialized_cost = pReader->Read<DWORD>();
	_category = pReader->Read<int>();
	_chargen_use = pReader->Read<int>();
	_min_level = pReader->Read<int>();
	_formula.UnPack(pReader);
	_upper_bound = pReader->Read<double>();
	_lower_bound = pReader->Read<double>();
	_learn_mod = pReader->Read<double>();
	return true;
}

DEFINE_DBOBJ(SkillTable, SkillTables);
DEFINE_LEGACY_PACK_MIGRATOR(SkillTable);

DEFINE_PACK(SkillTable)
{
	UNFINISHED();
}

DEFINE_UNPACK(SkillTable)
{
	// ignore the file ID
	pReader->ReadDWORD();

	return _skillBaseHash.UnPack(pReader);
}

STypeSkill SkillTable::OldToNewSkill(STypeSkill old) // Custom
{
	switch (old)
	{
	case SWORD_SKILL:
		return SWORD_SKILL;
	case DAGGER_SKILL:
		return DAGGER_SKILL;
	case AXE_SKILL:
		return AXE_SKILL;
	case MACE_SKILL:
		return MACE_SKILL;
	case STAFF_SKILL:
		return STAFF_SKILL;
	case SPEAR_SKILL:
		return SPEAR_SKILL;
	case UNARMED_COMBAT_SKILL:
		return UNARMED_COMBAT_SKILL;
	case BOW_SKILL:
		return BOW_SKILL;
	case CROSSBOW_SKILL:
		return CROSSBOW_SKILL;
	case THROWN_WEAPON_SKILL:
		return THROWN_WEAPON_SKILL;

		//TOD SKILL TO OLD
	case TWO_HANDED_COMBAT_SKILL:
		return SWORD_SKILL;
	case FINESSE_WEAPONS_SKILL:
		return DAGGER_SKILL;
	case LIGHT_WEAPONS_SKILL:
		return UNARMED_COMBAT_SKILL;
	case HEAVY_WEAPONS_SKILL:
		return SWORD_SKILL;
	case MISSILE_WEAPONS_SKILL:
		return BOW_SKILL;
	}

	return old;
}

const SkillBase *SkillTable::GetSkillBaseRaw(STypeSkill key) // custom
{
	key = SkillTable::OldToNewSkill(key);
	return _skillBaseHash.lookup(key);
}

const SkillBase *SkillTable::GetSkillBase(STypeSkill key)
{
	key = SkillTable::OldToNewSkill(key);
	return _skillBaseHash.lookup(key);
}

std::string SkillTable::GetSkillName(STypeSkill key) // custom
{
	key = SkillTable::OldToNewSkill(key);
	if (const SkillBase *skillBase = GetSkillBaseRaw(key))
	{
		return skillBase->_name;
	}

	return "";
}

SkillTable *SkillSystem::GetSkillTable()
{
	return CachedSkillTable;
}

BOOL SkillSystem::GetSkillName(STypeSkill key, std::string &value)
{
	key = SkillTable::OldToNewSkill(key);
	if (SkillTable *pSkillTable = SkillSystem::GetSkillTable())
	{
		const SkillBase *pSkillBase = pSkillTable->GetSkillBase(key);
		if (pSkillBase)
		{
			value = pSkillBase->_name;
			return TRUE;
		}
	}

	return FALSE;
}