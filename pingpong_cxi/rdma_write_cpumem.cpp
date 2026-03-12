#include <algorithm>
#include <chrono>
#include <iostream>
#include <functional>
#include <inttypes.h>
#include <memory>
#include <netdb.h>
#include <pthread.h>
#include <random>
#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <time.h>
#include <unistd.h>
#include <vector>

#include <sched.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>
#include <sys/syscall.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                            int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

#define CHECK(stmt)                                                            \
  do {                                                                         \
    if (!(stmt)) {                                                             \
      fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #stmt);                \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#define FI_CHECK(stmt)                                                         \
  do {                                                                         \
    int rc = (stmt);                                                           \
    if (rc) {                                                                  \
      fprintf(stderr, "%s:%d %s failed with %d (%s)\n", __FILE__, __LINE__,    \
              #stmt, rc, fi_strerror(-rc));                                   \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

constexpr size_t kBufAlign = 128;
constexpr size_t kMessageBufferSize = 8192;
constexpr size_t kMemoryRegionSize = 8;
constexpr int kSocketPort = 7766;  

class Timer {
    private:
	    std::chrono::high_resolution_clock::time_point start_time;

    public:
	    void start(){
	    	start_time = std::chrono::high_resolution_clock::now();
	    }
	    uint64_t stop(){
	    	auto end_time = std::chrono::high_resolution_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time-start_time);
		return duration.count();
	    }
};


struct Address {
    uint8_t bytes[8];

    Address(uint8_t bytes[8]) { memcpy(this->bytes, bytes, 8); }
    Address() {
    	for(int i=0; i<8;i++){
		bytes[i] = 0;
	}
    }


    std::string ToString() const {
        char buf[17];
        for (size_t i = 0; i < 8; i++) {
            snprintf(buf + 2 * i, 3, "%02x", bytes[i]);
        }
        return std::string(buf, 16);
    }

    static Address Parse(const std::string &str) {
        if (str.size() != 16) {
            fprintf(stderr, "Unexpected address length %zu\n", str.size());
            exit(1);
        }
        uint8_t bytes[8];
        for (size_t i = 0; i < 8; i++) {
            sscanf(str.c_str() + 2 * i, "%02hhx", &bytes[i]);
        }
        return Address{bytes};
    }
};

struct Buffer {
    void *data;
    size_t size;
    void *raw_data;

    static Buffer Alloc(size_t size, size_t align) {
        void *raw_data = malloc(size + align - 1);
        CHECK(raw_data != nullptr);
        void *data = (void*)(((uintptr_t)raw_data + align - 1) & ~(align - 1));
        return Buffer{data, size, raw_data};
    }

    ~Buffer() {
        free(raw_data);
    }
};

struct ConnectMessage {
    Address client_addr;
    uint64_t mem_addr;
    uint64_t mem_size;
    uint64_t rkey;
};

struct Network {
    struct fi_info *fi;
    struct fid_fabric *fabric;
    struct fid_domain *domain;
    struct fid_cq *cq;
    struct fid_av *av;
    struct fid_ep *ep;
    Address addr;

    static Network Open(struct fi_info *fi) {
        struct fid_fabric *fabric;
        FI_CHECK(fi_fabric(fi->fabric_attr, &fabric, nullptr));

        struct fid_domain *domain;
        FI_CHECK(fi_domain(fabric, fi, &domain, nullptr));

        struct fid_cq *cq;
        struct fi_cq_attr cq_attr = {};
        cq_attr.format = FI_CQ_FORMAT_DATA;
        FI_CHECK(fi_cq_open(domain, &cq_attr, &cq, nullptr));

        struct fid_av *av;
        struct fi_av_attr av_attr = {};
        FI_CHECK(fi_av_open(domain, &av_attr, &av, nullptr));

        struct fid_ep *ep;
        FI_CHECK(fi_endpoint(domain, fi, &ep, nullptr));
        FI_CHECK(fi_ep_bind(ep, &cq->fid, FI_SEND | FI_RECV));
        FI_CHECK(fi_ep_bind(ep, &av->fid, 0));
        FI_CHECK(fi_enable(ep));

        uint8_t addr_bytes[16];
        size_t addrlen = sizeof(addr_bytes);
        FI_CHECK(fi_getname(&ep->fid, addr_bytes, &addrlen));
        CHECK(addrlen == 8);

        return Network{fi, fabric, domain, cq, av, ep, Address{addr_bytes}};
    }

    fi_addr_t AddPeerAddress(const Address &peer_addr) {
        printf("adding peer_addr:%s\n", peer_addr.ToString().c_str());
        fi_addr_t addr = FI_ADDR_UNSPEC;
        int ret = fi_av_insert(av, peer_addr.bytes, 1, &addr, 0, nullptr);
        CHECK(ret == 1);
        return addr;
    }

    struct fid_mr* RegisterMemory(Buffer &buf) {
        struct fid_mr *mr;
        struct fi_mr_attr mr_attr = {
            .iov_count = 1,
            .access = FI_SEND | FI_RECV | FI_REMOTE_WRITE | FI_REMOTE_READ |
                FI_WRITE | FI_READ,
        };
        struct iovec iov = {.iov_base = buf.data, .iov_len = buf.size};
        mr_attr.mr_iov = &iov;
        uint64_t flags = 0;
        auto ret = fi_mr_regattr(domain, &mr_attr, flags, &mr);
        printf("MR_REGATTR RET: %d\n", ret);

	// For tcp;ofi_rxm, we don't have FI_MR_ENDPOINT, so we don't need call fi_mr_bind and fi_mr_enable
        ret = fi_mr_bind(mr, (fid*)ep, 0);
        printf("MR_BIND RET: %d\n", ret);
        ret = fi_mr_enable(mr);
        printf("MR_ENABLE RET: %d\n", ret);

        auto key = fi_mr_key(mr);
        printf("KEY: %lX\n", key);

	auto desc = fi_mr_desc(mr);
	printf("mr->desc = %lX\n", &mr->mem_desc);
	printf("DESC: %lX\n", desc);

        return mr;
    }

    void PostWrite(fi_addr_t dest_addr, Buffer &src_buf, struct fid_mr *src_mr,
                   uint64_t dest_ptr, uint64_t dest_key, size_t len) {

	    
	/*
        printf("PostWrite Debug: Performing RDMA write:\n");
        printf("----dest_addr: 0x%lx\n", dest_ptr);
        printf("----size: %ld\n", len);
        printf("----dest_key: 0x%lx\n", dest_key);
	*/
	
	 int ret;
    	int max_retries = 40; 
    	int attempt = 0;

	auto src_mem_desc = fi_mr_desc(src_mr);

    	do {
        	ret = fi_writedata(ep, src_buf.data, len, src_mem_desc, 0, dest_addr,
                          0, dest_key, nullptr);
        
        	if (ret == -FI_EAGAIN) {
            		attempt++;
 //           		printf("FI_EAGAIN encountered, attempt %d, driving progress...\n", attempt);
            
            		struct fi_cq_err_entry cqe;
            		ssize_t cq_ret = fi_cq_read(cq, &cqe, 1);
            
            		if (cq_ret == -FI_EAGAIN) {
               			usleep(1000); 
            		} else if (cq_ret < 0 && cq_ret != -FI_EAGAIN) {
                		fprintf(stderr, "fi_cq_read error: %s\n", fi_strerror(-(int)cq_ret));
                		break;
            		} else if (cq_ret > 0) {
                		printf("Read completion event while driving progress\n");
            		}
        	} else if (ret) {
            		fprintf(stderr, "fi_writedata failed with %d (%s)\n", 
                    		ret, fi_strerror(-ret));
            		exit(1);
        	}
    	} while (ret == -FI_EAGAIN && attempt < max_retries);
    
    	if (ret == -FI_EAGAIN) {
        	fprintf(stderr, "Max retries (%d) exceeded, still getting -FI_EAGAIN\n", 
                	max_retries);
        	exit(1);
    	}
    
    	if (ret) {
        	fprintf(stderr, "fi_writedata failed with %d (%s)\n", 
                	ret, fi_strerror(-ret));
        	exit(1);
    	}
    
//    	printf("RDMA write initiated successfully after %d attempts\n", attempt);
	
    }

    void PollCompletion() {
        struct fi_cq_err_entry cqe;
        ssize_t ret;
        while ((ret = fi_cq_read(cq, &cqe, 1)) == -FI_EAGAIN) {
        }
        if (ret == -FI_EAVAIL) {
            struct fi_cq_err_entry err_entry;
            auto ret2 = fi_cq_readerr(cq, &err_entry, 0);
            if (ret2 > 0) {
                fprintf(stderr, "Failed libfabric operation err: %s; provider err: %s\n",
                    fi_cq_strerror(cq, err_entry.err, err_entry.err_data,
                                nullptr, 0), fi_cq_strerror(cq, err_entry.prov_errno, err_entry.err_data, nullptr, 0));
                std::exit(-1);
            }
        }
    }

    ~Network() {
        if (ep) fi_close(&ep->fid);
        if (av) fi_close(&av->fid);
        if (cq) fi_close(&cq->fid);
        if (domain) fi_close(&domain->fid);
        if (fabric) fi_close(&fabric->fid);
    }
};

struct fi_info* GetInfo() {
    struct fi_info *hints = fi_allocinfo();

    hints->caps = FI_MSG | FI_RMA | FI_LOCAL_COMM | FI_REMOTE_COMM | FI_REMOTE_WRITE | FI_REMOTE_READ;
    hints->ep_attr->type = FI_EP_RDM;
    hints->fabric_attr->prov_name = strdup("cxi");
//    hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY;
    hints->domain_attr->mr_mode = FI_MR_ENDPOINT | FI_MR_ALLOCATED | FI_MR_PROV_KEY;
    
//    hints->domain_attr->control_progress = FI_PROGRESS_AUTO;
//    hints->domain_attr->data_progress = FI_PROGRESS_AUTO;

    struct fi_info *info;
    FI_CHECK(fi_getinfo(FI_VERSION(1, 22), nullptr, nullptr, 0, hints, &info));
    fi_freeinfo(hints);
    return info;
}

int createSocketServer(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(sockfd >= 0);

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    CHECK(bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    CHECK(listen(sockfd, 1) == 0);

    printf("Socket server listening on port %d\n", port);
    return sockfd;
}

ConnectMessage acceptAndReceiveMessage(int server_sock) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
    CHECK(client_sock >= 0);

    printf("Accepted socket connection from %s:%d\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    ConnectMessage msg;
    ssize_t n = recv(client_sock, &msg, sizeof(msg), 0);
    CHECK(n == sizeof(msg));

    printf("Received ConnectMessage:\n");
    printf("  net address: %s\n", msg.client_addr.ToString().c_str());
    printf("  Memory address: 0x%lx\n", msg.mem_addr);
    printf("  Memory size: %lu\n", msg.mem_size);
    printf("  RKey: 0x%lx\n", msg.rkey);

    close(client_sock);
    return msg;
}

void sendConnectMessage(const std::string& server_ip, int port, const ConnectMessage& msg) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(sockfd >= 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    CHECK(inet_pton(AF_INET, server_ip.c_str(), &addr.sin_addr) == 1);

    printf("Connecting to %s:%d...\n", server_ip.c_str(), port);
    CHECK(connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    printf("Sending ConnectMessage:\n");
    printf("  net address: %s\n", msg.client_addr.ToString().c_str());
    printf("  Memory address: 0x%lx\n", msg.mem_addr);
    printf("  Memory size: %lu\n", msg.mem_size);
    printf("  RKey: 0x%lx\n", msg.rkey);

    ssize_t n = send(sockfd, &msg, sizeof(msg), 0);
    CHECK(n == sizeof(msg));

    printf("ConnectMessage sent successfully\n");
    close(sockfd);
}

int ServerMain(int argc, char **argv) {
    std::string server_ip = "10.100.88.18";  // change this to the custom server IP
    std::string client_ip = "10.100.88.54";

    if (argc >= 2) {
        server_ip = argv[1];  
    }

    int server_sock = createSocketServer(kSocketPort);

    struct fi_info *info = GetInfo();
    Network net = Network::Open(info);

    printf("Server RDMA address: %s\n", net.addr.ToString().c_str());
    printf("Server listening on %s:%d for metadata\n", server_ip.c_str(), kSocketPort);
    printf("Run client with: ./rdma_write_cpumem %s %s\n",
           net.addr.ToString().c_str(), server_ip.c_str());

    auto data_buf = Buffer::Alloc(kMemoryRegionSize, kBufAlign);
    auto data_buf_2 = Buffer::Alloc(kMemoryRegionSize, kBufAlign);

    struct fid_mr *data_mr = net.RegisterMemory(data_buf);
    struct fid_mr *data_mr_2 = net.RegisterMemory(data_buf_2);

    memset(data_buf.data, 0xBA, data_buf.size);

    printf("Waiting for client connection via socket...\n");

    ConnectMessage conn_msg = acceptAndReceiveMessage(server_sock);
    close(server_sock);

    ConnectMessage server_conn_msg;
    server_conn_msg.client_addr = net.addr;
    server_conn_msg.mem_addr = (uint64_t)data_buf_2.data;
    server_conn_msg.mem_size = data_buf_2.size;
    server_conn_msg.rkey = fi_mr_key(data_mr_2);

    printf("Server Debug: Ready to send metadata:\n");
    printf("----server_addr: %s\n", net.addr.ToString().c_str());
    printf("----server mem_addr: %p\n", data_buf_2.data);
    printf("----server mem_size: %ld\n", data_buf_2.size);
    printf("----server rkey: 0x%lx\n", server_conn_msg.rkey);

//    fi_addr_t server_fi_addr = net.AddPeerAddress(server_addr);
    sendConnectMessage(client_ip, kSocketPort, server_conn_msg);

    printf("Server send metadata to client\n");


    fi_addr_t client_addr = net.AddPeerAddress(conn_msg.client_addr);

    printf("Warmup phase...\n");
    for(int i=0; i<100; i++){
    
    	printf("Server -> Client write launched...\n");
    	net.PostWrite(client_addr, data_buf, data_mr,
                  conn_msg.mem_addr, conn_msg.rkey, kMemoryRegionSize);
        net.PollCompletion();
    	
	net.PollCompletion();

    	printf("Client -> Server write received...\n");
    }

    printf("Measuring phase...\n");


    Timer timer;

    timer.start();
    for(int i=0; i<1000; i++){
    
//    	printf("Server -> Client write launched...\n");
    	net.PostWrite(client_addr, data_buf, data_mr,
                  conn_msg.mem_addr, conn_msg.rkey, kMemoryRegionSize);
        net.PollCompletion();
    	
	net.PollCompletion();

//    	printf("Client -> Server write received...\n");
    }
    auto duration = timer.stop();
    printf("server duration: %f (ms)\n", (double)(duration)/(1000*1000*2));

    fi_close(&data_mr->fid);
    fi_close(&data_mr_2->fid);
    fi_freeinfo(info);
    return 0;
}

int ClientMain(int argc, char **argv) {
    std::string server_ip = "10.100.88.18";
    struct fi_info *info = GetInfo();
    Network net = Network::Open(info);

    auto data_buf = Buffer::Alloc(kMemoryRegionSize, kBufAlign);
    auto data_buf_2 = Buffer::Alloc(kMemoryRegionSize, kBufAlign);

    struct fid_mr *data_mr = net.RegisterMemory(data_buf);
    struct fid_mr *data_mr_2 = net.RegisterMemory(data_buf_2);

    ConnectMessage conn_msg;
    conn_msg.client_addr = net.addr;
    conn_msg.mem_addr = (uint64_t)data_buf.data;
    conn_msg.mem_size = data_buf.size;
    conn_msg.rkey = fi_mr_key(data_mr);

    printf("Client Debug: Ready to send metadata:\n");
    printf("----client_addr: %s\n", net.addr.ToString().c_str());
    printf("----client mem_addr: %p\n", data_buf.data);
    printf("----client mem_size: %ld\n", data_buf.size);
    printf("----client rkey: 0x%lx\n", conn_msg.rkey);


    sendConnectMessage(server_ip, kSocketPort, conn_msg);

    printf("ConnectMessage sent, waiting for server to exchange metadata\n");

    int client_sock = createSocketServer(kSocketPort);
    ConnectMessage server_conn_msg = acceptAndReceiveMessage(client_sock);
    close(client_sock);
    auto server_mem_addr = server_conn_msg.mem_addr;
    auto server_rkey = server_conn_msg.rkey;

    fi_addr_t server_fi_addr = net.AddPeerAddress(server_conn_msg.client_addr);

    printf("Warmup phase...\n");
    for(int i =0; i<100; i++){
    	printf("Server -> Client write: received...\n");
    	net.PollCompletion(); 

    	net.PostWrite(server_fi_addr, data_buf_2, data_mr_2,
                   server_mem_addr, server_rkey, kMemoryRegionSize);
    	net.PollCompletion();
    
	printf("Client -> Server write: launched...\n");
    }

    printf("Measuring phase...\n");
    for(int i =0; i<1000; i++){
//    	printf("Server -> Client write: received...\n");
    	net.PollCompletion(); 

    	net.PostWrite(server_fi_addr, data_buf_2, data_mr_2,
                   server_mem_addr, server_rkey, kMemoryRegionSize);
    	net.PollCompletion();
    
//	printf("Client -> Server write: launched...\n");
    }

    fi_close(&data_mr->fid);
    fi_close(&data_mr_2->fid);
    fi_freeinfo(info);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1) {
        return ServerMain(argc, argv);
    } else {
        return ClientMain(argc, argv);
    }
}
