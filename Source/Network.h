
#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>

#define MAX_CONNECTED_CLIENTS 600

class CBanDescription : public PackObj
{
public:
	DECLARE_PACKABLE()

	std::string m_AdminName;
	std::string m_Reason;
	DWORD m_Timestamp;
};

class CNetworkBanList : public PackObj
{
public:
	DECLARE_PACKABLE()

	PackableHashTable<DWORD, CBanDescription> m_BanTable;
};

class CQueuedPacket
{
public:
	CQueuedPacket() = default;
	CQueuedPacket(CQueuedPacket&&) = default;
	//CQueuedPacket(CQueuedPacket&) = default;
	CQueuedPacket& operator=(CQueuedPacket&&) = default;

	SOCKADDR_IN addr;
	//BYTE *data = NULL;
	std::unique_ptr<BYTE[]> data;
	DWORD len = 0;
	double recvTime;
	bool useReadStream;
};

class CNetwork
{
public:
	CNetwork(class CPhatServer *server, in_addr address, WORD port);
	~CNetwork();

	void Think();
	CClient *GetClient(WORD index);
	WORD GetServerID();

	void LogoutAll();
	void CompleteLogoutAll();

	void KickClient(class CClient* pClient);
	void KickClient(WORD slot);
	void KillClient(WORD slot);
	void QueuePacket(SOCKADDR_IN *peer, void *data, DWORD len) { QueuePacket(peer, data, len, false); };
	void QueuePacket(SOCKADDR_IN *peer, void *data, DWORD len, bool useReadStream);
	void SendConnectlessBlob(SOCKADDR_IN *peer, BlobPacket_s *blob, DWORD dwFlags, DWORD dwSequence, WORD wTime)
	{
		SendConnectlessBlob(peer, blob, dwFlags, dwSequence, wTime, false);
	};
	void SendConnectlessBlob(SOCKADDR_IN *peer, BlobPacket_s *blob, DWORD dwFlags, DWORD dwSequence, WORD wTime, bool useReadStream);

	void AddBan(in_addr ipaddr, const char *admin, const char *reason);
	bool RemoveBan(in_addr ipaddr);
	std::string GetBanList();

	DWORD GetNumClients();

private:

	bool IsBannedIP(in_addr ipaddr);
	void LoadBans();
	void SaveBans();

	WORD AllocOpenClientSlot();

	CClient *ValidateClient(WORD, sockaddr_in *);
	CClient *FindClientByAccount(const char *);

	void SendConnectLoginFailure(sockaddr_in *addr, int error, const char *accountname, const char *password);
	void ConnectionRequest(sockaddr_in *addr, BlobPacket_s *p);
	void ProcessConnectionless(sockaddr_in *, BlobPacket_s *);

	class CPhatServer *m_server;
	int m_socketCount;

	WORD m_ServerID;

	/*
	WORD m_freeslot;
	WORD m_slotrange;
	CClient *m_clients[400];
	*/

	CClient **m_ClientArray = NULL;
	CClient **m_ClientSlots = NULL;
	std::list<WORD> m_OpenSlots;
	DWORD m_NumClients = 0;
	DWORD m_MaxClients = MAX_CONNECTED_CLIENTS;

	CNetworkBanList m_Bans;

	std::list<CQueuedPacket *> m_PacketQueue;

	void Init();
	void Shutdown();

	void IncomingThreadProc();
	void OutgoingThreadProc();

	void QueueIncomingOnSocket(SOCKET socket);
	void ProcessQueuedIncoming();
	bool SendPacket(SOCKET socket, SOCKADDR_IN *peer, void *data, DWORD len);

	bool m_running;

	WORD m_port;
	in_addr m_addr;

	SOCKET m_read_sock;
	SOCKET m_write_sock;

	std::thread m_incomingThread;
	std::thread m_outgoingThread;

	std::mutex m_incomingLock;
	std::mutex m_outgoingLock;

	std::mutex m_sigIncomingLock;
	std::condition_variable m_sigIncoming;

	std::mutex m_sigOutgoingLock;
	std::condition_variable m_sigOutgoing;

	std::list<CQueuedPacket> _queuedIncoming;
	std::list<CQueuedPacket> _queuedOutgoing;
};



