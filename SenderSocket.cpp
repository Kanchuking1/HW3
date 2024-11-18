/* 
    Submission for HW3P3
    Wahib Sabir Kapdi 635009343
    
*/
#include "pch.h"
#include "SenderSocket.h"
#include <string>

using namespace std;

#pragma region Constructor/Deconstructor
SenderSocket::SenderSocket(bool printOutput) {
    startTime = clock();
    open = false;
    synSize = 0;
    memset(&server, 0, sizeof(server));
    RTO = 1;
    nextSeq = 0;
    timeoutCount = 0;
    bytesAcked = 0;
    senderBase = 0;
    devRTT = 0.01;
    packetsRemaining = 0;
    lastReleased = 0;
    nextToSend = 0;
    closeCalled = false;

    this->printOutput = printOutput;
}

SenderSocket::~SenderSocket() {
    CloseHandle(full);
    CloseHandle(empty);
    CloseHandle(socketReceiveReady);
    CloseHandle(eventQuit);
    CloseHandle(worker);
    CloseHandle(stats);
}
#pragma endregion

#pragma region Public APIs
DWORD SenderSocket::Open(string targetHost, int port, int senderWindow, LinkProperties* lp) {
    if (open) {
        return ALREADY_CONNECTED;
    }
    // Initialize all local variables
    open = true;

    int status = 0;

    if ((status = SetupAndBindToServer(targetHost, port, senderWindow, lp)) != STATUS_OK) {
        return status;
    }

    // Initialize Semaphores and Events and WSASelectEvent
    empty = CreateSemaphore(NULL, senderWindow, senderWindow, NULL);
    full = CreateSemaphore(NULL, 0, senderWindow, NULL);

    eventQuit = CreateEvent(NULL, TRUE, FALSE, NULL);
    socketReceiveReady = CreateEvent(NULL, FALSE, FALSE, NULL);

    WSAEventSelect(sock, socketReceiveReady, FD_READ);

    // Initialize SYN/FIN Packet
    synPacket = new SenderSynHeader();
    synPacket->lp.RTT = lp->RTT;

    synPacket->lp.bufferSize = senderWindow + MAX_SYN_ATTEMPTS;

    synPacket->lp.speed = lp->speed;
    synPacket->lp.pLoss[0] = lp->pLoss[0];
    synPacket->lp.pLoss[1] = lp->pLoss[1];

    pending_pkts = new char[senderWindow * sizeof(Packet)];

    if ((status = doSYNACK()) != STATUS_OK) {
        return status;
    }

    // Initialize the worker and stats thread
    startTime = clock();
    worker = CreateThread(NULL, 0, WorkerRun, this, 0, NULL);
    stats = CreateThread(NULL, 0, statsThread, this, 0, NULL);

    return status;
}

DWORD SenderSocket::Send(char* buffer, int bufferSize)
{
    if (!open) {
        return NOT_CONNECTED;
    }
    //return STATUS_OK;

    HANDLE arr[] = { eventQuit, empty };
    WaitForMultipleObjects(2, arr, FALSE, INFINITE);
    // no need for mutex as no shared variables are modified
    int slot = nextSeq % senderWindow;
    Packet* p = ((Packet*)(pending_pkts)) + slot;  // pointer to packet struct
    SenderData* pkt = new SenderData();
    pkt->sdh.seq = nextSeq;
    p->type = 1;
    p->size = bufferSize + sizeof(SenderDataHeader);
    memcpy(pkt->data, buffer, bufferSize);
    memcpy(&(p->pkt), pkt, sizeof(SenderData));
    mLock.lock();
    nextSeq++;
    packetsRemaining++;
    mLock.unlock();
    ReleaseSemaphore(full, 1, NULL);

    return STATUS_OK;
}

DWORD SenderSocket::Close(double* timeElapsed)
{
    if (!open) {
        return NOT_CONNECTED;
    }
    open = false;
    closeCalled = true;

    if (packetsRemaining > 0)
        WaitForSingleObject(eventQuit, INFINITE);

    CloseHandle(stats);
    CloseHandle(worker);

    return doFINACK(timeElapsed);
}
#pragma endregion

#pragma region Private Helpers
void SenderSocket::updateRTO(double sampleRTT) {
    estimatedRTT = (1 - ALPHA) * estimatedRTT + ALPHA * sampleRTT;
    devRTT = (1 - BETA) * devRTT + BETA * abs(estimatedRTT - sampleRTT);
    RTO = estimatedRTT + 4 * max(devRTT, 0.010);
}

DWORD SenderSocket::SetupAndBindToServer(string targetHost, int port, int senderWindow, LinkProperties* lp) {
    WSADATA wsaData;
    WORD wVersionRequested = MAKEWORD(2, 2);
    if (WSAStartup(wVersionRequested, &wsaData) != 0) {
        WSACleanup();
        return BAD_SOCKET;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return -1;
    }

    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(0);

    if (bind(sock, (struct sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        WSACleanup();
        return -1;
    }

    this->senderWindow = senderWindow;

    DWORD serverIP = inet_addr(targetHost.c_str());
    if (serverIP == INADDR_NONE) {
        if ((remote = gethostbyname(targetHost.c_str())) == NULL) {
            printOutput&& printf(" [ %.3f] --> target %s in invalid\n", TIME_SINCE_START, targetHost.c_str());
            //WSACleanup();
            return INVALID_NAME;
        }
        memcpy((char*)&server.sin_addr, remote->h_addr, remote->h_length);
    }
    else {
        server.sin_addr.S_un.S_addr = serverIP;
    }

    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    return STATUS_OK;
}

DWORD SenderSocket::doSYNACK() {
    synPacket->sdh.flags.SYN = 1;
    synPacket->sdh.flags.FIN = 0;
    synPacket->sdh.seq = nextSeq;
    RTO = max(1, 2 * synPacket->lp.RTT);
    int available = 0;

    for (int i = 1; i <= MAX_SYN_ATTEMPTS; i++) {
        printOutput&& printf(" [ %.3f] --> ", TIME_SINCE_START);
        printOutput&& printf("SYN 0 (attempt %d of %d, RTO %.3f) to %s\n",
            i,
            MAX_SYN_ATTEMPTS,
            RTO,
            inet_ntoa(server.sin_addr));

        processStartTime = clock();

        if (sendto(sock,
            (char*)synPacket,
            sizeof(SenderSynHeader),
            0,
            (sockaddr*)&server,
            sizeof(server)) == SOCKET_ERROR) {
            printOutput&& printf(" [ %.3f] --> failed sendto with %d\n", TIME_SINCE_START, WSAGetLastError());
            //WSACleanup();
            return FAILED_SEND;
        }

        fd_set fdRead;
        FD_ZERO(&fdRead);
        FD_SET(sock, &fdRead);

        available = WaitForSingleObject(socketReceiveReady, (DWORD)(RTO * 1000));

        if (available == 0) {
            continue;
        }
        else if (available < 0) {
            printOutput&& printf(" [ %.3f] <-- failed recvfrom with %d\n", TIME_SINCE_START, WSAGetLastError());
            return FAILED_RECV;
        }

        char* synBuffer = new char[sizeof(SenderSynHeader)];

        sockaddr synResponseAddr;
        int synAddrSize = sizeof(synResponseAddr);

        int synBytes = recvfrom(sock,
            synBuffer,
            sizeof(SenderSynHeader),
            0,
            (struct sockaddr*)&synResponseAddr,
            &synAddrSize);

        if (synBytes < 0) {
            printOutput&& printf(" [ %.3f] <-- failed recvfrom with %d\n", TIME_SINCE_START, WSAGetLastError());
            return FAILED_RECV;
        }

        synSize += synBytes;

        ReceiverHeader* synResponse = (ReceiverHeader*)(synBuffer);

        RTO = 3 * TIME_SINCE(processStartTime);
        estimatedRTT = RTO;

        if (synResponse->flags.SYN == 1 && synResponse->flags.ACK == 1) {
            printOutput&& printf(" [ %.3f] <-- SYN-ACK 0 window %d; setting initial RTO to %.3f\n",
                TIME_SINCE_START,
                synResponse->recvWnd,
                RTO);
            return STATUS_OK;
        }
    }

    return TIMEOUT;
}

DWORD SenderSocket::doFINACK(double* timeElapsed) {
    int available = 0;
    
    synPacket->sdh.flags.SYN = 0;
    synPacket->sdh.flags.FIN = 1;
    synPacket->sdh.flags.reserved = 0;
    synPacket->sdh.flags.ACK = 0;
    synPacket->sdh.seq = senderBase;

    for (int i = 1; i <= MAX_FIN_ATTEMPTS; i++) {
        printOutput&& printf(" [ %.3f] --> ", TIME_SINCE_START);

        printOutput&& printf("FIN %d (attempt %d of %d, RTO %.3f)\n",
            synPacket->sdh.seq,
            i,
            MAX_FIN_ATTEMPTS,
            RTO);

        processStartTime = clock();

        if (sendto(sock,
            (char*)synPacket,
            sizeof(SenderSynHeader),
            0,
            (sockaddr*)&server,
            sizeof(server)) == SOCKET_ERROR) {
            printOutput&& printf(" [ %.3f] --> failed sendto with %d\n", TIME_SINCE_START, WSAGetLastError());
            return FAILED_SEND;
        }

        fd_set fdRead;
        FD_ZERO(&fdRead);
        FD_SET(sock, &fdRead);

        timeval timeout;
        timeout.tv_sec = floor(RTO);
        timeout.tv_usec = (RTO - floor(RTO)) * 1e6;

        available = select(0, &fdRead, NULL, NULL, &timeout);

        if (available == 0) {
            continue;
        }
        else if (available < 0) {
            printCurrentSequenceNumbers();
            printOutput&& printf(" [ %.3f] <-- failed recvfrom with %d\n", TIME_SINCE_START, WSAGetLastError());
            if (i == MAX_FIN_ATTEMPTS) {
                return FAILED_RECV;
            }
            continue;
        }

        char* synBuffer = new char[sizeof(SenderSynHeader)];

        sockaddr synResponseAddr;
        int synAddrSize = sizeof(synResponseAddr);

        int synBytes = recvfrom(sock,
            synBuffer,
            sizeof(SenderSynHeader),
            0,
            (struct sockaddr*)&synResponseAddr,
            &synAddrSize);

        if (synBytes < 0) {
            printCurrentSequenceNumbers();
            printOutput&& printf(" [ %.3f] <-- failed recvfrom with %d\n", TIME_SINCE_START, WSAGetLastError());
            if (i == MAX_FIN_ATTEMPTS) {
                return FAILED_RECV;
            }
            continue;
        }

        struct ReceiverHeader* synResponse = (ReceiverHeader*)(synBuffer);

        if (synResponse->flags.FIN == 1 && synResponse->flags.ACK == 1) {
            printf("[ %.2f] <-- FIN-ACK %d window %X\n",
                TIME_SINCE_START,
                synResponse->ackSeq,
                synResponse->recvWnd);
            *timeElapsed = TIME_SINCE_START;
            return STATUS_OK;
        }
    }

    return TIMEOUT;
}

DWORD SenderSocket::ReceiveACK() {
    char* recvBuffer = new char[sizeof(SenderSynHeader)];
    sockaddr responseAddr;
    bool flag = false;
    int addrSize = sizeof(responseAddr);
    int bytes = recvfrom(sock,
        recvBuffer,
        sizeof(SenderSynHeader),
        0,
        (struct sockaddr*)&responseAddr,
        &addrSize);

    ReceiverHeader* response = (ReceiverHeader*)recvBuffer;

    if (response->flags.SYN || response->flags.FIN) {
        return -1;
    }

    //printReceivedPkt(response);
    int y = response->ackSeq;
    if (y > senderBase) {
        int diff = y - senderBase;
        //printf("Set Base to %d from %d and nextSeq %d\n", y, senderBase, nextSeq);
        senderBase = y;
        dupACK = 0;
        
        mLock.lock();
        packetsRemaining -= diff;
        mLock.unlock();
        updateRTO(TIME_SINCE((PACKET_IN_BUFFER((y - 1) % senderWindow))->txTime));
        effectiveWin = min(senderWindow, response->recvWnd);
        bytesAcked += MAX_PKT_SIZE * diff;

         //how much we can advance the semaphore
        /*int newReleased = senderBase + effectiveWin - lastReleased;

        lastReleased += newReleased;
        ReleaseSemaphore(empty, newReleased, NULL);*/
        ReleaseSemaphore(empty, diff, NULL);
        flag = true;
    }
    else if (y == senderBase) {
        dupACK++;
        if (dupACK == 3) {
            dupACK = 0;
            SendDataPkt(PACKET_IN_BUFFER(senderBase % senderWindow));
            fastRetxCount++;
            flag = true;
        }
    }
    else {
        return -1;
    }

    if (flag) recomputeTimerExpire();

    if (packetsRemaining == 0 && closeCalled) {
        SetEvent(eventQuit);
    }
    else {
        ResetEvent(eventQuit);
    }
    //printCurrentSequenceNumbers();

    return STATUS_OK;
}

DWORD SenderSocket::SendDataPkt(Packet* p) {
    p->txTime = clock();
    //printOutput && printf("Sending Pkt with Seq: %d\n", p->pkt.sdh.seq);
    if (sendto(sock, (char*)&(p->pkt), p->size, 0, (sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) {
        return FAILED_SEND;
    }
    lastReleased = max(p->pkt.sdh.seq, lastReleased);
}

void SenderSocket::recomputeTimerExpire() {
    timerExpire = (PACKET_IN_BUFFER(senderBase % senderWindow))->txTime + (clock_t)(RTO * CLOCKS_PER_SEC);
}
#pragma endregion

#pragma region Threads
DWORD WINAPI SenderSocket::statsThread(LPVOID self) {
    SenderSocket* ss = (SenderSocket*)self;
    int previousSeqNo = 0;
    double bitsSinceLastPrint = 0;
    while (true) {
        this_thread::sleep_for(chrono::seconds(TIME_BETWEEN_STAT));
        bitsSinceLastPrint = (double)((ss->senderBase - previousSeqNo) * 8 * (MAX_PKT_SIZE - sizeof(SenderDataHeader)));
        printf("[%3.0f] B %5d ( %.1f MB ) N %5d T %2d F %d W %d S %0.3f Mbps RTT %.3f\n",
            floor(TIME_SINCE(ss->startTime)),
            ss->senderBase,
            (double)(ss->bytesAcked / 1e6),
            ss->nextToSend,
            ss->timeoutCount,
            ss->fastRetxCount,
            ss->effectiveWin,
            (bitsSinceLastPrint / (TIME_BETWEEN_STAT * 1e6)),
            ss->estimatedRTT);
        previousSeqNo = ss->senderBase - 1;
    }
}

DWORD WINAPI SenderSocket::WorkerRun(LPVOID self)
{
    SenderSocket* ss = (SenderSocket*)self;
    //WSAEventSelect(ss->sock, ss->socketReceiveReady, FD_READ);
    HANDLE events[] = { ss->socketReceiveReady, ss->full };
    int kernelBuffer = 20e6;
    if (setsockopt(ss->sock, SOL_SOCKET, SO_RCVBUF, (char*)&kernelBuffer, sizeof(int)) == SOCKET_ERROR) {
        return -1;
    }
    kernelBuffer = 20e6;
    if (setsockopt(ss->sock, SOL_SOCKET, SO_SNDBUF, (char*)&kernelBuffer, sizeof(int)) == SOCKET_ERROR) {
        return -1;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    while (true)
    {
        DWORD timeout = INFINITE;
        if (!(ss->closeCalled)) {
            ss->recomputeTimerExpire();
            timeout = ss->timerExpire - clock();
        }
        else
            timeout = INFINITE;
        int ret = WaitForMultipleObjects(2, events, FALSE, timeout);
        switch (ret)
        {
            // TIMEOUT
            case WAIT_TIMEOUT:
                //if (ss->nextSeq > ss->senderWindow) {
                //    //ss->printSendPkt(&((Packet*)(ss->pending_pkts))[ss->senderBase % ss->senderWindow]);
                //}
                //ss->printCurrentSequenceNumbers();
                ss->timeoutCount++;
                ss->SendDataPkt(&((Packet*)(ss->pending_pkts))[ss->senderBase % ss->senderWindow]);
                ss->recomputeTimerExpire();
                break;
            // SOCKET
            case WAIT_OBJECT_0:
                // move senderBase; update RTT; handle fast retx; do flow control
                ss->ReceiveACK();
                break;
            // SENDER
            case WAIT_OBJECT_0 + 1: 
                // sendto(pending_pkts[nextToSend % senderWindow].pkt);
                //if (ss->nextToSend >= ss->nextSeq - 1) break;
                ss->SendDataPkt(&((Packet*)(ss->pending_pkts))[(ss->lastReleased + 1) % ss->senderWindow]);
                break;
            // HANDLE FAILED WAIT
            default:
                cout << "handle failed wait" << endl;
                // handle failed wait;
        }
    }
}
#pragma endregion

#pragma region Debug Helpers
void SenderSocket::printReceivedPkt(ReceiverHeader* response) {
    if (!printOutput) {
        return;
    }
    printf("recvWnd: %d, seq: %d, FIN: %d, SYN: %d, ACK: %d\n",
        response->recvWnd,
        response->ackSeq,
        response->flags.FIN,
        response->flags.SYN,
        response->flags.ACK);
}

void SenderSocket::printSendPkt(Packet* pkt) {
    if (!printOutput) {
        return;
    }
    printf("type: %d, seq: %d, size: %d\n",
        pkt->type,
        pkt->pkt.sdh.seq,
        pkt->size);
}

void SenderSocket::printCurrentSequenceNumbers() {
    if (!printOutput) {
        return;
    }
    for (int i = 0; i < senderWindow; i++) {
        Packet* p = ((Packet*)(pending_pkts)) + i;
        cout << p->pkt.sdh.seq << ",";
    }
    printf(" Base: %d, NextSeq: %d, LastReleased: %d, PacketsInBuffer: %d\n", 
        senderBase, 
        nextSeq, 
        lastReleased,
        packetsRemaining);
}
#pragma endregion
