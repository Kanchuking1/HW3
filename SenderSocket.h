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
#define MAX_SEND_ATTEMPTS	10

#define TIME_BETWEEN_STAT	2

#define ALPHA				0.125
#define BETA				0.25

#define TIME_SINCE_START (double)(clock() - startTime) / CLOCKS_PER_SEC
#define TIME_SINCE(val) (double)(clock() - val) / CLOCKS_PER_SEC


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

class SenderDataPacket {
public:
	SenderDataHeader sdh;
	char data[MAX_PKT_SIZE];
};
#pragma pack(pop)

class SenderSocket {
	struct sockaddr_in local;
	struct sockaddr_in server;
	struct hostent* remote;
	bool open;
	SOCKET sock;
	bool printOutput;

	clock_t processStartTime;

	int receiverWindow;
	int previousSeqNo;

	double RTO;
	double devRTT;

	DWORD doSYNACK(SenderSynHeader* packet);
	DWORD doFINACK(double* timeElapsed);
	DWORD sendDataPkt(char* buffer, int bufferSize);

	static DWORD WINAPI statsThread(LPVOID lpParam);

	HANDLE stats;

	void updateRTO(double sampleRTT);

	SenderSynHeader* packet;
public: 
	clock_t startTime;
	double estimatedRTT;
	int synSize;
	int dataSize;
	int sequenceNumber;
	int bytesAcked;
	int timeoutCount;

	SenderSocket(bool printOutput);
	SenderSocket();
	DWORD Open(string targetHost, int port, int senderWindow, LinkProperties* lp);
	DWORD Send(char* ptr, int bytes);
	DWORD Close(double *timeElapsed);
};