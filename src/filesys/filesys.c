#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "devices/disk.h"
#include "filesys/cache.h"
#include "threads/thread.h"
#include "threads/malloc.h"

/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);
struct dir * get_dir (const char *);
char *get_filename (const char *);
/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  filesys_disk = disk_get (0, 1);
  if (filesys_disk == NULL)
    PANIC ("hd0:1 (hdb) not present, file system initialization failed");

  inode_init ();
  init_buffer_cache ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  write_behind_all (true);
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size, int is_dir) 
{
  disk_sector_t inode_sector = 0;
  bool success = false;
  
  /* tokenize name to get filename and directory's name */
  char *filename = get_filename(name);
  struct dir *dir = get_dir(name);

  /* disallow creation with filename = '.' or '..' */
  if(strcmp(filename, ".") != 0 && strcmp(filename, "..") != 0)
  {
    success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, is_dir)
                  && dir_add (dir, filename, inode_sector));
  }
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);
  free(filename);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  if(strlen(name) == 0)
    return NULL;
  struct inode *inode = NULL;

  /* tokenize name to get filename and directory's name */
  char *filename = get_filename(name);
  struct dir *dir = get_dir(name);
  
  if (dir != NULL){
    if(strcmp(filename, "..") == 0){
      inode = dir_get_parent(dir);
      if(!inode){
        /* no parent, so do not close dir */
        free(filename);
        return NULL;
      }
    }
    else if(strcmp(filename, ".") == 0 || (dir_is_root(dir) && strlen(filename) == 0)){
      /* should return dir, so do not close dir */
      free(filename);
      return (struct file *)dir;
    }
    else{
      dir_lookup (dir, filename, &inode);
    }
  }
  dir_close (dir);
  free(filename);

  if(!inode)
    return NULL;

  if(inode_is_dir(inode))
    return (struct file *)dir_open(inode);

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  /* tokenize name to get filename and directory's name */
  char *filename = get_filename(name);
  struct dir *dir = get_dir(name);

  bool success = dir != NULL && dir_remove (dir, filename);
  dir_close (dir); 
  free(filename);

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

/////////* newly made functions for project 4 */////////
char * get_filename (const char * name)
{
  char s[strlen(name) + 1];
  memcpy(s, name, strlen(name) + 1);

  char *token, *save_ptr, *prev_token = "";
  char *filename;
  for (token = strtok_r(s, "/", &save_ptr); token != NULL; token = strtok_r (NULL, "/", &save_ptr))
    prev_token = token;

  filename = malloc(strlen(prev_token) + 1);
  memcpy(filename, prev_token, strlen(prev_token) + 1);
  return filename;
}

struct dir * get_dir (const char * name)
{
  char s[strlen(name) + 1];
  memcpy(s, name, strlen(name) + 1);

  char *save_ptr, *prev_token, *token;
  struct dir *dir;
  struct inode *inode;
  if(s[0] == '/' || !thread_current()->dir)
    dir = dir_open_root();
  else
    dir = dir_reopen(thread_current()->dir);

  prev_token = strtok_r(s, "/", &save_ptr);
  for(token = strtok_r(NULL, "/", &save_ptr); token != NULL; token = strtok_r(NULL, "/", &save_ptr))
  {
    if(strcmp(prev_token, ".") == 0)
      continue;
    else if(strcmp(prev_token, "..") == 0)
    {
      inode = dir_get_parent(dir);
      if(inode == NULL)
        return NULL;
    }
    else if(dir_lookup(dir, prev_token, &inode) == false)
      return NULL;

    if(inode_is_dir(inode))
    {
      dir_close(dir);
      dir = dir_open(inode);
    }
    else
      inode_close(inode);

    prev_token = token;
  }
  return dir;
}

bool filesys_chdir (const char * name){
  char *filename = get_filename(name);
  struct dir *dir = get_dir(name);
  struct inode *parent_inode = NULL;

  if(dir == NULL)
  {
    free(filename);
    return false;
  }
  else if(strcmp(filename, ".") == 0 || (strlen(filename) == 0 && dir_is_root(dir)))
  {
    thread_current()->dir = dir;
    free(filename);
    return true;
  }
  else if(strcmp(filename, "..") == 0)
  {
    parent_inode = dir_get_parent(dir);
    if(parent_inode == NULL)
    {
      free(filename);
      return false;
    }
  }
  else
    dir_lookup(dir, filename, &parent_inode);

  dir_close(dir);
  dir = dir_open(parent_inode);
  if(dir == NULL)
  {
    free(filename);
    return false;
  }
  else
  {
    dir_close(thread_current()->dir);
    thread_current()->dir = dir;
    free(filename);
    return true;
  }
  return false;
}
