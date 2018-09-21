
#include "StdAfx.h"

#include "Network.h"
#include "Client.h"
#include "ClientEvents.h"
#include "World.h"
#include "Database.h"
#include "Database2.h"
#include "DatabaseIO.h"
#include "CRC.h"
#include "BinaryWriter.h"
#include "BinaryReader.h"
#include "PacketController.h"
#include "Server.h"
#include "Config.h"

// NOTE: A client can easily perform denial of service attacks by issuing a large number of connection requests if there ever comes a time this matters to fix

CNetwork::CNetwork(class CPhatServer *server, in_addr address, WORD port)
{
	m_server = server;
	m_port = port;
	m_addr = address;
	m_ServerID = 11; // arbitrary

	m_ClientArray = new CClient *[m_MaxClients];
	memset(m_ClientArray, 0, sizeof(CClient *) * m_MaxClients);

	m_ClientSlots = new CClient *[m_MaxClients + 1];
	memset(m_ClientSlots, 0, sizeof(CClient *) * (m_MaxClients + 1));

	for (DWORD i = 1; i <= m_MaxClients; i++)
	{
		m_OpenSlots.push_back(i);
	}

	LoadBans();

	Init();

	m_running = true;
	m_incomingThread = std::thread([&]() { IncomingThreadProc(); });
	m_outgoingThread = std::thread([&]() { OutgoingThreadProc(); });

}

CNetwork::~CNetwork()
{
	m_running = false;

	if (m_outgoingThread.joinable())
		m_outgoingThread.join();

	if (m_incomingThread.joinable())
		m_incomingThread.join();

	Shutdown();

	for (int i = 0; i < m_NumClients; i++)
	{
		SafeDelete(m_ClientArray[i]);
	}

	_queuedIncoming.clear();
	_queuedOutgoing.clear();

	SafeDeleteArray(m_ClientSlots);
	SafeDeleteArray(m_ClientArray);
}

void CNetwork::Init()
{
	srand((unsigned int)time(NULL));

	WSADATA	wsaData;
	USHORT wVersionRequested = 0x0202;
	WSAStartup(wVersionRequested, &wsaData);

	SOCKADDR_IN local_read;
	local_read.sin_family = AF_INET;
	local_read.sin_addr = m_addr;

	m_read_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	m_write_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	WINLOG(Temp, Normal, "Binding to addr %s\n", inet_ntoa(m_addr));

	local_read.sin_port = htons(m_port);
	if (bind(m_read_sock, reinterpret_cast<const sockaddr*>(&local_read), sizeof(SOCKADDR_IN)))
	{
		WINLOG(Temp, Normal, "Failed bind on recv port %u!\n", m_port);
		SERVER_ERROR << "Failed bind on recv port:" << m_port;
	}

	SOCKADDR_IN local_write;
	local_write.sin_family = AF_INET;
	local_write.sin_addr = m_addr;

	WORD sport = m_port + 1;
	local_write.sin_port = htons(sport);
	if (bind(m_write_sock, reinterpret_cast<const sockaddr*>(&local_write), sizeof(SOCKADDR_IN)))
	{
		WINLOG(Temp, Normal, "Failed bind on send port %u!\n", sport);
		SERVER_ERROR << "Failed bind on send port:" << sport;
	}

	// configure for non-blocking
	int buffsize = 131072;

	unsigned long arg = 1;
	ioctlsocket(m_read_sock, FIONBIO, &arg);
	setsockopt(m_read_sock, SOL_SOCKET, SO_SNDBUF, (char *)&buffsize, sizeof(buffsize));
	setsockopt(m_read_sock, SOL_SOCKET, SO_RCVBUF, (char *)&buffsize, sizeof(buffsize));

	ioctlsocket(m_write_sock, FIONBIO, &arg);
	setsockopt(m_write_sock, SOL_SOCKET, SO_SNDBUF, (char *)&buffsize, sizeof(buffsize));
	setsockopt(m_write_sock, SOL_SOCKET, SO_RCVBUF, (char *)&buffsize, sizeof(buffsize));
}

void CNetwork::Shutdown()
{
	closesocket(m_read_sock);
	closesocket(m_write_sock);
	
	WSACleanup();
}

void CNetwork::IncomingThreadProc()
{
	// one second delay
	timeval waittime = { 1, 0 };
	fd_set fd = { 0 };

	while (m_running)
	{
		FD_ZERO(&fd);
		FD_SET(m_read_sock, &fd);
		FD_SET(m_write_sock, &fd);

		// not going to be real picky about the return of select
		// just check the sockets anyway
		select(0, &fd, nullptr, nullptr, &waittime);

		if (FD_ISSET(m_read_sock, &fd))
			QueueIncomingOnSocket(m_read_sock);
		if (FD_ISSET(m_write_sock, &fd))
			QueueIncomingOnSocket(m_write_sock);

		std::this_thread::yield();
	}
}

void CNetwork::OutgoingThreadProc()
{
	// one second delay
	//timeval waittime = { 1, 0 };
	//fd_set fd = { 0 };

	while (m_running)
	{
		{
			std::unique_lock sigLock(m_sigOutgoingLock);
			m_sigOutgoing.wait_for(sigLock, std::chrono::seconds(1));
		}

		while (!_queuedOutgoing.empty())
		{
			//FD_ZERO(&fd);
			//FD_SET(m_read_sock, &fd);
			//FD_SET(m_write_sock, &fd);

			//select(0, nullptr, &fd, nullptr, &waittime);

			std::scoped_lock lock(m_outgoingLock);

			auto entry = _queuedOutgoing.begin();
			if (entry != _queuedOutgoing.end())
			{
				CQueuedPacket& queued = *entry;
				//if (SendPacket(queued.useReadStream ? m_read_sock : m_write_sock, &queued.addr, queued.data.get(), queued.len))
				if (SendPacket(queued.useReadStream ? m_read_sock : m_read_sock, &queued.addr, queued.data.get(), queued.len))
				{
					_queuedOutgoing.erase(entry);
				}

				std::this_thread::yield();
			}
		}

		std::this_thread::yield();
	}
}

WORD CNetwork::GetServerID(void)
{
	return m_ServerID;
}

void CNetwork::SendConnectlessBlob(SOCKADDR_IN *peer, BlobPacket_s *blob, DWORD dwFlags, DWORD dwSequence, WORD wTime, bool useReadStream)
{
	BlobHeader_s *header = &blob->header;

	header->dwSequence = dwSequence;
	header->dwFlags = dwFlags;
	header->dwCRC = 0;
	header->wRecID = GetServerID();
	header->wTime = wTime;
	header->wTable = 0x01;

	GenericCRC(blob);

	QueuePacket(peer, blob, BLOBLEN(blob), useReadStream);
}

bool CNetwork::SendPacket(SOCKET socket, SOCKADDR_IN *peer, void *data, DWORD len)
{
	//SOCKET socket = m_sockets[0];

	if (socket == INVALID_SOCKET)
	{
		return false;
	}

	//WINLOG(Temp, Normal, "Sending to %s:%d\n", inet_ntoa(peer->sin_addr), ntohs(peer->sin_port));

	int bytesSent = sendto(socket, (char *)data, len, 0, (sockaddr *)peer, sizeof(SOCKADDR_IN));

	bool success = (SOCKET_ERROR != bytesSent);

	if (success)
	{
		/*
#ifdef _DEBUG
		if (false) // g_bDebugToggle)
		{
			LOG(Network, Normal, "Sent:\n");
			LOG_BYTES(Network, Normal, data, len);
		}
		else
		{
			LOG(Network, Verbose, "Sent to %s:\n", inet_ntoa(peer->sin_addr));
			LOG_BYTES(Network, Verbose, data, len);
		}
#endif

		g_pGlobals->PacketSent(len);
		*/
	}

	return success;
}

void CNetwork::QueuePacket(SOCKADDR_IN *peer, void *data, DWORD len, bool useReadStream)
{
	CQueuedPacket qp;
	qp.addr = *peer;
	qp.data = std::make_unique<BYTE[]>(len);
	memcpy(qp.data.get(), data, len);
	qp.len = len;
	qp.useReadStream = useReadStream;

	{
		std::scoped_lock lock(m_outgoingLock);
		_queuedOutgoing.push_back(std::move(qp));
	}
	m_sigOutgoing.notify_all();
}

void CNetwork::QueueIncomingOnSocket(SOCKET socket)
{
	if (socket == INVALID_SOCKET)
	{
		return;
	}
	
	static BYTE	buffer[0x1E4];

	while (TRUE)
	{
		int clientaddrlen = sizeof(sockaddr_in);
		sockaddr_in clientaddr;

		// Doing it similar to AC..
		int bloblen = recvfrom(socket, (char *)buffer, 0x1E4, NULL, (sockaddr *)&clientaddr, &clientaddrlen);

		if (bloblen == SOCKET_ERROR)
		{
			DWORD dwCode = WSAGetLastError();

			if (dwCode != 10035)
			{
				// LOG(Temp, Normal, "Winsock Error %lu\n", dwCode);
			}

			break;
		}
		else if (!bloblen)
		{
			break;
		}
		else if (bloblen < sizeof(BlobHeader_s))
		{
			continue;
		}

		g_pGlobals->PacketRecv(bloblen);

		BlobPacket_s *blob = reinterpret_cast<BlobPacket_s*>(buffer);

		WORD wSize = blob->header.wSize;
		WORD wRecID = blob->header.wRecID;

		if ((bloblen - sizeof(BlobHeader_s)) != wSize)
			continue;

		CQueuedPacket qp;
		memcpy(&qp.addr, &clientaddr, sizeof(sockaddr_in));
		qp.data = std::make_unique<BYTE[]>(bloblen);
		memcpy(qp.data.get(), buffer, bloblen);
		qp.len = bloblen;
		qp.recvTime = g_pGlobals->UpdateTime();

		{
			std::scoped_lock lock(m_incomingLock);
			_queuedIncoming.push_back(std::move(qp));
		}

		// blob->header.dwCRC -= CalcTransportCRC((DWORD *)blob);
		
		/*
#ifdef _DEBUG
		SOCKADDR_IN addr;
		memset(&addr, 0, sizeof(addr));
		int namelen = sizeof(addr);
		getsockname(socket, (sockaddr *)&addr, &namelen);

		if (false) // g_bDebugToggle)
		{
			LOG(Network, Normal, "Received on port %d:\n", ntohs(addr.sin_port));
			LOG_BYTES(Network, Normal, &blob->header, blob->header.wSize + sizeof(blob->header));
		}
		else
		{
			LOG(Network, Verbose, "Received on port %d:\n", ntohs(addr.sin_port));
			LOG_BYTES(Network, Verbose, &blob->header, blob->header.wSize + sizeof(blob->header));
		}
#endif
*/

		/*
		if (!wRecID)
		{
			ProcessConnectionless(&clientaddr, blob);
		}
		else
		{
			CClient *client = ValidateClient(wRecID, &clientaddr);
			if (client)
				client->IncomingBlob(blob);
		}
		*/
	}

}

void CNetwork::ProcessQueuedIncoming()
{
	while (!_queuedIncoming.empty())
	{
		CQueuedPacket qp;
		{
			std::scoped_lock lock(m_incomingLock);
			qp = std::move(_queuedIncoming.front());
			_queuedIncoming.pop_front();
		}
			
		BlobPacket_s *blob = (BlobPacket_s *)qp.data.get();
		blob->header.dwCRC -= CalcTransportCRC((DWORD *)blob);

		if (!blob->header.wRecID)
		{
			ProcessConnectionless(&qp.addr, blob);
		}
		else if (CClient *client = ValidateClient(blob->header.wRecID, &qp.addr))
		{
			client->IncomingBlob(blob, qp.recvTime);
		}
	}
}

void CNetwork::LogoutAll()
{
	for (DWORD i = 0; i < m_NumClients; i++)
	{
		if (CClient *client = m_ClientArray[i])
		{
			if (client->IsAlive() && client->GetEvents())
			{
				client->GetEvents()->ForceLogout();
			}
		}
	}
}

void CNetwork::CompleteLogoutAll()
{
	for (DWORD i = 0; i < m_NumClients; i++)
	{
		if (CClient *client = m_ClientArray[i])
		{
			if (client->IsAlive() && client->GetEvents())
			{
				//todo: send the user to the appropriate server connection lost screen.
				BinaryWriter EnterPortal;
				EnterPortal.Write<DWORD>(0xF751);
				EnterPortal.Write<DWORD>(0);
				client->SendNetMessage(EnterPortal.GetData(), EnterPortal.GetSize(), OBJECT_MSG);

				BinaryWriter popupString;
				popupString.Write<DWORD>(4);
				popupString.WriteString("The server has shutdown.");
				client->SendNetMessage(&popupString, PRIVATE_MSG, FALSE, FALSE);
			}
		}
	}
}

void CNetwork::Think()
{
	ProcessQueuedIncoming();

	for (DWORD i = 0; i < m_NumClients; i++)
	{
		if (CClient *client = m_ClientArray[i])
		{
			client->Think();

			if (!client->IsAlive())
			{
				KillClient(client->GetSlot());
			}
		}
	}
}

CClient* CNetwork::GetClient(WORD slot)
{
	if (!slot || slot > m_MaxClients)
		return NULL;

	return m_ClientSlots[slot];
}

void CNetwork::KickClient(CClient *pClient)
{
	if (!pClient)
		return;

	SERVER_INFO << "Client" << pClient->GetSlot() << "(" << pClient->GetAccount() << ") is being kicked.";
	BinaryWriter KC;
	KC.Write<long>(0xF7DC);
	KC.Write<long>(0);

	pClient->SendNetMessage(KC.GetData(), KC.GetSize(), PRIVATE_MSG);
	pClient->ThinkOutbound();
	pClient->Kill(NULL, NULL);
}

void CNetwork::KickClient(WORD slot)
{
	KickClient(GetClient(slot));
}

CClient* CNetwork::ValidateClient(WORD index, sockaddr_in *peer)
{
	CClient* pClient = GetClient(index);

	if (!pClient)
		return NULL;

	if (!pClient->CheckAddress(peer))
		return NULL;

	return pClient;
}

void CNetwork::KillClient(WORD slot)
{
	if (CClient *pClient = GetClient(slot))
	{		
		SERVER_INFO << "Client" << pClient->GetAccount() << "(" << inet_ntoa(pClient->GetHostAddress()->sin_addr) << ") disconnected";

		DWORD arrayPos = pClient->GetArrayPos();

		delete pClient;

		m_NumClients--;
		m_ClientArray[arrayPos] = m_ClientArray[m_NumClients];
		m_ClientArray[m_NumClients] = NULL;
		m_ClientSlots[slot] = NULL;
		m_OpenSlots.push_front(slot);

		if (m_ClientArray[arrayPos])
		{
			m_ClientArray[arrayPos]->SetArrayPos(arrayPos);
		}

		// m_server->Stats().UpdateClientList(NULL, 0);
	}
}

CClient* CNetwork::FindClientByAccount(const char* account)
{
	for (DWORD i = 0; i < m_NumClients; i++)
	{
		if (CClient *client = m_ClientArray[i])
		{
			if (client->CheckAccount(account))
				return client;
		}
	}

	return NULL;
}

void CNetwork::SendConnectLoginFailure(sockaddr_in *addr, int error, const char *accountname, const char *password)
{
	//Bad login.
	CREATEBLOB(BadLogin, sizeof(DWORD));
	*((DWORD *)BadLogin->data) = 0x00000000;

	SendConnectlessBlob(addr, BadLogin, BT_ERROR, 0, 0, true);

	DELETEBLOB(BadLogin);

	if (strcmp(accountname, "acservertracker"))
	{
		SERVER_INFO << "Invalid login from " << inet_ntoa(addr->sin_addr) << ", used account name '" << accountname << "'";
	}
}

void CNetwork::ConnectionRequest(sockaddr_in *addr, BlobPacket_s *p)
{
	BinaryReader loginRequest(p->data, p->header.wSize);

	char *version_string = loginRequest.ReadString();
	loginRequest.ReadDWORD(); // 0x20 ?

	DWORD auth_method = loginRequest.ReadDWORD();
	loginRequest.ReadDWORD(); // 0x0 ?
	DWORD client_unix_timestamp = loginRequest.ReadDWORD(); // client unix timestamp

	char *login_credentials;

	if (auth_method != 1) // case 3 was turbine ticket method, took that out
		return; 

	login_credentials = loginRequest.ReadString();

	DWORD portal_stamp = loginRequest.ReadDWORD();
	DWORD cell_stamp = loginRequest.ReadDWORD();

	if (loginRequest.GetLastError()) 
		return;

	char *szPassword = strstr(login_credentials, ":");
	if (!szPassword) return;

	*(szPassword) = '\0';
	szPassword++;

	if (!strcmp(login_credentials, "acservertracker"))
	{
		SendConnectLoginFailure(addr, 0, login_credentials, szPassword);
		return;
	}

	// Try to login to the login_credentials
	AccountInformation_t accountInfo;
	int error = 0;
	if (!g_pDBIO->VerifyAccount(login_credentials, szPassword, &accountInfo, &error))
	{
		if (error == VERIFYACCOUNT_ERROR_DOESNT_EXIST && g_pConfig->AutoCreateAccounts())
		{
			std::string ipaddress = inet_ntoa(addr->sin_addr);

			// try to create the login_credentials
			if (g_pDBIO->CreateAccount(login_credentials, szPassword, &error, ipaddress.c_str()))
			{
				// now try to verify the newly created login_credentials
				if (!g_pDBIO->VerifyAccount(login_credentials, szPassword, &accountInfo, &error))
				{
					// fail
					SendConnectLoginFailure(addr, error, login_credentials, szPassword);
					return;
				}
			}
			else
			{
				// fail
				SendConnectLoginFailure(addr, error, login_credentials, szPassword);
				return;
			}
		}
		else
		{
			// fail
			SendConnectLoginFailure(addr, error, login_credentials, szPassword);
			return;
		}
	}
	
	CClient *client = FindClientByAccount(accountInfo.username.c_str());

	if (client)
	{
		if (_stricmp(client->GetAccount(), "admin"))
		{
			KickClient(client);
			// TODO don't allow this player to login for a few seconds while the world handles the other player
		}
		else
		{
			return;
		}
	}

	WORD slot = AllocOpenClientSlot();

	if (!slot)
	{
		// Server unavailable.
		CREATEBLOB(ServerFull, sizeof(DWORD));
		*((DWORD *)ServerFull->data) = 0x00000005;

		SendConnectlessBlob(addr, ServerFull, BT_ERROR, 0, 0, true);

		DELETEBLOB(ServerFull);
		return;
	}

	SERVER_INFO << "Client" << login_credentials << "(" << inet_ntoa(addr->sin_addr) << ":" << ntohs(addr->sin_port) << ") connected on slot" << slot;

	client = m_ClientSlots[slot] = new CClient(addr, slot, accountInfo);
	client->SetLoginData(client_unix_timestamp, portal_stamp, cell_stamp);

	m_ClientArray[m_NumClients] = client;
	client->SetArrayPos(m_NumClients);
	m_NumClients++;

	// Add the client to the HUD
	// m_server->Stats().UpdateClientList(m_clients, m_slotrange);

	BinaryWriter AcceptConnect;

	// Some server variables
	AcceptConnect.Write<double>(g_pGlobals->Time());

	BYTE cookie[] = {
		0xbe, 0xc8, 0x8a, 0x58, 0x0b, 0x1e, 0x99, 0x43
	};
	AcceptConnect.Write(cookie, sizeof(cookie));

	AcceptConnect.Write<DWORD>(slot);
	AcceptConnect.Write<DWORD>(client->GetPacketController()->GetServerCryptoSeed());
	AcceptConnect.Write<DWORD>(client->GetPacketController()->GetClientCryptoSeed());
	AcceptConnect.Write<DWORD>(2);

	DWORD dwLength = AcceptConnect.GetSize();

	if (dwLength <= 0x1D0)
	{
		CREATEBLOB(Woot, (WORD)dwLength);
		memcpy(Woot->data, AcceptConnect.GetData(), dwLength);

		SendConnectlessBlob(addr, Woot, BT_LOGINREPLY, 0x00000000, 0, true);

		DELETEBLOB(Woot);
	}
	else
	{
		SERVER_INFO << "AcceptConnect.GetSize() > 0x1D0";
	}
}

WORD CNetwork::AllocOpenClientSlot()
{
	// Allocate an available slot for a connecting client

	if (m_OpenSlots.empty())
		return 0;

	DWORD slot = *m_OpenSlots.begin();
	m_OpenSlots.pop_front();

	return slot;
}

void CNetwork::ProcessConnectionless(sockaddr_in *peer, BlobPacket_s *blob)
{
	DWORD dwFlags = blob->header.dwFlags;

	if (dwFlags == BT_LOGIN)
	{
		if (!IsBannedIP(peer->sin_addr))
		{
			ConnectionRequest(peer, blob);
		}

		return;
	}

	// LOG(Network, Verbose, "Unhandled connectionless packet received: 0x%08X Look into this\n", dwFlags);
}

DEFINE_PACK(CBanDescription)
{
	pWriter->Write<DWORD>(1); // version
	pWriter->WriteString(m_AdminName);
	pWriter->WriteString(m_Reason);
	pWriter->Write<DWORD>(m_Timestamp);
}

DEFINE_UNPACK(CBanDescription)
{
	DWORD version = pReader->Read<DWORD>();
	m_AdminName = pReader->ReadString();
	m_Reason = pReader->ReadString();
	m_Timestamp = pReader->Read<DWORD>();
	return true;
}

DEFINE_PACK(CNetworkBanList)
{
	pWriter->Write<DWORD>(1); // version
	m_BanTable.Pack(pWriter);
}

DEFINE_UNPACK(CNetworkBanList)
{
	m_BanTable.clear();

	DWORD version = pReader->Read<DWORD>();
	m_BanTable.UnPack(pReader);
	return true;
}

bool CNetwork::IsBannedIP(in_addr ipaddr)
{
	return m_Bans.m_BanTable.lookup(ipaddr.s_addr) ? true : false;
}

void CNetwork::LoadBans()
{
	void *data = NULL;
	DWORD length = 0;
	if (g_pDBIO->GetGlobalData(DBIO_GLOBAL_BAN_DATA, &data, &length))
	{
		BinaryReader reader(data, length);
		m_Bans.UnPack(&reader);
	}
}

void CNetwork::SaveBans()
{
	BinaryWriter banData;
	m_Bans.Pack(&banData);
	g_pDBIO->CreateOrUpdateGlobalData(DBIO_GLOBAL_BAN_DATA, banData.GetData(), banData.GetSize());
}

void CNetwork::AddBan(in_addr ipaddr, const char *admin, const char *reason)
{
	CBanDescription *pBan = &m_Bans.m_BanTable[ipaddr.s_addr];

	pBan->m_AdminName = admin;
	pBan->m_Reason = reason;
	pBan->m_Timestamp = time(0);

	SaveBans();
}

bool CNetwork::RemoveBan(in_addr ipaddr)
{
	if (m_Bans.m_BanTable.lookup(ipaddr.s_addr))
	{
		m_Bans.m_BanTable.erase(ipaddr.s_addr);

		SaveBans();
		return true;
	}

	return false;
}

std::string CNetwork::GetBanList()
{
	std::string banList = csprintf("Ban List (%d entries):", m_Bans.m_BanTable.size());
	for (auto &entry : m_Bans.m_BanTable)
	{
		CBanDescription *pBan = &entry.second;
		banList += csprintf("\n%s - Admin: %s @ %s Reason: %s\n", inet_ntoa(*(in_addr *)&entry.first), pBan->m_AdminName.c_str(), timestampDateString(pBan->m_Timestamp), pBan->m_Reason.c_str());
	}

	return banList;
}








