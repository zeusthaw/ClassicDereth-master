
#include "StdAfx.h"
#include "ObjectIDGen.h"
#include "DatabaseIO.h"
#include "Server.h"

const uint32_t IDQUEUEMAX = 250000;
const uint32_t IDQUEUEMIN = 50000;
const uint32_t IDRANGESTART = 0x80000000;
const uint32_t IDRANGEEND = 0xff000000;

const uint32_t IDEPHEMERALSTART = 0x60000000;
const uint32_t IDEPHEMERALMASK = 0x6fffffff;


CObjectIDGenerator::CObjectIDGenerator()
{
	m_ephemeral = IDEPHEMERALSTART;
	if (g_pConfig->UseIncrementalID())
	{
		WINLOG(Data, Normal, "Using Incremental ID system.....\n");
	}
	else
	{
		// Verify IDRanges tables exists and has values
		if (g_pDBIO->IDRangeTableExistsAndValid())
		{
			WINLOG(Data, Normal, "Using ID Mined system.....\n");
		}
		else
		{
			isIdRangeValid = false;
		}
	}
	LoadRangeStart();
	LoadState();
}

CObjectIDGenerator::~CObjectIDGenerator()
{
}

void CObjectIDGenerator::LoadState()
{
	if (!g_pConfig->UseIncrementalID())
	{
		queryInProgress = true;
		std::list<unsigned int> startupRange = g_pDBIO->GetNextIDRange(m_dwHintDynamicGUID, IDQUEUEMAX);
		m_dwHintDynamicGUID += IDQUEUEMAX;
		SaveRangeStart();

		DEBUG_DATA << "New Group count:" << startupRange.size();

		for (std::list<unsigned int>::iterator it = startupRange.begin(); it != startupRange.end(); ++it)
			listOfIdsForWeenies.push(*it);

		if (listOfIdsForWeenies.empty())
			outOfIds = true;

		startupRange.clear();

		m_bLoadingState = false;
		queryInProgress = false;
	}
	else
		m_dwHintDynamicGUID = g_pDBIO->GetHighestWeenieID(IDRANGESTART, IDRANGEEND);
}

DWORD CObjectIDGenerator::GenerateGUID(eGUIDClass type)
{
	switch (type)
	{
	case eDynamicGUID:
	{

		DWORD result = 0;

		if (!g_pConfig->UseIncrementalID())
		{
			if (outOfIds || listOfIdsForWeenies.empty())
			{
				SERVER_ERROR << "Dynamic GUID overflow!";
				return 0;
			}

			result = listOfIdsForWeenies.front();
			listOfIdsForWeenies.pop();

			if (!m_bLoadingState && !queryInProgress && listOfIdsForWeenies.size() < IDQUEUEMIN)
			{
				m_bLoadingState = true;
				LoadState();
			}
		}
		else
		{
			if (m_dwHintDynamicGUID >= IDRANGEEND)
			{
				SERVER_ERROR << "Dynamic GUID overflow!";
				return 0;
			}
			result = ++m_dwHintDynamicGUID;
		}

		return result;
	}
	case eEphemeral:
	{
		m_ephemeral &= IDEPHEMERALMASK;
		return m_ephemeral++;
	} 
	}

	return 0;
}

void CObjectIDGenerator::SaveRangeStart()
{
	BinaryWriter banData;
	banData.Write<DWORD>(m_dwHintDynamicGUID);
	if (!g_pConfig->UseIncrementalID())
	{
		g_pDBIO->CreateOrUpdateGlobalData(DBIO_GLOBAL_ID_RANGE_START, banData.GetData(), banData.GetSize());
	}
}

void CObjectIDGenerator::LoadRangeStart()
{
	void *data = NULL;
	DWORD length = 0;
	if (!g_pConfig->UseIncrementalID())
	{
		g_pDBIO->GetGlobalData(DBIO_GLOBAL_ID_RANGE_START, &data, &length);
		BinaryReader reader(data, length);
		m_dwHintDynamicGUID = reader.ReadDWORD() + IDQUEUEMIN;
		if (m_dwHintDynamicGUID < IDRANGESTART)
			m_dwHintDynamicGUID = IDRANGESTART;
	}
}