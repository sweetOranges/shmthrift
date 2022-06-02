# shmthrift
用共享内存的方式来进行thrift调用，ipc的一种选择

# 简介
thrift支持多种transport， 我提供了一种通过shared memory 来进行消息传输的 transport，速度非常快

# 实现
* 无锁变长环形缓冲队列
* 共享内存

# 性能
常规一个thrift的tcp调用需要80个us，通过shared memory调用仅仅需要5个us
比传统的基于tcp模式的thrift调用快16倍，
