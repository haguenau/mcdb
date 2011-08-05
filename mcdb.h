/* mmap constant database (mcdb)
 *
 * Copyright 2010  Glue Logic LLC
 * License: GPLv3
 * Originally based upon the Public Domain 'cdb-0.75' by Dan Bernstein
 *
 * - updated to C99 and POSIX.1-2001 (not available/portable when djb wrote cdb)
 * - optimized for mmap access to constant db (and avoid double buffering)
 * - redesigned for use in threaded programs (thread-safe interface available)
 * - convenience routines to check for updated constant db and to refresh mmap
 * - support cdb > 4 GB with 64-bit program (required to mmap() mcdb > 4 GB)
 * - 64-bit safe (for use in 64-bit programs)
 *
 * Advantages over external database
 * - performance: better; avoids context switch to external database process
 * Advantages over specialized hash map
 * - generic, reusable
 * - maintained (created and verified) externally from process (less overhead)
 * - shared across processes (though shared-memory could be used for hash map)
 * - read-only (though memory pages could also be marked read-only for hash map)
 * Disadvantages to specialized hash map
 * - performance: slightly slower than specialized hash map
 * Disadvantages to djb cdb
 * - mmap requires address space be available into which to mmap the const db
 *   (i.e. large const db might fail to mmap into 32-bit process)
 * - mmap page alignment requirements and use of address space limits const db
 *   max size when created by 32-bit process.  Sizes approaching 4 GB may fail.
 * - arbitrary limit of each key or data set to (INT_MAX - 8 bytes; almost 2 GB)
 *   (djb cdb doc states there is no limit besides cdb fitting into 4 GB)
 *   (writev() on some platforms in 32-bit exe might also have 2 GB limit)
 *
 * Incompatibilities with djb cdb
 * - padding added at the end of key,value data to 8-byte align hash tables
 *   (incompatible with djb cdbdump)
 * - initial table and hash tables have 8-byte values instead of 4-byte values
 *   in order to support cdb > 4 GB.  cdb uses 24 bytes per record plus 2048,
 *   whereas mcdb uses 40 bytes per record plus 4096.
 * - packing of integral lengths into char strings is done big-endian for
 *   performance in packing/unpacking integer data in 4-byte (or better)
 *   aligned addresses.  (incompatible with all djb cdb* tools and cdb's)
 *   (djb cdb documents all 32-bit quantities stored in little-endian form)
 *   Memory load latency is limiting factor, not x86 assembly instruction
 *   to convert uint32_t to and from big-endian.
 *
 * Limitations
 * - 2 billion keys
 *   As long as djb hash is 32-bit, mcdb_make.c limits number of hash keys to
 *   2 billion.  cdb handles hash collisions, but there is a small expense each
 *   collision.  As the key space becomes denser within the 2 billion, there is
 *   greater chance of collisions.  Input strings also affect this probability,
 *   as do the sizes of the hash tables.
 * - process must mmap() entire mcdb
 *   Each mcdb is mmap()d in its entirety into the address space.  For 32-bit
 *   programs that means there is a 4 GB limit on size of mcdb, minus address
 *   space used by the program (including stack, heap, shared libraries, shmat
 *   and other mmaps, etc).  Compile and link 64-bit to remove this limitation.
 */

#ifndef MCDB_H
#define MCDB_H

#include <stdbool.h>  /* bool     */
#include <stdint.h>   /* uint32_t, uintptr_t */
#include <unistd.h>   /* size_t   */
#include <sys/time.h> /* time_t   */

#if !defined(_POSIX_MAPPED_FILES) || !(_POSIX_MAPPED_FILES-0) \
 || !defined(_POSIX_SYNCHRONIZED_IO) || !(_POSIX_SYNCHRONIZED_IO-0)
#error "mcdb requires mmap() and msync() support"
#endif

#include "code_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mcdb_mmap {
  unsigned char *ptr;         /* mmap pointer */
  uintptr_t size;             /* mmap size */
  time_t mtime;               /* mmap file mtime */
  struct mcdb_mmap * volatile next;    /* updated (new) mcdb_mmap */
  void * (*fn_malloc)(size_t);         /* fn ptr to malloc() */
  void (*fn_free)(void *);             /* fn ptr to free() */
  volatile uint32_t refcnt;            /* registered access reference count */
  int dfd;                    /* fd open to dir in which mmap file resides */
  char *fname;                /* basename of mmap file, relative to dir fd */
  char fnamebuf[64];          /* buffer in which to store short fname */
};

struct mcdb {
  struct mcdb_mmap *map;
  uint32_t loop;   /* number of hash slots searched under this key */
  uint32_t hslots; /* initialized if loop is nonzero */
  uintptr_t kpos;  /* initialized if loop is nonzero */
  uintptr_t hpos;  /* initialized if loop is nonzero */
  uintptr_t dpos;  /* initialized if cdb_findnext() returns 1 */
  uint32_t dlen;   /* initialized if cdb_findnext() returns 1 */
  uint32_t khash;  /* initialized if loop is nonzero */
};

extern bool
mcdb_findtagstart(struct mcdb * restrict, const char * restrict, size_t,
                  unsigned char)/* note: must be 0 or cast to (unsigned char) */
  __attribute_nonnull__  __attribute_warn_unused_result__  __attribute_hot__;
extern bool
mcdb_findtagnext(struct mcdb * restrict, const char * restrict, size_t,
                 unsigned char) /* note: must be 0 or cast to (unsigned char) */
  __attribute_nonnull__  __attribute_warn_unused_result__  __attribute_hot__;

#define mcdb_findstart(m,key,klen) mcdb_findtagstart((m),(key),(klen),0)
#define mcdb_findnext(m,key,klen)  mcdb_findtagnext((m),(key),(klen),0)
#define mcdb_find(m,key,klen) \
  (mcdb_findstart((m),(key),(klen)) && mcdb_findnext((m),(key),(klen)))

extern void *
mcdb_read(struct mcdb * restrict, uintptr_t, uint32_t, void * restrict)
  __attribute_nonnull__  __attribute_warn_unused_result__;

#define mcdb_datapos(m)      ((m)->dpos)
#define mcdb_datalen(m)      ((m)->dlen)
#define mcdb_dataptr(m)      ((m)->map->ptr+(m)->dpos)

extern struct mcdb_mmap *  __attribute_malloc__
mcdb_mmap_create(struct mcdb_mmap * restrict,
                 const char *,const char *,void * (*)(size_t),void (*)(void *))
  __attribute_nonnull_x__((3,4,5))  __attribute_warn_unused_result__;
extern void
mcdb_mmap_destroy(struct mcdb_mmap * restrict)
  __attribute_nonnull__;
/* check if constant db has been updated and refresh mmap
 * (for use with mcdb mmaps held open for any period of time)
 * (i.e. for any use other than mcdb_mmap_create(), query, mcdb_mmap_destroy())
 * caller may call mcdb_mmap_refresh() before mcdb_find() or mcdb_findstart(),
 * or at other scheduled intervals, or not at all, depending on program need.
 * Note: threaded programs should use thread-safe mcdb_thread_refresh()
 * (which passes a ptr to the map ptr and might update value of that ptr ptr) */
#define mcdb_mmap_refresh(map) \
  (__builtin_expect(!mcdb_mmap_refresh_check(map), true) \
   || __builtin_expect(mcdb_mmap_reopen(map), true))
#define mcdb_mmap_refresh_threadsafe(mapptr) \
  (__builtin_expect(!mcdb_mmap_refresh_check(*(mapptr)), true) \
   || __builtin_expect(mcdb_mmap_reopen_threadsafe(mapptr), true))

extern bool
mcdb_mmap_init(struct mcdb_mmap * restrict, int)
  __attribute_nonnull__  __attribute_warn_unused_result__;
extern void
mcdb_mmap_free(struct mcdb_mmap * restrict)
  __attribute_nonnull__;
extern bool
mcdb_mmap_reopen(struct mcdb_mmap * restrict)
  __attribute_nonnull__  __attribute_warn_unused_result__;
extern bool
mcdb_mmap_refresh_check(const struct mcdb_mmap * restrict)
  __attribute_nonnull__  __attribute_warn_unused_result__;


enum mcdb_flags {
  MCDB_REGISTER_USE_DECR = 0,
  MCDB_REGISTER_USE_INCR = 1,
  MCDB_REGISTER_MUNMAP_SKIP = 2,
  MCDB_REGISTER_MUTEX_LOCK_HOLD = 4,
  MCDB_REGISTER_MUTEX_UNLOCK_HOLD = 8
};

extern bool
mcdb_mmap_thread_registration(struct mcdb_mmap ** restrict,
                              enum mcdb_flags)
  __attribute_nonnull__;
extern bool
mcdb_mmap_reopen_threadsafe(struct mcdb_mmap ** restrict)
  __attribute_nonnull__  __attribute_warn_unused_result__;


#define mcdb_thread_register(mcdb) \
  mcdb_mmap_thread_registration(&(mcdb)->map, MCDB_REGISTER_USE_INCR)
#define mcdb_thread_unregister(mcdb) \
  mcdb_mmap_thread_registration(&(mcdb)->map, MCDB_REGISTER_USE_DECR)
#define mcdb_thread_refresh(mcdb) \
  mcdb_mmap_refresh_threadsafe(&(mcdb)->map)
#define mcdb_thread_refresh_self(mcdb) \
  (__builtin_expect((mcdb)->map->next == NULL, true) \
   || __builtin_expect(mcdb_thread_register(mcdb), true))


#define MCDB_SLOT_BITS 8                    /* 2^8 = 256 */
#define MCDB_SLOTS (1u<<(MCDB_SLOT_BITS-1)) /* must be power-of-2 */
#define MCDB_SLOT_MASK (MCDB_SLOTS-1)       /* bitmask */
#define MCDB_HEADER_SZ (MCDB_SLOTS<<4)      /* MCDB_SLOTS * 16  (256*16=4096) */
#define MCDB_MMAP_SZ (1<<19)     /* 512KB; must be larger than MCDB_HEADER_SZ */


#ifdef __cplusplus
}
#endif

#endif
