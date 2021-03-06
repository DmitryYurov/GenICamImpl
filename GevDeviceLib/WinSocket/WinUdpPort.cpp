#include "UdpPort.h"

#include <list>
#include <cassert>
#include <thread>
#include <atomic>
#include <Winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h> // shall follow winsock2

namespace gevdevice{

class UdpPortImpl
{
public:
	UdpPortImpl() : m_started(false)
	{}

	SOCKET sockfd;
	sockaddr_in remote;

	std::shared_ptr<std::thread> m_threadPtr;
	std::shared_ptr<std::thread> m_queueThreadPtr;
	std::atomic<bool> m_started;
	std::atomic<bool> m_queueStarted;
	std::vector<UdpPort::ReceaveHandler> m_heandlerList;

	std::mutex m_bufferMutex;

	struct msg
	{
		msg() {}

		msg(const sockaddr_in& from, const uint8_t* data, int length)
			: from(from), data(data, data + length)
		{
		}

		sockaddr_in from;
		std::vector<uint8_t> data;
	};

	std::list<msg> m_buffer;
};

bool UdpPort::InitSockets()
{
	static WSADATA wsaData;
	int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (rc != 0) {
		return false;
	}

	return true;
}


void UdpPort::FinishSockets()
{
	WSACleanup();
}

UdpPort::UdpPort()
{
	m_implPtr.reset(new UdpPortImpl());
}

UdpPort::~UdpPort()
{
	Stop();
}

std::vector<uint32_t> UdpPort::GetLocalAddressList()
{
	std::vector<uint32_t> result;

	char ac[80];
	if (gethostname(ac, sizeof(ac)) == SOCKET_ERROR) {
		return result;
	}

	struct addrinfo *addrResult = NULL;
	struct addrinfo hints;

	ZeroMemory( &hints, sizeof(hints) );
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	DWORD dwRetval = getaddrinfo(ac, "0", &hints, &addrResult);
	if ( dwRetval != 0 ) {
		return result;
	}

	struct sockaddr_in *sockaddr_ipv4;
	struct addrinfo *ptr = NULL;
	for(ptr=addrResult; ptr != NULL ;ptr=ptr->ai_next) {
		switch (ptr->ai_family) {
		case AF_INET:
			sockaddr_ipv4 = (struct sockaddr_in *) ptr->ai_addr;
			result.push_back(sockaddr_ipv4->sin_addr.S_un.S_addr);
			break;
		default:
			break;
		}
	}

	return result;
}

void UdpPort::AddHandler(const ReceaveHandler & handler)
{
	assert(!m_implPtr->m_started);
	m_implPtr->m_heandlerList.push_back(handler);
}

bool UdpPort::Start(uint32_t localip, uint16_t localport, uint32_t remoteip, uint16_t remoteport, const int sndbuf, const int rcvbuf)
{
	if (m_implPtr->m_started){
		return false;
	}

	m_implPtr->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (m_implPtr->sockfd == -1) {
		return false;
	}

	char yes = 1;
	if (setsockopt(m_implPtr->sockfd, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes)) == -1) {
		return false;
	}

	if (remoteip == 0xFFFFFFFF) {
		char broadcast = 1;
		if (setsockopt(m_implPtr->sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast) == -1)){
			return false;
		}
	}

	if (sndbuf != 0) {
		setsockopt(m_implPtr->sockfd, SOL_SOCKET, SO_SNDBUF, (char *)& sndbuf, sizeof(int));
	}

	if (rcvbuf != 0) {
		setsockopt(m_implPtr->sockfd, SOL_SOCKET, SO_RCVBUF, (char *)& rcvbuf, sizeof(int));
	}

	listen(m_implPtr->sockfd, 5);

	sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.S_un.S_addr = localip;
	local.sin_port = htons(localport);

	if (bind(m_implPtr->sockfd, (sockaddr*)&local, sizeof(local)) == -1) {
		closesocket(m_implPtr->sockfd);
		return false;
	}

	memset(&m_implPtr->remote, 0, sizeof(m_implPtr->remote));
	m_implPtr->remote.sin_family = AF_INET;
	m_implPtr->remote.sin_port = htons(remoteport);
	m_implPtr->remote.sin_addr.S_un.S_addr = remoteip;

	std::condition_variable cv;

	m_implPtr->m_threadPtr.reset(new std::thread([this, &cv](){
		m_implPtr->m_started = true;
		cv.notify_one();

		const int tn = 1024 * 16;
		uint8_t t[tn];

		sockaddr_in from;
		socklen_t fromlen = sizeof(sockaddr_in);
			
		while (m_implPtr->m_started){
			fd_set setread;
			FD_ZERO(&setread);
			FD_SET(m_implPtr->sockfd, &setread);

			timeval timeout_directly = { 1, 0 };

			int ret = select(0, &setread, NULL, NULL, &timeout_directly);

			if (ret < 0){
				break;
			}
			else if (ret == 0){
				continue;
			}

			int recvd = recvfrom(m_implPtr->sockfd, (char*)t, tn, 0, (sockaddr*)&from, &fromlen);
			if (recvd > 0){
				std::unique_lock<std::mutex> lock(m_implPtr->m_bufferMutex);
				m_implPtr->m_buffer.emplace(m_implPtr->m_buffer.end(), from, t, recvd);
			}
		}
	}));

	m_implPtr->m_queueThreadPtr.reset(new std::thread([this, &cv](){
		m_implPtr->m_queueStarted = true;
		cv.notify_one();

		while (m_implPtr->m_queueStarted){
			std::list<UdpPortImpl::msg> msgs;
			{
				std::unique_lock<std::mutex> lock(m_implPtr->m_bufferMutex);
				std::swap(msgs, m_implPtr->m_buffer);
			}

			if (msgs.empty()){
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
			else {
				for (const auto& m : msgs) {
					for (const auto& handler : m_implPtr->m_heandlerList) {
						handler(m.data, m.from.sin_addr.s_addr, m.from.sin_port);
					}
				}
			}
		}
	}));

	std::mutex m;
	std::unique_lock<std::mutex> lk(m);
	cv.wait(lk, [this] {
		return IsStarted();
	});

	return true;
}

void UdpPort::Stop()
{
	if (!IsStarted()) {
		return;
	}

	m_implPtr->m_started = false;
	m_implPtr->m_queueStarted = false;

	if (m_implPtr->m_threadPtr) {
		m_implPtr->m_threadPtr->join();
	}

	closesocket(m_implPtr->sockfd);

	if (m_implPtr->m_queueThreadPtr) {
		m_implPtr->m_queueThreadPtr->join();
	}

	m_implPtr->m_heandlerList.clear();
}

bool UdpPort::IsStarted()
{
	return m_implPtr->m_started && m_implPtr->m_queueStarted;
}

bool UdpPort::Send(const std::vector<uint8_t>& data) const
{
	return sendto(m_implPtr->sockfd, (const char*)data.data(), int(data.size()), 0, (sockaddr*)&m_implPtr->remote, sizeof(sockaddr_in)) != 0;
}

uint16_t UdpPort::Htons(uint16_t val)
{
	return htons(val);
}

uint32_t UdpPort::Htonl(uint32_t val)
{
	return htonl(val);
}

uint16_t UdpPort::Ntohs(uint16_t val)
{
	return ntohs(val);
}

uint32_t UdpPort::Ntohl(uint32_t val)
{
	return ntohl(val);
}

uint64_t UdpPort::MacAddress(uint32_t ip)
{
	uint64_t macAddress = 0;
	ULONG phyAddrLen = 8;  /* six bytes would be enough - but uint64_t is 8 bytes long */

	//Send an arp packet
	DWORD err = SendARP(ip, 0, &macAddress, &phyAddrLen);

	return phyAddrLen > 0 && err == NO_ERROR ? macAddress : 0;
}

}
