

#include "StdAfx.h"
#include "Globals.h"

//
#include "TurbineData.h"
#include "TurbineObject.h"

TurbineData::TurbineData()
{
	m_dwVersion = 0;
	m_mFileInfo.clear();
	m_strPath = csprintf("%s\\Data\\", g_pGlobals->GetGameDirectory());
}

TurbineData::~TurbineData()
{
	CloseFile();
}

void TurbineData::FileFoundCallback(void *This, DWORD dwFileID, BTEntry *pEntry)
{
	((TurbineData *)This)->FileFoundCallbackInternal(dwFileID, pEntry);
}

void TurbineData::FileFoundCallbackInternal(DWORD dwFileID, BTEntry *pEntry)
{
	FILEINFO info;
	info.dwPosition = pEntry->Offset_;
	info.dwLength = pEntry->size_;
	m_mFileInfo[dwFileID] = info;
}

void TurbineData::LoadFile(const char* szFile)
{
	WINLOG(Temp, Normal, "Loading %s.. ", szFile);
	SERVER_INFO << "Loading" << szFile;

	m_strFile = szFile;

	std::string fullpath = m_strPath + "\\" + szFile;
	m_pDATDisk = new DATDisk(fullpath.c_str());

	if (!m_pDATDisk->Open())
	{
		SERVER_ERROR << "Error loading file" << fullpath.c_str();
		SafeDelete(m_pDATDisk);
	}
	else
	{
#ifndef _DEBUG
		WINLOG(Temp, Normal, "mapping.. ");
		SERVER_INFO << "mapping.. ";
		m_pDATDisk->FindFileIDsWithinRange(0, (DWORD)-1, FileFoundCallback, NULL, this);
#endif

		WINLOG(Temp, Normal, "done!\n");
		SERVER_INFO << szFile << "done! ";
#ifdef PRE_TOD_DATA_FILES
#else
		SERVER_INFO << szFile << ": version" << m_pDATDisk->GetHeader()->VersionMinor << "," << m_mFileInfo.size() << "entries.";
#endif
	}
}

void TurbineData::CloseFile()
{
	SafeDelete(m_pDATDisk);

	m_mFileInfo.clear();
}

BOOL TurbineData::FileExists(DWORD dwID)
{
	FILEMAP::iterator i = m_mFileInfo.find(dwID);

	return (i != m_mFileInfo.end()) ? TRUE : FALSE;
}

TURBINEFILE *TurbineData::GetFile(DWORD dwID)
{
	DATEntry entry;
	if (m_pDATDisk && m_pDATDisk->GetData(dwID, &entry))
	{
		return new TurbineFile(dwID, entry.Data, entry.Length);
	}

	return NULL;
}

DWORD TurbineData::GetFileCount()
{
	return (DWORD)m_mFileInfo.size();
}

DWORD TurbineData::GetVersion()
{
	return m_dwVersion;
}

FILEMAP *TurbineData::GetFiles()
{
	return &m_mFileInfo;
}









