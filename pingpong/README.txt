This directory implements the ping-pong latency test on Slimfly.

Steps:

1. bash compile.sh
2. ssh slimfly26, then "bash run_server.sh"
3. ssh slimfly27, then "bash run_client.sh"

The workflow of the script is as follows:

1. the server and client both allocted buffers (data_buf, data_buf_2) and register them.
2. the server and client exchange metadata using sockets, before RDMA calls.
3. the warmup phase: (100 iterations)
	a. server RDMA_write to client
        b. after the client knows the RDMA_write from server is done, it RDMA_writes to the server
4. the measure phase: (1000 iterations)
	0. the timer starts
	a. server RDMA_write to client
        b. after the client knows the RDMA_write from server is done, it RDMA_writes to the server
	c. the timer stops.
	d. calculate duration (in nanoseconds) between timer_start and time_stop

The one-way RDMA_write latency is calculated as the duration/(1000*1000*2) (us)


The pingpong_cxi directory is almost the same as this one, except:

1. FI_MR_ENDPOINT required by the cxi provider
2. fi_writedata call uses 0 instead of data_ptr (as CXI does not support FI_MR_VIRT_ADDR)
3. fi_mr_bind and fi_mr_enable calls (required by FI_MR_ENDPOINT)
