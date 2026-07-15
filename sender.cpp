#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <ctime>
#include <algorithm>


#define PKT_SIZE 164
#define PAYLOAD_SIZE 160


struct HistSlot {
    uint32_t seq;
    uint8_t data[PAYLOAD_SIZE];
};


HistSlot history[128];
uint8_t accum_even[PAYLOAD_SIZE] = {0};
uint8_t accum_odd[PAYLOAD_SIZE] = {0};
double last_retransmit[128] = {0};


// Dynamically matches Python's clock (REALTIME or MONOTONIC)
double get_current_time(double t0) {
    struct timespec ts;
    if (t0 > 100000000.0) {
        clock_gettime(CLOCK_REALTIME, &ts);
    } else {
        clock_gettime(CLOCK_MONOTONIC, &ts);
    }
    return ts.tv_sec + ts.tv_nsec / 1e9;
}


void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


int main() {
    const char* t0_str = getenv("T0");
    if (!t0_str) {
        std::cerr << "Missing T0\n";
        return 1;
    }
    double t0 = strtod(t0_str, nullptr);
    if (t0 > 1000000000000.0) t0 /= 1000.0; // ms -> s


    // Sockets
    int fd_in = socket(AF_INET, SOCK_DGRAM, 0);
    int fd_fb = socket(AF_INET, SOCK_DGRAM, 0);
    int fd_out = socket(AF_INET, SOCK_DGRAM, 0);


    sockaddr_in addr_in = {AF_INET, htons(47010), inet_addr("127.0.0.1")};
    sockaddr_in addr_fb = {AF_INET, htons(47004), inet_addr("127.0.0.1")};
    sockaddr_in relay_addr = {AF_INET, htons(47001), inet_addr("127.0.0.1")};


    bind(fd_in, (sockaddr*)&addr_in, sizeof(addr_in));
    bind(fd_fb, (sockaddr*)&addr_fb, sizeof(addr_fb));
    set_nonblocking(fd_in);
    set_nonblocking(fd_fb);


    int max_fd = std::max(fd_in, fd_fb);
    uint8_t buf[2048];


    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd_in, &readfds);
        FD_SET(fd_fb, &readfds);


        if (select(max_fd + 1, &readfds, nullptr, nullptr, nullptr) < 0) continue;


        double now = get_current_time(t0);


        // Harness input
        if (FD_ISSET(fd_in, &readfds)) {
            ssize_t n = recvfrom(fd_in, buf, sizeof(buf), 0, nullptr, nullptr);
            if (n == PKT_SIZE) {
                uint32_t seq = ntohl(*(uint32_t*)buf);
                int idx = seq % 128;
                history[idx].seq = seq;
                memcpy(history[idx].data, buf + 4, PAYLOAD_SIZE);


                sendto(fd_out, buf, n, 0, (sockaddr*)&relay_addr, sizeof(relay_addr));


                uint8_t* accum = (seq % 2 == 0) ? accum_even : accum_odd;
                for (int i = 0; i < PAYLOAD_SIZE; ++i) accum[i] ^= buf[4 + i];


                if ((seq % 4 == 2 && seq % 2 == 0) || (seq % 4 == 3 && seq % 2 == 1)) {
                    uint8_t parity[PKT_SIZE];
                    uint32_t ctrl = htonl(seq | 0x80000000u);
                    memcpy(parity, &ctrl, 4);
                    memcpy(parity + 4, accum, PAYLOAD_SIZE);
                    sendto(fd_out, parity, PKT_SIZE, 0, (sockaddr*)&relay_addr, sizeof(relay_addr));
                    memset(accum, 0, PAYLOAD_SIZE);
                }
            }
        }


        // Feedback NACK
        if (FD_ISSET(fd_fb, &readfds)) {
            ssize_t n = recvfrom(fd_fb, buf, 4, 0, nullptr, nullptr);
            if (n == 4) {
                uint32_t req_seq = ntohl(*(uint32_t*)buf);
                int idx = req_seq % 128;
                if (history[idx].seq == req_seq && (now - last_retransmit[idx] >= 0.100)) {
                    uint8_t resend[PKT_SIZE];
                    uint32_t net_seq = htonl(req_seq);
                    memcpy(resend, &net_seq, 4);
                    memcpy(resend + 4, history[idx].data, PAYLOAD_SIZE);
                    sendto(fd_out, resend, PKT_SIZE, 0, (sockaddr*)&relay_addr, sizeof(relay_addr));
                    last_retransmit[idx] = now;
                }
            }
        }
    }
    return 0;
}

