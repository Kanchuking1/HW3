
#include "pch.h"
#include "SenderSocket.h"
#include "checksum.h"

using namespace std;

bool debugMode = false;

int main(int argv, char** argc)
{
    if (argv != 8) {
        string usage = "Usage : HW3.exe ";
        usage += "<destination host/ip> ";
        usage += "<power of 2 buffer size (DWORDS)> ";
        usage += "<sender window (in packets)> ";
        usage += "<RT propagation delay (seconds)> ";
        usage += "<forward loss rate> ";
        usage += "<return loss rate> ";
        usage += "<speed of bottleneck-link (Mbps)> ";
        cerr << usage << endl;
        return -1;
    }
    
    string targetHost = argc[1];
    int power = atoi(argc[2]); // command-line specified integer
    int senderWindow = atoi(argc[3]); // command-line specified integer

    DWORD status;

    LinkProperties lp;
    lp.RTT = atof(argc[4]);
    lp.speed = 1e6 * atof(argc[7]); // convert to megabits
    lp.pLoss[FORWARD_PATH] = atof(argc[5]);
    lp.pLoss[RETURN_PATH] = atof(argc[6]);

    printf("Main:\tsender W = %d, RTT %.3f sec, loss %g / %g, link %d Mbps\n",
        senderWindow,
        lp.RTT,
        lp.pLoss[FORWARD_PATH],
        lp.pLoss[RETURN_PATH],
        (int)(lp.speed / 1e6));
    printf("Main:\tinitializing DWORD array with 2^%d elements...", power);
    clock_t startTime = clock();
    double timeTaken;

    UINT64 dwordBufSize = (UINT64)1 << power;
    DWORD* dwordBuf = new DWORD[dwordBufSize]; // user-requested buffer
    for (UINT64 i = 0; i < dwordBufSize; i++) // required initialization
        dwordBuf[i] = i;
    SenderSocket ss(debugMode); // instance of your class

    printf(" done in %.0f ms\n", TIME_SINCE_START * 1000);

    if ((status = ss.Open(targetHost, MAGIC_PORT, senderWindow, &lp)) != STATUS_OK) {
        printf("Main:\tconnect failed with status %d\n", status);
        return -1;
    }
    printf("Main:\tconnected to %s in %.3f sec, packet size %d bytes\n", 
        targetHost.c_str(), 
        TIME_SINCE_START, 
        MAX_PKT_SIZE);
    // error handling: print status and quit
    char* charBuf = (char*)dwordBuf; // this buffer goes into socket
    UINT64 byteBufferSize = dwordBufSize << 2; // convert to bytes
    UINT64 off = 0; // current position in buffer
    clock_t processStartTime = clock();
    
    while (off < byteBufferSize)
    {
        // decide the size of next chunk
        int bytes = min(byteBufferSize - off, MAX_PKT_SIZE - sizeof(SenderDataHeader));
        // send chunk into socket
        if ((status = ss.Send(charBuf + off, bytes)) != STATUS_OK) {
            printf("Main:\tsend failed with status %d\n", status);
            return -1;
        }
        // error handing: print status and quit
        off += bytes;
    }
    
    double timeSinceStart = (double)TIME_SINCE(processStartTime);
    //double elapsedTime = timeSinceStart;
    if ((status = ss.Close(&timeSinceStart)) != STATUS_OK) {
        printf("Main:\tdisconnect failed with status %d\n", status);
        return -1;
    }

    Checksum cs;

    printf("Main:\ttransfer finished in %.3f sec, %.2f Kbps, checksum %X\n", 
        timeSinceStart, 
        (double)dwordBufSize * 32 / (double)(1000 * timeSinceStart),
        cs.CRC32((unsigned char*)charBuf, byteBufferSize));


    printf("Main:\testRTT %.3f, ideal rate %.2f Kbps\n", 
        ss.estimatedRTT, 
        8 * (MAX_PKT_SIZE - sizeof(SenderDataHeader)) / (ss.estimatedRTT * 1000));
}
