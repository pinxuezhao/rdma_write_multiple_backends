export LD_LIBRARY_PATH=/home/pzhao/libfabric/install/lib:$LD_LIBRARY_PATH
#export FI_LOG_LEVEL=debug
./rdma_write_cpumem "client" 
