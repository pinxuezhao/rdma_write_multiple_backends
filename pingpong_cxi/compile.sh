export LD_LIBRARY_PATH=/usr/lib64:$LD_LIBRARY_PATH
export LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu:$LD_LIBRARY_PATH

g++ rdma_write_cpumem.cpp -o rdma_write_cpumem \
         -I/capstor/scratch/cscs/pzhao/LIBFABRIC/blogs/minimal_example/libfabric_writedata/install/include \
         -L/capstor/scratch/cscs/pzhao/LIBFABRIC/blogs/minimal_example/libfabric_writedata/install/lib -lfabric \
         -L/usr/lib64 -lnl-3
