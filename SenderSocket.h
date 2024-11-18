#pragma once
#pragma comment(lib, "ws2_32")

#include "pch.h"

using namespace std;

#define MAGIC_PORT			22345 // receiver listens on this port
#define MAX_PKT_SIZE		(1500-28) // maximum UDP packet size accepted by receiver

#define STATUS_OK			0 // no error
#define ALREADY_CONNECTED	1 // second call to ss.Open() without closing connection
#define NOT_CONNECTED		2 // call to ss.Send()/Close() without ss.Open()
#define INVALID_NAME		3 // ss.Open() with targetHost that has no DNS entry
#define FAILED_SEND			4 // sendto() failed in kernel
#define TIMEOUT				5 // timeout after all retx attempts are exhausted
#define FAILED_RECV			6 // recvfrom() failed in kernel
#define NOT_IMPLEMENTED		7
#define BAD_SOCKET			8

#define FORWARD_PATH		0
#define RETURN_PATH			1

#define MAGIC_PROTOCOL		0x8311AA

#define MAX_SYN_ATTEMPTS	3
#define MAX_FIN_ATTEMPTS	5
#define MAX_SEND_ATTEMPTS	50

#define TIME_BETWEEN_STAT	2

#define ALPHA				0.125
#define BETA				0.25

#define TIME_SINCE_START (double)(clock() - startTime) / CLOCKS_PER_SEC
#define TIME_SINCE(val) (double)(clock() - val) / CLOCKS_PER_SEC
#define PACKET_IN_BUFFER(slot) &((Packet*)(pending_pkts))[slot]
#define PACKET_IN_BUFFER_SS(slot) &((Packet*)(ss->pending_pkts))[slot]


#pragma pack(push, 1)
class LinkProperties {
public:
	// transfer parameters
	float RTT; // propagation RTT (in sec)
	float speed; // bottleneck bandwidth (in bits/sec)
	float pLoss[2]; // probability of loss in each direction
	DWORD bufferSize; // buffer size of emulated routers (in packets)
	LinkProperties() { memset(this, 0, sizeof(*this)); }
};

class Flags {
public:
	DWORD reserved : 5; // must be zero
	DWORD SYN : 1;
	DWORD ACK : 1;
	DWORD FIN : 1;
	DWORD magic : 24;
	Flags() { memset(this, 0, sizeof(*this)); magic = MAGIC_PROTOCOL; }
};

class SenderDataHeader {
public:
	Flags flags;
	DWORD seq; // must begin from 0
};

class SenderSynHeader {
public:
	SenderDataHeader sdh;
	LinkProperties lp;
};

class ReceiverHeader {
public:
	Flags flags;
	DWORD recvWnd; // receiver window for flow control (in pkts)
	DWORD ackSeq; // ack value = next expected sequence
};

class SenderData {
public:
	SenderDataHeader sdh;
	char data[MAX_PKT_SIZE];
};

class Packet {
public:
	int type; // SYN, FIN, data
	int size; // bytes in packet data
	clock_t txTime; // transmission time
	SenderData pkt; // packet with header
};
#pragma pack(pop)

class SenderSocket {
	// CONNECTION DETAILS
	struct sockaddr_in local;
	struct sockaddr_in server;
	struct hostent* remote;
	SOCKET sock;

	// DEBUG VARIABLES
	bool open;
	bool printOutput;

	// RDT VARIABLES
	int senderBase; // Base of the current Window
	int nextSeq; // Where to add the next packet
	int senderWindow;
	int dupACK;
	char* pending_pkts; // Circular Queue
	int packetsRemaining;
	int effectiveWin;
	int lastReleased; // Last pkt that was sent
	int nextToSend; // Next Packet to be sent
	
	double RTO;
	double devRTT;

	// Private Helpers
	DWORD SetupAndBindToServer(string targetHost, int port, int senderWindow, LinkProperties* lp);
	void updateRTO(double sampleRTT);
	DWORD doSYNACK();
	DWORD doFINACK(double* timeElapsed);
	DWORD ReceiveACK();
	DWORD SendDataPkt(Packet* p);
	void recomputeTimerExpire();
	void printReceivedPkt(ReceiverHeader* response);
	void printSendPkt(Packet* pkt);
	void printCurrentSequenceNumbers();
	
	// Special Packet for SYN and FIN
	SenderSynHeader* synPacket;

	clock_t processStartTime;
	HANDLE eventQuit, empty, socketReceiveReady, full;

	static DWORD WINAPI statsThread(LPVOID self);
	static DWORD WINAPI WorkerRun(LPVOID self);

	HANDLE stats;
	HANDLE worker;

	clock_t timerExpire;
	clock_t timeout;

	mutex mLock;

	bool closeCalled;
public: 
	clock_t startTime;
	double estimatedRTT;
	int synSize;
	int bytesAcked;
	int timeoutCount;
	int fastRetxCount;

	SenderSocket(bool printOutput = false);
	~SenderSocket();
	DWORD Open(string targetHost, int port, int senderWindow, LinkProperties* lp);
	DWORD Send(char* ptr, int bytes);
	DWORD Close(double *timeElapsed);
};