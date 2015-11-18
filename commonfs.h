#ifndef _COMMONFS_H
#define _COMMONFS_H
#include <fuse.h>

typedef enum { false, true } bool;
#define MAX_PATH_SIZE (1024 + 256 + 3)
#define THREAD_NAMELEN 16

// utimens support
#define HEADER_TEXT_MTIME "X-Object-Meta-Mtime"
#define HEADER_TEXT_ATIME "X-Object-Meta-Atime"
#define HEADER_TEXT_CTIME "X-Object-Meta-Ctime"
#define HEADER_TEXT_FILEPATH "X-Object-Meta-FilePath"
//#define TEMP_FILE_NAME_FORMAT "%s/.cloudfuse%ld-%s"
#define TEMP_FILE_NAME_FORMAT "%s/.cloudfuse_%s"

#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KBLU  "\x1B[34m"
#define KMAG  "\x1B[35m"
#define KCYN  "\x1B[36m"
#define KWHT  "\x1B[37m"

#define min(x, y) ({                \
  typeof(x) _min1 = (x);          \
  typeof(y) _min2 = (y);          \
  (void)(&_min1 == &_min2);      \
  _min1 < _min2 ? _min1 : _min2; })

//linked list with files in a directory
typedef struct dir_entry
{
  char *name;
  char *full_name;
  char *content_type;
  off_t size;
  time_t last_modified;
  // implement utimens
  struct timespec mtime;
  struct timespec ctime;
  struct timespec atime;
  char *md5sum; //interesting capability for rsync/scrub
  int chmod;
	int issegmented;
	time_t accessed_in_cache;//cache support based on access time
  // end change
  int isdir;
  int islink;
  struct dir_entry *next;
} dir_entry;

// linked list with cached folder names
typedef struct dir_cache
{
  char *path;
  dir_entry *entries;
  time_t cached;
	//added cache support based on access time
	time_t accessed_in_cache;
	//end change
  struct dir_cache *next, *prev;
} dir_cache;

time_t my_timegm(struct tm *tm);
time_t get_time_from_str_as_gmt(char *time_str);
time_t get_time_as_local(time_t time_t_val, char time_str[], int char_buf_size);
int get_time_as_string(time_t time_t_val, char *time_str);
time_t get_time_now();

char *str2md5(const char *str, int length);
void debug_print_descriptor(struct fuse_file_info *info);
int get_safe_cache_file_path(const char *file_path, char *file_path_safe, char *temp_dir);

void init_dir_entry(dir_entry *de);
dir_cache *new_cache(const char *path);
void dir_for(const char *path, char *dir);
void debug_list_cache_content();
void update_dir_cache(const char *path, off_t size, int isdir, int islink);
dir_entry *path_info(const char *path);
dir_entry *check_path_info(const char *path);
dir_entry * check_parent_folder_for_file(const char *path);
void dir_decache(const char *path);
void cloudfs_free_dir_list(dir_entry *dir_list);
extern int cloudfs_list_directory(const char *path, dir_entry **);
int caching_list_directory(const char *path, dir_entry **list);

char *get_home_dir();
void cloudfs_debug(int dbg);
void debugf(char *fmt, ...);


#endif
