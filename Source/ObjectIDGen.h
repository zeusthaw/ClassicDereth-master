#pragma once

enum eGUIDClass {
	ePresetGUID = 0,
	ePlayerGUID = 1,
	// eStaticGUID = 2,
	eDynamicGUID = 3,
	eEphemeral = 4
};

class CObjectIDGenerator
{
public:
	CObjectIDGenerator();
	~CObjectIDGenerator();

	void LoadState();
	void SaveRangeStart();
	void LoadRangeStart();
	bool IsIdRangeValid(){ return isIdRangeValid;}

	DWORD GenerateGUID(eGUIDClass guidClass);

protected:
	DWORD m_dwHintDynamicGUID = 0x80000000;
	bool outOfIds = false;
	bool m_bLoadingState = false;
	bool queryInProgress = false;
	std::queue<unsigned int, std::deque<unsigned int>> listOfIdsForWeenies;
	bool isIdRangeValid = true;
	DWORD m_ephemeral;
};
