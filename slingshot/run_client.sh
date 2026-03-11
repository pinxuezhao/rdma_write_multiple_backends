export LD_LIBRARY_PATH=/capstor/scratch/cscs/pzhao/LIBFABRIC/blogs/minimal_example/libfabric_writedata/install/lib:$LD_LIBRARY_PATH
export LD_LIBRARY_PATH=/usr/lib64:$LD_LIBRARY_PATH
export LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu:$LD_LIBRARY_PATH

export FI_LOG_LEVEL=debug
export FI_CXI_ENABLE_WRITEDATA=1

./rdma_write_cpumem 00a2bd0100000000 10.100.88.113 
