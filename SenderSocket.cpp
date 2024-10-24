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
        WSACleanup();
        return BAD_SOCKET;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        closesocket(sock);
        WSACleanup();
        return -1;
    }

    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(0);

    if (bind(sock, (struct sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        closesocket(sock);
        WSACleanup();
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
    packet->sdh.seq = 0;

    struct hostent* remote;

    // Get server IP
    DWORD serverIP = inet_addr(targetHost.c_str());
    if (serverIP == INADDR_NONE) {
        if ((remote = gethostbyname(targetHost.c_str())) == NULL) {
            closesocket(sock);
            WSACleanup();
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

DWORD SenderSocket::Send(char* ptr, int bytes)
{
    if (!open) {
        return NOT_CONNECTED;
    }
    return STATUS_OK;
}

DWORD SenderSocket::Close()
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

    int err = doFINACK();
    return err;
}

DWORD SenderSocket::doSYNACK(SenderSynHeader* packet) {
    RTO = 1;
    int available = 0;
    // Send packet
    for (int i = 1; i <= MAX_SYN_ATTEMPTS; i++) {
        printf(" [ %.3f] --> ", TIME_SINCE_START);

        printf("SYN 0 (attempt %d of %d, RTO %.3f) to %s\n",
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
            printf(" [ %.3f] --> failed sendto with %d\n", TIME_SINCE_START, WSAGetLastError());
            closesocket(sock);
            WSACleanup();
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
            printf(" [ %.3f] --> failed recvfrom with %d\n", TIME_SINCE_START, WSAGetLastError());
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
            printf(" [ %.3f] --> failed recvfrom with %d\n", TIME_SINCE_START, WSAGetLastError());
            return FAILED_RECV;
        }

        synSize += synBytes;

        struct ReceiverHeader* synResponse = (ReceiverHeader*)(synBuffer);

        RTO = 3 * TIME_SINCE(processStartTime);

        if (synResponse->flags.SYN == 1 && synResponse->flags.ACK == 1) {
            printf(" [ %.3f] <-- SYN-ACK 0 window %d; setting initial RTO to %.3f\n",
                TIME_SINCE_START,
                synResponse->recvWnd,
                RTO);
            receiverWindow = synResponse->recvWnd;

            return STATUS_OK;
        }
    }

    return TIMEOUT;
}

DWORD SenderSocket::doFINACK() {
    int available = 0;
    // Send packet
    for (int i = 1; i <= MAX_FIN_ATTEMPTS; i++) {
        printf(" [ %.3f] --> ", TIME_SINCE_START);

        printf("FIN 0 (attempt %d of %d, RTO %.3f)\n",
            i,
            MAX_FIN_ATTEMPTS,
            RTO);

        processStartTime = clock();

        if (sendto(sock,
            (char*)packet,
            sizeof(SenderSynHeader),
            0,
            (sockaddr*)&server,
            sizeof(server)) == SOCKET_ERROR) {
            printf(" [ %.3f] --> failed sendto with %d\n", TIME_SINCE_START, WSAGetLastError());
            closesocket(sock);
            WSACleanup();
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
            printf(" [ %.3f] --> failed recvfrom with %d\n", TIME_SINCE_START, WSAGetLastError());
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
            printf(" [ %.3f] --> failed recvfrom with %d\n", TIME_SINCE_START, WSAGetLastError());
            return FAILED_RECV;
        }

        synSize += synBytes;

        struct ReceiverHeader* synResponse = (ReceiverHeader*)(synBuffer);

        RTO = 3 * TIME_SINCE(processStartTime);

        if (synResponse->flags.FIN == 1 && synResponse->flags.ACK == 1) {
            printf(" [ %.3f] <-- FIN-ACK 0 window %d\n",
                TIME_SINCE_START,
                synResponse->recvWnd);

            return STATUS_OK;
        }
    }

    return TIMEOUT;
}
