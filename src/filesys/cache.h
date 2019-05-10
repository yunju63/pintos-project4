/* buffer cache with 64 lines is defined as list of cache_line structures */

#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <list.h>
#include "devices/disk.h"
#include "threads/synch.h"

struct list buffer_cache;

struct cache_line{ 
  uint8_t block[DISK_SECTOR_SIZE];  /* cache block size = disk sector size = 512B */
  disk_sector_t sector_idx;         /* sector index */
  int accessed;                     /* used when we selecting cache line to evict */
  int dirty;                        /* set to 1 when write is done */
  //int accessing_processes;          /* number of processes accessing this cache line */
  struct list_elem elem;
};

int buffer_cache_size;
struct lock buffer_cache_lock;

void init_buffer_cache(void);
struct cache_line * get_cache_line(disk_sector_t sector_idx, int dirty);
struct cache_line * find_cache_line(disk_sector_t sector_idx);
struct cache_line * add_cache_line(disk_sector_t sector_idx);
struct cache_line * evict_cache_line(void);
void write_behind_all(bool);
void read_ahead_put(disk_sector_t sector);

#endif  /* threads/cache.h */
