/* buffer cache implementation */
/* buffer cache with 64 lines is defined as list of cache_line structures */

#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/////////* added to implement read-ahead */////////
struct list read_ahead_queue;
struct lock read_ahead_lock;
struct read_ahead_elem{
  disk_sector_t sector;
  struct list_elem elem;
};
struct condition not_empty;

static thread_func read_ahead_get NO_RETURN;
static thread_func periodical_write_back NO_RETURN;

/* initiation of buffer cache */
void init_buffer_cache(){
  list_init(&buffer_cache);
  lock_init(&buffer_cache_lock);
  list_init(&read_ahead_queue);
  lock_init(&read_ahead_lock);
  cond_init(&not_empty);
  buffer_cache_size = 0;
  /* background thread for periodic write-back */
  thread_create("periodical_writer", PRI_DEFAULT, periodical_write_back, NULL);
  /* background thread for read-ahead */
  thread_create("read-aheader", PRI_DEFAULT, read_ahead_get, NULL);
}

static void periodical_write_back(void *unused){
  while(1){
    timer_sleep(500);       /* '500 ticks' is arbitrary period */
    write_behind_all(false);
  }
}

/* inode_read_at() and inode_write_at() call this function. 
   to get cache line of given sector, call find_cache_line() first.
   if return value is null, read from disk and add by calling add_cache_line(). */
struct cache_line * get_cache_line(disk_sector_t sector_idx, int dirty){
  lock_acquire(&buffer_cache_lock);
  struct cache_line *cl = find_cache_line(sector_idx);
  if(cl){
    //cl->accessing_processes ++;
    if(dirty){/* write */
      cl->dirty = 1;
    }
  }
  else{
    cl = add_cache_line(sector_idx);
    cl->dirty = dirty;
  }
  cl->accessed = 1;
  lock_release(&buffer_cache_lock);
  return cl;
}

/* find cache line with given sector and return it.
   if it does not exist, return null. */
struct cache_line * find_cache_line(disk_sector_t sector_idx){
  struct cache_line *cl;
  struct list_elem *elem;
  for(elem = list_begin(&buffer_cache); elem != list_end(&buffer_cache); elem = list_next(elem)){
    cl = list_entry(elem, struct cache_line, elem);
    if(sector_idx == cl->sector_idx)
      return cl;
  }
  return NULL;
}

/* add new cache line by reading from disk.
   if cache is already full, add after eviction by calling evict_cache_line(). */
struct cache_line * add_cache_line(disk_sector_t sector_idx){
  struct cache_line *cl;
  if(buffer_cache_size > 63){
    cl = evict_cache_line();
  }
  else{
    cl = malloc(sizeof *cl);
    list_push_back(&buffer_cache, &cl->elem);
    buffer_cache_size ++;
  }

  /* if cl is still null, it is error */
  if(!cl)
    PANIC("no space for buffer cache to add");
  
  //cl->accessing_processes =1;
  cl->sector_idx = sector_idx;
  disk_read(filesys_disk, sector_idx, &cl->block);
  return cl;
}

/* select which cache line to evict and do eviction(no need to free and delete. just use it.)
   algorithm is second-chance algorithm */
struct cache_line * evict_cache_line(){
  struct list_elem *e = list_begin(&buffer_cache);
  struct cache_line *cl;
  while(1){
    cl = list_entry(e, struct cache_line, elem);
    //if(cl->accessing_processes > 0){
      //continue;
    //}
    if(cl->accessed)
      cl->accessed = 0;
    else{
      if(cl->dirty){/* write-behind */
        disk_write(filesys_disk, cl->sector_idx, &cl->block);
      }
      return cl;
    }
    e = list_next(e);
    if(e == list_end(&buffer_cache))
      e = list_begin(&buffer_cache);
  }
}

/* write-behind of all cache lines */
void write_behind_all(bool done){
  lock_acquire(&buffer_cache_lock);
  struct cache_line * cl;
  struct list_elem * elem = list_begin(&buffer_cache);
  while(elem != list_end(&buffer_cache)){
    cl = list_entry(elem, struct cache_line, elem);
    elem = list_next(elem);
    if(cl->dirty){
      disk_write(filesys_disk, cl->sector_idx, &cl->block);
      cl->dirty = 0;
    }
    /* when called by filesys_done */
    if(done){
      list_remove(&cl->elem);
      buffer_cache_size--;
      free(cl);
    }
  }
  lock_release(&buffer_cache_lock);
}

/////////* added for implementing read-ahead */////////


/* produce sectors to read-ahead */
void read_ahead_put(disk_sector_t sector){
  struct read_ahead_elem * ra = malloc(sizeof (struct read_ahead_elem));
  ra->sector = sector;
  
  lock_acquire(&read_ahead_lock);
  list_push_back(&read_ahead_queue,&ra->elem);
  cond_signal(&not_empty, &read_ahead_lock);
  lock_release(&read_ahead_lock);
}

/* consume sectors to read-ahead to do read-ahead */
static void read_ahead_get(void *unused){
  struct read_ahead_elem * ra;
  struct list_elem * elem;
  struct cache_line * cl;
  while(1){
    lock_acquire(&read_ahead_lock);
    while(list_empty(&read_ahead_queue))
      cond_wait(&not_empty, &read_ahead_lock);
    elem = list_pop_front(&read_ahead_queue);
    lock_release(&read_ahead_lock);

    ra = list_entry(elem, struct read_ahead_elem, elem);
    
    /* read sectors */
    lock_acquire(&buffer_cache_lock);
    cl = find_cache_line(ra->sector);
    if(!cl){
      add_cache_line(ra->sector);
    }
    lock_release(&buffer_cache_lock);

    free(ra);
  }
}
