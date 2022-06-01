# shmthrift
用共享内存的方式来进行thrift调用

# 简介
thrift支持多种transport， 我提供了一种支持oneway的shared memory transport，内部采用了一个支持变长的spmc队列， 完全基于共享内存。

# 性能
比传统的基于tcp模式的thrift调用快5倍
