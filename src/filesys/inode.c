#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* 8MB-sizeof(struct inode_disk) */
#define MAX_FILE_SIZE 8388096

/* number of pointers on indirect block is (512B/4B)=128 */
//define NUM_PTRS_INDIRECT_BLOCK 128

//#define NUM_DIRECT_BLOCKS 1
//#define NUM_INDIRECT_BLOCKS 1
//#define NUM_DOUBLY_INDIRECT_BLOCKS 1

/* On-disk inode.
   Must be exactly DISK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    /* for subdirectory implementation */
    int is_dir;                         
    disk_sector_t parent;

    /* for extensible file implementation */
    disk_sector_t direct_ptr;           /* points data sector */
    disk_sector_t indirect_ptr;         /* points sector pointing data sector */
    disk_sector_t doubly_indirect_ptr;  /* points sector pointing sector pointing data sector */
    
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    uint32_t unused[121];               /* Not used. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, DISK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    disk_sector_t sector;               /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    off_t length;                       /* file size in bytes */
    off_t read_length;

    /* pointers to blocks */
    disk_sector_t direct_ptr;
    disk_sector_t indirect_ptr;
    disk_sector_t doubly_indirect_ptr;

    /* for subdirectory implementation */
    int is_dir;
    disk_sector_t parent;

    /* for synchronization */
    struct lock lock;
  };

/* Returns the disk sector that contains byte offset POS within
   INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static disk_sector_t
byte_to_sector (const struct inode *inode, off_t pos, off_t length) 
{
  ASSERT (inode != NULL);
  uint32_t index;
  disk_sector_t indirect_buffer[128];

  if (pos >= length)
    return -1;

  /* on data sector pointed by direct block */
  if (pos < DISK_SECTOR_SIZE){
    return inode->direct_ptr;
  }
  
  /* on data sector pointed by indirect block */
  else if(pos < DISK_SECTOR_SIZE*129){
    disk_read(filesys_disk, inode->indirect_ptr, &indirect_buffer);
    pos -= DISK_SECTOR_SIZE;            /* subtract data offset of 'direct block' */
    index = pos/DISK_SECTOR_SIZE;       /* index among 128 pointers on 'pointer block' */
    return indirect_buffer[index];
  }
  
  /* on data sector pointed by doubly indirect block */
  else{
    disk_read(filesys_disk, inode->doubly_indirect_ptr, &indirect_buffer);
    pos -= DISK_SECTOR_SIZE*129;        /* subtract data offset of 'direct block' and 'indirect block' */
    index = pos/(DISK_SECTOR_SIZE*128); /* index among 128 pointers on 'pointer-of-pointer block' */
    disk_read(filesys_disk, indirect_buffer[index], &indirect_buffer);
    pos -= index*DISK_SECTOR_SIZE*128;  /* consider some of data sectors pointed by 'doubly indirect block' */
    index = pos/DISK_SECTOR_SIZE;       /* index among 128 pointers on 'pointer block' */
    return indirect_buffer[index];
  }
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

void inode_grow(struct inode *inode, off_t new_length);

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   disk.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (disk_sector_t sector, off_t length, int is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);
  if(length > MAX_FILE_SIZE)
    length = MAX_FILE_SIZE;

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_dir = is_dir;
      disk_inode->parent = ROOT_DIR_SECTOR;

      struct inode i;
      i.length = 0;
      inode_grow(&i, length);
      disk_inode->direct_ptr = i.direct_ptr;
      disk_inode->indirect_ptr = i.indirect_ptr;
      disk_inode->doubly_indirect_ptr = i.doubly_indirect_ptr;
     
      disk_write(filesys_disk, sector, disk_inode);
      success = true;

      free(disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (disk_sector_t sector) 
{
  struct list_elem *e;
  struct inode *inode;
  struct inode_disk data;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&inode->lock);
  disk_read (filesys_disk, inode->sector, &data);
  inode->length = data.length;
  inode->read_length = data.length;
  inode->direct_ptr = data.direct_ptr;
  inode->indirect_ptr = data.indirect_ptr;
  inode->doubly_indirect_ptr = data.doubly_indirect_ptr;
  inode->is_dir = data.is_dir;
  inode->parent = data.parent;
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
disk_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

void inode_free(struct inode *inode);
/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1);
          inode_free(inode); 
        }
      /* writeback needed */
      else{
        struct inode_disk disk_inode;
        disk_inode.length = inode->length;
        disk_inode.magic = INODE_MAGIC;
        disk_inode.direct_ptr = inode->direct_ptr;
        disk_inode.indirect_ptr = inode->indirect_ptr;
        disk_inode.doubly_indirect_ptr = inode->doubly_indirect_ptr;
        disk_inode.is_dir = inode->is_dir;
        disk_inode.parent = inode->parent;
        disk_write(filesys_disk, inode->sector, &disk_inode);
      }
      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  
  if(offset >= inode->read_length)  
    return 0;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      disk_sector_t sector_idx = byte_to_sector (inode, offset, inode->read_length);
      int sector_ofs = offset % DISK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = (inode->read_length) - offset;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      /* read-ahead */
      disk_sector_t next_sector_idx = byte_to_sector (inode, offset+chunk_size, inode->read_length);
      if(next_sector_idx != -1)
        read_ahead_put(next_sector_idx);

      struct cache_line *cl = get_cache_line(sector_idx, 0);
      memcpy(buffer + bytes_read, (uint8_t *) &cl->block + sector_ofs, chunk_size);
      /* end of accessing the cache line */

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  /* beyond EOF, file grow needed */
  if(offset + size > inode->length){
    if(!inode->is_dir)
      lock_acquire(&inode->lock);
    inode_grow(inode, offset+size);
    if(!inode->is_dir)
      lock_release(&inode->lock);
  }

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      disk_sector_t sector_idx = byte_to_sector (inode, offset,inode->length);
      int sector_ofs = offset % DISK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      struct cache_line *cl = get_cache_line(sector_idx, 1);
      memcpy ((uint8_t *) &cl->block + sector_ofs, buffer + bytes_written, chunk_size);
      /* end of accessing the cache line */

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  inode->read_length = inode->length;
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->length;
}

/////////* newly made functions for project4 *////////////
void inode_grow(struct inode *inode, off_t new_length){
  static uint8_t zeros[DISK_SECTOR_SIZE];
  size_t old_sectors = bytes_to_sectors(inode->length);
  size_t sectors_to_add = bytes_to_sectors(new_length) - old_sectors;
  disk_sector_t indirect_buffer[128];
  disk_sector_t indirect_buffer2[128];
  uint32_t index;
  uint32_t index2;

  if(sectors_to_add == 0){
    inode->length = new_length;
    return;
  }

  /* case1. if inode->direct_ptr is not full */
  if(old_sectors == 0){
    free_map_allocate(1, &inode->direct_ptr);
    disk_write(filesys_disk, inode->direct_ptr, zeros);
    sectors_to_add --;
    old_sectors ++;
    if(sectors_to_add == 0){
      inode->length = new_length;
      return;
    }
  }

  /* case2. if inode->indirect_ptr is not full */
  if(old_sectors == 1)/* inode->indirect_ptr is not allocated */
    free_map_allocate(1, &inode->indirect_ptr);
  else
    disk_read(filesys_disk, inode->indirect_ptr, &indirect_buffer);
  
  while(old_sectors < 129){
    index = old_sectors - 1;/* next sector index to grow */
    free_map_allocate(1, &indirect_buffer[index]);
    disk_write(filesys_disk, indirect_buffer[index], zeros);
    sectors_to_add --;
    old_sectors ++;
    if(sectors_to_add == 0){
      disk_write(filesys_disk, inode->indirect_ptr, &indirect_buffer);
      inode->length = new_length;
      return;
    }
  }
  disk_write(filesys_disk, inode->indirect_ptr, &indirect_buffer);

  /* case3. here, inode->doubly_indirect_ptr is not full */
  ASSERT(old_sectors <= 16513);/* 1+128+128*128 */

  if(old_sectors == 129)/* inode->doubly_indirect_ptr is not allocated */
    free_map_allocate(1, &inode->doubly_indirect_ptr);
  else
    disk_read(filesys_disk, inode->doubly_indirect_ptr, &indirect_buffer);

  while(1){/* while (old_sectors < 16513) */
    index = (old_sectors - 129)/128;
    if(old_sectors % 128 == 1)/* the pointer block is not allocated */
      free_map_allocate(1, &indirect_buffer[index]);
    else
      disk_read(filesys_disk, indirect_buffer[index], &indirect_buffer2);
    while(old_sectors < (129+(index+1)*128)){/* escape when this pointer block's data sectors = 128 */
      index2 = ((old_sectors-129) % 128);/* next sector index to grow */
      free_map_allocate(1, &indirect_buffer2[index2]);
      disk_write(filesys_disk, indirect_buffer2[index2], zeros);
      sectors_to_add --;
      old_sectors ++;
      if(sectors_to_add == 0){
        disk_write(filesys_disk, indirect_buffer[index], &indirect_buffer2);
        disk_write(filesys_disk, inode->doubly_indirect_ptr, &indirect_buffer);
        inode->length = new_length;
        return;
      }
    }
    disk_write(filesys_disk, indirect_buffer[index], &indirect_buffer2);
  }
}

void inode_free(struct inode *inode){
  size_t sectors = bytes_to_sectors(inode->length);
  disk_sector_t indirect_buffer[128];
  disk_sector_t indirect_buffer2[128];
  uint32_t index;
  uint32_t index2;

  /* case1. if inode->direct_ptr is occupied */
  if(sectors > 0){
    free_map_release(inode->direct_ptr, 1);
  }

  /* case2. if inode->indirect_ptr is occupied */
  if(sectors > 1){
    disk_read(filesys_disk, inode->indirect_ptr, &indirect_buffer);
    free_map_release(inode->indirect_ptr, 1);
    index = sectors - 2;/* current sector index that will be deallocated */
    while(index < 0){
      free_map_release(indirect_buffer[index], 1);
      index --;
    }
  }

  /* case3. if inode->doubly_indirect_ptr is occupied */
  if(sectors > 129){
    disk_read(filesys_disk, inode->doubly_indirect_ptr, &indirect_buffer);
    free_map_release(inode->doubly_indirect_ptr, 1);
    index = (sectors - 129)/128;
    while(index < 0){
      disk_read(filesys_disk, indirect_buffer[index], &indirect_buffer2);
      free_map_release(indirect_buffer[index], 1);
      index2 = ((sectors - 129) % 128) - 1;/* current sector index that will be deallocated */
      while(index2 < 0){
        free_map_release(indirect_buffer2[index], 1);
        index2 --;
      }
      index --;
    }
  }
}

int inode_is_dir(const struct inode *inode){
  return inode->is_dir;
}

disk_sector_t inode_get_parent(const struct inode *inode){
  return inode->parent;
}

/* set child_sector's parent as parent_inode->sector*/
bool inode_set_parent(disk_sector_t child_sector, struct inode *parent_inode){
  struct inode* inode;

  if(!(inode = inode_open(child_sector)))
    return false;

  inode->parent = parent_inode->sector;
  inode_close(inode);
  return true;
}

int inode_get_opencnt(const struct inode *inode){
  return inode->open_cnt;
}

void inode_lock_acquire(struct inode *inode){
  lock_acquire(&inode->lock);
}

void inode_lock_release(struct inode *inode){
  lock_release(&inode->lock);
}
