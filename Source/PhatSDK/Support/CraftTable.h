
#pragma once

template<typename TStatType, typename TDataType>
class TYPEMod : public PackObj, public PackableJson
{
public:

	virtual void Pack(class BinaryWriter *pWriter) override;
	virtual bool UnPack(class BinaryReader *pReader) override;
	virtual void PackJson(json& writer) override;
	virtual bool UnPackJson(const json& reader) override;

	int _unk;
	int _operationType;
	TStatType _stat;
	TDataType _value;
};

template<typename TStatType, typename TDataType>
class TYPERequirement : public PackObj, public PackableJson
{
public:

	virtual void Pack(class BinaryWriter *pWriter) override;
	virtual bool UnPack(class BinaryReader *pReader) override;
	virtual void PackJson(json& writer) override;
	virtual bool UnPackJson(const json& reader) override;


	TStatType _stat;
	TDataType _value;
	int _operationType;
	std::string _message;
};

class CraftRequirements : public PackObj, public PackableJson
{
public:
	DECLARE_PACKABLE()
	DECLARE_PACKABLE_JSON();

	PackableListWithJson<TYPERequirement<STypeInt, int>> _intRequirement;
	PackableListWithJson<TYPERequirement<STypeDID, DWORD>> _didRequirement;
	PackableListWithJson<TYPERequirement<STypeIID, DWORD>> _iidRequirement;
	PackableListWithJson<TYPERequirement<STypeFloat, double>> _floatRequirement;
	PackableListWithJson<TYPERequirement<STypeString, std::string>> _stringRequirement;
	PackableListWithJson<TYPERequirement<STypeBool, BOOL>> _boolRequirement;
};

class CraftMods : public PackObj, public PackableJson
{
public:

	virtual void Pack(class BinaryWriter *pWriter) override;
	virtual bool UnPack(class BinaryReader *pReader) override;
	virtual void PackJson(json& writer) override;
	virtual bool UnPackJson(const json& reader) override;

	PackableListWithJson<TYPEMod<STypeInt, int>> _intMod;
	PackableListWithJson<TYPEMod<STypeDID, DWORD>> _didMod;
	PackableListWithJson<TYPEMod<STypeIID, DWORD>> _iidMod;
	PackableListWithJson<TYPEMod<STypeFloat, double>> _floatMod;
	PackableListWithJson<TYPEMod<STypeString, std::string>> _stringMod;
	PackableListWithJson<TYPEMod<STypeBool, BOOL>> _boolMod;

	int _ModifyHealth = 0;
	int _ModifyStamina = 0;
	int _ModifyMana = 0;
	int _RequiresHealth = 0;
	int _RequiresStamina = 0;
	int _RequiresMana = 0;

	bool _unknown7 = false;
	DWORD _modificationScriptId = 0;

	int _unknown9 = 0;
	DWORD _unknown10 = 0;
};

class CCraftOperation : public PackObj, public PackableJson
{
public:
	DECLARE_PACKABLE()
	DECLARE_PACKABLE_JSON();

	DWORD _unk = 0;
	STypeSkill _skill = STypeSkill::UNDEF_SKILL;
	int _difficulty = 0;
	DWORD _SkillCheckFormulaType = 0;
	DWORD _successWcid = 0;
	DWORD _successAmount = 0;
	std::string _successMessage;
	DWORD _failWcid = 0;
	DWORD _failAmount = 0;
	std::string _failMessage;

	double _successConsumeTargetChance;
	int _successConsumeTargetAmount;
	std::string _successConsumeTargetMessage;

	double _successConsumeToolChance;
	int _successConsumeToolAmount;
	std::string _successConsumeToolMessage;

	double _failureConsumeTargetChance;
	int _failureConsumeTargetAmount;
	std::string _failureConsumeTargetMessage;

	double _failureConsumeToolChance;
	int _failureConsumeToolAmount;
	std::string _failureConsumeToolMessage;

	CraftRequirements _requirements[3];
	CraftMods _mods[8];

	DWORD _dataID = 0;
};

class CCraftTable : public PackObj, public PackableJson
{
public:
	CCraftTable();
	virtual ~CCraftTable() override;

	DECLARE_PACKABLE()
	DECLARE_PACKABLE_JSON();


	PackableHashTable<DWORD, CCraftOperation> _operations;
	PackableHashTable<DWORD64, DWORD, DWORD64> _precursorMap;
};

class CraftPrecursor : public PackableJson
{
public:
	DECLARE_PACKABLE_JSON();

	DWORD Tool = 0;
	DWORD Target = 0;
	DWORD RecipeID = 0;
	DWORD64 ToolTargetCombo = 0;

};

class JsonCraftOperation : public PackableJson
{
public:
	DECLARE_PACKABLE_JSON();

	DWORD _recipeID = 0;
	DWORD _unk = 0;
	STypeSkill _skill = STypeSkill::UNDEF_SKILL;
	int _difficulty = 0;
	DWORD _SkillCheckFormulaType = 0;
	DWORD _successWcid = 0;
	DWORD _successAmount = 0;
	std::string _successMessage;
	DWORD _failWcid = 0;
	DWORD _failAmount = 0;
	std::string _failMessage;

	double _successConsumeTargetChance;
	int _successConsumeTargetAmount;
	std::string _successConsumeTargetMessage;

	double _successConsumeToolChance;
	int _successConsumeToolAmount;
	std::string _successConsumeToolMessage;

	double _failureConsumeTargetChance;
	int _failureConsumeTargetAmount;
	std::string _failureConsumeTargetMessage;

	double _failureConsumeToolChance;
	int _failureConsumeToolAmount;
	std::string _failureConsumeToolMessage;

	CraftRequirements _requirements[3];
	CraftMods _mods[8];

	DWORD _dataID = 0;
};

