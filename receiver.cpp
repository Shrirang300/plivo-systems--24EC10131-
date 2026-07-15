

#include <iostream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <ctime>
#include <algorithm>


#define PKT_SIZE 164
#define PAYLOAD_SIZE 160


struct Slot { uint32_t seq; bool present; uint8_t data[PAYLOAD_SIZE]; };
struct ParitySlot { uint32_t base; bool present; uint8_t data[PAYLOAD_SIZE]; };


Slot jitter[4096] = {};
ParitySlot pbuf[4096] = {};
double last_nack_time[4096] = {0};
double nack_sent_time[4096] = {0};
double rtt_est = 0.050;
uint32_t highest_received = 0xFFFFFFFF;
uint32_t playout_index = 0xFFFFFFFF;
double t0_offset = 0.0;


double get_current_time(double t0) {
    struct timespec ts;
    if (t0 > 1e8) clock_gettime(CLOCK_REALTIME, &ts);
    else clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}


void try_fec_recover(uint32_t base) {
    if (base < 2) return;
    ParitySlot* p = &pbuf[base % 4096];
    if (!p->present || p->base != base) return;


    uint32_t m1 = base, m2 = base - 2;
    bool has_m1 = (jitter[m1 % 4096].present && jitter[m1 % 4096].seq == m1);
    bool has_m2 = (jitter[m2 % 4096].present && jitter[m2 % 4096].seq == m2);


    if (has_m1 && !has_m2 && m2 >= playout_index) {
        jitter[m2 % 4096].present = true; jitter[m2 % 4096].seq = m2;
        for (int i = 0; i < PAYLOAD_SIZE; ++i) jitter[m2 % 4096].data[i] = p->data[i] ^ jitter[m1 % 4096].data[i];
        p->present = false;
    } else if (!has_m1 && has_m2 && m1 >= playout_index) {
        jitter[m1 % 4096].present = true; jitter[m1 % 4096].seq = m1;
        for (int i = 0; i < PAYLOAD_SIZE; ++i) jitter[m1 % 4096].data[i] = p->data[i] ^ jitter[m2 % 4096].data[i];
        p->present = false;
    }
}


int main() {
    const char* t0_str = getenv("T0"), *delay_str = getenv("DELAY_MS");
    if (!t0_str || !delay_str) return 1;
    double t0 = strtod(t0_str, nullptr); if (t0 > 1e12) t0 /= 1000.0;
    double delay_ms = strtod(delay_str, nullptr);


    int fd_in = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr_in = {AF_INET, htons(47002), inet_addr("127.0.0.1")};
    bind(fd_in, (sockaddr*)&addr_in, sizeof(addr_in));
    fcntl(fd_in, F_SETFL, O_NONBLOCK);


    int fd_player = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in player_addr = {AF_INET, htons(47020), inet_addr("127.0.0.1")};


    int fd_fb_out = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in feedback_addr = {AF_INET, htons(47003), inet_addr("127.0.0.1")};


    while (true) {
        double now = get_current_time(t0);
       
        if (playout_index != 0xFFFFFFFF) {
            double deadline = t0 + t0_offset + (delay_ms / 1000.0) + (playout_index * 0.020) + 0.035;
           
            while (deadline <= now) {
                if (playout_index >= 1500) break;
               
                int idx = playout_index % 4096;
                if (jitter[idx].present && jitter[idx].seq == playout_index) {
                    uint8_t out[PKT_SIZE]; uint32_t ns = htonl(playout_index);
                    memcpy(out, &ns, 4); memcpy(out + 4, jitter[idx].data, PAYLOAD_SIZE);
                    sendto(fd_player, out, PKT_SIZE, 0, (sockaddr*)&player_addr, sizeof(player_addr));
                }
                jitter[idx].present = false; playout_index++;
                deadline = t0 + t0_offset + (delay_ms / 1000.0) + (playout_index * 0.020) + 0.035;
            }
        }


        fd_set readfds; FD_ZERO(&readfds); FD_SET(fd_in, &readfds);
        struct timeval tv = {0, 5000};
        if (select(fd_in + 1, &readfds, nullptr, nullptr, &tv) > 0) {
            uint8_t buf[PKT_SIZE]; ssize_t n = recvfrom(fd_in, buf, PKT_SIZE, 0, nullptr, nullptr);
            if (n == PKT_SIZE) {
                uint32_t ctrl = ntohl(*(uint32_t*)buf);
                if (ctrl & 0x80000000) {
                    uint32_t base = ctrl & ~0x80000000;
                    if (base >= 2) {
                        pbuf[base % 4096] = {base, true}; memcpy(pbuf[base % 4096].data, buf + 4, PAYLOAD_SIZE);
                        try_fec_recover(base);
                    }
                } else {
                    uint32_t seq = ctrl;
                    if (playout_index == 0xFFFFFFFF) {
                        playout_index = seq;
                        t0_offset = now - (t0 + (delay_ms / 1000.0) + (seq * 0.020));
                    }
                    int idx = seq % 4096;
                    if (seq >= playout_index && !jitter[idx].present) {
                        jitter[idx] = {seq, true}; memcpy(jitter[idx].data, buf + 4, PAYLOAD_SIZE);
                    }
                    uint32_t start = (highest_received == 0xFFFFFFFF) ? seq : highest_received + 1;
                    if (seq > 256 && start < seq - 256) start = seq - 256;
                    for (uint32_t j = start; j < seq; j++) {
                        if (!jitter[j % 4096].present && (now - last_nack_time[j % 4096] > 0.04)) {
                            uint32_t nack = htonl(j);
                            sendto(fd_fb_out, &nack, 4, 0, (sockaddr*)&feedback_addr, sizeof(feedback_addr));
                            last_nack_time[j % 4096] = now; nack_sent_time[j % 4096] = now;
                        }
                    }
                    if (highest_received == 0xFFFFFFFF || seq > highest_received) highest_received = seq;
                    try_fec_recover(seq); try_fec_recover(seq + 2);
                }
            }
        }
    }
    return 0;
}

