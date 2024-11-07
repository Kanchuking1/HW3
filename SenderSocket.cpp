#include "pch.h"
#include "SenderSocket.h"
#include <string>

using namespace std;

SenderSocket::SenderSocket() {
    startTime = clock();
    open = false;
    synSize = 0;
    memset(&server, 0, sizeof(server));
    RTO = 1;
    sequenceNumber = 0;
    dataSize = 0;
    timeoutCount = 0;
    bytesAcked = 0;
    previousSeqNo = 0;
}

SenderSocket::SenderSocket(bool printOutput) {
    SenderSocket();
    this->printOutput = printOutput;
}

DWORD SenderSocket::Open(string targetHost, int port, int senderWindow, LinkProperties* lp) {
    if (open) {
        return ALREADY_CONNECTED;
    }
    open = true;
    // Initialize and create socket for UDP
    WSADATA wsaData;
    WORD wVersionRequested = MAKEWORD(2, 2);
    if (WSAStartup(wVersionRequested, &wsaData) != 0) {
        //WSACleanup();
        return BAD_SOCKET;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        //WSACleanup();
        return -1;
    }

    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(0);

    if (bind(sock, (struct sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        //WSACleanup();
        return -1;
    }

    // Generate SYN packet
    packet = new SenderSynHeader();
    packet->lp.RTT = lp->RTT;


    packet->lp.bufferSize = senderWindow + MAX_SYN_ATTEMPTS;
    
    packet->lp.speed = lp->speed;
    packet->lp.pLoss[0] = lp->pLoss[0];
    packet->lp.pLoss[1] = lp->pLoss[1];

    packet->sdh.flags.reserved = 0;
    packet->sdh.flags.SYN = 1;
    packet->sdh.flags.FIN = 0;
    packet->sdh.flags.ACK = 0;
    packet->sdh.seq = sequenceNumber;

    remote;

    // Get server IP
    DWORD serverIP = inet_addr(targetHost.c_str());
    if (serverIP == INADDR_NONE) {
        if ((remote = gethostbyname(targetHost.c_str())) == NULL) {
            printOutput && printf(" [ %.3f] --> target %s in invalid\n", TIME_SINCE_START, targetHost.c_str());
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

    int err = doSYNACK(packet);
    
    delete packet;

    return err;
}

DWORD SenderSocket::Send(char* buffer, int bufferSize)
{
    if (!open) {
        return NOT_CONNECTED;
    }

    int status = 0;

    for (int i = 0; i < MAX_SEND_ATTEMPTS; i++) {
        if ((status = sendDataPkt(buffer, bufferSize)) == STATUS_OK) {
            return STATUS_OK;
        }
        else {
            continue;
        }
    }

    return status;
}

DWORD SenderSocket::Close(double *timeElapsed)
{
    if (!open) {
        return NOT_CONNECTED;
    }
    open = false;

    packet->sdh.flags.reserved = 0;
    packet->sdh.flags.SYN = 0;
    packet->sdh.flags.FIN = 1;
    packet->sdh.flags.ACK = 0;
    packet->sdh.seq = 0;

    int err = doFINACK(timeElapsed);
    return err;
}

DWORD SenderSocket::doSYNACK(SenderSynHeader* packet) {
    RTO = max(1, 2 * packet->lp.RTT);
    int available = 0;
    // Send packet
    for (int i = 1; i <= MAX_SYN_ATTEMPTS; i++) {
        printOutput && printf(" [ %.3f] --> ", TIME_SINCE_START);
        printOutput && printf("SYN 0 (attempt %d of %d, RTO %.3f) to %s\n",
            i,
            MAX_SYN_ATTEMPTS,
            RTO,
            inet_ntoa(server.sin_addr));

        processStartTime = clock();

        if (sendto(sock,
            (char*)packet,
            sizeof(SenderSynHeader),
            0,
            (sockaddr*)&server,
            sizeof(server)) == SOCKET_ERROR) {
            printOutput && printf(" [ %.3f] --> failed sendto with %d\n", TIME_SINCE_START, WSAGetLastError());
            //WSACleanup();
            return FAILED_SEND;
        }

        timeval timeout;
        timeout.tv_sec = floor(RTO);
        timeout.tv_usec = (RTO - floor(RTO)) * 1e6;

        fd_set fdRead;
        FD_ZERO(&fdRead);
        FD_SET(sock, &fdRead);

        available = select(0, &fdRead, NULL, NULL, &timeout);

        if (available == 0) {
            continue;
        }
        else if (available < 0) {
            printOutput && printf(" [ %.3f] <-- failed recvfrom with %d\n", TIME_SINCE_START, WSAGetLastError());
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
            printOutput && printf(" [ %.3f] <-- failed recvfrom with %d\n", TIME_SINCE_START, WSAGetLastError());
            return FAILED_RECV;
        }

        synSize += synBytes;

        struct ReceiverHeader* synResponse = (ReceiverHeader*)(synBuffer);

        RTO = 3 * TIME_SINCE(processStartTime);
        estimatedRTT = RTO;

        if (synResponse->flags.SYN == 1 && synResponse->flags.ACK == 1) {
            printOutput && printf(" [ %.3f] <-- SYN-ACK 0 window %d; setting initial RTO to %.3f\n",
                TIME_SINCE_START,
                synResponse->recvWnd,
                RTO);
            receiverWindow = synResponse->recvWnd;
            stats = CreateThread(NULL, 0, &statsThread, this, 0, NULL);
            return STATUS_OK;
        }
    }

    return TIMEOUT;
}

DWORD SenderSocket::doFINACK(double *timeElapsed) {
    int available = 0;
    // Send packet
    for (int i = 1; i <= MAX_FIN_ATTEMPTS; i++) {
        printOutput && printf(" [ %.3f] --> ", TIME_SINCE_START);

        printOutput && printf("FIN 0 (attempt %d of %d, RTO %.3f)\n",
            i,
            MAX_FIN_ATTEMPTS,
            RTO);

        processStartTime = clock();
        packet->sdh.seq = sequenceNumber;

        if (sendto(sock,
            (char*)packet,
            sizeof(SenderSynHeader),
            0,
            (sockaddr*)&server,
            sizeof(server)) == SOCKET_ERROR) {
            printOutput && printf(" [ %.3f] --> failed sendto with %d\n", TIME_SINCE_START, WSAGetLastError());
            return FAILED_SEND;
        }

        timeval timeout;
        timeout.tv_sec = floor(RTO);
        timeout.tv_usec = (RTO - floor(RTO)) * 1e6;

        fd_set fdRead;
        FD_ZERO(&fdRead);
        FD_SET(sock, &fdRead);

        available = select(0, &fdRead, NULL, NULL, &timeout);

        if (available == 0) {
            continue;
        }
        else if (available < 0) {
            printOutput && printf(" [ %.3f] <-- failed recvfrom with %d\n", TIME_SINCE_START, WSAGetLastError());
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
            printOutput && printf(" [ %.3f] <-- failed recvfrom with %d\n", TIME_SINCE_START, WSAGetLastError());
            return FAILED_RECV;
        }

        synSize += synBytes;

        struct ReceiverHeader* synResponse = (ReceiverHeader*)(synBuffer);

        if (synResponse->flags.FIN == 1 && synResponse->flags.ACK == 1) {
            printf("[ %.2f] <-- FIN-ACK %d window %X\n",
                TIME_SINCE_START,
                sequenceNumber,
                synResponse->recvWnd);
            *timeElapsed = TIME_SINCE_START;
            CloseHandle(stats);
            return STATUS_OK;
        }
    }

    return TIMEOUT;
}

DWORD SenderSocket::sendDataPkt(char* buffer, int bufferSize) {
    SenderDataHeader* sdh = new SenderDataHeader();
    sdh->seq = sequenceNumber;
    SenderDataPacket* sendPkt = new SenderDataPacket();

    sendPkt->sdh = *sdh;
    memcpy(sendPkt->data, buffer, bufferSize);

    processStartTime = clock();

    if (sendto(sock, 
        (char*)sendPkt, 
        sizeof(SenderDataHeader) + bufferSize,
        0, 
        (sockaddr*)&server, 
        sizeof(server)) == SOCKET_ERROR) {
        printf(": sendto() generated error %d\n", WSAGetLastError());
        return FAILED_SEND;
    }

    timeval timeout;
    timeout.tv_sec = floor(RTO);
    timeout.tv_usec = (RTO - floor(RTO)) * 1e6;

    fd_set fdRead;
    FD_ZERO(&fdRead);
    FD_SET(sock, &fdRead);

    int available = select(0, &fdRead, NULL, NULL, &timeout);

    if (available <= 0) {
        timeoutCount++;
        return TIMEOUT;
    }

    char* respBuffer = new char[sizeof(SenderSynHeader)];

    sockaddr sendResponseAddr;
    int sendAddrSize = sizeof(sendResponseAddr);

    int bytes = recvfrom(sock,
        respBuffer,
        sizeof(SenderSynHeader),
        0,
        (struct sockaddr*)&sendResponseAddr,
        &sendAddrSize);

    if (bytes < 0) {
        printOutput&& printf(" [ %.3f] <-- failed recvfrom with %d\n", TIME_SINCE_START, WSAGetLastError());
        return FAILED_RECV;
    }

    dataSize += bytes;

    struct ReceiverHeader* sendResponse = (ReceiverHeader*)(respBuffer);

    double sampleRTT = TIME_SINCE(processStartTime);

    updateRTO(sampleRTT);

    if (sendResponse->ackSeq == sequenceNumber + 1) {
        printOutput&& printf(" [ %.3f] <-- SYN-ACK 0 window %d; setting initial RTO to %.3f\n",
            TIME_SINCE_START,
            sendResponse->recvWnd,
            RTO);
        receiverWindow = sendResponse->recvWnd;
        sequenceNumber = sendResponse->ackSeq;
        bytesAcked += MAX_PKT_SIZE;
        return STATUS_OK;
    }

    return -1;
}

void SenderSocket::updateRTO(double sampleRTT) {
    estimatedRTT = (1 - ALPHA) * estimatedRTT + ALPHA * sampleRTT;
    devRTT = (1 - BETA) * devRTT + BETA * abs(estimatedRTT - sampleRTT);
    RTO = estimatedRTT + 4 * max(devRTT, 0.010);
}

DWORD WINAPI SenderSocket::statsThread(LPVOID self) {
    SenderSocket* ss = (SenderSocket*)self;
    int previousSeqNo = 0;
    double bitsSinceLastPrint = 0;

    while (true) {
        this_thread::sleep_for(chrono::seconds(TIME_BETWEEN_STAT));
        bitsSinceLastPrint = (double)((ss->sequenceNumber - previousSeqNo) * 8 * (MAX_PKT_SIZE - sizeof(SenderDataHeader)));
        printf("[%3.0f] B %5d ( %.1f MB ) N %5d T %2d F 0 W 1 S %0.3f Mbps RTT %.3f\n",
            floor(TIME_SINCE(ss->startTime)),
            ss->sequenceNumber,
            (double)(ss->bytesAcked / 1e6),
            ss->sequenceNumber + 1,
            ss->timeoutCount,
            (bitsSinceLastPrint / (TIME_BETWEEN_STAT * 1e6)),
            ss->estimatedRTT);
        previousSeqNo = ss->sequenceNumber;
    }
}