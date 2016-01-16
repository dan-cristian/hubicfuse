#define _GNU_SOURCE
#include <stdio.h>
#include <magic.h>
#include <string.h>
#include <stdarg.h>
#include <stdarg.h>
#include <math.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef __linux__
#include <alloca.h>
#endif
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <libxml/tree.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <json.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <errno.h>
#include <fuse.h>
#include <assert.h>
#include <openssl/md5.h>
#include "commonfs.h"
#include "cloudfsapi.h"
#include "config.h"

#define RHEL5_LIBCURL_VERSION 462597
#define RHEL5_CERTIFICATE_FILE "/etc/pki/tls/certs/ca-bundle.crt"
#define REQUEST_RETRIES 5
#define MAX_FILES 10000
// size of buffer for writing to disk look at ioblksize.h in coreutils
// and try some values on your own system if you want the best performance
#define DISK_BUFF_SIZE 32768

static char storage_url[MAX_URL_SIZE];
static char storage_token[MAX_HEADER_SIZE];
static pthread_mutex_t pool_mut;
extern pthread_mutexattr_t segment_mutex_attr;
static CURL* curl_pool[1024];
static int curl_pool_count = 0;
extern int debug;
extern int verify_ssl;
extern bool option_get_extended_metadata;
extern bool option_curl_verbose;
extern int option_curl_progress_state;
extern int option_cache_statfs_timeout;
extern bool option_extensive_debug;
extern bool option_enable_chown;
extern bool option_enable_chmod;
extern bool option_enable_progressive_upload;
extern bool option_enable_progressive_download;
extern long option_min_speed_limit_progressive;
extern long option_min_speed_timeout;
extern char* temp_dir;
extern long option_read_ahead;
static int rhel5_mode = 0;
static struct statvfs statcache =
{
  .f_bsize = 4096,
  .f_frsize = 4096,
  .f_blocks = INT_MAX,
  .f_bfree = INT_MAX,
  .f_bavail = INT_MAX,
  .f_files = MAX_FILES,
  .f_ffree = 0,
  .f_favail = 0,
  .f_namemax = INT_MAX
};
static time_t last_stat_read_time = 0;//used to compute cache interval
extern FuseOptions options;

struct MemoryStruct
{
  char* memory;
  size_t size;
};

#ifdef HAVE_OPENSSL
#include <openssl/crypto.h>
static pthread_mutex_t* ssl_lockarray;
static void lock_callback(int mode, int type, char* file, int line)
{
  if (mode & CRYPTO_LOCK)
    pthread_mutex_lock(&(ssl_lockarray[type]));
  else
    pthread_mutex_unlock(&(ssl_lockarray[type]));
}

static unsigned long thread_id()
{
  return (unsigned long)pthread_self();
}
#endif

static size_t xml_dispatch(void* ptr, size_t size, size_t nmemb, void* stream)
{
  xmlParseChunk((xmlParserCtxtPtr)stream, (char*)ptr, size * nmemb, 0);
  return size * nmemb;
}

static CURL* get_connection(const char* path)
{
  pthread_mutex_lock(&pool_mut);
  CURL* curl = curl_pool_count ? curl_pool[--curl_pool_count] : curl_easy_init();
  if (!curl)
  {
    debugf(DBG_LEVEL_NORM, KRED"curl alloc failed");
    abort();
  }
  pthread_mutex_unlock(&pool_mut);
  return curl;
}

static void return_connection(CURL* curl)
{
  pthread_mutex_lock(&pool_mut);
  curl_pool[curl_pool_count++] = curl;
  pthread_mutex_unlock(&pool_mut);
}

static void add_header(curl_slist** headers, const char* name,
                       const char* value)
{
  char x_header[MAX_HEADER_SIZE];
  char safe_value[256];
  const char* value_ptr;
  debugf(DBG_LEVEL_EXT, "add_header(%s:%s)", name, value);
  if (strlen(value) > 256)
  {
    debugf(DBG_LEVEL_NORM, KRED"add_header: warning, value size > 256 (%s:%s) ",
           name, value);
    //hubic will throw an HTTP 400 error on X-Copy-To operation
    //if X-Object-Meta-FilePath header value is larger than 256 chars
    //fix for issue #95 https://github.com/TurboGit/hubicfuse/issues/95
    if (!strcasecmp(name, "X-Object-Meta-FilePath"))
    {
      debugf(DBG_LEVEL_NORM,
             KRED"add_header: trimming header (%s) value to max allowed", name);
      //trim header size to max allowed
      strncpy(safe_value, value, 256 - 1);
      safe_value[255] = '\0';
      value_ptr = safe_value;
    }
    else
      value_ptr = value;
  }
  else
    value_ptr = value;
  snprintf(x_header, sizeof(x_header), "%s: %s", name, value_ptr);
  *headers = curl_slist_append(*headers, x_header);
}

static size_t header_dispatch(void* ptr, size_t size, size_t nmemb,
                              void* dir_entry)
{
  char* header = (char*)alloca(size * nmemb + 1);
  char* head = (char*)alloca(size * nmemb + 1);
  char* value = (char*)alloca(size * nmemb + 1);
  memcpy(header, (char*)ptr, size * nmemb);
  header[size * nmemb] = '\0';
  if (sscanf(header, "%[^:]: %[^\r\n]", head, value) == 2)
  {
    if (!strncasecmp(head, "x-auth-token", size * nmemb))
      strncpy(storage_token, value, sizeof(storage_token));
    if (!strncasecmp(head, "x-storage-url", size * nmemb))
      strncpy(storage_url, value, sizeof(storage_url));
    if (!strncasecmp(head, "x-account-meta-quota", size * nmemb))
      statcache.f_blocks = (unsigned long) (strtoull(value, NULL,
                                            10) / statcache.f_frsize);
    if (!strncasecmp(head, "x-account-bytes-used", size * nmemb))
      statcache.f_bfree = statcache.f_bavail = statcache.f_blocks - (unsigned long) (
                            strtoull(value, NULL, 10) / statcache.f_frsize);
    if (!strncasecmp(head, "x-account-object-count", size * nmemb))
    {
      unsigned long object_count = strtoul(value, NULL, 10);
      statcache.f_ffree = MAX_FILES - object_count;
      statcache.f_favail = MAX_FILES - object_count;
    }
  }
  return size * nmemb;
}

static void header_set_time_from_str(char* time_str,
                                     struct timespec* time_entry)
{
  char sec_value[TIME_CHARS];
  char nsec_value[TIME_CHARS];
  time_t sec;
  long nsec;
  sscanf(time_str, "%[^.].%[^\n]", sec_value, nsec_value);
  sec = strtoll(sec_value, NULL, 10);//to allow for larger numbers
  nsec = atol(nsec_value);
  debugf(DBG_LEVEL_EXTALL, "Received time=%s.%s / %li.%li, existing=%li.%li",
         sec_value, nsec_value, sec, nsec, time_entry->tv_sec, time_entry->tv_nsec);
  if (sec != time_entry->tv_sec || nsec != time_entry->tv_nsec)
  {
    debugf(DBG_LEVEL_EXTALL,
           "Time changed, setting new time=%li.%li, existing was=%li.%li",
           sec, nsec, time_entry->tv_sec, time_entry->tv_nsec);
    time_entry->tv_sec = sec;
    time_entry->tv_nsec = nsec;
    char time_str_local[TIME_CHARS] = "";
    get_time_as_string((time_t)sec, nsec, time_str_local, sizeof(time_str_local));
    debugf(DBG_LEVEL_EXTALL, "header_set_time_from_str received time=[%s]",
           time_str_local);
    get_timespec_as_str(time_entry, time_str_local, sizeof(time_str_local));
    debugf(DBG_LEVEL_EXTALL, "header_set_time_from_str set time=[%s]",
           time_str_local);
  }
}

/*
   get file metadata from HTTP response headers
*/
static size_t header_get_meta_dispatch(void* ptr, size_t size, size_t nmemb,
                                       void* userdata)
{
  size_t memsize = size * nmemb;
  char* header = (char*)alloca(memsize + 1);
  char* head = (char*)alloca(memsize + 1);
  char* value = (char*)alloca(memsize + 1);
  memcpy(header, (char*)ptr, memsize);
  header[memsize] = '\0';
  static char storage[MAX_HEADER_SIZE];
  if (sscanf(header, "%[^:]: %[^\r\n]", head, value) == 2)
  {
    //sometimes etag is formated on hubic with "" (for segmented files?), check inconsistency!
    char* quote_lptr = strchr(value, '"');
    char* quote_rptr = strrchr(value, '"');
    if (quote_lptr || quote_rptr)
    {
      debugf(DBG_LEVEL_NORM, "header_get_meta_dispatch: " KRED
             "header value incorrectly formated on cloud, head=[%s], value=[%s]",
             head, value);

      if (quote_lptr && quote_rptr)
      {
        debugf(DBG_LEVEL_NORM, "header_get_meta_dispatch: " KYEL
               "fixing by stripping quotes, value=[%s]", value);
        removeSubstr(value, "\"");
        debugf(DBG_LEVEL_NORM, "header_get_meta_dispatch: " KGRN
               "fixed value=[%s]", value);
      }
      else
        debugf(DBG_LEVEL_NORM, "header_get_meta_dispatch: " KRED
               "unable to fix value=[%s]", value);
    }
    strncpy(storage, head, sizeof(storage));
    debugf(DBG_LEVEL_EXTALL, "header_get_meta_dispatch: " KCYN
           "head=[%s] val=[%s]", head, value);
    dir_entry* de = (dir_entry*)userdata;

    if (de != NULL)
    {
      if (!strncasecmp(head, HEADER_TEXT_ATIME, memsize))
        header_set_time_from_str(value, &de->atime);
      else if (!strncasecmp(head, HEADER_TEXT_CTIME, memsize))
      {
        header_set_time_from_str(value, &de->ctime);
        header_set_time_from_str(value, &de->ctime_local);
      }
      else if (!strncasecmp(head, HEADER_TEXT_MTIME, memsize))
        header_set_time_from_str(value, &de->mtime);
      else if (!strncasecmp(head, HEADER_TEXT_CHMOD, memsize))
        de->chmod = atoi(value);
      else if (!strncasecmp(head, HEADER_TEXT_GID, memsize))
        de->gid = atoi(value);
      else if (!strncasecmp(head, HEADER_TEXT_UID, memsize))
        de->uid = atoi(value);
      else if (!strncasecmp(head, HEADER_TEXT_MD5HASH, memsize))
      {
        if (de->md5sum == NULL)
        {
          de->md5sum = strdup(value);
          debugf(DBG_LEVEL_EXT, "header_get_meta_dispatch: set md5sum=%s",
                 de->md5sum);
        }
        else if (strcasecmp(de->md5sum, value))
        {
          //todo: hash is different, usually on large segmented files
          debugf(DBG_LEVEL_NORM, "header_get_meta_dispatch: " KYEL
                 "hash difference, unreliable data, cache=%s cloud=%s",
                 de->md5sum, value);
          //fixme: sometimes etag on hubic is incorrect,
          //noticed on segmented files or newly uploaded files (rigth after PUT)
          free(de->md5sum);
          de->md5sum = strdup(value);//this is ok for PUT
        }
      }
      else if (!strncasecmp(head, HEADER_TEXT_IS_SEGMENTED, memsize))
      {
        de->is_segmented = atoi(value);
        debugf(DBG_LEVEL_EXT, "header_get_meta_dispatch: manual is_segmented=%d",
               de->is_segmented);
      }
      else if (!strncasecmp(head, HEADER_TEXT_MANIFEST, memsize))
      {
        free(de->manifest_cloud);
        char manifest[MAX_URL_SIZE] = "";
        //manifest path needs to start with / on HEAD (list) operations
        snprintf(manifest, MAX_URL_SIZE, "/%s", value);
        decode_path(manifest);
        de->manifest_cloud = strdup(manifest);
        debugf(DBG_LEVEL_EXT, "header_get_meta_dispatch: manifest=%s",
               de->manifest_cloud);
      }
      else if (!strncasecmp(head, HEADER_TEXT_SEGMENT_SIZE, memsize))
        de->segment_size = atol(value);
    }
    else abort();
    //debugf(DBG_LEVEL_EXT,
    //       "Unexpected NULL dir_entry on header(%s), file should be in cache already",
    //       storage);
  }
  else
  {
    //debugf(DBG_LEVEL_NORM, "Received unexpected header line");
  }
  return memsize;
}

void set_direntry_headers(dir_entry* de, curl_slist* headers)
{
  char atime_str_nice[TIME_CHARS] = "";
  char mtime_str_nice[TIME_CHARS] = "";
  char ctime_str_nice[TIME_CHARS] = "";
  get_timespec_as_str(&(de->atime), atime_str_nice, sizeof(atime_str_nice));
  debugf(DBG_LEVEL_EXTALL, KCYN"send_request_size: atime=[%s]", atime_str_nice);
  get_timespec_as_str(&(de->mtime), mtime_str_nice, sizeof(mtime_str_nice));
  debugf(DBG_LEVEL_EXTALL, KCYN"send_request_size: mtime=[%s]", mtime_str_nice);
  get_timespec_as_str(&(de->ctime), ctime_str_nice, sizeof(ctime_str_nice));
  debugf(DBG_LEVEL_EXTALL, KCYN"send_request_size: ctime=[%s]", ctime_str_nice);
  char mtime_str[TIME_CHARS];
  char atime_str[TIME_CHARS];
  char ctime_str[TIME_CHARS];
  char string_float[TIME_CHARS];
  snprintf(mtime_str, TIME_CHARS, "%lu.%lu", de->mtime.tv_sec,
           de->mtime.tv_nsec);
  snprintf(atime_str, TIME_CHARS, "%lu.%lu", de->atime.tv_sec,
           de->atime.tv_nsec);
  snprintf(ctime_str, TIME_CHARS, "%lu.%lu", de->ctime.tv_sec,
           de->ctime.tv_nsec);
  add_header(&headers, HEADER_TEXT_FILEPATH, de->full_name);//orig_path);
  add_header(&headers, HEADER_TEXT_MTIME, mtime_str);
  add_header(&headers, HEADER_TEXT_ATIME, atime_str);
  add_header(&headers, HEADER_TEXT_CTIME, ctime_str);
  add_header(&headers, HEADER_TEXT_MTIME_DISPLAY, mtime_str_nice);
  add_header(&headers, HEADER_TEXT_ATIME_DISPLAY, atime_str_nice);
  add_header(&headers, HEADER_TEXT_CTIME_DISPLAY, ctime_str_nice);
  char gid_str[INT_CHAR_LEN], uid_str[INT_CHAR_LEN], chmod_str[INT_CHAR_LEN];
  char is_segmented_str[INT_CHAR_LEN], seg_size_str[INT_CHAR_LEN];
  snprintf(gid_str, INT_CHAR_LEN, "%d", de->gid);
  snprintf(uid_str, INT_CHAR_LEN, "%d", de->uid);
  snprintf(chmod_str, INT_CHAR_LEN, "%d", de->chmod);
  snprintf(is_segmented_str, INT_CHAR_LEN, "%d", de->is_segmented);
  snprintf(seg_size_str, INT_CHAR_LEN, "%lu", de->segment_size);
  add_header(&headers, HEADER_TEXT_GID, gid_str);
  add_header(&headers, HEADER_TEXT_UID, uid_str);
  add_header(&headers, HEADER_TEXT_CHMOD, chmod_str);
  add_header(&headers, HEADER_TEXT_IS_SEGMENTED, is_segmented_str);
  add_header(&headers, HEADER_TEXT_PRODUCED_BY, APP_ID);
  add_header(&headers, HEADER_TEXT_SEGMENT_SIZE, seg_size_str);
}
/*
   write data to file from segmented download
*/
size_t fwrite2(void* ptr, size_t size, size_t nmemb, FILE* filep)
{
  //debugf(DBG_LEVEL_EXT, KCYN "fwrite2: size=%lu",
  //       size * nmemb);
  size_t result = fwrite((const void*)ptr, size, nmemb, filep);
  assert(result == size * nmemb);
  return result;
}

/*
   helper for uploading/downloading file
   detects file end
*/
static size_t rw_callback(size_t (*rw)(void*, size_t, size_t, FILE*),
                          void* ptr,
                          size_t size, size_t nmemb, void* userp)
{
  struct segment_info* info = (struct segment_info*)userp;
  size_t result;
  size_t mem = size * nmemb;
  if (mem < 1 || info->size_left < 1)
    result = 0;
  else
  {
    size_t amt_read = rw(ptr, 1, info->size_left < mem ? info->size_left : mem,
                         info->fp);
    info->size_left -= amt_read;
    info->size_processed += amt_read;
    result = amt_read;
  }
  assert((mem == result) || (info->size_processed == info->size_copy));
  return result;
}

size_t fread2(void* ptr, size_t size, size_t nmemb, FILE* filep)
{
  size_t result = fread(ptr, size, nmemb, filep);
  assert(result == size * nmemb);
  return result;
}
/*
   pass data for uploading multiple segments
*/
static size_t read_callback(void* ptr, size_t size, size_t nmemb, void* userp)
{
  struct segment_info* info = (struct segment_info*)userp;
  debugf(DBG_LEVEL_EXT, KMAG
         "read_callback: progressive=%d size=%lu current=%lu",
         info->de->is_progressive, size * nmemb, info->size_processed);
  return rw_callback(fread2, ptr, size, nmemb, userp);
}

/*
   write data to file from segmented download
*/
static size_t write_callback_progressive(void* ptr, size_t size, size_t nmemb,
    void* userp)
{
  size_t http_size = size * nmemb;
  size_t result = http_size;
  int rnd = random_at_most(50);

  debugf(DBG_LEVEL_EXTALL,
         KMAG"write_callback_progrs: http_size=%lu rnd=%d", http_size, rnd);
  sleep_ms(rnd);
  struct segment_info* info = (struct segment_info*)userp;
  dir_entry* de;
  if (info->de->is_segmented)
    de = info->de_seg;
  else de = info->de;
  //todo: in case of progressive ops signal we have data to cfs_read
  debugf(DBG_LEVEL_EXT,
         KMAG"write_callback_progrs: http buffer full, fuse_size=%lu, wait empty",
         de->downld_buf.fuse_read_size);
  size_t data_copy_size;
  size_t http_ptr_index = 0;
  const void* src;
  const void* dest;
  int sem_val;

  sem_wait(de->downld_buf.sem_list[SEM_EMPTY]);

  //copy data to fuse buffer until is full OR until no data left to copy in http buf
  while (de->downld_buf.work_buf_size < de->downld_buf.fuse_read_size
         && (http_size - http_ptr_index > 0))
  {
    //data left needed
    data_copy_size = min(de->downld_buf.fuse_read_size -
                         de->downld_buf.work_buf_size,
                         http_size - http_ptr_index);
    src = ptr + http_ptr_index;
    dest = de->downld_buf.readptr + de->downld_buf.work_buf_size;
    memcpy((void*)dest, src, data_copy_size);
    de->downld_buf.work_buf_size += data_copy_size;
    http_ptr_index += data_copy_size;
    debugf(DBG_LEVEL_EXT, KCYN
           "write_callback_progrs: data_copy_size=%lu ptr=%lu src=%lu dest=%lu wrksize=%lu lefthttp=%lu",
           data_copy_size, ptr, src, dest, de->downld_buf.work_buf_size,
           http_size - http_ptr_index);

    if ((de->downld_buf.work_buf_size == de->downld_buf.fuse_read_size)
        && (http_size != http_ptr_index))
    {
      sem_getvalue(de->downld_buf.sem_list[SEM_FULL], &sem_val);
      //fuse buffer full, http data remains
      debugf(DBG_LEVEL_EXT, KMAG
             "write_callback_progrs: copied, post full, some http data left=%lu sem=%d",
             http_size - http_ptr_index, sem_val);
      sem_post(de->downld_buf.sem_list[SEM_FULL]);
      sem_getvalue(de->downld_buf.sem_list[SEM_EMPTY], &sem_val);
      debugf(DBG_LEVEL_EXT, KMAG
             "write_callback_progrs: wait [1] fuse buffer empty, sem=%d", sem_val);
      sem_wait(de->downld_buf.sem_list[SEM_EMPTY]);
      //after this work_buf_size = 0, set by cfs_read
      debugf(DBG_LEVEL_EXT, KMAG
             "write_callback_progrs: done wait empty [1] work_buf=%lu",
             de->downld_buf.work_buf_size);
    }

    if (data_copy_size == http_size)
    {
      //http buffer fully copied, more http data needed
      debugf(DBG_LEVEL_EXT, KMAG
             "write_callback_progrs: http buffer fully copied");
      break;
    }
    //fuse size can change from time to time in cfs_read (why?)
  }
  //exit here if http buffer was fully copied

  //fuse buffer full
  if (de->downld_buf.work_buf_size == de->downld_buf.fuse_read_size)
  {
    sem_getvalue(de->downld_buf.sem_list[SEM_FULL], &sem_val);
    //fuse buffer is full, http fully copied, signal cfs_read to return it in user space
    debugf(DBG_LEVEL_EXT, KMAG"write_callback_progrs: post full sem=%d",
           sem_val);
    sem_post(de->downld_buf.sem_list[SEM_FULL]);
    sem_getvalue(de->downld_buf.sem_list[SEM_EMPTY], &sem_val);
    debugf(DBG_LEVEL_EXT, KMAG
           "write_callback_progrs: wait [2] fuse buffer to get empty sem=%d", sem_val);
    sem_wait(de->downld_buf.sem_list[SEM_EMPTY]);
    debugf(DBG_LEVEL_EXT, KMAG
           "write_callback_progrs: done wait empty [2] work_buf=%lu",
           de->downld_buf.work_buf_size);
    //post to avoid being stuck on this callback
    sem_post(de->downld_buf.sem_list[SEM_EMPTY]);
  }
  else
  {
    debugf(DBG_LEVEL_EXT, KMAG
           "write_callback_progrs: incomplete fuse_buf, need more http data work_buf=%lu",
           de->downld_buf.work_buf_size);
    //post to avoid being stuck on this callback
    sem_post(de->downld_buf.sem_list[SEM_EMPTY]);
  }

  debugf(DBG_LEVEL_EXT, KMAG"exit: write_callback_progrs result=%lu", result);
  return result;
}

static size_t write_callback(void* ptr, size_t size, size_t nmemb, void* userp)
{
  struct segment_info* info = (struct segment_info*)userp;
  //show progress from time to time
  if (info->size_processed % 100000 == 0)
    debugf(DBG_LEVEL_EXT, KMAG
           "write_callback: progressive=%d size=%lu max=%lu current=%lu",
           info->de->is_progressive, size * nmemb, CURL_MAX_WRITE_SIZE,
           info->size_processed);
  //send data to fuse buffer
  if (info->de->is_progressive)
    write_callback_progressive(ptr, size, nmemb, userp);

  //write data to local cache file
  size_t result = rw_callback(fwrite2, ptr, size, nmemb, userp);
  assert(result == size * nmemb);
  if (result == 0 && !info->de->is_progressive)
  {
    debugf(DBG_LEVEL_EXT, KMAG "write_callback: post buf full, res=%lu, size=%lu",
           result, info->size_processed);
  }
  debugf(DBG_LEVEL_EXTALL, KMAG"write_callback: result=%lu", result);
  return result;
}

/*
   called during http operations, currently used only for debug purposes
   http://curl.haxx.se/libcurl/c/CURLOPT_XFERINFOFUNCTION.html
*/
int progress_callback_xfer(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                           curl_off_t ultotal, curl_off_t ulnow)
{
  struct curl_progress* myp = (struct curl_progress*)clientp;
  CURL* curl = myp->curl;
  double curtime = 0;
  double dspeed = 0, uspeed = 0;
  curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &curtime);
  curl_easy_getinfo(curl, CURLINFO_SPEED_DOWNLOAD, &dspeed);
  curl_easy_getinfo(curl, CURLINFO_SPEED_UPLOAD, &uspeed);
  /* under certain circumstances it may be desirable for certain functionality
     to only run every N seconds, in order to do this the transaction time can
     be used */
  //http://curl.haxx.se/cvssource/src/tool_cb_prg.c
  if ((curtime - myp->lastruntime) >= MINIMAL_PROGRESS_FUNCTIONALITY_INTERVAL)
  {
    myp->lastruntime = curtime;
    curl_off_t total;
    curl_off_t point;
    double frac, percent;
    total = dltotal + ultotal;
    point = dlnow + ulnow;
    frac = (double)point / (double)total;
    percent = frac * 100.0f;
    debugf(DBG_LEVEL_EXT, "TOTAL TIME: %.0f sec Down=%.0f Kbps UP=%.0f Kbps",
           curtime, dspeed / 1024, uspeed / 1024);
    debugf(DBG_LEVEL_EXT, "UP: %lld of %lld DOWN: %lld/%lld Completion %.1f %%",
           ulnow, ultotal, dlnow, dltotal, percent);
  }
  return 0;
}

/*
   for compatibility purposes, will be deprecated
   http://curl.haxx.se/libcurl/c/CURLOPT_PROGRESSFUNCTION.html
*/
int progress_callback(void* clientp, double dltotal, double dlnow,
                      double ultotal, double ulnow)
{
  return progress_callback_xfer(clientp, (curl_off_t)dltotal, (curl_off_t)dlnow,
                                (curl_off_t)ultotal, (curl_off_t)ulnow);
}

/*
   get the response from HTTP requests, mostly for debug purposes
   http://stackoverflow.com/questions/2329571/c-libcurl-get-output-into-a-string
   http://curl.haxx.se/libcurl/c/getinmemory.html
*/
size_t writefunc_callback(void* contents, size_t size, size_t nmemb,
                          void* userp)
{
  size_t realsize = size * nmemb;
  struct MemoryStruct* mem = (struct MemoryStruct*)userp;
  mem->memory = realloc(mem->memory, mem->size + realsize + 1);
  if (mem->memory == NULL)
  {
    /* out of memory! */
    debugf(DBG_LEVEL_NORM, KRED"writefunc_callback: realloc() failed");
    return 0;
  }
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;
  return realsize;
}

// provides progressive data on upload for PUT/POST
static size_t progressive_upload_callback(void* ptr, size_t size, size_t nmemb,
    void* userp)
{
  dir_entry* de = (dir_entry*)userp;
  assert(de);
  size_t http_buf_size = size * nmemb;
  struct progressive_data_buf* upload_buf = &de->upload_buf;
  assert(upload_buf);
  debugf(DBG_LEVEL_EXTALL,
         "progressive_upload_callback: entering for path(%s) size=%lu nmemb=%lu",
         de->full_name, size, nmemb);
  if (http_buf_size < 1)
  {
    debugf(DBG_LEVEL_EXT,
           "progressive_upload_callback: "KYEL"exit as size*nmemb < 1");
    return 0;
  }
  size_t max_size_to_upload;
  int sem_val_empty, sem_val_full;

  /*if (upload_buf->offset == 0 && upload_buf->work_buf_size != 0)
    {
      //opening semaphores might not be needed - same process?
      if ((de->upload_buf.isempty_semaphore = sem_open(
        de->upload_buf.isempty_semaphore_name, O_CREAT, 0644, 0)) == SEM_FAILED)
      {
      int errsv = errno;
      debugf(DBG_LEVEL_NORM,
             KRED"progressive_read_callback: cannot open isempty_semaphore, err=%s",
             strerror(errsv));
      }
      else
      debugf(DBG_LEVEL_EXTALL,
             "progressive_read_callback: isempty_semaphore opened");
      if ((de->upload_buf.isfull_semaphore = sem_open(
        de->upload_buf.isfull_semaphore_name, O_CREAT, 0644, 0)) == SEM_FAILED)
      {
      int errsv = errno;
      debugf(DBG_LEVEL_NORM,
             KRED"progressive_read_callback: cannot open isfull_semaphore, err=%s",
             strerror(errsv));
      }
      else
      debugf(DBG_LEVEL_EXTALL, "progressive_read_callback: isfull_semaphore opened");

    }
  */

  debugf(DBG_LEVEL_EXT,
         "progressive_upload_callback(%s): processing size_proc=%lu",
         de->name, upload_buf->size_processed);
  assert(de->upload_buf.sem_list[SEM_FULL]);

  //wait to get fuse buffer data
  sem_wait(de->upload_buf.sem_list[SEM_FULL]);

  //ensure we upload no more than fuse buf available
  max_size_to_upload = min(http_buf_size, de->upload_buf.work_buf_size);
  //ensure we upload up to max segment size
  max_size_to_upload = min(max_size_to_upload,
                           de->size - de->upload_buf.size_processed);

  if (max_size_to_upload > 0)
  {
    //todo: check if this mem copy can be optimised
    //http://sourceforge.net/p/fuse/mailman/message/29119987/
    memcpy(ptr, upload_buf->readptr, max_size_to_upload);
    upload_buf->readptr += max_size_to_upload;
    upload_buf->work_buf_size -= max_size_to_upload;
    upload_buf->size_processed += max_size_to_upload;
    debugf(DBG_LEVEL_EXTALL, "progressive_upload_callback(%s): " KMAG
           "feed http data size=%lu", de->name, max_size_to_upload);
    sem_post(de->upload_buf.sem_list[SEM_EMPTY]);
    return max_size_to_upload;
  }

  //all data uploaded and write completed, exit
  debugf(DBG_LEVEL_EXT, KMAG
         "progressive_upload_callback(%s): segment upload done, blen=%lu, tlen=%lu",
         de->name, max_size_to_upload, upload_buf->size_processed);
  //set segment actual size
  de->size = upload_buf->size_processed;

  //signal we're done, usefull for cfs_flush etc
  if (de->upload_buf.sem_list[SEM_EMPTY])
    sem_post(de->upload_buf.sem_list[SEM_EMPTY]);

  //fixme: get's stuck here as it pased lock in cfs_write
  //pthread_mutex_lock(&de->upload_buf.mutex);
  //free both
  //pthread_mutex_unlock(&de->upload_buf.mutex);

  return 0;
}

/*
   if de_seg != null assumes this sends a segment request
*/
static int send_request_size(const char* method, const char* encoded_path,
                             void* fp, xmlParserCtxtPtr xmlctx,
                             curl_slist* extra_headers, off_t file_size,
                             int is_segment, dir_entry* de, dir_entry* de_seg)
{
  debugf(DBG_LEVEL_NORM,
         "send_request_size(%s) size=%lu is_seg=%d (%s) de=%p seg_de=%p %s:%s",
         method, file_size, is_segment, encoded_path, de, de_seg,
         (de ? de->name : "nil"), (de_seg ? de_seg->name : "nil"));
  char url[MAX_URL_SIZE];
  char header_data[MAX_HEADER_SIZE];


  char* slash;
  long response = -1;
  int tries = 0;
  double total_time;
  bool is_download = false;
  bool is_upload = false;
  double size_downloaded = 0;
  double size_uploaded = 0;
  struct segment_info* info = NULL;
  //needed to keep the response data, for debug purposes
  struct MemoryStruct chunk;
  assert(storage_url[0]);

  char* path, *orig_path;
  const char* print_path;
  if (de_seg != NULL)
  {
    path = curl_escape(de_seg->full_name, 0);
    print_path = de_seg->full_name;
  }
  else if (de != NULL)
  {
    path = curl_escape(de->full_name, 0);
    print_path = de->full_name;
  }
  else
  {
    path = (char*)encoded_path;
    print_path = encoded_path;
  }

  orig_path = path; //copy to be freed ok as path ptr will change
  decode_path(path);
  //remove "/" prefix
  while (*path == '/')
    path++;

  snprintf(url, sizeof(url), "%s/%s", storage_url, path);
  //snprintf(orig_path, sizeof(orig_path), "/%s", path);
  // retry on HTTP failures
  for (tries = 0; tries < REQUEST_RETRIES; tries++)
  {
    debugf(DBG_LEVEL_EXT, "send_request_size(%s): try #%d/%d", print_path, tries,
           REQUEST_RETRIES);
    chunk.memory = malloc(1);  /* will be grown as needed by the realloc above */
    chunk.size = 0;    /* no data at this point */
    CURL* curl = get_connection(path);
    if (rhel5_mode)
      curl_easy_setopt(curl, CURLOPT_CAINFO, RHEL5_CERTIFICATE_FILE);
    curl_slist* headers = NULL;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HEADER, 0);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
    //reversed logic, 0 to enable progress
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,
                     option_curl_progress_state ? 0 : 1);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verify_ssl ? 1 : 0);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify_ssl);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, option_curl_verbose ? 1 : 0);
    //if previous error was 0 (usually on SSL timeouts), try a fresh connect
    if (response == 0)
      curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1);
    else
      curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 0);
    add_header(&headers, "X-Auth-Token", storage_token);

    // add headers to save utimens attribs only on upload
    if (!strcasecmp(method, HTTP_PUT) //|| !strcasecmp(method, "MKDIR")
        || !strcasecmp(method, HTTP_POST))
    {
      debugf(DBG_LEVEL_EXTALL, "send_request_size: Saving utimens for file %s",
             orig_path);
      //on rename de is null
      if (de)
      {
        debugf(DBG_LEVEL_EXTALL,
               "send_request_size: Cached utime for path=%s ctime=%li.%li mtime=%li.%li atime=%li.%li",
               orig_path,
               de->ctime.tv_sec, de->ctime.tv_nsec, de->mtime.tv_sec, de->mtime.tv_nsec,
               de->atime.tv_sec, de->atime.tv_nsec);
        set_direntry_headers(de, headers);
      }
    }
    else
      debugf(DBG_LEVEL_EXTALL, "send_request_size: not setting utimes (%s)",
             orig_path);

    if (!strcasecmp(method, "MKDIR"))
    {
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1);
      curl_easy_setopt(curl, CURLOPT_INFILESIZE, 0);
      add_header(&headers, "Content-Type", "application/directory");
    }
    else if (!strcasecmp(method, HTTP_POST))
    {
      debugf(DBG_LEVEL_EXT, "send_request_size: POST (%s)", orig_path);
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_INFILESIZE, 0);
      curl_easy_setopt(curl, CURLOPT_EXPECT_100_TIMEOUT_MS, 10000L);

      //curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &header_dispatch);
      //add_header(&headers, "Transfer-Encoding", "chunked");
      //add_header(&headers, "Expect", "");
    }
    else if (!strcasecmp(method, "MKLINK") && fp)
    {
      rewind(fp);
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1);
      curl_easy_setopt(curl, CURLOPT_INFILESIZE, file_size);
      curl_easy_setopt(curl, CURLOPT_READDATA, fp);
      add_header(&headers, "Content-Type", "application/link");
    }
    else if (!strcasecmp(method, HTTP_PUT))
    {
      is_upload = true;
      //todo: read response headers and update file meta (etag & last-modified)
      //http://blog.chmouel.com/2012/02/06/anatomy-of-a-swift-put-query-to-object-server/
      debugf(DBG_LEVEL_EXT, "send_request_size: enter PUT (%s) size=%lu de=%p",
             orig_path, file_size, de);
      //don't do progressive on file creation, when size=0 (why?)
      //http://curl.haxx.se/libcurl/c/post-callback.html
      if (option_enable_progressive_upload && file_size > 0)
      {
        assert(de);
        if (de->is_segmented)
          assert(de_seg);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1); //1=upload
        debugf(DBG_LEVEL_EXT, "send_request_size: progressive PUT (%s)", orig_path);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, progressive_upload_callback);
        curl_easy_setopt(curl, CURLOPT_READDATA, (void*)de_seg);
      }
      else//not progressive
      {
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1); //1=upload
        if (fp)
        {
          curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, file_size);
          debugf(DBG_LEVEL_EXT, "send_request_size: standard PUT (%s)",
                 orig_path);
          curl_easy_setopt(curl, CURLOPT_READDATA, fp);//actually infoseg
          curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
          //don't add header parsing on PUT as etag is incorrect
          //better run a head (get_meta) right after PUT
        }
        else//no fp
        {
          debugf(DBG_LEVEL_EXT,
                 "send_request_size: 0 content PUT, for updating meta (%s)",
                 orig_path);
          curl_easy_setopt(curl, CURLOPT_INFILESIZE, 0);
        }
      }
      if (is_segment)
      {
        //fixme: progressive upload not working if file is segmented. conflict on read_callback?
        debugf(DBG_LEVEL_EXT, "send_request_size(%s): PUT is segmented, "
               KYEL "readcallback used", orig_path);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
      }
      //get the response for debug purposes.
      //fixme: carefull as conflicts with progressive download (GET)
      //send all data to this function
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc_callback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
    }
    else if (!strcasecmp(method, HTTP_GET))
    {
      curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
      //reset local cache md5sum on file retrieval
      //avoid reseting de on segments download
      if (de_seg)
        free_de_before_get(de_seg);
      else if (de)
        free_de_before_get(de);

      if (fp)//is_segment)
      {
        is_download = true;
        info = (struct segment_info*)fp;
        if (is_segment)
        {
          debugf(DBG_LEVEL_EXT,
                 "send_request_size: GET SEGMENT (%s) fp=%p part=%d proc=%lu",
                 orig_path, fp, info->part, info->size_processed);
          curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void*)info->de_seg);
        }
        else
        {
          debugf(DBG_LEVEL_EXT,
                 "send_request_size: GET FILE (%s) fp=%p proc=%lu de=%p",
                 orig_path, fp, info->size_processed, de);
          curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void*)info->de);
        }
        /**/
        //download via callback
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, info);
        //interrupt a slow download as this could make the next attempt faster
        //due to hubic throtling. but do this only for half of the tries
        if (option_min_speed_limit_progressive > 0
            && tries <= (REQUEST_RETRIES / 2))
        {
          curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT,
                           option_min_speed_limit_progressive);
          curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, option_min_speed_timeout);
        }
        //assume we need to append to an existing file
        if (info->size_processed > 0)
        {
          curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, info->size_processed);
          debugf(DBG_LEVEL_EXT, "send_request_size(%s): "
                 KYEL " resuming from %lu", orig_path, info->size_processed);
          //abort();
        }
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &header_get_meta_dispatch);
        //de is null on segments

        /**/
        /*
          //download directly to file
          curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
          curl_easy_setopt(curl, CURLOPT_WRITEDATA, info->fp);
        */
      }
      else if (xmlctx)
      {
        debugf(DBG_LEVEL_EXT, "send_request_size: GET XML (%s)", print_path);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, xmlctx);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &xml_dispatch);
      }
      else
      {
        //asumming retrieval of headers only
        if (de_seg)
          free_de_before_head(de_seg);
        else if (de)
          free_de_before_head(de);
        debugf(DBG_LEVEL_EXT, "send_request_size: GET HEADERS (%s)", print_path);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &header_get_meta_dispatch);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void*)de);
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
      }
    }
    else if (!strcasecmp(method, HTTP_DELETE))
    {
      debugf(DBG_LEVEL_EXT, "send_request_size: DELETE (%s)", print_path);
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }
    else
    {
      debugf(DBG_LEVEL_EXT, "send_request_size: catch_all (%s)");
      // HEAD request (e.g. for statfs)
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
      curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &header_dispatch);
    }
    //common code for all operations
    if (option_curl_progress_state)
    {
      //enable progress reporting
      //http://curl.haxx.se/libcurl/c/progressfunc.html
      struct curl_progress prog;
      prog.lastruntime = 0;
      prog.curl = curl;
      curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progress_callback);
      /* pass the struct pointer into the progress function */
      curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, &prog);
    }
    /* add the headers from extra_headers if any */
    curl_slist* extra;
    for (extra = extra_headers; extra; extra = extra->next)
    {
      debugf(DBG_LEVEL_EXT, "adding extra header: %s", extra->data);
      headers = curl_slist_append(headers, extra->data);
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    debugf(DBG_LEVEL_EXTALL, "status: send_request_size(%s) started HTTP(%s)",
           orig_path, url);
    curl_easy_perform(curl);

    char* effective_url;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_time);
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &size_downloaded);
    curl_easy_getinfo(curl, CURLINFO_SIZE_UPLOAD, &size_uploaded);
    debugf(DBG_LEVEL_EXTALL,
           "status: send_request_size(%s) completed HTTP REQ:%s total_time=%.1f seconds",
           orig_path, effective_url, total_time);
    curl_slist_free_all(headers);
    curl_easy_reset(curl);
    return_connection(curl);

    if (response != 404 && (response >= 400 || response < 200))
    {
      /*
         Now, our chunk.memory points to a memory block that is chunk.size
         bytes big and contains the remote file.
      */
      debugf(DBG_LEVEL_NORM, KRED
             "send_request_size: error, resp=%d , size=%lu, [HTTP %d] (%s)(%s)",
             response, (long)chunk.size, response, method, print_path);
      debugf(DBG_LEVEL_NORM, KRED"send_request_size: error message=[%s]",
             chunk.memory);
    }

    //detect if download was incomplete
    if (is_download && info
        && info->size_copy > 0  && info->size_copy != info->size_processed
        && (de && info->size_processed != de->size)
        && (de_seg && info->size_processed != de_seg->size))
    {
      debugf(DBG_LEVEL_NORM, KRED
             "send_request_size: download interrupted, expect size=%lu but got size=%.0f",
             info->size_copy, size_downloaded);
      response = 417;//too slow, signal error and hope for faster download
    }
    free(chunk.memory);

    if ((response >= 200 && response < 400) || (!strcasecmp(method, "DELETE")
        && response == 409))
    {
      debugf(DBG_LEVEL_NORM,
             "exit 0: send_request_size(%s) speed=%.1f sec res=%d dwn=%.0f upld=%.0f"
             KCYN "(%s) "KGRN"[HTTP OK]", print_path, total_time, response, size_downloaded,
             size_uploaded, method);
      break;
    }
    if (response == 401 && !cloudfs_connect())   // re-authenticate on 401s
    {
      debugf(DBG_LEVEL_NORM, KYEL"exit 1: send_request_size(%s) (%s) [HTTP REAUTH]",
             print_path, method);
      break;
    }
    //something went really wrong
    if (response == 0)
    {
      debugf(DBG_LEVEL_NORM, KRED
             "status: send_request_size(%s:%s) unexpected error, try fresh connect",
             print_path, method);
      //abort();
    }
    //handle cases when file is not found, no point in retrying, should exit
    if (response == 404)
    {
      debugf(DBG_LEVEL_NORM,
             "send_request_size: not found error for (%s)(%s), ignored "KYEL"[HTTP 404]",
             method, print_path);
      break;
    }
    else
    {
      debugf(DBG_LEVEL_NORM,
             "send_request_size: httpcode=%d (%s)(%s), retrying "KRED"[HTTP ERR]", response,
             method, print_path);
      if (response != 417)//skip pause on slow speed errors
        //sleep(8 << tries); // backoff
        sleep(1);
    }

    if (xmlctx)
      xmlCtxtResetPush(xmlctx, NULL, 0, NULL, NULL);
  }//end for
  if (encoded_path == NULL)
    curl_free(orig_path);
  debugf(DBG_LEVEL_NORM, "exit 2: send_request_size(%s)" KCYN
         "(%s) response=%d total_time=%.1f seconds",
         print_path, method, response, total_time);

  return response;
}

//if path is encoded cache entry will not be found, need to pass unencoded as well
//todo: implement use of dir_entry instead of path for performance reasons.
int send_request(char* method, const char* path, FILE* fp,
                 xmlParserCtxtPtr xmlctx, curl_slist* extra_headers,
                 dir_entry* de, dir_entry* de_seg)
{
  off_t flen = 0;
  if (fp)
  {
    // if we don't flush the size will probably be zero
    fflush(fp);
    flen = cloudfs_file_size(fileno(fp));
  }
  return send_request_size(method, path, fp, xmlctx, extra_headers, flen, 0, de,
                           de_seg);
}

//thread that downloads or uploads large file segments
void* upload_segment(void* seginfo)
{
  struct segment_info* info = (struct segment_info*)seginfo;
  debugf(DBG_LEVEL_EXT,
         "upload_segment: started segment part=%d seginfo=%p",
         info->part, seginfo);
  //debugf(DBG_LEVEL_NORM,
  //       KMAG"upload_segment: started segment part=%d seginfo=%p fp=%p prog=%d",
  //       info->part, seginfo, info->fp, info->is_progressive);
  char seg_path[MAX_URL_SIZE] = { 0 };
  //set pointer to the segment start index in the complete
  //large file (several threads will write/read to/from same large file)
  fseek(info->fp, info->part * info->segment_size, SEEK_SET);
  //debugf(DBG_LEVEL_NORM,
  //      KMAG"upload_segment: step 1 part=%d seginfo=%p fp=%p prog=%d",
  //       info->part, seginfo, info->fp, info->is_progressive);
  setvbuf(info->fp, NULL, _IOFBF, DISK_BUFF_SIZE);
  //debugf(DBG_LEVEL_NORM,
  //       KMAG"upload_segment: step 2 part=%d seginfo=%p fp=%p prog=%d",
  //       info->part, seginfo, info->fp, info->is_progressive);
  snprintf(seg_path, MAX_URL_SIZE, "%s%08i", info->seg_base, info->part);
  /*debugf(DBG_LEVEL_NORM,
         KMAG"upload_segment: step 3 part=%d seginfo=%p fp=%p prog=%d",
         info->part, seginfo, info->fp, info->is_progressive);
  */
  char* encoded = curl_escape(seg_path, 0);
  debugf(DBG_LEVEL_EXT, KCYN "upload_segment(%s) part=%d size=%d seg_size=%d %s",
         info->method, info->part, info->size_copy, info->segment_size, seg_path);
  int response = send_request_size(info->method, encoded, info, NULL, NULL,
                                   info->size_copy, 1, info->de, NULL);
  if (!(response >= 200 && response < 300))
    fprintf(stderr, "Segment %s failed with response %d", seg_path,
            response);
  curl_free(encoded);
  debugf(DBG_LEVEL_NORM,
         KMAG"upload_segment: completed, part=%d, http response=%d, progressive=%d",
         info->part, response, info->de->is_progressive);
  fclose(info->fp);
  // exit only when this is a child thread started on a segmented file
  if (!info->de->is_single_thread)
  {
    debugf(DBG_LEVEL_NORM,
           KMAG"upload_segment: closing thread part=%d, http response=%d, progressive=%d",
           info->part, response, info->de->is_progressive);
    pthread_exit(NULL);
  }
}

//thread that downloads or uploads large file segments
void* upload_segment_progressive(void* seginfo)
{
  struct segment_info* info = (struct segment_info*)seginfo;
  debugf(DBG_LEVEL_EXT,
         "upload_segment_progressive: started segment part=%d size=%lu",
         info->part, info->size_copy);
  fseek(info->fp, 0, SEEK_SET);
  setvbuf(info->fp, NULL, _IOFBF, DISK_BUFF_SIZE);
  char* encoded = curl_escape(info->seg_base, 0);
  debugf(DBG_LEVEL_EXT, KCYN
         "upload_segment_progressive(%s) part=%d size=%d seg_size=%d %s",
         info->method, info->part, info->size_copy, info->segment_size, info->seg_base);
  int response = send_request_size(info->method, encoded, info, NULL, NULL,
                                   info->size_copy, 1, info->de, info->de_seg);//, info->seg_base);
  if (!(response >= 200 && response < 300))
    debugf(DBG_LEVEL_NORM, KRED
           "upload_segment_progressive: Segment %s failed with response %d",
           info->seg_base,
           response);
  curl_free(encoded);
  debugf(DBG_LEVEL_NORM,
         KMAG"upload_segment_progressive: done, part=%d, http response=%d",
         info->part, response);
  fflush(info->fp);
  // exit only when this is a child thread started on a segmented file
  if (!info->de->is_single_thread)//fixme: not thread safe
  {
    debugf(DBG_LEVEL_NORM,
           KMAG"upload_segment_progressive: closing thread part=%d, http response=%d",
           info->part, response);
    pthread_exit(NULL);
  }
}

/*
  segment_size is the globabl config variable and size_of_segment is local
  TODO: return whether the upload/download failed or not
  Changed function to support progressive operations, where multiple threads are not
  desired as on download you want to get first segment faster than the rest
*/
void run_segment_threads_progressive(const char* method, char* seg_base,
                                     thread_job* job)
{
  debugf(DBG_LEVEL_NORM,
         "run_segment_threads_progressive(%s) part=%d size=%d",
         method, job->segment_part, job->de->segment_size);
  int ret;
  bool multi_thread = false;
  FILE* seg_file;
  struct segment_info info;
  info.method = method;
  info.part = job->segment_part;
  info.segment_size = job->de->segment_size;
  info.size_left = job->segment_part < job->de->segment_full_count ?
                   job->de->segment_size :
                   job->de->segment_remaining;
  //need a copy for resume as info.size will be changed during download
  info.size_copy = info.size_left;
  info.size_processed = 0;
  info.seg_base = seg_base;
  //info.de = job->de_seg;
  info.de = job->de;
  info.de_seg = job->de_seg;
  if (job->de->is_segmented)
  {
    info.fp = job->de_seg->downld_buf.local_cache_file;
    if (info.fp == NULL)
    {
      debugf(DBG_LEVEL_NORM, KRED
             "run_segment_threads_progressive: can't open segment cache file");
    }
  }
  info.de->is_progressive = false;
  info.de->is_single_thread = true;
  debugf(DBG_LEVEL_NORM,
         "run_segment_threads_progressive: progressive, single thread part=%d/%d, info=%p",
         job->segment_part, job->de->segment_count, info);
  upload_segment_progressive((void*) & (info));

  debugf(DBG_LEVEL_EXT, "exit: run_segment_threads_progressive(%s)", method);
}

/*
  segment_size is the globabl config variable and size_of_segment is local
  TODO: return whether the upload/download failed or not
  Changed function to support progressive operations, where multiple threads are not
  desired as on download you want to get first segment faster than the rest
*/
void run_segment_threads(const char* method, int segments, int full_segments,
                         int remaining,
                         FILE* fp, char* seg_base, int size_of_segments, dir_entry* de)
{
  debugf(DBG_LEVEL_NORM, "run_segment_threads(%s) segments=%d fp=%p", method,
         segments, fp);
  char file_path[PATH_MAX] = { 0 };
  struct segment_info* info = (struct segment_info*)
                              malloc(segments * sizeof(struct segment_info));
  pthread_t* threads = (pthread_t*)malloc(segments * sizeof(pthread_t));

  //debug_print_file_name(fp);
#ifdef __linux__
  snprintf(file_path, PATH_MAX, "/proc/self/fd/%d", fileno(fp));
  debugf(DBG_LEVEL_NORM, KMAG"run_segment_threads: filepath=%s", file_path);
#else
  //TODO: I haven't actually tested this
  if (fcntl(fileno(fp), F_GETPATH, file_path) == -1)
    fprintf(stderr, "couldn't get the path name\n");
  debugf(DBG_LEVEL_NORM, KMAG"run_segment_threads: ALT filepath=%s", file_path);
#endif
  //sleep_ms(2000);
  int i, ret;
  bool multi_thread = false;

  for (i = 0; i < segments; i++)
  {
    info[i].method = method;
    info[i].fp = fopen(file_path, method[0] == 'G' ? "r+" : "r");
    debugf(DBG_LEVEL_NORM, KMAG"run_segment_threads() part=%d fp=%p", i,
           info[i].fp);
    info[i].part = i;
    info[i].segment_size = size_of_segments;
    info[i].size_left = i < full_segments ? size_of_segments : remaining;
    info[i].seg_base = seg_base;
    info[i].de = de;
    if (full_segments > MAX_SEGMENT_THREADS)
    {
      info[i].de->is_single_thread = true;
      info[i].de->is_progressive = false;
      debugf(DBG_LEVEL_NORM, KMAG
             "run_segment_threads: single thread part=%d/%d, info=%p",
             i, segments, info);
      upload_segment((void*) & (info[i]));
    }
    else
    {
      debugf(DBG_LEVEL_NORM, KMAG
             "run_segment_threads: going multi-threaded part=%d",
             i);
      info[i].de->is_progressive = false;
      info[i].de->is_single_thread = false;
      pthread_create(&threads[i], NULL, upload_segment, (void*) & (info[i]));
      multi_thread = true;
    }
  }
  if (multi_thread)
    for (i = 0; i < segments; i++)
    {
      if ((ret = pthread_join(threads[i], NULL)) != 0)
        fprintf(stderr, "error waiting for thread %d, status = %d\n", i, ret);
    }
  free(info);
  free(threads);
  debugf(DBG_LEVEL_EXT, "exit: run_segment_threads(%s)", method);
}



//checks on the cloud if this file (seg_path) have an associated segment folder
int internal_is_segmented(const char* seg_path, const char* object,
                          const char* parent_path)
{
  debugf(DBG_LEVEL_EXT, "internal_is_segmented seg_path(%s) object(%s)",
         seg_path, object);
  //try to avoid one additional http request for small files
  bool potentially_segmented;
  dir_entry* de = check_path_info(parent_path);
  if (!de)
  {
    //when files in folders are first loaded the path will not be yet in cache, so need
    //to force segment meta download for segmented files
    debugf(DBG_LEVEL_EXTALL,
           "internal_is_segmented: potentially YES as (%s) not in cache", parent_path);
    potentially_segmented = true;
  }
  else
  {
    //potentially segmented, assumption is that 0 size files are potentially segmented
    //while size>0 is for sure not segmented, so no point in making an expensive HTTP GET call
    //UPDATE: above assumption is invalid for segmented files composed of small segments (e.g. 10 MB)
    if (de->is_segmented)
      potentially_segmented = true;//force for files we know we're uploaded segmented
    else
      potentially_segmented = (de->size_on_cloud == 0 && !de->isdir) ? true : false;
    debugf(DBG_LEVEL_EXTALL,
           "internal_is_segmented: size_cloud=%lu isdir=%d for (%s)", de->size_on_cloud,
           de->isdir, parent_path);
  }
  debugf(DBG_LEVEL_EXT, "internal_is_segmented: potentially segmented=%d",
         potentially_segmented);
  dir_entry* seg_dir;
  if (potentially_segmented && cloudfs_list_directory(seg_path, &seg_dir))
  {
    if (seg_dir && seg_dir->isdir)
    {
      do
      {
        if (!strncmp(seg_dir->name, object, MAX_URL_SIZE))
        {
          debugf(DBG_LEVEL_EXT, "exit 0: internal_is_segmented(%s) "KGRN"TRUE",
                 seg_path);
          return 1;
        }
      }
      while ((seg_dir = seg_dir->next));
    }
  }
  debugf(DBG_LEVEL_EXT, "exit 1: internal_is_segmented(%s) "KYEL"FALSE",
         seg_path);
  return 0;
}

int is_segmented(const char* path)
{
  debugf(DBG_LEVEL_EXT, "is_segmented(%s)", path);
  char container[MAX_URL_SIZE] = "";
  char object[MAX_URL_SIZE] = "";
  char seg_base[MAX_URL_SIZE] = "";
  split_path(path, seg_base, container, object);
  char seg_path[MAX_URL_SIZE];
  snprintf(seg_path, MAX_URL_SIZE, "%s/%s_segments", seg_base, container);
  return internal_is_segmented(seg_path, object, path);
}

/*returns segmented file properties by parsing and retrieving the folder structure on the cloud
  added totalsize as parameter to return the file size on list directory for segmented files
  old implementation returns file size=0 (issue #91)
  populates parent file with link to segment list
*/
int format_segments(const char* path, char* seg_base,  long* segments,
                    long* full_segments, long* remaining, long* size_of_segments,
                    long* total_size)
{
  debugf(DBG_LEVEL_EXT, "format_segments(%s) seg_base(%s)", path, seg_base);
  char container[MAX_URL_SIZE] = "";
  char object[MAX_URL_SIZE] = "";
  split_path(path, seg_base, container, object);
  char seg_path[MAX_URL_SIZE];
  snprintf(seg_path, MAX_URL_SIZE, "%s/%s_segments", seg_base, container);
  if (internal_is_segmented(seg_path, object, path))
  {
    //operations with segments
    //http://docs.openstack.org/developer/swift/overview_large_objects.html
    char manifest[MAX_URL_SIZE];
    //fixme: memory is not freed for seg_dir after cloudfs_list_directory()
    dir_entry* seg_dir;
    snprintf(manifest, MAX_URL_SIZE, "%s/%s", seg_path, object);
    debugf(DBG_LEVEL_EXTALL, "format_segments manifest(%s)", manifest);
    if (!cloudfs_list_directory(manifest, &seg_dir))
    {
      debugf(DBG_LEVEL_EXTALL, "exit 0: format_segments(%s)", path);
      return 0;
    }
    // snprintf seesaw between manifest and seg_path to get
    // the total_size and the segment size as well as the actual objects
    char* timestamp = seg_dir->name;
    snprintf(seg_path, MAX_URL_SIZE, "%s/%s", manifest, timestamp);
    debugf(DBG_LEVEL_EXTALL, "format_segments seg_path(%s)", seg_path);

    //fix as sometimes seg_dir is null
    if (!cloudfs_list_directory(seg_path, &seg_dir) || !seg_dir)
    {
      debugf(DBG_LEVEL_EXT, "exit 1: format_segments(%s)", path);
      return 0;
    }
    char* str_size = seg_dir->name;
    snprintf(manifest, MAX_URL_SIZE, "%s/%s", seg_path, str_size);
    debugf(DBG_LEVEL_EXTALL, KMAG"format_segments manifest2(%s) size=%s", manifest,
           str_size);
    if (!cloudfs_list_directory(manifest, &seg_dir))
    {
      debugf(DBG_LEVEL_EXTALL, "exit 2: format_segments(%s)", path);
      return 0;
    }
    //following folder name actually represents the parent file size
    char* str_segment = seg_dir->name;
    snprintf(seg_path, MAX_URL_SIZE, "%s/%s", manifest, str_segment);
    debugf(DBG_LEVEL_EXTALL, KMAG"format_segments seg_path2(%s)", seg_path);
    //here is where we get a list with all segment files composing the parent large file
    if (!cloudfs_list_directory(seg_path, &seg_dir))
    {
      debugf(DBG_LEVEL_EXTALL, "exit 3: format_segments(%s)", path);
      return 0;
    }
    else
    {
      //most reliable way to get true size is to add all segments
      //as size from folder name is 0 on progressive uploads
      *total_size = 0;
      dir_entry* tmp_de;
      tmp_de = seg_dir;
      while (tmp_de)
      {
        *total_size += tmp_de->size;
        tmp_de = tmp_de->next;
      }
    }

    //*total_size = strtoll(str_size, NULL, 10);
    *size_of_segments = strtoll(str_segment, NULL, 10);
    *remaining = *total_size % *size_of_segments;
    *full_segments = *total_size / *size_of_segments;
    *segments = *full_segments + (*remaining > 0);
    snprintf(manifest, MAX_URL_SIZE, "%s_segments/%s/%s/%s/%s/",
             container, object, timestamp, str_size, str_segment);

    //save segments dir list into parent file entry
    //fixme: potentially unsafe as it get's overwritten and data is lost
    dir_entry* de = check_path_info(path);
    if (de)
    {
      if (!de->segments)
      {
        debugf(DBG_LEVEL_EXT, KMAG
               "format_segments: adding segment list to (%s)", de->full_name);
        de->segments = seg_dir;
        de->segment_count = *segments;
      }
      else
      {
        //todo: free if is old and no op is active for this segment
        dir_entry* new_seg = seg_dir;
        dir_entry* old_seg;
        int de_segment;
        debugf(DBG_LEVEL_EXT, KMAG
               "format_segments: checking seglist changes (%s)", de->full_name);
        while (new_seg)
        {
          de_segment = atoi(new_seg->name);
          old_seg = get_segment(de, de_segment);
          //check if segments are identical
          //if not replace de with new segment list
          if (!old_seg || !old_seg->md5sum
              || strcasecmp(old_seg->md5sum, new_seg->md5sum))
          {
            debugf(DBG_LEVEL_EXT, KMAG
                   "format_segments: modifing segment list for (%s)", de->full_name);
            //todo: what if there are download segmented ops in progress?
            cloudfs_free_dir_list(de->segments);
            de->segments = seg_dir;
            de->segment_count = *segments;
            break;
          }
          new_seg = new_seg->next;
        }
      }
    }
    else
      debugf(DBG_LEVEL_EXT, KYEL "format_segments: de(%s) not found!", path);

    char tmp[MAX_URL_SIZE];
    strncpy(tmp, seg_base, MAX_URL_SIZE);
    snprintf(seg_base, MAX_URL_SIZE, "%s/%s", tmp, manifest);
    debugf(DBG_LEVEL_EXT, KMAG"format_segments: seg_base=(%s)", seg_base);
    debugf(DBG_LEVEL_EXT,
           "exit 4: format_segments(%s) total=%d size_of_segments=%d remaining=%d, full_segments=%d segments=%d",
           path, *total_size, *size_of_segments, *remaining, *full_segments, *segments);
    return 1;
  }
  else
  {
    debugf(DBG_LEVEL_EXT, "exit 5: format_segments(%s) not segmented?", path);
    return 0;
  }
}


/*
  updates file entry with segment list & details
*/
int update_segments(dir_entry* de)
{
  assert(de);
  if (!de->manifest_cloud)
  {
    de->is_segmented = false;
    debugf(DBG_LEVEL_EXT, "update_segments(%s): not segmented ", de->name);
    return false;
  }

  debugf(DBG_LEVEL_EXT, "update_segments(%s): man=%s ", de->name,
         de->manifest_cloud);
  dir_entry* seg_dir;
  char* manifest_path;
  //get a list with all segment files composing the parent large file
  //dig through subfolders until a file is found
  manifest_path = de->manifest_cloud;
  do
  {
    if (!cloudfs_list_directory(manifest_path, &seg_dir))
    {
      abort();
      debugf(DBG_LEVEL_EXTALL, "exit 1: update_segments(%s)", de->name);
      return 0;
    }
    if (seg_dir)
      manifest_path = seg_dir->full_name;
  }
  while (seg_dir && seg_dir->isdir);

  if (seg_dir && !seg_dir->isdir)
  {
    //most reliable way to get true size is to add all segments
    long total_size = 0;
    dir_entry* tmp_de = seg_dir;
    while (tmp_de)
    {
      assert(!tmp_de->isdir);//folders are not welcomed
      total_size += tmp_de->size;
      tmp_de = tmp_de->next;
    }
    de->size = total_size;
    if (de->segment_size == 0)
      de->segment_size = seg_dir->size;
    else
      assert(de->segment_size == seg_dir->size || de->size == seg_dir->size);
  }
  de->is_segmented = true;
  if (de->segment_size == 0)
    de->segment_size = segment_size;
  de->segment_remaining = de->size % de->segment_size;
  de->segment_full_count = de->size / de->segment_size;
  de->segment_count = de->segment_full_count + (de->segment_remaining > 0);
  //save segments dir list into parent file entry
  //fixme: potentially unsafe as it get's overwritten and data is lost
  if (de)
  {
    if (!de->segments)
    {
      debugf(DBG_LEVEL_EXT, KMAG
             "format_segments: adding segment list to (%s)", de->full_name);
      de->segments = seg_dir;
    }
    else
    {
      //todo: free if is old and no op is active for this segment
      dir_entry* new_seg = seg_dir;
      dir_entry* old_seg;
      int de_segment;
      debugf(DBG_LEVEL_EXT, KMAG
             "format_segments: checking seglist changes (%s)", de->full_name);
      while (new_seg)
      {
        de_segment = atoi(new_seg->name);
        old_seg = get_segment(de, de_segment);
        //check if segments are identical
        //if not replace de with new segment list
        if (!old_seg || !old_seg->md5sum
            || strcasecmp(old_seg->md5sum, new_seg->md5sum))
        {
          debugf(DBG_LEVEL_EXT, KMAG
                 "format_segments: modifing segment list for (%s)", de->full_name);
          //todo: what if there are download segmented ops in progress?
          cloudfs_free_dir_list(de->segments);
          de->segments = seg_dir;
          break;
        }
        new_seg = new_seg->next;
      }
    }
  }
}

/*
   Public interface
*/

void cloudfs_init()
{
  LIBXML_TEST_VERSION
  xmlXPathInit();
  curl_global_init(CURL_GLOBAL_ALL);
  pthread_mutex_init(&pool_mut, NULL);
  curl_version_info_data* cvid = curl_version_info(CURLVERSION_NOW);
  debugf(DBG_LEVEL_NORM, KYEL "CURL version=%s ssl=%s",
         cvid->version, cvid->ssl_version);
  // CentOS/RHEL 5 get stupid mode, because they have a broken libcurl
  if (cvid->version_num == RHEL5_LIBCURL_VERSION)
  {
    debugf(DBG_LEVEL_NORM, "RHEL5 mode enabled.");
    rhel5_mode = 1;
  }
  if (!strncasecmp(cvid->ssl_version, "openssl", 7))
  {
#ifdef HAVE_OPENSSL
    int i;
    ssl_lockarray = (pthread_mutex_t*)OPENSSL_malloc(CRYPTO_num_locks() *
                    sizeof(pthread_mutex_t));
    for (i = 0; i < CRYPTO_num_locks(); i++)
      pthread_mutex_init(&(ssl_lockarray[i]), NULL);
    CRYPTO_set_id_callback((unsigned long (*)())thread_id);
    CRYPTO_set_locking_callback((void (*)())lock_callback);
#endif
  }
  else if (!strncasecmp(cvid->ssl_version, "nss", 3))
  {
    // allow https to continue working after forking (for RHEL/CentOS 6)
    setenv("NSS_STRICT_NOFORK", "DISABLED", 1);
  }
}

void cloudfs_free()
{
  debugf(DBG_LEVEL_EXT, "Destroy mutex");
  pthread_mutex_destroy(&pool_mut);
  int n;
  for (n = 0; n < curl_pool_count; ++n)
  {
    debugf(DBG_LEVEL_EXT, "Cleaning curl conn %d", n);
    curl_easy_cleanup(curl_pool[n]);
  }
}


int file_is_readable(const char* fname)
{
  FILE* file;
  if ( file = fopen( fname, "r" ) )
  {
    fclose( file );
    return 1;
  }
  return 0;
}

const char* get_file_mimetype ( const char* path )
{
  if ( file_is_readable( path ) == 1 )
  {
    magic_t magic;
    const char* mime;
    magic = magic_open( MAGIC_MIME_TYPE );
    magic_load( magic, NULL );
    magic_compile( magic, NULL );
    mime = magic_file( magic, path );
    magic_close( magic );
    return mime;
  }
  const char* error = "application/octet-stream";
  return error;
}

/*
  progressive segment upload to cloud
*/
void internal_upload_segment_progressive(void* arg)
{
  struct thread_job* job = arg;
  debugf(DBG_LEVEL_EXT, "internal_upload_segment_progressive(%s): seg=%s",
         job->de->name, job->de_seg->full_name);
  //increment before upload finishes to avoid cfs_flush to miss last segment
  job->de->segment_count++;
  //mark file size = 1 to signal we have some data coming in
  int response;
  int i;
  bool op_ok = false;
  for (i = 0; i < 2; i++)
  {
    response = send_request_size(HTTP_PUT, NULL, NULL, NULL, NULL,
                                 1, 0, job->de, job->de_seg);
    op_ok = valid_http_response(response);
    //if error, decrement
    if (!op_ok)
    {
      if (job->de_seg->segment_part == 0 && response == 404)
      {
        //first: try to create parent segment storage
        if (i == 0)
          cloudfs_create_directory(HUBIC_SEGMENT_STORAGE_ROOT);
        else abort();
      }
      else
      {
        job->de->segment_count--;
        break;
      }
    }
    else
    {
      //last uploaded segment is freed in cfs_flush
      if (job->de_seg->upload_buf.sem_list[SEM_EMPTY])
        free_semaphores(&job->de_seg->upload_buf, SEM_EMPTY);
      if (job->de_seg->upload_buf.sem_list[SEM_FULL])
        free_semaphores(&job->de_seg->upload_buf, SEM_FULL);
      //don't free DONE semaphore on last segment upload, is freed in cfsflush
      if (job->de_seg->segment_part != job->de->segment_count - 1)
        if (job->de_seg->upload_buf.sem_list[SEM_DONE])
          free_semaphores(&job->de_seg->upload_buf, SEM_DONE);
      break;
    }
  }

  if (!op_ok)
  {
    debugf(DBG_LEVEL_NORM, KRED
           "internal_upload_segment_progressive(%s:%s): upload failed",
           job->de->name, job->de_seg->full_name);
    //todo: signal error to running threads
    abort();
  }

  //if this is the last segment, upload the zero size parent file
  if (job->de_seg->segment_part == job->de->segment_count - 1)
  {
    debugf(DBG_LEVEL_EXT,
           "internal_upload_segment_prog(%s): creating 0 size parent, de_seg=%s",
           job->de->name, job->de_seg->full_name);
    curl_slist* headers = NULL;
    const char* filemimetype = get_file_mimetype(job->de->full_name);
    add_header(&headers, HEADER_TEXT_MANIFEST, job->de->manifest_time);
    add_header(&headers, "Content-Length", "0");
    add_header(&headers, "Content-Type", filemimetype);
    response = send_request_size(HTTP_PUT, NULL, NULL, NULL, headers,
                                 0, 0, job->de, NULL);
    curl_slist_free_all(headers);

    //detect manifest location where to search and delete previous versions
    char* paths_list[2];
    //for existing files
    paths_list[0] = job->de->manifest_cloud;

    //this is for new files without a previous manifest
    char manifest_root[MAX_URL_SIZE];
    snprintf(manifest_root, MAX_URL_SIZE, "/%s/%s",
             job->de->manifest_seg, job->de->name);
    paths_list[1] = manifest_root;
    int i;

    dir_entry* de_versions, *de_tmp;
    for (i = 0; i < 2; i++)
    {
      debugf(DBG_LEVEL_EXT, "internal_upload_segment_prog: try del vers. in %s",
             paths_list[i]);
      if (paths_list[i])
        if (cloudfs_list_directory(paths_list[i], &de_versions))
        {
          assert(job->de->segments);
          //delete folder if first segment in cloud
          //does not match new first uploaded segment
          //+1 due to slash prefix
          if (de_versions)
            if (!strstr(job->de->segments->full_name, de_versions->full_name + 1))
            {
              dir_entry* tmp = init_dir_entry();
              tmp->manifest_cloud = strdup(paths_list[i]);
              tmp->full_name = strdup(paths_list[i]);
              tmp->name = "";
              tmp->isdir = 1;
              cloudfs_delete_object(tmp);
              free(tmp);
            }
            else
              debugf(DBG_LEVEL_EXT, KMAG
                     "not deleting just uploaded version %s",
                     de_versions->full_name);
        }
        else
          abort();
    }

    //mark file meta as obsolete to force a reload (for md5sums mostly)
    job->de->metadata_downloaded = false;
    get_file_metadata(job->de);

    //signal cfs_flush we're done uploading main file
    if (job->de_seg->upload_buf.sem_list[SEM_DONE])
      sem_post(job->de_seg->upload_buf.sem_list[SEM_DONE]);
  }
  debugf(DBG_LEVEL_EXT, "exit: internal_upload_segment_progressive(%s)",
         (job->de ? job->de->name : "nil"));
  free(job->self_reference);
  pthread_exit(NULL);
}


/*
  uploads a new segment folder in cloud
  NOTE!: it alters de_seg fields
*/
bool cloudfs_create_segment(dir_entry* de_seg, dir_entry* de)
{
  assert(de_seg);
  assert(de);
  debugf(DBG_LEVEL_EXT, "cloudfs_create_segment(%s:%s)", de_seg->name, de->name);
  struct timespec now;
  int response;
  //segmenting file for upload, mark as segmented

  int i;
  long remaining = de->size % segment_size;
  int full_segments = de->size / segment_size;
  int segments = full_segments + (remaining > 0);
  if (de_seg->segment_part == 0)
  {
    de->segment_count = 0;
    de->is_segmented = true;
    de->segment_size = segment_size;
    de->segment_remaining = segment_size;
  }
  //launch async upload for this segment
  init_semaphores(&de_seg->upload_buf, de_seg, "upload");
  struct thread_job* job = malloc(sizeof(struct thread_job));
  job->de = de;
  job->de_seg = de_seg;
  job->self_reference = job;//freed in internal_upload_segment_progressive
  pthread_create(&job->thread, NULL,
                 (void*)internal_upload_segment_progressive, job);
  debugf(DBG_LEVEL_EXT,
         "exit 0: cloudfs_create_segment(%s:%s) upload started ok",
         de_seg->full_name, de_seg->name);
  return true;
}

/*
  upload segment to cloud
*/
int cloudfs_upload_segment(dir_entry* de_seg, dir_entry* de)
{
  assert(de_seg);
  assert(de);
  debugf(DBG_LEVEL_EXT, "cloudfs_upload_segment(%s:%s)", de_seg->name, de->name);
  const char* filemimetype = get_file_mimetype(de->full_name);
  // delete the previously uploaded segments
  if (is_segmented(de->full_name))
  {
    if (!cloudfs_delete_object(de))
      debugf(DBG_LEVEL_NORM, KRED
             "cloudfs_upload_segment: couldn't delete existing file");
    else
      debugf(DBG_LEVEL_EXT, KYEL"cloudfs_upload_segment: deleted existing file");
  }
  FILE* fp = NULL;
  open_file_in_cache(de, &fp, HTTP_PUT);
  struct timespec now;
  int response;
  //check if file is qualified to be segmented
  if (de->size >= segment_above)
  {
    //segmenting file for upload, mark as segmented
    de->is_segmented = true;
    int i;
    long remaining = de->size % segment_size;
    int full_segments = de->size / segment_size;
    int segments = full_segments + (remaining > 0);
    // The best we can do here is to get the current time that way tools that
    // use the mtime can at least check if the file was changing after now
    clock_gettime(CLOCK_REALTIME, &now);
    char string_float[TIME_CHARS];
    snprintf(string_float, TIME_CHARS, "%lu.%lu", now.tv_sec, now.tv_nsec);
    char meta_mtime[TIME_CHARS];
    snprintf(meta_mtime, TIME_CHARS, "%f", atof(string_float));
    char seg_base[MAX_URL_SIZE] = "";
    char container[MAX_URL_SIZE] = "";
    char object[MAX_URL_SIZE] = "";
    split_path(de->full_name, seg_base, container, object);
    char manifest[MAX_URL_SIZE];
    snprintf(manifest, MAX_URL_SIZE, "%s_segments", container);
    // create the segments container
    cloudfs_create_directory(manifest);
    // reusing manifest
    // TODO: check how addition of meta_mtime in manifest impacts utimens implementation
    snprintf(manifest, MAX_URL_SIZE, "%s_segments/%s/%s/%ld/%ld/",
             container, object, meta_mtime, de->size, segment_size);
    char tmp[MAX_URL_SIZE];
    strncpy(tmp, seg_base, MAX_URL_SIZE);
    snprintf(seg_base, MAX_URL_SIZE, "%s/%s", tmp, manifest);
    //uploading all segments in separate threads
    run_segment_threads(HTTP_PUT, segments, full_segments, remaining, fp,
                        seg_base, segment_size, de);
    curl_slist* headers = NULL;
    add_header(&headers, "x-object-manifest", manifest);
    //due to utimens changes, not needed anymore
    add_header(&headers, "Content-Length", "0");
    add_header(&headers, "Content-Type", filemimetype);
    //if path is encoded cache entry will not be found
    //complete upload (write parent file, 0 size?)
    response = send_request_size("PUT", NULL, NULL, NULL, headers, 0, 0,
                                 de, de_seg);
    curl_slist_free_all(headers);
    debugf(DBG_LEVEL_EXT,
           "exit 0: cloudfs_upload_segment(%s) uploaded ok, response=%d",
           de->name, response);
  }
  else
  {
    // file is not segmented, upload just one file
    debugf(DBG_LEVEL_EXT, "cloudfs_upload_segment(%s) non-segmented up", de->name);

    struct segment_info info;
    info.method = HTTP_PUT;
    info.part = -1;
    info.segment_size = -1;
    info.size_left = de->size;
    info.size_copy = de->size;
    info.fp = fp;
    info.de = de;
    info.de_seg = de_seg;
    info.size_processed = 0;
    info.de->is_progressive = false;
    info.de->is_single_thread = true;
    assert(fseek(info.fp, 0, SEEK_SET) == 0);
    char* encoded = curl_escape(de->full_name, 0);
    response = send_request_size(HTTP_PUT, encoded, &info, NULL, NULL,
                                 info.size_copy, 0, de, de_seg);
    update_direntry_md5sum(de->md5sum_local, fp);
    get_file_metadata(de);
    curl_free(encoded);
    debugf(DBG_LEVEL_EXT, "exit 1: cloudfs_upload_segment(%s)", de->name);
  }
  fclose(fp);
  return (response >= 200 && response < 300);
}


/*
  uploads file to cloud
*/
int cloudfs_object_read_fp(dir_entry* de, FILE* fp)
{
  debugf(DBG_LEVEL_EXT, "cloudfs_object_read_fp(%s)", de->name);
  long flen;
  fflush(fp);
  const char* filemimetype = get_file_mimetype(de->full_name);
  // determine the size of the file and segment if it is above the threshhold
  fseek(fp, 0, SEEK_END);
  flen = ftell(fp);
  // delete the previously uploaded segments
  if (is_segmented(de->full_name))
  {
    if (!cloudfs_delete_object(de))
      debugf(DBG_LEVEL_NORM,
             KRED"cloudfs_object_read_fp: couldn't delete existing file");
    else
      debugf(DBG_LEVEL_EXT, KYEL"cloudfs_object_read_fp: deleted existing file");
  }
  struct timespec now;
  //check if file is qualified to be segmented
  if (flen >= segment_above)
  {
    //segmenting file for upload, mark as segmented
    if (de)
      de->is_segmented = true;
    int i;
    long remaining = flen % segment_size;
    int full_segments = flen / segment_size;
    int segments = full_segments + (remaining > 0);
    // The best we can do here is to get the current time that way tools that
    // use the mtime can at least check if the file was changing after now
    clock_gettime(CLOCK_REALTIME, &now);
    char string_float[TIME_CHARS];
    snprintf(string_float, TIME_CHARS, "%lu.%lu", now.tv_sec, now.tv_nsec);
    char meta_mtime[TIME_CHARS];
    snprintf(meta_mtime, TIME_CHARS, "%f", atof(string_float));
    char seg_base[MAX_URL_SIZE] = "";
    char container[MAX_URL_SIZE] = "";
    char object[MAX_URL_SIZE] = "";
    split_path(de->full_name, seg_base, container, object);
    char manifest[MAX_URL_SIZE];
    snprintf(manifest, MAX_URL_SIZE, "%s_segments", container);
    // create the segments container
    cloudfs_create_directory(manifest);
    // reusing manifest
    // TODO: check how addition of meta_mtime in manifest impacts utimens implementation
    snprintf(manifest, MAX_URL_SIZE, "%s_segments/%s/%s/%ld/%ld/",
             container, object, meta_mtime, flen, segment_size);
    char tmp[MAX_URL_SIZE];
    strncpy(tmp, seg_base, MAX_URL_SIZE);
    snprintf(seg_base, MAX_URL_SIZE, "%s/%s", tmp, manifest);
    //uploading all segments in separate threads
    run_segment_threads("PUT", segments, full_segments, remaining, fp,
                        seg_base, segment_size, de);
    char* encoded = curl_escape(de->full_name, 0);
    curl_slist* headers = NULL;
    add_header(&headers, "x-object-manifest", manifest);
    //due to utimens changes, not needed anymore
    //add_header(&headers, "x-object-meta-mtime", meta_mtime);
    add_header(&headers, "Content-Length", "0");
    add_header(&headers, "Content-Type", filemimetype);
    //if path is encoded cache entry will not be found
    //complete upload (write parent file, 0 size?)
    int response = send_request_size(HTTP_PUT, encoded, NULL, NULL, headers, 0, 0,
                                     de, NULL);
    curl_slist_free_all(headers);
    curl_free(encoded);
    debugf(DBG_LEVEL_EXT,
           "exit 0: cloudfs_object_read_fp(%s) uploaded ok, response=%d",
           de->name, response);
    return (response >= 200 && response < 300);
  }
  else
  {
    // assume enters here when file is composed of only one segment (small files)
    debugf(DBG_LEVEL_EXT, "cloudfs_object_read_fp(%s) non-segmented up", de->name);
  }
  rewind(fp);
  char* encoded = curl_escape(de->full_name, 0);
  int response = send_request(HTTP_PUT, encoded, fp, NULL, NULL, de, NULL);
  get_file_metadata(de);
  curl_free(encoded);
  debugf(DBG_LEVEL_EXT, "exit 1: cloudfs_object_read_fp(%s)", de->name);
  return (response >= 200 && response < 300);
}

/*
   download file from cloud and write to local file
*/
int cloudfs_object_write_fp(dir_entry* de, FILE* fp)
{
  debugf(DBG_LEVEL_EXT, "cloudfs_object_write_fp(%s) fp=%p", de->name, fp);
  char* encoded = curl_escape(de->full_name, 0);
  char seg_base[MAX_URL_SIZE] = "";
  long segments;
  long full_segments;
  long remaining;
  long size_of_segments;
  long total_size;

  //checks if this file is a segmented one
  if (format_segments(de->full_name, seg_base, &segments, &full_segments,
                      &remaining, &size_of_segments, &total_size))
  {
    rewind(fp);
    fflush(fp);
    if (ftruncate(fileno(fp), 0) < 0)
    {
      debugf(DBG_LEVEL_NORM, KMAG
             "cloudfs_object_write_fp: ftruncate failed, aborting!");
      abort();
    }

    //download all segments from cloud to local file, wait until completed
    run_segment_threads("GET", segments, full_segments, remaining, fp,
                        seg_base, size_of_segments, de);
    debugf(DBG_LEVEL_EXT, "exit 0: cloudfs_object_write_fp(%s)", de->name);
    return 1;
  }

  //get not segmented file
  int response = send_request("GET", encoded, fp, NULL, NULL, de, NULL);
  curl_free(encoded);
  fflush(fp);
  if ((response >= 200 && response < 300) || ftruncate(fileno(fp), 0))
  {
    debugf(DBG_LEVEL_EXT, "exit 1: cloudfs_object_write_fp(%s)", de->name);
    return 1;
  }
  rewind(fp);
  debugf(DBG_LEVEL_EXT, "exit 2: cloudfs_object_write_fp(%s) " KRED" error",
         de->name);
  return 0;
}

/*
  progressive download from cloud
*/
void* cloudfs_object_downld_progressive(void* arg)// //const char* path)
{
  struct thread_job* job = arg;
  debugf(DBG_LEVEL_NORM, "cloudfs_object_downld_progressive(%s)",
         job->de->full_name);
  char* encoded = curl_escape(job->de->full_name, 0);
  if (job->de->is_segmented)
  {
    //job->de_seg->downld_buf.download_started = true;
    debugf(DBG_LEVEL_EXT,
           "cloudfs_object_downld_progressive(%s:%s): started seg download part=%d",
           job->de->name, job->de_seg->name, job->segment_part);
    //download all segments from cloud to local file, single or multi threaded
    run_segment_threads_progressive(HTTP_GET, job->de_seg->full_name, job);
    //job->de_seg->downld_buf.download_started = false;
    //debugf(DBG_LEVEL_EXT, KMAG
    //       "cloudfs_object_downld_progressive: 1-post buffer signal full");
    //sem_post(job->de_seg->downld_buf.sem_list[SEM_FULL]);
    //debugf(DBG_LEVEL_EXT, KMAG
    //       "cloudfs_object_downld_progressive: 2-post buffer signal full");
    //sem_post(job->de_seg->downld_buf.sem_list[SEM_FULL]);
    //todo: check what close/clean ops with download_buf needs done
    debugf(DBG_LEVEL_EXT,
           "cloudfs_object_downld_progressive(%s:%s): done download",
           job->de->name, job->de_seg->name);
  }
  else
  {
    debugf(DBG_LEVEL_NORM,
           "cloudfs_object_downld_progressive(%s): started non-segmented download",
           job->de->name);
    //get an un-segmented file
    int response = send_request(HTTP_GET, encoded,
                                job->de->downld_buf.local_cache_file,
                                NULL, NULL, job->de, job->de_seg);
    curl_free(encoded);
    fflush(job->de->downld_buf.local_cache_file);
    if ((response >= 200 && response < 300) ||
        ftruncate(fileno(job->de->downld_buf.local_cache_file), 0))
    {
      debugf(DBG_LEVEL_EXT, "exit 1: cloudfs_object_downld_progressive(%s)",
             job->de->name);
      //return 1;
    }
    else
    {
      rewind(job->de->downld_buf.local_cache_file);
      debugf(DBG_LEVEL_NORM, "exit 2: cloudfs_object_downld_progressive(%s) "
             KRED"error", job->de->name);
      //return 0;
    }
  }
  if (!job->is_single_thread)
    pthread_exit(NULL);
  debugf(DBG_LEVEL_EXT, "exit 3: cloudfs_object_downld_progressive(%s)",
         job->de->name);
}

int download_ahead_segment_thread(void* arg)
{
  struct thread_job* job = arg;
  int segindex;
  dir_entry* de_seg;
  bool in_cache;
  long seg_read_ahead_count;
  if (option_read_ahead == -1)
    seg_read_ahead_count = job->de->segment_count - job->segment_part;
  else //ceil or floor does not compile/link properly
    seg_read_ahead_count = (option_read_ahead / job->de->segment_size)
                           + (option_read_ahead % job->de->segment_size == 0 ? 0 : 1);
  FILE* fp_segment = NULL;

  for (segindex = job->segment_part;
       (segindex < job->de->segment_count
        && segindex < job->segment_part + seg_read_ahead_count); segindex++)
  {
    de_seg = get_segment(job->de, segindex);
    assert(de_seg);
    debugf(DBG_LEVEL_EXT, KCYN
           "download_ahead_segment_th(%s:%s): from segindex=%d count=%d",
           job->de->name, (de_seg ? de_seg->name : ""), segindex,
           job->segment_part + seg_read_ahead_count);
    if (de_seg)
    {
      in_cache = open_segment_cache_md5(job->de, de_seg, &fp_segment, HTTP_GET);
      assert(fp_segment);
      if (!in_cache)
        cloudfs_download_segment(de_seg, job->de, fp_segment, 0);
      fclose(fp_segment);
      fp_segment = NULL;
    }
  }
  debugf(DBG_LEVEL_EXT, KCYN
         "download_ahead_segment_th(%s): exit segindex=%d ahead_count=%lu",
         job->de->name, segindex, seg_read_ahead_count);
  job->de->downld_buf.ahead_thread_count--;
  free(job->self_reference);
  pthread_exit(NULL);
}

/*
  downloading ahead segments.
  if sync_first = true, get first segment sync and rest async.
*/
int download_ahead_segment(dir_entry* de_seg, dir_entry* de, FILE* fp,
                           bool sync_first)
{
  debugf(DBG_LEVEL_EXT, KCYN "download_ahead_segment(%s): segindex=%d",
         de->name, de_seg->segment_part);
  de->downld_buf.ahead_thread_count++;
  int index = 0;
  if (sync_first)
  {
    //get current segment sync.
    cloudfs_download_segment(de_seg, de, fp, 0);
    index = 1;
  }
  //get the rest async.
  struct thread_job* job = malloc(sizeof(struct thread_job));
  job->de = de;
  job->segment_part = de_seg->segment_part + index;
  job->self_reference = job;//todo: free this
  pthread_create(&job->thread, NULL,
                 (void*)download_ahead_segment_thread, job);
  return true;
}

/*
  download a segment synch and return immediately
  size = 0 for segments read ahead and saved to local cache files
  download completion is signaled via semaphores
*/
int cloudfs_download_segment(dir_entry* de_seg, dir_entry* de, FILE* fp,
                             size_t size)
{
  debugf(DBG_LEVEL_EXT, KMAG
         "cloudfs_download_segment: starting download %s part=%lu",
         de->name, de_seg->segment_part);
  if (!de_seg->downld_buf.mutex_initialised)
  {
    pthread_mutex_init(&de_seg->downld_buf.mutex, &segment_mutex_attr);
    de_seg->downld_buf.mutex_initialised = true;
  }
  pthread_mutex_lock(&de_seg->downld_buf.mutex);
  debugf(DBG_LEVEL_EXT, KMAG "cloudfs_download_segment: passed lock %s part=%lu",
         de->name, de_seg->segment_part);
  //after lock file might be in cache already
  if (check_segment_cache_md5(de, de_seg, fp))
  {
    debugf(DBG_LEVEL_EXT, KMAG
           "cloudfs_download_segment: cache ok after lock %s part=%lu",
           de->name, de_seg->segment_part);
    pthread_mutex_unlock(&de_seg->downld_buf.mutex);
    return true;
  }
  else
  {
    debugf(DBG_LEVEL_EXT, KMAG
           "cloudfs_download_segment: cache NOT ok after lock %s part=%lu",
           de->name, de_seg->segment_part);
  }

  init_semaphores(&de_seg->downld_buf, de_seg, "dwnld");
  de_seg->downld_buf.fuse_read_size = size;
  struct segment_info info;
  info.method = HTTP_GET;
  info.part = de_seg->segment_part;
  info.segment_size = de->segment_size;
  if (de->is_segmented)
    info.size_left = de_seg->segment_part < de->segment_full_count ?
                     de->segment_size : de->segment_remaining;
  else
    info.size_left = de->size;
  //need a copy for resume as info.size_left will be changed during download
  info.size_copy = info.size_left;

  //get existing segment size on disk for resume ops
  //if size is less than full segment size,
  //otherwise assume is corrupted as md5check above failed
  struct stat st;
  int fd = fileno(fp);
  assert(fd != -1);
  assert(fstat(fd, &st) ==  0);
  if (st.st_size < de_seg->size)
    info.size_processed = st.st_size;
  else
  {
    //sould never happen?
    debugf(DBG_LEVEL_EXT, KRED "cloudfs_download_segment: segment corrupted");
    abort();
  }

  info.seg_base = de_seg->full_name;
  info.de_seg = de_seg;
  info.de = de;
  info.fp = fp;
  info.de->is_progressive = false;
  info.de->is_single_thread = true;
  assert(info.fp);
  assert(fseek(info.fp, info.size_processed, SEEK_SET) == 0);
  setvbuf(info.fp, NULL, _IOFBF, DISK_BUFF_SIZE);
  int response = send_request_size(info.method, NULL, &info, NULL, NULL,
                                   info.size_copy, de->is_segmented, de, de_seg);
  if (!(response >= 200 && response < 300))
    debugf(DBG_LEVEL_NORM, KRED
           "cloudfs_download_segment: %s failed resp=%d proc=%lu",
           info.seg_base, response, info.size_processed);
  fflush(info.fp);
  //compute md5sum after each seg download
  if (de_seg->md5sum_local)
    free(de_seg->md5sum_local);

  assert(fseek(info.fp, 0, SEEK_SET) == 0);
  char md5_file_hash_str[MD5_DIGEST_HEXA_STRING_LEN] = { 0 };
  file_md5(fp, md5_file_hash_str);
  if (md5_file_hash_str && !strcasecmp(md5_file_hash_str, de_seg->md5sum))
    de_seg->md5sum_local = strdup(md5_file_hash_str);
  else
  {
    //todo: delete segment content if fully loaded but corrupted
    if (info.size_processed == de_seg->size)
    {
      debugf(DBG_LEVEL_NORM, KRED
             "cloudfs_download_segment(%s): corrupted segment", de_seg->name);
      assert(ftruncate(fd, 0) == 0);
    }
  }
  sem_post(de_seg->downld_buf.sem_list[SEM_FULL]);
  free_semaphores(&de_seg->downld_buf, SEM_EMPTY);
  free_semaphores(&de_seg->downld_buf, SEM_FULL);
  pthread_mutex_unlock(&de_seg->downld_buf.mutex);
  debugf(DBG_LEVEL_EXT, KMAG
         "cloudfs_download_segment: done fp=%p part=%lu proc=%lu",
         de_seg->downld_buf.local_cache_file, de_seg->segment_part,
         info.size_processed);
  return true;
}

int cloudfs_object_truncate(dir_entry* de, off_t size)
{
  char* encoded = curl_escape(de->full_name, 0);
  int response;

  if (size == 0)
  {
    FILE* fp = fopen("/dev/null", "r");
    response = send_request("PUT", encoded, fp, NULL, NULL, de, NULL);
    fclose(fp);
  }
  else
  {
    //TODO: this is busted
    response = send_request("GET", encoded, NULL, NULL, NULL, de, NULL);
  }
  curl_free(encoded);
  return (response >= 200 && response < 300);
}

//get metadata from cloud, like time attribs. create new entry if not cached yet.
//todo: not thread-safe?
void get_file_metadata(dir_entry* de)
{

  if (option_get_extended_metadata)
  {
    debugf(DBG_LEVEL_EXT, KCYN "get_file_metadata(%s) size_cloud=%lu",
           de->full_name, de->size_on_cloud);
    //clear existing segments cache
    dir_decache_segments(de);
    //retrieve additional file metadata with a quick HEAD query
    int response = send_request("GET", NULL, NULL, NULL, NULL, de, NULL);
    debugf(DBG_LEVEL_EXT, KCYN "status: get_file_metadata(%s) hash=%s",
           de->full_name, de->md5sum);
    //generate hash if was not generated on directory list (on copy_object)
    if (!de->full_name_hash)
      de->full_name_hash = strdup(str2md5(de->full_name, strlen(de->full_name)));
  }

  if ((de->size_on_cloud == 0 || de->is_segmented) && !de->isdir
      && !de->metadata_downloaded)
  {
    //this can be a potential segmented file, try to read segments size
    debugf(DBG_LEVEL_EXT, KMAG"get_file_metadata: get segments file=%s",
           de->full_name);
    /*char seg_base[MAX_URL_SIZE] = "";
      long segments;
      long full_segments;
      long remaining;
      long size_of_segments;
      long total_size;
      //if (format_segments(de->full_name, seg_base, &segments, &full_segments,
      //                    &remaining,
      //                    &size_of_segments, &total_size))

      update_segments(de);
      {
      de->size = total_size;
      de->segment_size = size_of_segments;
      de->is_segmented = true;
      de->segment_count = segments;
      de->segment_full_count = full_segments;
      de->segment_remaining = remaining;
      }
    */
    update_segments(de);
  }
  else debugf(DBG_LEVEL_EXT, KCYN
                "get_file_metadata(%s) skip seg_downld, size_cloud=%lu meta_down=%d",
                de->full_name, de->size_on_cloud, de->metadata_downloaded);

  de->metadata_downloaded = true;
  return;
}

//get list of folders from cloud
// return 1 for OK, 0 for error
int cloudfs_list_directory(const char* path, dir_entry** dir_list)
{
  debugf(DBG_LEVEL_EXT, "cloudfs_list_directory(%s)", path);
  char container[MAX_PATH_SIZE * 3] = "";
  char object[MAX_PATH_SIZE] = "";
  char last_subdir[MAX_PATH_SIZE] = "";
  int prefix_length = 0;
  int response = 0;
  int retval = 0;
  int entry_count = 0;
  dir_entry* last_dir = NULL;
  *dir_list = NULL;
  xmlNode* onode = NULL, *anode = NULL, *text_node = NULL;
  xmlParserCtxtPtr xmlctx = xmlCreatePushParserCtxt(NULL, NULL, "", 0, NULL);


  if (!strcmp(path, "") || !strcmp(path, "/"))
  {
    path = "";
    strncpy(container, "/?format=xml", sizeof(container));
  }
  else
  {
    sscanf(path, "/%[^/]/%[^\n]", container, object);
    char* encoded_container = curl_escape(container, 0);
    char* encoded_object = curl_escape(object, 0);
    // The empty path doesn't get a trailing slash, everything else does
    //fix needed as slash must not be appended if there already
    char* trailing_slash = "";
    prefix_length = strlen(object);
    if (object[0] == 0 || object[prefix_length - 1] == '/')
      trailing_slash = "";
    else
    {
      trailing_slash = "/";
      prefix_length++;
    }
    snprintf(container, sizeof(container), "%s?format=xml&delimiter=/&prefix=%s%s",
             encoded_container, encoded_object, trailing_slash);
    curl_free(encoded_container);
    curl_free(encoded_object);
  }
  if ((!strcmp(path, "") || !strcmp(path, "/")) && *override_storage_url)
    response = 404;
  else
  {
    // this was generating 404 err on non segmented files (small files)
    response = send_request("GET", container, NULL, xmlctx, NULL, NULL, NULL);
  }
  if (response >= 200 && response < 300)
    xmlParseChunk(xmlctx, "", 0, 1);
  if (response >= 200 && response < 300 && xmlctx->wellFormed )
  {
    xmlNode* root_element = xmlDocGetRootElement(xmlctx->myDoc);
    for (onode = root_element->children; onode; onode = onode->next)
    {
      if (onode->type != XML_ELEMENT_NODE) continue;
      char is_object = !strcasecmp((const char*)onode->name, "object");
      char is_container = !strcasecmp((const char*)onode->name, "container");
      char is_subdir = !strcasecmp((const char*)onode->name, "subdir");
      if (is_object || is_container || is_subdir)
      {
        dir_entry* de = init_dir_entry();
        if (is_object)//usefull for segments
          de->segment_part = entry_count;
        entry_count++;
        // useful docs on nodes here: http://developer.openstack.org/api-ref-objectstorage-v1.html
        if (is_container || is_subdir)
          de->content_type = strdup("application/directory");
        for (anode = onode->children; anode; anode = anode->next)
        {

          char* content = "<?!?>";
          for (text_node = anode->children; text_node; text_node = text_node->next)
          {
            if (text_node->type == XML_TEXT_NODE)
            {
              content = (char*)text_node->content;
              //debugf(DBG_LEVEL_EXT, KYEL
              //       "List dir anode=[%s] content=[%s]",
              //       (const char*)anode->name, content);
            }
            else
            {
              //debugf(DBG_LEVEL_EXT, KYEL
              //       "List dir anode=[%s]", (const char*)anode->name);
            }
          }
          //debugf(DBG_LEVEL_EXT, KCYN
          //       "cloudfs_list_directory(%s): anode [%s]=[%s]", path,
          //       (const char*)anode->name, content);
          if (!strcasecmp((const char*)anode->name, "name"))
          {
            de->name = strdup(content + prefix_length);
            // Remove trailing slash from name
            char* slash = strrchr(de->name, '/');
            if (slash && (0 == *(slash + 1)))
              *slash = 0;
            //concat full name but remove trailing slash from path
            slash = strrchr(path, '/');
            if (slash && (0 == *(slash + 1)))
            {
              if (asprintf(&(de->full_name), "%s%s", path, de->name) < 0)
                de->full_name = NULL;
              //keep this line for else if otherwise auto format will break it
            }
            else if (asprintf(&(de->full_name), "%s/%s", path, de->name) < 0)
              de->full_name = NULL;
            //need a unique file id for semaphores
            de->full_name_hash = strdup(str2md5(de->full_name, strlen(de->full_name)));
          }
          if (!strcasecmp((const char*)anode->name, "bytes"))
          {
            //this will be overwriten with true size if file is segmented
            de->size = strtoll(content, NULL, 10);
            //need the original size to check if file is segmented
            de->size_on_cloud = strtoll(content, NULL, 10);
          }
          if (!strcasecmp((const char*)anode->name, "content_type"))
          {
            de->content_type = strdup(content);
            char* semicolon = strchr(de->content_type, ';');
            if (semicolon)
              *semicolon = '\0';
          }
          if (!strcasecmp((const char*)anode->name, "hash"))
            de->md5sum = strdup(content);
          if (!strcasecmp((const char*)anode->name, "last_modified"))
          {
            time_t last_modified_t = get_time_from_str_as_gmt(content);
            char local_time_str[64];
            time_t local_time_t = get_time_as_local(last_modified_t, local_time_str,
                                                    sizeof(local_time_str));
            de->last_modified = local_time_t;
            de->ctime.tv_sec = local_time_t;
            de->ctime.tv_nsec = 0;
            //initialise all fields with hubic last modified date in case the file does not have extended attributes set
            de->mtime.tv_sec = local_time_t;
            de->mtime.tv_nsec = 0;
            de->atime.tv_sec = local_time_t;
            de->atime.tv_nsec = 0;
            // TODO check if I can retrieve nano seconds?
          }
        }
        de->isdir = de->content_type &&
                    ((strstr(de->content_type, "application/folder") != NULL) ||
                     (strstr(de->content_type, "application/directory") != NULL));
        de->islink = de->content_type &&
                     ((strstr(de->content_type, "application/link") != NULL));
        if (de->isdir)
        {
          //removes a dir_entry from cache if is an older version?
          if (!strncasecmp(de->name, last_subdir, sizeof(last_subdir)))
          {
            //not sure when / why this is called, seems to generate many missed delete ops.
            //cloudfs_free_dir_list(de);
            debugf(DBG_LEVEL_EXT, "cloudfs_list_directory: " KYEL
                   "ignore "KNRM"cloudfs_free_dir_list(%s) command", de->name);
            continue;
          }
          strncpy(last_subdir, de->name, sizeof(last_subdir));
        }
        //fixed, saves elements in default read order
        if (!*dir_list)
          *dir_list = de;
        else
          last_dir->next = de;
        last_dir = de;
        //saves elements in the list in reversed order, not good!
        /*
          de->next = *dir_list;
          dir_list = de;
        */
        char time_str[TIME_CHARS] = "";
        get_timespec_as_str(&(de->mtime), time_str, sizeof(time_str));
        debugf(DBG_LEVEL_NORM,
               KCYN"new dir_entry %s size=%d %s dir=%d lnk=%d mod=[%s] md5=%s",
               de->full_name, de->size, de->content_type, de->isdir, de->islink, time_str,
               de->md5sum);
      }
      else
        debugf(DBG_LEVEL_EXT, "unknown element: %s", onode->name);
    }
    retval = 1;
  }
  else if ((!strcmp(path, "") || !strcmp(path, "/")) && *override_storage_url)
  {
    entry_count = 1;
    debugf(DBG_LEVEL_NORM, KRED "Init cache entry container=[%s] !!!???",
           public_container);
    sleep_ms(3000);
    dir_entry* de = init_dir_entry();
    de->name = strdup(public_container);
    struct tm last_modified;
    // TODO check what this default time means?
    strptime("1388434648.01238", "%FT%T", &last_modified);
    de->last_modified = mktime(&last_modified);
    de->content_type = strdup("application/directory");
    if (asprintf(&(de->full_name), "%s/%s", path, de->name) < 0)
      de->full_name = NULL;
    //need a unique file id for semaphores
    de->full_name_hash = strdup(str2md5(de->full_name, strlen(de->full_name)));
    de->isdir = 1;
    de->islink = 0;
    de->size = 4096;
    de->next = *dir_list;
    *dir_list = de;
    retval = 1;
  }
  xmlFreeDoc(xmlctx->myDoc);
  xmlFreeParserCtxt(xmlctx);
  debugf(DBG_LEVEL_EXT, "exit: cloudfs_list_directory(%s)", path);
  return retval;
}

void thread_cloudfs_delete_object(dir_entry* de)
{
  cloudfs_delete_object(de);
  pthread_exit(NULL);
}

/*
  remove folder and it's subfolders & files recursively
  todo: implement bulk delete:
  https://docs.hpcloud.com/publiccloud/api/swift-api.html
*/
int cloudfs_delete_object(dir_entry* de)
{
  debugf(DBG_LEVEL_EXT, "cloudfs_delete_object(%s): dir=%d seg=%d",
         de->full_name, de->isdir, de->is_segmented);
  if (de->is_segmented || de->isdir)
  {
    //delete object segments or subfolders
    if (de->manifest_cloud || de->isdir)
    {
      dir_entry* de_content;
      char* prefix_path = de->isdir ? de->full_name : de->manifest_cloud;
      if (cloudfs_list_directory(prefix_path, &de_content))
      {
        dir_entry* de_tmp;
        pthread_t* threads = (pthread_t*)malloc(MAX_DELETE_THREADS * sizeof(
            pthread_t));
        int active_threads = 0;
        int th_ret, i;
        de_tmp = de_content;
        while (de_tmp)
        {
          if (active_threads < MAX_DELETE_THREADS)
          {
            pthread_create(&threads[active_threads], NULL,
                           (void*)thread_cloudfs_delete_object, de_tmp);
            active_threads++;
          }
          //start threads if pool is full or this is the last object
          if (active_threads == MAX_DELETE_THREADS || !de_tmp->next)
            for (i = 0; i < active_threads; i++)
            {
              if ((th_ret = pthread_join(threads[i], NULL)) != 0)
                debugf(DBG_LEVEL_NORM, KRED
                       "cloudfs_delete_object(%s): Error wait thread %d, stat=%d",
                       de->full_name, active_threads, th_ret);
              else
                active_threads = 0;
            }
          de_tmp = de_tmp->next;
        }
      }
      else
        abort();
    }
  }
  //delete main object
  int response = send_request(HTTP_DELETE, NULL, NULL, NULL, NULL, de, NULL);
  int ret = (response >= 200 && response < 300);

  debugf(DBG_LEVEL_EXT, "status: cloudfs_delete_object(%s) response=%d",
         de->full_name, response);
  if (response == 409)
  {
    debugf(DBG_LEVEL_EXT, "status: cloudfs_delete_object(%s) NOT EMPTY",
           de->full_name);
    ret = -1;
  }
  if (ret) //clear cache if ok
    dir_decache(de->full_name);
  return ret;
}



/*
  remove folder and it's subfolders & files recursively
  todo: implement bulk delete:
    https://docs.hpcloud.com/publiccloud/api/swift-api.html
*/
/*
  int cloudfs_delete_folder(dir_entry* de)
  {
  debugf(DBG_LEVEL_EXT, "cloudfs_delete_folder(%s)", de->full_name);
  int response = -1;
  dir_entry* de_content, *de_tmp;
  char manifest_root[MAX_URL_SIZE] = "";
  int active_threads = 0;
  if (cloudfs_list_directory(de->full_name, &de_content))
  {
    pthread_t* threads = (pthread_t*)malloc(MAX_DELETE_THREADS * sizeof(
        pthread_t));
    int active_threads = 0;
    de_tmp = de_content;
    int i = 0, th_ret;
    while (de_tmp)
    {
      if (active_threads < MAX_DELETE_THREADS)
      {
        pthread_create(&threads[i], NULL,
                       (void*)thread_cloudfs_delete_object, de_tmp);
        active_threads++;
      }
      //start threads if pool is full or this is the last object
      if (active_threads == MAX_DELETE_THREADS || !de_tmp->next)
        for (i = 0; i < active_threads; i++)
        {
          if ((th_ret = pthread_join(threads[i], NULL)) != 0)
            debugf(DBG_LEVEL_NORM, KRED
                   "cloudfs_delete_folder(%s): Error waiting thread %d, stat=%d",
                   de->full_name, i, th_ret);
          else
            active_threads = 0;
        }
      //cloudfs_delete_object(de_tmp);
      //if (de_tmp->isdir)
      //  cloudfs_delete_folder(de_tmp);
      //  else
      //  response = send_request(HTTP_DELETE, NULL, NULL, NULL, NULL, de_tmp, NULL);

      de_tmp = de_tmp->next;
      i++;
    }
    free(threads);
  }
  //delete this folder. it might throw 404 errors, not sure why
  int ok = cloudfs_delete_object(de);
  if (de_content)
    cloudfs_free_dir_list(de_content);
  debugf(DBG_LEVEL_EXT, "cloudfs_delete_folder(%s): exit ok=%d",
         de->full_name, ok);
  }

*/


//fixme: this op does not preserve src attributes (e.g. will make rsync not work well)
// https://ask.openstack.org/en/question/14307/is-there-a-way-to-moverename-an-object/
// this operation also causes an HTTP 400 error if X-Object-Meta-FilePath value is larger than 256 chars
int cloudfs_copy_object(dir_entry* de, const char* dst)
{
  debugf(DBG_LEVEL_EXT, "cloudfs_copy_object(%s, %s)", de->name, dst);
  char* dst_encoded = curl_escape(dst, 0);
  char* src_encoded = curl_escape(de->full_name, 0);

  dir_entry* de_versions, *de_tmp;
  dir_entry* new_de = NULL;
  //convert encoded string (slashes are encoded as well) to encoded string with slashes
  decode_path(src_encoded);

  if (de->is_segmented)
  {
    //copy segments with destination prefix
    assert(de->manifest_cloud);
    if (cloudfs_list_directory(de->manifest_cloud, &de_versions))
    {
      new_de = init_dir_entry();
      path_to_de(dst, new_de);//get manifest root
      char seg_path[MAX_URL_SIZE] = "";
      de_tmp = de_versions;
      while (de_tmp)
      {
        //format segment full path
        snprintf(seg_path, MAX_URL_SIZE, "/%s/%08i", new_de->manifest_time,
                 de_tmp->segment_part);
        if (cloudfs_copy_object(de_tmp, seg_path))
          de_tmp = de_tmp->next;
        else
          abort();
      }
    }
    else
      abort();

  }

  //copy source file (or manifest) as destination
  curl_slist* headers = NULL;
  if (de->is_segmented)
  {
    add_header(&headers, HEADER_TEXT_MANIFEST, new_de->manifest_time);
    cloudfs_free_dir_list(new_de);
  }
  add_header(&headers, "X-Copy-From", src_encoded);
  add_header(&headers, "Content-Length", "0");
  //pass src metadata so that PUT will set time attributes of the src file
  int response = send_request(HTTP_PUT, dst_encoded, NULL, NULL, headers, NULL,
                              NULL);
  int op_ok = (response >= 200 && response < 300);

  curl_free(dst_encoded);
  curl_free(src_encoded);
  curl_slist_free_all(headers);

  if (de->is_segmented)
  {
    if (op_ok)
    {
      //safe to delete segments from source manifest root
      de_tmp = de_versions;
      while (de_tmp)
      {
        if (cloudfs_delete_object(de_tmp))
          de_tmp = de_tmp->next;
        else
          abort();
      }
      //delete older segments from destination manifest root
      //todo
    }
    else
    {
      //todo: recover from failed copy, delete garbage?
      ;
    }
  }
  debugf(DBG_LEVEL_EXT, "exit: cloudfs_copy_object(%s,%s) response=%d", de->name,
         dst,
         response);
  return op_ok;
}

int cloudfs_post_object(dir_entry* de)
{
  debugf(DBG_LEVEL_EXT, "cloudfs_post_object(%s) ", de->name);
  char* encoded = curl_escape(de->full_name, 0);
  curl_slist* headers = NULL;
  add_header(&headers, "Content-Length", "0");
  int response = send_request_size(HTTP_POST, encoded, NULL, NULL, headers,
                                   de->size, de->is_segmented, de, NULL);
  curl_free(encoded);
  curl_slist_free_all(headers);
  debugf(DBG_LEVEL_EXT, "exit: cloudfs_post_object(%s) response=%d", de->name,
         response);
  return (response >= 200 && response < 300);
}


/*
  update an existing file metadata on cloud.
  ca be done with COPY (slow) or with POST
  http://developer.openstack.org/api-ref-objectstorage-v1.html#updateObjectMeta
  http://www.17od.com/2012/12/19/ten-useful-openstack-swift-features/
*/
int cloudfs_update_meta(dir_entry* de)
{
  //copy version
  /*
    int response = cloudfs_copy_object(de->full_name, de->full_name);
    return response;
  */
  //POST version
  int response = cloudfs_post_object(de);
  return response;
}

//optimised with cache
int cloudfs_statfs(const char* path, struct statvfs* stat)
{
  time_t now = get_time_now();
  int lapsed = now - last_stat_read_time;
  if (lapsed > option_cache_statfs_timeout)
  {
    //todo: check why stat head request is always set to /, why not path?
    int response = send_request("HEAD", "/", NULL, NULL, NULL, NULL, NULL);
    *stat = statcache;
    debugf(DBG_LEVEL_EXT,
           "exit: cloudfs_statfs (new recent values, was cached since %d seconds)",
           lapsed);
    last_stat_read_time = now;
    return (response >= 200 && response < 300);
  }
  else
  {
    debugf(DBG_LEVEL_EXT,
           "exit: cloudfs_statfs (old values, cached since %d seconds)", lapsed);
    return 1;
  }
}

int cloudfs_create_symlink(dir_entry* de, const char* dst)
{
  char* dst_encoded = curl_escape(dst, 0);
  FILE* lnk = tmpfile();
  fwrite(de->full_name, 1, strlen(de->full_name), lnk);
  fwrite("\0", 1, 1, lnk);
  int response = send_request("MKLINK", dst_encoded, lnk, NULL, NULL, de, NULL);
  curl_free(dst_encoded);
  fclose(lnk);
  return (response >= 200 && response < 300);
}

bool cloudfs_create_directory(const char* path)
{
  debugf(DBG_LEVEL_EXT, "cloudfs_create_directory(%s)", path);
  assert(path);
  //dir_entry* de_tmp = init_dir_entry();
  //de_tmp->full_name = strdup(path);
  //de_tmp->isdir = 1;
  char* encoded = curl_escape(path, 0);
  int response = send_request("MKDIR", encoded, NULL, NULL, NULL, NULL, NULL);
  //cloudfs_free_dir_list(de_tmp);
  curl_free(encoded);
  debugf(DBG_LEVEL_EXT, "cloudfs_create_directory(%s) response=%d",
         path, response);
  return (response >= 200 && response < 300);
}

off_t cloudfs_file_size(int fd)
{
  struct stat buf;
  fstat(fd, &buf);
  return buf.st_size;
}

/*void cloudfs_verify_ssl(int vrfy)
  {
  verify_ssl = vrfy ? 2 : 0;
  }

  void cloudfs_option_get_extended_metadata(int option)
  {
  option_get_extended_metadata  = option ? true : false;
  }

  void cloudfs_option_curl_verbose(int option)
  {
  option_curl_verbose = option ? true : false;
  }
*/
static struct reconnect_args
{
  char client_id    [MAX_HEADER_SIZE];
  char client_secret[MAX_HEADER_SIZE];
  char refresh_token[MAX_HEADER_SIZE];
} reconnect_args;

void cloudfs_set_credentials(char* client_id, char* client_secret,
                             char* refresh_token)
{
  strncpy(reconnect_args.client_id    , client_id    ,
          sizeof(reconnect_args.client_id    ));
  strncpy(reconnect_args.client_secret, client_secret,
          sizeof(reconnect_args.client_secret));
  strncpy(reconnect_args.refresh_token, refresh_token,
          sizeof(reconnect_args.refresh_token));
}

struct htmlString
{
  char* text;
  size_t size;
};

static size_t writefunc_string(void* contents, size_t size, size_t nmemb,
                               void* data)
{
  struct htmlString* mem = (struct htmlString*) data;
  size_t realsize = size * nmemb;
  mem->text = realloc(mem->text, mem->size + realsize + 1);
  if (mem->text == NULL)   /* out of memory! */
  {
    perror(__FILE__);
    exit(EXIT_FAILURE);
  }
  memcpy(&(mem->text[mem->size]), contents, realsize);
  mem->size += realsize;
  return realsize;
}

char* htmlStringGet(CURL* curl)
{
  struct htmlString chunk;
  chunk.text = malloc(sizeof(char));
  chunk.size = 0;
  chunk.text[0] = '\0';//added to avoid valgrind unitialised warning
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
  do
  {
    curl_easy_perform(curl);
  }
  while (chunk.size == 0);
  chunk.text[chunk.size] = '\0';
  return chunk.text;
}

/* thanks to http://devenix.wordpress.com */
char* unbase64(unsigned char* input, int length)
{
  BIO* b64, *bmem;
  char* buffer = (char*)malloc(length);
  memset(buffer, 0, length);
  b64 = BIO_new(BIO_f_base64());
  bmem = BIO_new_mem_buf(input, length);
  bmem = BIO_push(b64, bmem);
  BIO_set_flags(bmem, BIO_FLAGS_BASE64_NO_NL);
  BIO_read(bmem, buffer, length);
  BIO_free_all(bmem);
  return buffer;
}

int safe_json_string(json_object* jobj, char* buffer, char* name)
{
  int result = 0;
  if (jobj)
  {
    json_object* o;
    int found;
    found = json_object_object_get_ex(jobj, name, &o);
    if (found)
    {
      strcpy (buffer, json_object_get_string(o));
      result = 1;
    }
  }
  if (!result)
    debugf(DBG_LEVEL_NORM, KRED"HUBIC cannot get json field '%s'\n", name);
  return result;
}

int cloudfs_connect()
{
#define HUBIC_TOKEN_URL     "https://api.hubic.com/oauth/token"
#define HUBIC_CRED_URL      "https://api.hubic.com/1.0/account/credentials"
#define HUBIC_CLIENT_ID     (reconnect_args.client_id)
#define HUBIC_CLIENT_SECRET (reconnect_args.client_secret)
#define HUBIC_REFRESH_TOKEN (reconnect_args.refresh_token)
#define HUBIC_OPTIONS_SIZE  2048
  long response = -1;
  char url[HUBIC_OPTIONS_SIZE];
  char payload[HUBIC_OPTIONS_SIZE];
  struct json_object* json_obj;
  pthread_mutex_lock(&pool_mut);
  debugf(DBG_LEVEL_NORM, "Authenticating... (client_id = '%s')",
         HUBIC_CLIENT_ID);
  storage_token[0] = storage_url[0] = '\0';
  CURL* curl = curl_easy_init();
  /* curl default options */
  curl_easy_setopt(curl, CURLOPT_VERBOSE, debug);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verify_ssl ? 1 : 0);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify_ssl);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10);
  curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1);
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
  curl_easy_setopt(curl, CURLOPT_POST, 0L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc_string);
  /* Step 1 : request a token  - Not needed anymore with refresh_token */
  /* Step 2 : get request code - Not needed anymore with refresh_token */
  /* Step 3 : get access token */
  sprintf(payload, "refresh_token=%s&grant_type=refresh_token",
          HUBIC_REFRESH_TOKEN);
  curl_easy_setopt(curl, CURLOPT_URL, HUBIC_TOKEN_URL);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_HEADER, 0);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(payload));
  curl_easy_setopt(curl, CURLOPT_USERNAME, HUBIC_CLIENT_ID);
  curl_easy_setopt(curl, CURLOPT_PASSWORD, HUBIC_CLIENT_SECRET);
  curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
  char* json_str = htmlStringGet(curl);
  json_obj = json_tokener_parse(json_str);
  debugf(DBG_LEVEL_NORM, "HUBIC TOKEN_URL result: '%s'\n", json_str);
  free(json_str);
  char access_token[HUBIC_OPTIONS_SIZE];
  char token_type[HUBIC_OPTIONS_SIZE];
  int expire_sec;
  int found;
  json_object* o;
  if (!safe_json_string(json_obj, access_token, "access_token"))
    return 0;
  if (!safe_json_string(json_obj, token_type, "token_type"))
    return 0;
  found = json_object_object_get_ex(json_obj, "expires_in", &o);
  expire_sec = json_object_get_int(o);
  debugf(DBG_LEVEL_NORM, "HUBIC Access token: %s\n", access_token);
  debugf(DBG_LEVEL_NORM, "HUBIC Token type  : %s\n", token_type);
  debugf(DBG_LEVEL_NORM, "HUBIC Expire in   : %d\n", expire_sec);
  /* Step 4 : request OpenStack storage URL */
  curl_easy_setopt(curl, CURLOPT_URL, HUBIC_CRED_URL);
  curl_easy_setopt(curl, CURLOPT_POST, 0L);
  curl_easy_setopt(curl, CURLOPT_HEADER, 0);
  curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_NONE);
  /* create the Bearer authentication header */
  curl_slist* headers = NULL;
  sprintf (payload, "Bearer %s", access_token);
  add_header(&headers, "Authorization", payload);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  char token[HUBIC_OPTIONS_SIZE];
  char endpoint[HUBIC_OPTIONS_SIZE];
  char expires[HUBIC_OPTIONS_SIZE];
  json_str = htmlStringGet(curl);
  json_obj = json_tokener_parse(json_str);
  debugf(DBG_LEVEL_NORM, "CRED_URL result: '%s'\n", json_str);
  free(json_str);
  if (!safe_json_string(json_obj, token, "token"))
    return 0;
  if (!safe_json_string(json_obj, endpoint, "endpoint"))
    return 0;
  if (!safe_json_string(json_obj, expires, "expires"))
    return 0;
  /* set the global storage_url and storage_token, the only parameters needed for swift */
  strcpy (storage_url, endpoint);
  strcpy (storage_token, token);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
  curl_easy_cleanup(curl);
  pthread_mutex_unlock(&pool_mut);
  return (response >= 200 && response < 300 && storage_token[0]
          && storage_url[0]);
}
