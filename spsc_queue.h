#pragma once

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>

struct spsc_var_queue_block
{
	int64_t size;
};

template <typename T>
struct spsc_var_queue
{
	T udata;
	int lock;
	uint64_t size;
	uint64_t block_cnt;
	sem_t wakeup;
	alignas(64) volatile uint64_t write_idx;
	alignas(64) volatile uint64_t read_idx;
	spsc_var_queue_block data[];
};

template <typename T>
inline spsc_var_queue<T> *spsc_var_queue_construct(void *mem, int64_t len)
{
	spsc_var_queue<T> *q = (spsc_var_queue<T> *)mem;
	q->size = len * sizeof(spsc_var_queue_block);
	q->block_cnt = len;
	q->write_idx = 0;
	q->read_idx = 0;
	q->lock = 0;
	memset(&q->udata, 0, sizeof(T));
	sem_init(&q->wakeup, 1, 0);
	return q;
}

template <typename T>
inline spsc_var_queue<T> *spmc_var_queue_init(int len)
{
	void *mem = malloc(len * sizeof(spsc_var_queue_block) + sizeof(spsc_var_queue<T>));
	return spsc_var_queue_construct<T>(mem, len);
}
template <typename T>
inline spsc_var_queue<T> *spsc_var_queue_init_shm(const char *filename, int64_t len)
{
	int64_t size = len * sizeof(spsc_var_queue_block) + sizeof(spsc_var_queue<T>);
	int shm_fd = shm_open(filename, O_CREAT | O_RDWR, 0666);
	if (shm_fd == -1)
	{
		return nullptr;
	}
	if (ftruncate(shm_fd, size))
	{
		close(shm_fd);
		return nullptr;
	}
	void *mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	close(shm_fd);
	if (mem == MAP_FAILED)
	{
		return nullptr;
	}
	return spsc_var_queue_construct<T>(mem, len);
}
template <typename T>
inline void spsc_var_queue_spin_lock(spsc_var_queue<T> *q)
{
	while (__sync_lock_test_and_set(&q->lock, 1))
	{
	}
}
template <typename T>
inline void spsc_var_queue_spin_unlock(spsc_var_queue<T> *q)
{
	__sync_lock_release(&q->lock);
}
template <typename T>
inline void spsc_var_queue_notify(spsc_var_queue<T> *q)
{
	sem_post(&q->wakeup);
}

template <typename T>
inline spsc_var_queue<T> *spsc_var_queue_connect_shm(const char *filename)
{
	int shm_fd = shm_open(filename, O_CREAT | O_RDWR, 0666);
	if (shm_fd == -1)
	{
		return nullptr;
	}
	spsc_var_queue<T> *q = (spsc_var_queue<T> *)mmap(0, sizeof(spsc_var_queue<T>), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	int64_t size = sizeof(spsc_var_queue<T>) + q->size;
	q = (spsc_var_queue<T> *)mremap(q, sizeof(spsc_var_queue<T>), size, MREMAP_MAYMOVE);
	return q;
}
template <typename T>
inline void *spsc_var_queue_alloc(spsc_var_queue<T> *q, uint64_t size)
{
	size += sizeof(spsc_var_queue_block);
	uint64_t blk_sz = (size + sizeof(spsc_var_queue_block) - 1) / sizeof(spsc_var_queue_block);
	uint64_t padding_sz = q->block_cnt - (q->write_idx % q->block_cnt);
	bool rewind = blk_sz > padding_sz;
	uint64_t min_read_idx = q->write_idx + blk_sz + (rewind ? padding_sz : 0) - q->block_cnt;
	if ((int)(__atomic_load_n(&q->read_idx, __ATOMIC_ACQUIRE) - min_read_idx) < 0)
	{
		return nullptr;
	}
	if (rewind)
	{
		q->data[q->write_idx % q->block_cnt].size = 0;
		__atomic_add_fetch(&q->write_idx, padding_sz, __ATOMIC_RELEASE);
	}
	spsc_var_queue_block *header = &q->data[q->write_idx % q->block_cnt];
	header->size = size;
	header++;
	return header;
}
template <typename T>
inline void spsc_var_queue_push(spsc_var_queue<T> *q)
{
	uint64_t blk_sz = (q->data[q->write_idx % q->block_cnt].size + sizeof(spsc_var_queue_block) - 1) / sizeof(spsc_var_queue_block);
	__atomic_add_fetch(&q->write_idx, blk_sz, __ATOMIC_RELEASE);
}
template <typename T>
inline void *spsc_var_queue_read(spsc_var_queue<T> *q)
{
	if (__atomic_load_n(&q->read_idx, __ATOMIC_ACQUIRE) == __atomic_load_n(&q->write_idx, __ATOMIC_ACQUIRE))
	{
		return nullptr;
	}
	uint64_t size = q->data[q->read_idx % q->block_cnt].size;
	if (size == 0)
	{
		__atomic_add_fetch(&q->read_idx, q->block_cnt - (q->read_idx % q->block_cnt), __ATOMIC_RELEASE);
		if (q->read_idx == __atomic_load_n(&q->write_idx, __ATOMIC_ACQUIRE))
		{
			return nullptr;
		}
	}
	spsc_var_queue_block *header = &q->data[q->read_idx % q->block_cnt];
	header++;
	return header;
}
template <typename T>
inline void spsc_var_queue_pop(spsc_var_queue<T> *q)
{
	uint64_t blk_sz = (q->data[q->read_idx % q->block_cnt].size + sizeof(spsc_var_queue_block) - 1) / sizeof(spsc_var_queue_block);
	__atomic_add_fetch(&q->read_idx, blk_sz, __ATOMIC_RELEASE);
}
