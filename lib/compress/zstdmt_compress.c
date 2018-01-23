/*
 * Copyright (c) 2016-present, Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */


/* ======   Tuning parameters   ====== */
#define ZSTDMT_NBTHREADS_MAX 200
#define ZSTDMT_JOBSIZE_MAX  (MEM_32bits() ? (512 MB) : (2 GB))  /* note : limited by `jobSize` type, which is `unsigned` */
#define ZSTDMT_OVERLAPLOG_DEFAULT 6


/* ======   Compiler specifics   ====== */
#if defined(_MSC_VER)
#  pragma warning(disable : 4204)   /* disable: C4204: non-constant aggregate initializer */
#endif


/* ======   Dependencies   ====== */
#include <string.h>      /* memcpy, memset */
#include <limits.h>      /* INT_MAX */
#include "pool.h"        /* threadpool */
#include "threading.h"   /* mutex */
#include "zstd_compress_internal.h"  /* MIN, ERROR, ZSTD_*, ZSTD_highbit32 */
#include "zstdmt_compress.h"


/* ======   Debug   ====== */
#if defined(ZSTD_DEBUG) && (ZSTD_DEBUG>=2)

#  include <stdio.h>
#  include <unistd.h>
#  include <sys/times.h>
#  define DEBUGLOGRAW(l, ...) if (l<=ZSTD_DEBUG) { fprintf(stderr, __VA_ARGS__); }

#  define DEBUG_PRINTHEX(l,p,n) {            \
    unsigned debug_u;                        \
    for (debug_u=0; debug_u<(n); debug_u++)  \
        DEBUGLOGRAW(l, "%02X ", ((const unsigned char*)(p))[debug_u]); \
    DEBUGLOGRAW(l, " \n");                   \
}

static unsigned long long GetCurrentClockTimeMicroseconds(void)
{
   static clock_t _ticksPerSecond = 0;
   if (_ticksPerSecond <= 0) _ticksPerSecond = sysconf(_SC_CLK_TCK);

   { struct tms junk; clock_t newTicks = (clock_t) times(&junk);
     return ((((unsigned long long)newTicks)*(1000000))/_ticksPerSecond); }
}

#define MUTEX_WAIT_TIME_DLEVEL 6
#define ZSTD_PTHREAD_MUTEX_LOCK(mutex) {          \
    if (ZSTD_DEBUG >= MUTEX_WAIT_TIME_DLEVEL) {   \
        unsigned long long const beforeTime = GetCurrentClockTimeMicroseconds(); \
        ZSTD_pthread_mutex_lock(mutex);           \
        {   unsigned long long const afterTime = GetCurrentClockTimeMicroseconds(); \
            unsigned long long const elapsedTime = (afterTime-beforeTime); \
            if (elapsedTime > 1000) {  /* or whatever threshold you like; I'm using 1 millisecond here */ \
                DEBUGLOG(MUTEX_WAIT_TIME_DLEVEL, "Thread took %llu microseconds to acquire mutex %s \n", \
                   elapsedTime, #mutex);          \
        }   }                                     \
    } else {                                      \
        ZSTD_pthread_mutex_lock(mutex);           \
    }                                             \
}

#else

#  define ZSTD_PTHREAD_MUTEX_LOCK(m) ZSTD_pthread_mutex_lock(m)
#  define DEBUG_PRINTHEX(l,p,n) {}

#endif


/* =====   Buffer Pool   ===== */
/* a single Buffer Pool can be invoked from multiple threads in parallel */

typedef struct buffer_s {
    void* start;
    size_t size;
} buffer_t;

static const buffer_t g_nullBuffer = { NULL, 0 };

typedef struct ZSTDMT_bufferPool_s {
    ZSTD_pthread_mutex_t poolMutex;
    size_t bufferSize;
    unsigned totalBuffers;
    unsigned nbBuffers;
    ZSTD_customMem cMem;
    buffer_t bTable[1];   /* variable size */
} ZSTDMT_bufferPool;

static ZSTDMT_bufferPool* ZSTDMT_createBufferPool(unsigned nbThreads, ZSTD_customMem cMem)
{
    unsigned const maxNbBuffers = 2*nbThreads + 3;
    ZSTDMT_bufferPool* const bufPool = (ZSTDMT_bufferPool*)ZSTD_calloc(
        sizeof(ZSTDMT_bufferPool) + (maxNbBuffers-1) * sizeof(buffer_t), cMem);
    if (bufPool==NULL) return NULL;
    if (ZSTD_pthread_mutex_init(&bufPool->poolMutex, NULL)) {
        ZSTD_free(bufPool, cMem);
        return NULL;
    }
    bufPool->bufferSize = 64 KB;
    bufPool->totalBuffers = maxNbBuffers;
    bufPool->nbBuffers = 0;
    bufPool->cMem = cMem;
    return bufPool;
}

static void ZSTDMT_freeBufferPool(ZSTDMT_bufferPool* bufPool)
{
    unsigned u;
    DEBUGLOG(3, "ZSTDMT_freeBufferPool (address:%08X)", (U32)(size_t)bufPool);
    if (!bufPool) return;   /* compatibility with free on NULL */
    for (u=0; u<bufPool->totalBuffers; u++) {
        DEBUGLOG(4, "free buffer %2u (address:%08X)", u, (U32)(size_t)bufPool->bTable[u].start);
        ZSTD_free(bufPool->bTable[u].start, bufPool->cMem);
    }
    ZSTD_pthread_mutex_destroy(&bufPool->poolMutex);
    ZSTD_free(bufPool, bufPool->cMem);
}

/* only works at initialization, not during compression */
static size_t ZSTDMT_sizeof_bufferPool(ZSTDMT_bufferPool* bufPool)
{
    size_t const poolSize = sizeof(*bufPool)
                          + (bufPool->totalBuffers - 1) * sizeof(buffer_t);
    unsigned u;
    size_t totalBufferSize = 0;
    ZSTD_pthread_mutex_lock(&bufPool->poolMutex);
    for (u=0; u<bufPool->totalBuffers; u++)
        totalBufferSize += bufPool->bTable[u].size;
    ZSTD_pthread_mutex_unlock(&bufPool->poolMutex);

    return poolSize + totalBufferSize;
}

static void ZSTDMT_setBufferSize(ZSTDMT_bufferPool* const bufPool, size_t const bSize)
{
    ZSTD_pthread_mutex_lock(&bufPool->poolMutex);
    DEBUGLOG(4, "ZSTDMT_setBufferSize: bSize = %u", (U32)bSize);
    bufPool->bufferSize = bSize;
    ZSTD_pthread_mutex_unlock(&bufPool->poolMutex);
}

/** ZSTDMT_getBuffer() :
 *  assumption : bufPool must be valid
 * @return : a buffer, with start pointer and size
 *  note: allocation may fail, in this case, start==NULL and size==0 */
static buffer_t ZSTDMT_getBuffer(ZSTDMT_bufferPool* bufPool)
{
    size_t const bSize = bufPool->bufferSize;
    DEBUGLOG(5, "ZSTDMT_getBuffer: bSize = %u", (U32)bufPool->bufferSize);
    ZSTD_pthread_mutex_lock(&bufPool->poolMutex);
    if (bufPool->nbBuffers) {   /* try to use an existing buffer */
        buffer_t const buf = bufPool->bTable[--(bufPool->nbBuffers)];
        size_t const availBufferSize = buf.size;
        bufPool->bTable[bufPool->nbBuffers] = g_nullBuffer;
        if ((availBufferSize >= bSize) & ((availBufferSize>>3) <= bSize)) {
            /* large enough, but not too much */
            DEBUGLOG(5, "ZSTDMT_getBuffer: provide buffer %u of size %u",
                        bufPool->nbBuffers, (U32)buf.size);
            ZSTD_pthread_mutex_unlock(&bufPool->poolMutex);
            return buf;
        }
        /* size conditions not respected : scratch this buffer, create new one */
        DEBUGLOG(5, "ZSTDMT_getBuffer: existing buffer does not meet size conditions => freeing");
        ZSTD_free(buf.start, bufPool->cMem);
    }
    ZSTD_pthread_mutex_unlock(&bufPool->poolMutex);
    /* create new buffer */
    DEBUGLOG(5, "ZSTDMT_getBuffer: create a new buffer");
    {   buffer_t buffer;
        void* const start = ZSTD_malloc(bSize, bufPool->cMem);
        buffer.start = start;   /* note : start can be NULL if malloc fails ! */
        buffer.size = (start==NULL) ? 0 : bSize;
        if (start==NULL) {
            DEBUGLOG(5, "ZSTDMT_getBuffer: buffer allocation failure !!");
        } else {
            DEBUGLOG(5, "ZSTDMT_getBuffer: created buffer of size %u", (U32)bSize);
        }
        return buffer;
    }
}

/* store buffer for later re-use, up to pool capacity */
static void ZSTDMT_releaseBuffer(ZSTDMT_bufferPool* bufPool, buffer_t buf)
{
    if (buf.start == NULL) return;   /* compatible with release on NULL */
    DEBUGLOG(5, "ZSTDMT_releaseBuffer");
    ZSTD_pthread_mutex_lock(&bufPool->poolMutex);
    if (bufPool->nbBuffers < bufPool->totalBuffers) {
        bufPool->bTable[bufPool->nbBuffers++] = buf;  /* stored for later use */
        DEBUGLOG(5, "ZSTDMT_releaseBuffer: stored buffer of size %u in slot %u",
                    (U32)buf.size, (U32)(bufPool->nbBuffers-1));
        ZSTD_pthread_mutex_unlock(&bufPool->poolMutex);
        return;
    }
    ZSTD_pthread_mutex_unlock(&bufPool->poolMutex);
    /* Reached bufferPool capacity (should not happen) */
    DEBUGLOG(5, "ZSTDMT_releaseBuffer: pool capacity reached => freeing ");
    ZSTD_free(buf.start, bufPool->cMem);
}


/* =====   CCtx Pool   ===== */
/* a single CCtx Pool can be invoked from multiple threads in parallel */

typedef struct {
    ZSTD_pthread_mutex_t poolMutex;
    unsigned totalCCtx;
    unsigned availCCtx;
    ZSTD_customMem cMem;
    ZSTD_CCtx* cctx[1];   /* variable size */
} ZSTDMT_CCtxPool;

/* note : all CCtx borrowed from the pool should be released back to the pool _before_ freeing the pool */
static void ZSTDMT_freeCCtxPool(ZSTDMT_CCtxPool* pool)
{
    unsigned u;
    for (u=0; u<pool->totalCCtx; u++)
        ZSTD_freeCCtx(pool->cctx[u]);  /* note : compatible with free on NULL */
    ZSTD_pthread_mutex_destroy(&pool->poolMutex);
    ZSTD_free(pool, pool->cMem);
}

/* ZSTDMT_createCCtxPool() :
 * implies nbThreads >= 1 , checked by caller ZSTDMT_createCCtx() */
static ZSTDMT_CCtxPool* ZSTDMT_createCCtxPool(unsigned nbThreads,
                                              ZSTD_customMem cMem)
{
    ZSTDMT_CCtxPool* const cctxPool = (ZSTDMT_CCtxPool*) ZSTD_calloc(
        sizeof(ZSTDMT_CCtxPool) + (nbThreads-1)*sizeof(ZSTD_CCtx*), cMem);
    if (!cctxPool) return NULL;
    if (ZSTD_pthread_mutex_init(&cctxPool->poolMutex, NULL)) {
        ZSTD_free(cctxPool, cMem);
        return NULL;
    }
    cctxPool->cMem = cMem;
    cctxPool->totalCCtx = nbThreads;
    cctxPool->availCCtx = 1;   /* at least one cctx for single-thread mode */
    cctxPool->cctx[0] = ZSTD_createCCtx_advanced(cMem);
    if (!cctxPool->cctx[0]) { ZSTDMT_freeCCtxPool(cctxPool); return NULL; }
    DEBUGLOG(3, "cctxPool created, with %u threads", nbThreads);
    return cctxPool;
}

/* only works during initialization phase, not during compression */
static size_t ZSTDMT_sizeof_CCtxPool(ZSTDMT_CCtxPool* cctxPool)
{
    ZSTD_pthread_mutex_lock(&cctxPool->poolMutex);
    {   unsigned const nbThreads = cctxPool->totalCCtx;
        size_t const poolSize = sizeof(*cctxPool)
                                + (nbThreads-1)*sizeof(ZSTD_CCtx*);
        unsigned u;
        size_t totalCCtxSize = 0;
        for (u=0; u<nbThreads; u++) {
            totalCCtxSize += ZSTD_sizeof_CCtx(cctxPool->cctx[u]);
        }
        ZSTD_pthread_mutex_unlock(&cctxPool->poolMutex);
        return poolSize + totalCCtxSize;
    }
}

static ZSTD_CCtx* ZSTDMT_getCCtx(ZSTDMT_CCtxPool* cctxPool)
{
    DEBUGLOG(5, "ZSTDMT_getCCtx");
    ZSTD_pthread_mutex_lock(&cctxPool->poolMutex);
    if (cctxPool->availCCtx) {
        cctxPool->availCCtx--;
        {   ZSTD_CCtx* const cctx = cctxPool->cctx[cctxPool->availCCtx];
            ZSTD_pthread_mutex_unlock(&cctxPool->poolMutex);
            return cctx;
    }   }
    ZSTD_pthread_mutex_unlock(&cctxPool->poolMutex);
    DEBUGLOG(5, "create one more CCtx");
    return ZSTD_createCCtx_advanced(cctxPool->cMem);   /* note : can be NULL, when creation fails ! */
}

static void ZSTDMT_releaseCCtx(ZSTDMT_CCtxPool* pool, ZSTD_CCtx* cctx)
{
    if (cctx==NULL) return;   /* compatibility with release on NULL */
    ZSTD_pthread_mutex_lock(&pool->poolMutex);
    if (pool->availCCtx < pool->totalCCtx)
        pool->cctx[pool->availCCtx++] = cctx;
    else {
        /* pool overflow : should not happen, since totalCCtx==nbThreads */
        DEBUGLOG(5, "CCtx pool overflow : free cctx");
        ZSTD_freeCCtx(cctx);
    }
    ZSTD_pthread_mutex_unlock(&pool->poolMutex);
}


/* ------------------------------------------ */
/* =====          Thread worker         ===== */
/* ------------------------------------------ */

typedef struct {
    buffer_t src;
    const void* srcStart;
    size_t   prefixSize;
    size_t   srcSize;
    size_t   consumed;
    buffer_t dstBuff;
    size_t   cSize;
    size_t   dstFlushed;
    unsigned firstChunk;
    unsigned lastChunk;
    unsigned jobCompleted;
    unsigned frameChecksumNeeded;
    ZSTD_pthread_mutex_t* jobCompleted_mutex;
    ZSTD_pthread_cond_t* jobCompleted_cond;
    ZSTD_CCtx_params params;
    const ZSTD_CDict* cdict;
    ZSTDMT_CCtxPool* cctxPool;
    ZSTDMT_bufferPool* bufPool;
    unsigned long long fullFrameSize;
} ZSTDMT_jobDescription;

/* ZSTDMT_compressChunk() is a POOL_function type */
void ZSTDMT_compressChunk(void* jobDescription)
{
    ZSTDMT_jobDescription* const job = (ZSTDMT_jobDescription*)jobDescription;
    ZSTD_CCtx* const cctx = ZSTDMT_getCCtx(job->cctxPool);
    const void* const src = (const char*)job->srcStart + job->prefixSize;
    buffer_t dstBuff = job->dstBuff;

    /* ressources */
    if (cctx==NULL) {
        job->cSize = ERROR(memory_allocation);
        goto _endJob;
    }
    if (dstBuff.start == NULL) {
        dstBuff = ZSTDMT_getBuffer(job->bufPool);
        if (dstBuff.start==NULL) {
            job->cSize = ERROR(memory_allocation);
            goto _endJob;
        }
        job->dstBuff = dstBuff;
    }

    /* init */
    if (job->cdict) {
        size_t const initError = ZSTD_compressBegin_advanced_internal(cctx, NULL, 0, ZSTD_dm_auto, job->cdict, job->params, job->fullFrameSize);
        assert(job->firstChunk);  /* only allowed for first job */
        if (ZSTD_isError(initError)) { job->cSize = initError; goto _endJob; }
    } else {  /* srcStart points at reloaded section */
        U64 const pledgedSrcSize = job->firstChunk ? job->fullFrameSize : job->srcSize;
        ZSTD_CCtx_params jobParams = job->params;   /* do not modify job->params ! copy it, modify the copy */
        {   size_t const forceWindowError = ZSTD_CCtxParam_setParameter(&jobParams, ZSTD_p_forceMaxWindow, !job->firstChunk);
            if (ZSTD_isError(forceWindowError)) {
                job->cSize = forceWindowError;
                goto _endJob;
        }   }
        {   size_t const initError = ZSTD_compressBegin_advanced_internal(cctx,
                                        job->srcStart, job->prefixSize, ZSTD_dm_rawContent, /* load dictionary in "content-only" mode (no header analysis) */
                                        NULL, /*cdict*/
                                        jobParams, pledgedSrcSize);
            if (ZSTD_isError(initError)) {
                job->cSize = initError;
                goto _endJob;
        }   }
    }
    if (!job->firstChunk) {  /* flush and overwrite frame header when it's not first job */
        size_t const hSize = ZSTD_compressContinue(cctx, dstBuff.start, dstBuff.size, src, 0);
        if (ZSTD_isError(hSize)) { job->cSize = hSize; /* save error code */ goto _endJob; }
        DEBUGLOG(5, "ZSTDMT_compressChunk: flush and overwrite %u bytes of frame header (not first chunk)", (U32)hSize);
        ZSTD_invalidateRepCodes(cctx);
    }

    /* compress */
#if 0
    job->cSize = (job->lastChunk) ?
                 ZSTD_compressEnd     (cctx, dstBuff.start, dstBuff.size, src, job->srcSize) :
                 ZSTD_compressContinue(cctx, dstBuff.start, dstBuff.size, src, job->srcSize);
#else
    if (sizeof(size_t) > sizeof(int)) assert(job->srcSize < ((size_t)INT_MAX) * ZSTD_BLOCKSIZE_MAX);   /* check overflow */
    {   int const nbBlocks = (int)((job->srcSize + (ZSTD_BLOCKSIZE_MAX-1)) / ZSTD_BLOCKSIZE_MAX);
        const BYTE* ip = (const BYTE*) src;
        BYTE* const ostart = (BYTE*)dstBuff.start;
        BYTE* op = ostart;
        BYTE* oend = op + dstBuff.size;
        int blockNb;
        DEBUGLOG(5, "ZSTDMT_compressChunk: compress %u bytes in %i blocks", (U32)job->srcSize, nbBlocks);
        assert(job->cSize == 0);
        for (blockNb = 1; blockNb < nbBlocks; blockNb++) {
            size_t const cSize = ZSTD_compressContinue(cctx, op, oend-op, ip, ZSTD_BLOCKSIZE_MAX);
            if (ZSTD_isError(cSize)) { job->cSize = cSize; goto _endJob; }
            ip += ZSTD_BLOCKSIZE_MAX;
            op += cSize; assert(op < oend);
            /* stats */
            ZSTD_PTHREAD_MUTEX_LOCK(job->jobCompleted_mutex);   /* note : it's a mtctx mutex */
            job->cSize += cSize;
            job->consumed = ZSTD_BLOCKSIZE_MAX * blockNb;
            DEBUGLOG(5, "ZSTDMT_compressChunk: compress new block : cSize==%u bytes (total: %u)",
                        (U32)cSize, (U32)job->cSize);
            ZSTD_pthread_cond_signal(job->jobCompleted_cond);
            ZSTD_pthread_mutex_unlock(job->jobCompleted_mutex);
        }
        /* last block */
        if ((nbBlocks > 0) | job->lastChunk /*must output a "last block" flag*/ ) {
            size_t const lastBlockSize1 = job->srcSize & (ZSTD_BLOCKSIZE_MAX-1);
            size_t const lastBlockSize = ((lastBlockSize1==0) & (job->srcSize>=ZSTD_BLOCKSIZE_MAX)) ? ZSTD_BLOCKSIZE_MAX : lastBlockSize1;
            size_t const cSize = (job->lastChunk) ?
                 ZSTD_compressEnd     (cctx, op, oend-op, ip, lastBlockSize) :
                 ZSTD_compressContinue(cctx, op, oend-op, ip, lastBlockSize);
            if (ZSTD_isError(cSize)) { job->cSize = cSize; goto _endJob; }
            /* stats */
            ZSTD_PTHREAD_MUTEX_LOCK(job->jobCompleted_mutex);   /* note : it's a mtctx mutex */
            job->cSize += cSize;
            job->consumed = job->srcSize;
            ZSTD_pthread_mutex_unlock(job->jobCompleted_mutex);
        }
    }
#endif

_endJob:
    /* release */
    ZSTDMT_releaseCCtx(job->cctxPool, cctx);
    ZSTDMT_releaseBuffer(job->bufPool, job->src);
    job->src = g_nullBuffer; job->srcStart = NULL;
    /* report */
    ZSTD_PTHREAD_MUTEX_LOCK(job->jobCompleted_mutex);
    job->consumed = job->srcSize;
    job->jobCompleted = 1;
    ZSTD_pthread_cond_signal(job->jobCompleted_cond);
    ZSTD_pthread_mutex_unlock(job->jobCompleted_mutex);
}


/* ------------------------------------------ */
/* =====   Multi-threaded compression   ===== */
/* ------------------------------------------ */

typedef struct {
    buffer_t buffer;
    size_t filled;
} inBuff_t;

struct ZSTDMT_CCtx_s {
    POOL_ctx* factory;
    ZSTDMT_jobDescription* jobs;
    ZSTDMT_bufferPool* bufPool;
    ZSTDMT_CCtxPool* cctxPool;
    ZSTD_pthread_mutex_t jobCompleted_mutex;
    ZSTD_pthread_cond_t jobCompleted_cond;
    ZSTD_CCtx_params params;
    size_t targetSectionSize;
    size_t inBuffSize;
    size_t prefixSize;
    size_t targetPrefixSize;
    inBuff_t inBuff;
    int jobReady;        /* 1 => one job is already prepared, but pool has shortage of workers. Don't create another one. */
    XXH64_state_t xxhState;
    unsigned singleBlockingThread;
    unsigned jobIDMask;
    unsigned doneJobID;
    unsigned nextJobID;
    unsigned frameEnded;
    unsigned allJobsCompleted;
    unsigned long long frameContentSize;
    unsigned long long consumed;
    unsigned long long produced;
    ZSTD_customMem cMem;
    ZSTD_CDict* cdictLocal;
    const ZSTD_CDict* cdict;
};

/* Sets parameters relevant to the compression job, initializing others to
 * default values. Notably, nbThreads should probably be zero. */
static ZSTD_CCtx_params ZSTDMT_initJobCCtxParams(ZSTD_CCtx_params const params)
{
    ZSTD_CCtx_params jobParams;
    memset(&jobParams, 0, sizeof(jobParams));

    jobParams.cParams = params.cParams;
    jobParams.fParams = params.fParams;
    jobParams.compressionLevel = params.compressionLevel;

    jobParams.ldmParams = params.ldmParams;
    return jobParams;
}

static ZSTDMT_jobDescription* ZSTDMT_allocJobsTable(U32* nbJobsPtr, ZSTD_customMem cMem)
{
    U32 const nbJobsLog2 = ZSTD_highbit32(*nbJobsPtr) + 1;
    U32 const nbJobs = 1 << nbJobsLog2;
    *nbJobsPtr = nbJobs;
    return (ZSTDMT_jobDescription*) ZSTD_calloc(
                            nbJobs * sizeof(ZSTDMT_jobDescription), cMem);
}

/* ZSTDMT_CCtxParam_setNbThreads():
 * Internal use only */
size_t ZSTDMT_CCtxParam_setNbThreads(ZSTD_CCtx_params* params, unsigned nbThreads)
{
    if (nbThreads > ZSTDMT_NBTHREADS_MAX) nbThreads = ZSTDMT_NBTHREADS_MAX;
    if (nbThreads < 1) nbThreads = 1;
    params->nbThreads = nbThreads;
    params->overlapSizeLog = ZSTDMT_OVERLAPLOG_DEFAULT;
    params->jobSize = 0;
    return nbThreads;
}

ZSTDMT_CCtx* ZSTDMT_createCCtx_advanced(unsigned nbThreads, ZSTD_customMem cMem)
{
    ZSTDMT_CCtx* mtctx;
    U32 nbJobs = nbThreads + 2;
    DEBUGLOG(3, "ZSTDMT_createCCtx_advanced (nbThreads = %u)", nbThreads);

    if (nbThreads < 1) return NULL;
    nbThreads = MIN(nbThreads , ZSTDMT_NBTHREADS_MAX);
    if ((cMem.customAlloc!=NULL) ^ (cMem.customFree!=NULL))
        /* invalid custom allocator */
        return NULL;

    mtctx = (ZSTDMT_CCtx*) ZSTD_calloc(sizeof(ZSTDMT_CCtx), cMem);
    if (!mtctx) return NULL;
    ZSTDMT_CCtxParam_setNbThreads(&mtctx->params, nbThreads);
    mtctx->cMem = cMem;
    mtctx->allJobsCompleted = 1;
    mtctx->factory = POOL_create_advanced(nbThreads, 0, cMem);
    mtctx->jobs = ZSTDMT_allocJobsTable(&nbJobs, cMem);
    mtctx->jobIDMask = nbJobs - 1;
    mtctx->bufPool = ZSTDMT_createBufferPool(nbThreads, cMem);
    mtctx->cctxPool = ZSTDMT_createCCtxPool(nbThreads, cMem);
    if (!mtctx->factory | !mtctx->jobs | !mtctx->bufPool | !mtctx->cctxPool) {
        ZSTDMT_freeCCtx(mtctx);
        return NULL;
    }
    if (ZSTD_pthread_mutex_init(&mtctx->jobCompleted_mutex, NULL)) {
        ZSTDMT_freeCCtx(mtctx);
        return NULL;
    }
    if (ZSTD_pthread_cond_init(&mtctx->jobCompleted_cond, NULL)) {
        ZSTDMT_freeCCtx(mtctx);
        return NULL;
    }
    DEBUGLOG(3, "mt_cctx created, for %u threads", nbThreads);
    return mtctx;
}

ZSTDMT_CCtx* ZSTDMT_createCCtx(unsigned nbThreads)
{
    return ZSTDMT_createCCtx_advanced(nbThreads, ZSTD_defaultCMem);
}


/* ZSTDMT_releaseAllJobResources() :
 * note : ensure all workers are killed first ! */
static void ZSTDMT_releaseAllJobResources(ZSTDMT_CCtx* mtctx)
{
    unsigned jobID;
    DEBUGLOG(3, "ZSTDMT_releaseAllJobResources");
    for (jobID=0; jobID <= mtctx->jobIDMask; jobID++) {
        DEBUGLOG(4, "job%02u: release dst address %08X", jobID, (U32)(size_t)mtctx->jobs[jobID].dstBuff.start);
        ZSTDMT_releaseBuffer(mtctx->bufPool, mtctx->jobs[jobID].dstBuff);
        mtctx->jobs[jobID].dstBuff = g_nullBuffer;
        DEBUGLOG(4, "job%02u: release src address %08X", jobID, (U32)(size_t)mtctx->jobs[jobID].src.start);
        ZSTDMT_releaseBuffer(mtctx->bufPool, mtctx->jobs[jobID].src);
        mtctx->jobs[jobID].src = g_nullBuffer;
    }
    memset(mtctx->jobs, 0, (mtctx->jobIDMask+1)*sizeof(ZSTDMT_jobDescription));
    DEBUGLOG(4, "input: release address %08X", (U32)(size_t)mtctx->inBuff.buffer.start);
    ZSTDMT_releaseBuffer(mtctx->bufPool, mtctx->inBuff.buffer);
    mtctx->inBuff.buffer = g_nullBuffer;
    mtctx->allJobsCompleted = 1;
}

static void ZSTDMT_waitForAllJobsCompleted(ZSTDMT_CCtx* zcs)
{
    DEBUGLOG(4, "ZSTDMT_waitForAllJobsCompleted");
    while (zcs->doneJobID < zcs->nextJobID) {
        unsigned const jobID = zcs->doneJobID & zcs->jobIDMask;
        ZSTD_PTHREAD_MUTEX_LOCK(&zcs->jobCompleted_mutex);
        while (zcs->jobs[jobID].jobCompleted==0) {
            DEBUGLOG(5, "waiting for jobCompleted signal from chunk %u", zcs->doneJobID);   /* we want to block when waiting for data to flush */
            ZSTD_pthread_cond_wait(&zcs->jobCompleted_cond, &zcs->jobCompleted_mutex);
        }
        ZSTD_pthread_mutex_unlock(&zcs->jobCompleted_mutex);
        zcs->doneJobID++;
    }
}

size_t ZSTDMT_freeCCtx(ZSTDMT_CCtx* mtctx)
{
    if (mtctx==NULL) return 0;   /* compatible with free on NULL */
    POOL_free(mtctx->factory);   /* stop and free worker threads */
    ZSTDMT_releaseAllJobResources(mtctx);  /* release job resources into pools first */
    ZSTD_free(mtctx->jobs, mtctx->cMem);
    ZSTDMT_freeBufferPool(mtctx->bufPool);
    ZSTDMT_freeCCtxPool(mtctx->cctxPool);
    ZSTD_freeCDict(mtctx->cdictLocal);
    ZSTD_pthread_mutex_destroy(&mtctx->jobCompleted_mutex);
    ZSTD_pthread_cond_destroy(&mtctx->jobCompleted_cond);
    ZSTD_free(mtctx, mtctx->cMem);
    return 0;
}

size_t ZSTDMT_sizeof_CCtx(ZSTDMT_CCtx* mtctx)
{
    if (mtctx == NULL) return 0;   /* supports sizeof NULL */
    return sizeof(*mtctx)
            + POOL_sizeof(mtctx->factory)
            + ZSTDMT_sizeof_bufferPool(mtctx->bufPool)
            + (mtctx->jobIDMask+1) * sizeof(ZSTDMT_jobDescription)
            + ZSTDMT_sizeof_CCtxPool(mtctx->cctxPool)
            + ZSTD_sizeof_CDict(mtctx->cdictLocal);
}

/* Internal only */
size_t ZSTDMT_CCtxParam_setMTCtxParameter(ZSTD_CCtx_params* params,
                                ZSTDMT_parameter parameter, unsigned value) {
    DEBUGLOG(4, "ZSTDMT_CCtxParam_setMTCtxParameter");
    switch(parameter)
    {
    case ZSTDMT_p_jobSize :
        DEBUGLOG(4, "ZSTDMT_CCtxParam_setMTCtxParameter : set jobSize to %u", value);
        if ( (value > 0)  /* value==0 => automatic job size */
           & (value < ZSTDMT_JOBSIZE_MIN) )
            value = ZSTDMT_JOBSIZE_MIN;
        params->jobSize = value;
        return value;
    case ZSTDMT_p_overlapSectionLog :
        if (value > 9) value = 9;
        DEBUGLOG(4, "ZSTDMT_p_overlapSectionLog : %u", value);
        params->overlapSizeLog = (value >= 9) ? 9 : value;
        return value;
    default :
        return ERROR(parameter_unsupported);
    }
}

size_t ZSTDMT_setMTCtxParameter(ZSTDMT_CCtx* mtctx, ZSTDMT_parameter parameter, unsigned value)
{
    DEBUGLOG(4, "ZSTDMT_setMTCtxParameter");
    switch(parameter)
    {
    case ZSTDMT_p_jobSize :
        return ZSTDMT_CCtxParam_setMTCtxParameter(&mtctx->params, parameter, value);
    case ZSTDMT_p_overlapSectionLog :
        return ZSTDMT_CCtxParam_setMTCtxParameter(&mtctx->params, parameter, value);
    default :
        return ERROR(parameter_unsupported);
    }
}

/* ZSTDMT_getNbThreads():
 * @return nb threads currently active in mtctx.
 * mtctx must be valid */
unsigned ZSTDMT_getNbThreads(const ZSTDMT_CCtx* mtctx)
{
    assert(mtctx != NULL);
    return mtctx->params.nbThreads;
}

/* ZSTDMT_getFrameProgression():
 * tells how much data has been consumed (input) and produced (output) for current frame.
 * able to count progression inside worker threads.
 * Note : mutex will be acquired during statistics collection. */
ZSTD_frameProgression ZSTDMT_getFrameProgression(ZSTDMT_CCtx* mtctx)
{
    ZSTD_frameProgression fs;
    DEBUGLOG(6, "ZSTDMT_getFrameProgression");
    ZSTD_PTHREAD_MUTEX_LOCK(&mtctx->jobCompleted_mutex);
    fs.consumed = mtctx->consumed;
    fs.produced = mtctx->produced;
    assert(mtctx->inBuff.filled >= mtctx->prefixSize);
    fs.ingested = mtctx->consumed + (mtctx->inBuff.filled - mtctx->prefixSize);
    {   unsigned jobNb;
        unsigned lastJobNb = mtctx->nextJobID + mtctx->jobReady; assert(mtctx->jobReady <= 1);
        DEBUGLOG(6, "ZSTDMT_getFrameProgression: jobs: from %u to <%u (jobReady:%u)",
                    mtctx->doneJobID, lastJobNb, mtctx->jobReady)
        for (jobNb = mtctx->doneJobID ; jobNb < lastJobNb ; jobNb++) {
            unsigned const wJobID = jobNb & mtctx->jobIDMask;
            size_t const cResult = mtctx->jobs[wJobID].cSize;
            size_t const produced = ZSTD_isError(cResult) ? 0 : cResult;
            fs.consumed += mtctx->jobs[wJobID].consumed;
            fs.ingested += mtctx->jobs[wJobID].srcSize;
            fs.produced += produced;
        }
    }
    ZSTD_pthread_mutex_unlock(&mtctx->jobCompleted_mutex);
    return fs;
}


/* ------------------------------------------ */
/* =====   Multi-threaded compression   ===== */
/* ------------------------------------------ */

static unsigned ZSTDMT_computeNbChunks(size_t srcSize, unsigned windowLog, unsigned nbThreads) {
    assert(nbThreads>0);
    {   size_t const chunkSizeTarget = (size_t)1 << (windowLog + 2);
        size_t const chunkMaxSize = chunkSizeTarget << 2;
        size_t const passSizeMax = chunkMaxSize * nbThreads;
        unsigned const multiplier = (unsigned)(srcSize / passSizeMax) + 1;
        unsigned const nbChunksLarge = multiplier * nbThreads;
        unsigned const nbChunksMax = (unsigned)(srcSize / chunkSizeTarget) + 1;
        unsigned const nbChunksSmall = MIN(nbChunksMax, nbThreads);
        return (multiplier>1) ? nbChunksLarge : nbChunksSmall;
}   }

/* ZSTDMT_compress_advanced_internal() :
 * This is a blocking function : it will only give back control to caller after finishing its compression job.
 */
static size_t ZSTDMT_compress_advanced_internal(
                ZSTDMT_CCtx* mtctx,
                void* dst, size_t dstCapacity,
          const void* src, size_t srcSize,
          const ZSTD_CDict* cdict,
                ZSTD_CCtx_params const params)
{
    ZSTD_CCtx_params const jobParams = ZSTDMT_initJobCCtxParams(params);
    unsigned const overlapRLog = (params.overlapSizeLog>9) ? 0 : 9-params.overlapSizeLog;
    size_t const overlapSize = (overlapRLog>=9) ? 0 : (size_t)1 << (params.cParams.windowLog - overlapRLog);
    unsigned nbChunks = ZSTDMT_computeNbChunks(srcSize, params.cParams.windowLog, params.nbThreads);
    size_t const proposedChunkSize = (srcSize + (nbChunks-1)) / nbChunks;
    size_t const avgChunkSize = (((proposedChunkSize-1) & 0x1FFFF) < 0x7FFF) ? proposedChunkSize + 0xFFFF : proposedChunkSize;   /* avoid too small last block */
    const char* const srcStart = (const char*)src;
    size_t remainingSrcSize = srcSize;
    unsigned const compressWithinDst = (dstCapacity >= ZSTD_compressBound(srcSize)) ? nbChunks : (unsigned)(dstCapacity / ZSTD_compressBound(avgChunkSize));  /* presumes avgChunkSize >= 256 KB, which should be the case */
    size_t frameStartPos = 0, dstBufferPos = 0;
    XXH64_state_t xxh64;
    assert(jobParams.nbThreads == 0);
    assert(mtctx->cctxPool->totalCCtx == params.nbThreads);

    DEBUGLOG(4, "ZSTDMT_compress_advanced_internal: nbChunks=%2u (rawSize=%u bytes; fixedSize=%u) ",
                nbChunks, (U32)proposedChunkSize, (U32)avgChunkSize);

    if ((nbChunks==1) | (params.nbThreads<=1)) {   /* fallback to single-thread mode : this is a blocking invocation anyway */
        ZSTD_CCtx* const cctx = mtctx->cctxPool->cctx[0];
        if (cdict) return ZSTD_compress_usingCDict_advanced(cctx, dst, dstCapacity, src, srcSize, cdict, jobParams.fParams);
        return ZSTD_compress_advanced_internal(cctx, dst, dstCapacity, src, srcSize, NULL, 0, jobParams);
    }

    assert(avgChunkSize >= 256 KB);  /* condition for ZSTD_compressBound(A) + ZSTD_compressBound(B) <= ZSTD_compressBound(A+B), required to compress directly into Dst (no additional buffer) */
    ZSTDMT_setBufferSize(mtctx->bufPool, ZSTD_compressBound(avgChunkSize) );
    XXH64_reset(&xxh64, 0);

    if (nbChunks > mtctx->jobIDMask+1) {  /* enlarge job table */
        U32 nbJobs = nbChunks;
        ZSTD_free(mtctx->jobs, mtctx->cMem);
        mtctx->jobIDMask = 0;
        mtctx->jobs = ZSTDMT_allocJobsTable(&nbJobs, mtctx->cMem);
        if (mtctx->jobs==NULL) return ERROR(memory_allocation);
        assert((nbJobs != 0) && ((nbJobs & (nbJobs - 1)) == 0));  /* ensure nbJobs is a power of 2 */
        mtctx->jobIDMask = nbJobs - 1;
    }

    {   unsigned u;
        for (u=0; u<nbChunks; u++) {
            size_t const chunkSize = MIN(remainingSrcSize, avgChunkSize);
            size_t const dstBufferCapacity = ZSTD_compressBound(chunkSize);
            buffer_t const dstAsBuffer = { (char*)dst + dstBufferPos, dstBufferCapacity };
            buffer_t const dstBuffer = u < compressWithinDst ? dstAsBuffer : g_nullBuffer;
            size_t dictSize = u ? overlapSize : 0;

            mtctx->jobs[u].src = g_nullBuffer;
            mtctx->jobs[u].srcStart = srcStart + frameStartPos - dictSize;
            mtctx->jobs[u].prefixSize = dictSize;
            mtctx->jobs[u].srcSize = chunkSize;
            mtctx->jobs[u].consumed = 0;
            mtctx->jobs[u].cSize = 0;
            mtctx->jobs[u].cdict = (u==0) ? cdict : NULL;
            mtctx->jobs[u].fullFrameSize = srcSize;
            mtctx->jobs[u].params = jobParams;
            /* do not calculate checksum within sections, but write it in header for first section */
            if (u!=0) mtctx->jobs[u].params.fParams.checksumFlag = 0;
            mtctx->jobs[u].dstBuff = dstBuffer;
            mtctx->jobs[u].cctxPool = mtctx->cctxPool;
            mtctx->jobs[u].bufPool = mtctx->bufPool;
            mtctx->jobs[u].firstChunk = (u==0);
            mtctx->jobs[u].lastChunk = (u==nbChunks-1);
            mtctx->jobs[u].jobCompleted = 0;
            mtctx->jobs[u].jobCompleted_mutex = &mtctx->jobCompleted_mutex;
            mtctx->jobs[u].jobCompleted_cond = &mtctx->jobCompleted_cond;

            if (params.fParams.checksumFlag) {
                XXH64_update(&xxh64, srcStart + frameStartPos, chunkSize);
            }

            DEBUGLOG(5, "ZSTDMT_compress_advanced_internal: posting job %u  (%u bytes)", u, (U32)chunkSize);
            DEBUG_PRINTHEX(6, mtctx->jobs[u].srcStart, 12);
            POOL_add(mtctx->factory, ZSTDMT_compressChunk, &mtctx->jobs[u]);

            frameStartPos += chunkSize;
            dstBufferPos += dstBufferCapacity;
            remainingSrcSize -= chunkSize;
    }   }

    /* collect result */
    {   size_t error = 0, dstPos = 0;
        unsigned chunkID;
        for (chunkID=0; chunkID<nbChunks; chunkID++) {
            DEBUGLOG(5, "waiting for chunk %u ", chunkID);
            ZSTD_PTHREAD_MUTEX_LOCK(&mtctx->jobCompleted_mutex);
            while (mtctx->jobs[chunkID].jobCompleted==0) {
                DEBUGLOG(5, "waiting for jobCompleted signal from chunk %u", chunkID);
                ZSTD_pthread_cond_wait(&mtctx->jobCompleted_cond, &mtctx->jobCompleted_mutex);
            }
            ZSTD_pthread_mutex_unlock(&mtctx->jobCompleted_mutex);
            DEBUGLOG(5, "ready to write chunk %u ", chunkID);

            mtctx->jobs[chunkID].srcStart = NULL;
            {   size_t const cSize = mtctx->jobs[chunkID].cSize;
                if (ZSTD_isError(cSize)) error = cSize;
                if ((!error) && (dstPos + cSize > dstCapacity)) error = ERROR(dstSize_tooSmall);
                if (chunkID) {   /* note : chunk 0 is written directly at dst, which is correct position */
                    if (!error)
                        memmove((char*)dst + dstPos, mtctx->jobs[chunkID].dstBuff.start, cSize);  /* may overlap when chunk compressed within dst */
                    if (chunkID >= compressWithinDst) {  /* chunk compressed into its own buffer, which must be released */
                        DEBUGLOG(5, "releasing buffer %u>=%u", chunkID, compressWithinDst);
                        ZSTDMT_releaseBuffer(mtctx->bufPool, mtctx->jobs[chunkID].dstBuff);
                }   }
                mtctx->jobs[chunkID].dstBuff = g_nullBuffer;
                dstPos += cSize ;
            }
        }  /* for (chunkID=0; chunkID<nbChunks; chunkID++) */

        DEBUGLOG(4, "checksumFlag : %u ", params.fParams.checksumFlag);
        if (params.fParams.checksumFlag) {
            U32 const checksum = (U32)XXH64_digest(&xxh64);
            if (dstPos + 4 > dstCapacity) {
                error = ERROR(dstSize_tooSmall);
            } else {
                DEBUGLOG(4, "writing checksum : %08X \n", checksum);
                MEM_writeLE32((char*)dst + dstPos, checksum);
                dstPos += 4;
        }   }

        if (!error) DEBUGLOG(4, "compressed size : %u  ", (U32)dstPos);
        return error ? error : dstPos;
    }
}

size_t ZSTDMT_compress_advanced(ZSTDMT_CCtx* mtctx,
                               void* dst, size_t dstCapacity,
                         const void* src, size_t srcSize,
                         const ZSTD_CDict* cdict,
                               ZSTD_parameters const params,
                               unsigned overlapLog)
{
    ZSTD_CCtx_params cctxParams = mtctx->params;
    cctxParams.cParams = params.cParams;
    cctxParams.fParams = params.fParams;
    cctxParams.overlapSizeLog = overlapLog;
    return ZSTDMT_compress_advanced_internal(mtctx,
                                             dst, dstCapacity,
                                             src, srcSize,
                                             cdict, cctxParams);
}


size_t ZSTDMT_compressCCtx(ZSTDMT_CCtx* mtctx,
                           void* dst, size_t dstCapacity,
                     const void* src, size_t srcSize,
                           int compressionLevel)
{
    U32 const overlapLog = (compressionLevel >= ZSTD_maxCLevel()) ? 9 : ZSTDMT_OVERLAPLOG_DEFAULT;
    ZSTD_parameters params = ZSTD_getParams(compressionLevel, srcSize, 0);
    params.fParams.contentSizeFlag = 1;
    return ZSTDMT_compress_advanced(mtctx, dst, dstCapacity, src, srcSize, NULL, params, overlapLog);
}


/* ====================================== */
/* =======      Streaming API     ======= */
/* ====================================== */

size_t ZSTDMT_initCStream_internal(
        ZSTDMT_CCtx* zcs,
        const void* dict, size_t dictSize, ZSTD_dictMode_e dictMode,
        const ZSTD_CDict* cdict, ZSTD_CCtx_params params,
        unsigned long long pledgedSrcSize)
{
    DEBUGLOG(4, "ZSTDMT_initCStream_internal (pledgedSrcSize=%u)", (U32)pledgedSrcSize);
    /* params are supposed to be fully validated at this point */
    assert(!ZSTD_isError(ZSTD_checkCParams(params.cParams)));
    assert(!((dict) && (cdict)));  /* either dict or cdict, not both */
    assert(zcs->cctxPool->totalCCtx == params.nbThreads);
    zcs->singleBlockingThread = (pledgedSrcSize <= ZSTDMT_JOBSIZE_MIN);  /* do not trigger multi-threading when srcSize is too small */
    if (params.jobSize == 0) {
        if (params.cParams.windowLog >= 29)
            params.jobSize = ZSTDMT_JOBSIZE_MAX;
        else
            params.jobSize = 1 << (params.cParams.windowLog + 2);
    }
    if (params.jobSize > ZSTDMT_JOBSIZE_MAX) params.jobSize = ZSTDMT_JOBSIZE_MAX;

    if (zcs->singleBlockingThread) {
        ZSTD_CCtx_params const singleThreadParams = ZSTDMT_initJobCCtxParams(params);
        DEBUGLOG(4, "ZSTDMT_initCStream_internal: switch to single blocking thread mode");
        assert(singleThreadParams.nbThreads == 0);
        return ZSTD_initCStream_internal(zcs->cctxPool->cctx[0],
                                         dict, dictSize, cdict,
                                         singleThreadParams, pledgedSrcSize);
    }
    DEBUGLOG(4, "ZSTDMT_initCStream_internal: %u threads", params.nbThreads);

    if (zcs->allJobsCompleted == 0) {   /* previous compression not correctly finished */
        ZSTDMT_waitForAllJobsCompleted(zcs);
        ZSTDMT_releaseAllJobResources(zcs);
        zcs->allJobsCompleted = 1;
    }

    zcs->params = params;
    zcs->frameContentSize = pledgedSrcSize;
    if (dict) {
        ZSTD_freeCDict(zcs->cdictLocal);
        zcs->cdictLocal = ZSTD_createCDict_advanced(dict, dictSize,
                                                    ZSTD_dlm_byCopy, dictMode, /* note : a loadPrefix becomes an internal CDict */
                                                    params.cParams, zcs->cMem);
        zcs->cdict = zcs->cdictLocal;
        if (zcs->cdictLocal == NULL) return ERROR(memory_allocation);
    } else {
        ZSTD_freeCDict(zcs->cdictLocal);
        zcs->cdictLocal = NULL;
        zcs->cdict = cdict;
    }

    assert(params.overlapSizeLog <= 9);
    zcs->targetPrefixSize = (params.overlapSizeLog==0) ? 0 : (size_t)1 << (params.cParams.windowLog - (9 - params.overlapSizeLog));
    DEBUGLOG(4, "overlapLog=%u => %u KB", params.overlapSizeLog, (U32)(zcs->targetPrefixSize>>10));
    zcs->targetSectionSize = params.jobSize;
    if (zcs->targetSectionSize < ZSTDMT_JOBSIZE_MIN) zcs->targetSectionSize = ZSTDMT_JOBSIZE_MIN;
    if (zcs->targetSectionSize < zcs->targetPrefixSize) zcs->targetSectionSize = zcs->targetPrefixSize;  /* job size must be >= overlap size */
    DEBUGLOG(4, "Job Size : %u KB (note : set to %u)", (U32)(zcs->targetSectionSize>>10), params.jobSize);
    zcs->inBuffSize = zcs->targetPrefixSize + zcs->targetSectionSize;
    DEBUGLOG(4, "inBuff Size : %u KB", (U32)(zcs->inBuffSize>>10));
    ZSTDMT_setBufferSize(zcs->bufPool, MAX(zcs->inBuffSize, ZSTD_compressBound(zcs->targetSectionSize)) );
    zcs->inBuff.buffer = g_nullBuffer;
    zcs->prefixSize = 0;
    zcs->doneJobID = 0;
    zcs->nextJobID = 0;
    zcs->frameEnded = 0;
    zcs->allJobsCompleted = 0;
    zcs->consumed = 0;
    zcs->produced = 0;
    if (params.fParams.checksumFlag) XXH64_reset(&zcs->xxhState, 0);
    return 0;
}

size_t ZSTDMT_initCStream_advanced(ZSTDMT_CCtx* mtctx,
                             const void* dict, size_t dictSize,
                                   ZSTD_parameters params,
                                   unsigned long long pledgedSrcSize)
{
    ZSTD_CCtx_params cctxParams = mtctx->params;  /* retrieve sticky params */
    DEBUGLOG(4, "ZSTDMT_initCStream_advanced (pledgedSrcSize=%u)", (U32)pledgedSrcSize);
    cctxParams.cParams = params.cParams;
    cctxParams.fParams = params.fParams;
    return ZSTDMT_initCStream_internal(mtctx, dict, dictSize, ZSTD_dm_auto, NULL,
                                       cctxParams, pledgedSrcSize);
}

size_t ZSTDMT_initCStream_usingCDict(ZSTDMT_CCtx* mtctx,
                               const ZSTD_CDict* cdict,
                                     ZSTD_frameParameters fParams,
                                     unsigned long long pledgedSrcSize)
{
    ZSTD_CCtx_params cctxParams = mtctx->params;
    if (cdict==NULL) return ERROR(dictionary_wrong);   /* method incompatible with NULL cdict */
    cctxParams.cParams = ZSTD_getCParamsFromCDict(cdict);
    cctxParams.fParams = fParams;
    return ZSTDMT_initCStream_internal(mtctx, NULL, 0 /*dictSize*/, ZSTD_dm_auto, cdict,
                                       cctxParams, pledgedSrcSize);
}


/* ZSTDMT_resetCStream() :
 * pledgedSrcSize can be zero == unknown (for the time being)
 * prefer using ZSTD_CONTENTSIZE_UNKNOWN,
 * as `0` might mean "empty" in the future */
size_t ZSTDMT_resetCStream(ZSTDMT_CCtx* zcs, unsigned long long pledgedSrcSize)
{
    if (!pledgedSrcSize) pledgedSrcSize = ZSTD_CONTENTSIZE_UNKNOWN;
    if (zcs->params.nbThreads==1)
        return ZSTD_resetCStream(zcs->cctxPool->cctx[0], pledgedSrcSize);
    return ZSTDMT_initCStream_internal(zcs, NULL, 0, ZSTD_dm_auto, 0, zcs->params,
                                       pledgedSrcSize);
}

size_t ZSTDMT_initCStream(ZSTDMT_CCtx* zcs, int compressionLevel) {
    ZSTD_parameters const params = ZSTD_getParams(compressionLevel, ZSTD_CONTENTSIZE_UNKNOWN, 0);
    ZSTD_CCtx_params cctxParams = zcs->params;   /* retrieve sticky params */
    DEBUGLOG(4, "ZSTDMT_initCStream (cLevel=%i)", compressionLevel);
    cctxParams.cParams = params.cParams;
    cctxParams.fParams = params.fParams;
    return ZSTDMT_initCStream_internal(zcs, NULL, 0, ZSTD_dm_auto, NULL, cctxParams, ZSTD_CONTENTSIZE_UNKNOWN);
}


static size_t ZSTDMT_createCompressionJob(ZSTDMT_CCtx* zcs, size_t srcSize, unsigned endFrame)
{
    unsigned const jobID = zcs->nextJobID & zcs->jobIDMask;

    if (zcs->nextJobID > zcs->doneJobID + zcs->jobIDMask) {
        DEBUGLOG(5, "ZSTDMT_createCompressionJob: will not create new job : table is full");
        assert((zcs->nextJobID & zcs->jobIDMask) == (zcs->doneJobID & zcs->jobIDMask));
        return 0;
    }

    if (!zcs->jobReady) {
        DEBUGLOG(5, "ZSTDMT_createCompressionJob: preparing job %u to compress %u bytes with %u preload ",
                    zcs->nextJobID, (U32)srcSize, (U32)zcs->prefixSize);
        zcs->jobs[jobID].src = zcs->inBuff.buffer;
        zcs->jobs[jobID].srcStart = zcs->inBuff.buffer.start;
        zcs->jobs[jobID].srcSize = srcSize;
        zcs->jobs[jobID].consumed = 0;
        zcs->jobs[jobID].cSize = 0;
        zcs->jobs[jobID].prefixSize = zcs->prefixSize;
        assert(zcs->inBuff.filled >= srcSize + zcs->prefixSize);
        zcs->jobs[jobID].params = zcs->params;
        /* do not calculate checksum within sections, but write it in header for first section */
        if (zcs->nextJobID) zcs->jobs[jobID].params.fParams.checksumFlag = 0;
        zcs->jobs[jobID].cdict = zcs->nextJobID==0 ? zcs->cdict : NULL;
        zcs->jobs[jobID].fullFrameSize = zcs->frameContentSize;
        zcs->jobs[jobID].dstBuff = g_nullBuffer;
        zcs->jobs[jobID].cctxPool = zcs->cctxPool;
        zcs->jobs[jobID].bufPool = zcs->bufPool;
        zcs->jobs[jobID].firstChunk = (zcs->nextJobID==0);
        zcs->jobs[jobID].lastChunk = endFrame;
        zcs->jobs[jobID].jobCompleted = 0;
        zcs->jobs[jobID].frameChecksumNeeded = endFrame && (zcs->nextJobID>0) && zcs->params.fParams.checksumFlag;
        zcs->jobs[jobID].dstFlushed = 0;
        zcs->jobs[jobID].jobCompleted_mutex = &zcs->jobCompleted_mutex;
        zcs->jobs[jobID].jobCompleted_cond = &zcs->jobCompleted_cond;

        if (zcs->params.fParams.checksumFlag)
            XXH64_update(&zcs->xxhState, (const char*)zcs->inBuff.buffer.start + zcs->prefixSize, srcSize);

        /* get a new buffer for next input */
        if (!endFrame) {
            size_t const newPrefixSize = MIN(srcSize + zcs->prefixSize, zcs->targetPrefixSize);
            zcs->inBuff.buffer = ZSTDMT_getBuffer(zcs->bufPool);
            if (zcs->inBuff.buffer.start == NULL) {   /* not enough memory to allocate next input buffer */
                zcs->jobs[jobID].jobCompleted = 1;
                zcs->nextJobID++;
                ZSTDMT_waitForAllJobsCompleted(zcs);
                ZSTDMT_releaseAllJobResources(zcs);
                return ERROR(memory_allocation);
            }
            zcs->inBuff.filled -= srcSize + zcs->prefixSize - newPrefixSize;
            memmove(zcs->inBuff.buffer.start,   /* copy end of current job into next job, as "prefix" */
                (const char*)zcs->jobs[jobID].srcStart + zcs->prefixSize + srcSize - newPrefixSize,
                zcs->inBuff.filled);
            zcs->prefixSize = newPrefixSize;
        } else {   /* endFrame==1 => no need for another input buffer */
            zcs->inBuff.buffer = g_nullBuffer;
            zcs->inBuff.filled = 0;
            zcs->prefixSize = 0;
            zcs->frameEnded = 1;
            if (zcs->nextJobID == 0) {
                /* single chunk exception : checksum is calculated directly within worker thread */
                zcs->params.fParams.checksumFlag = 0;
    }   }   }

    DEBUGLOG(5, "ZSTDMT_createCompressionJob: posting job %u : %u bytes  (end:%u) (note : doneJob = %u=>%u)",
                zcs->nextJobID,
                (U32)zcs->jobs[jobID].srcSize,
                zcs->jobs[jobID].lastChunk,
                zcs->doneJobID,
                zcs->doneJobID & zcs->jobIDMask);
    if (POOL_tryAdd(zcs->factory, ZSTDMT_compressChunk, &zcs->jobs[jobID])) {
        zcs->nextJobID++;
        zcs->jobReady = 0;
    } else {
        DEBUGLOG(5, "ZSTDMT_createCompressionJob: no worker available for job %u", zcs->nextJobID);
        zcs->jobReady = 1;
    }
    return 0;
}


/*! ZSTDMT_flushProduced() :
 * `output` : `pos` will be updated with amount of data flushed .
 * `blockToFlush` : if >0, the function will block and wait if there is no data available to flush .
 * @return : amount of data remaining within internal buffer, 0 if no more, 1 if unknown but > 0, or an error code */
static size_t ZSTDMT_flushProduced(ZSTDMT_CCtx* zcs, ZSTD_outBuffer* output, unsigned blockToFlush)
{
    unsigned const wJobID = zcs->doneJobID & zcs->jobIDMask;
    DEBUGLOG(5, "ZSTDMT_flushProduced (blocking:%u)", blockToFlush);
    assert(output->size >= output->pos);

    ZSTD_PTHREAD_MUTEX_LOCK(&zcs->jobCompleted_mutex);
    if (blockToFlush && (zcs->doneJobID < zcs->nextJobID)) {
        while (zcs->jobs[wJobID].dstFlushed == zcs->jobs[wJobID].cSize) {
            if (zcs->jobs[wJobID].jobCompleted==1) break;
            DEBUGLOG(5, "waiting for something to flush from job %u (currently flushed: %u bytes)",
                        zcs->doneJobID, (U32)zcs->jobs[wJobID].dstFlushed);
            ZSTD_pthread_cond_wait(&zcs->jobCompleted_cond, &zcs->jobCompleted_mutex);  /* block when nothing available to flush but more to come */
    }   }

    /* some output is available to be flushed */
    {   ZSTDMT_jobDescription job = zcs->jobs[wJobID];
        ZSTD_pthread_mutex_unlock(&zcs->jobCompleted_mutex);
        if (ZSTD_isError(job.cSize)) {
            DEBUGLOG(5, "ZSTDMT_flushProduced: job %u : compression error detected : %s",
                        zcs->doneJobID, ZSTD_getErrorName(job.cSize));
            ZSTDMT_waitForAllJobsCompleted(zcs);
            ZSTDMT_releaseAllJobResources(zcs);
            return job.cSize;
        }
        /* add frame checksum if necessary (can only happen once) */
        if ( job.jobCompleted
          && job.frameChecksumNeeded ) {
            U32 const checksum = (U32)XXH64_digest(&zcs->xxhState);
            DEBUGLOG(5, "ZSTDMT_flushProduced: writing checksum : %08X \n", checksum);
            MEM_writeLE32((char*)job.dstBuff.start + job.cSize, checksum);
            job.cSize += 4;
            zcs->jobs[wJobID].cSize += 4;
            zcs->jobs[wJobID].frameChecksumNeeded = 0;
        }
        assert(job.cSize >= job.dstFlushed);
        if (job.dstBuff.start != NULL) {  /* one buffer present : some job is ongoing */
            size_t const toWrite = MIN(job.cSize - job.dstFlushed, output->size - output->pos);
            DEBUGLOG(5, "ZSTDMT_flushProduced: Flushing %u bytes from job %u (completion:%.1f%%)",
                        (U32)toWrite, zcs->doneJobID, (double)job.consumed / job.srcSize * 100);
            memcpy((char*)output->dst + output->pos, (const char*)job.dstBuff.start + job.dstFlushed, toWrite);
            output->pos += toWrite;
            job.dstFlushed += toWrite;

            if ( job.jobCompleted
              && (job.dstFlushed == job.cSize) ) {   /* output buffer fully flushed => move to next one */
                DEBUGLOG(5, "Job %u completed (%u bytes), moving to next one",
                        zcs->doneJobID, (U32)job.dstFlushed);
                ZSTDMT_releaseBuffer(zcs->bufPool, job.dstBuff);
                zcs->jobs[wJobID].dstBuff = g_nullBuffer;
                zcs->jobs[wJobID].jobCompleted = 0;
                zcs->consumed += job.srcSize;
                zcs->produced += job.cSize;
                zcs->doneJobID++;
            } else {
                zcs->jobs[wJobID].dstFlushed = job.dstFlushed;   /* remember how much was flushed for next attempt */
        }   }

        /* return value : how many bytes left in buffer ; fake it to 1 when unknown but >0 */
        if (job.cSize > job.dstFlushed) return (job.cSize - job.dstFlushed);
        if (job.srcSize > job.consumed) return 1;   /* current job not completely compressed */
    }
    if (zcs->doneJobID < zcs->nextJobID) return 1;   /* some more jobs to flush */
    if (zcs->jobReady) return 1;   /* at least one more job to do ! */
    if (zcs->inBuff.filled > 0) return 1;   /* input not empty */
    zcs->allJobsCompleted = zcs->frameEnded;   /* last frame entirely flushed */
    return 0;   /* everything flushed */
}


/** ZSTDMT_compressStream_generic() :
 *  internal use only - exposed to be invoked from zstd_compress.c
 *  assumption : output and input are valid (pos <= size)
 * @return : minimum amount of data remaining to flush, 0 if none */
size_t ZSTDMT_compressStream_generic(ZSTDMT_CCtx* mtctx,
                                     ZSTD_outBuffer* output,
                                     ZSTD_inBuffer* input,
                                     ZSTD_EndDirective endOp)
{
    size_t const newJobThreshold = mtctx->prefixSize + mtctx->targetSectionSize;
    unsigned forwardInputProgress = 0;
    DEBUGLOG(5, "ZSTDMT_compressStream_generic (endOp=%u)", (U32)endOp);
    assert(output->pos <= output->size);
    assert(input->pos  <= input->size);

    if (mtctx->singleBlockingThread) {  /* delegate to single-thread (synchronous) */
        return ZSTD_compressStream_generic(mtctx->cctxPool->cctx[0], output, input, endOp);
    }

    if ((mtctx->frameEnded) && (endOp==ZSTD_e_continue)) {
        /* current frame being ended. Only flush/end are allowed */
        return ERROR(stage_wrong);
    }

    /* single-pass shortcut (note : synchronous-mode) */
    if ( (mtctx->nextJobID == 0)     /* just started */
      && (mtctx->inBuff.filled == 0) /* nothing buffered */
      && (endOp == ZSTD_e_end)       /* end order */
      && (output->size - output->pos >= ZSTD_compressBound(input->size - input->pos)) ) { /* enough room */
        size_t const cSize = ZSTDMT_compress_advanced_internal(mtctx,
                (char*)output->dst + output->pos, output->size - output->pos,
                (const char*)input->src + input->pos, input->size - input->pos,
                mtctx->cdict, mtctx->params);
        if (ZSTD_isError(cSize)) return cSize;
        input->pos = input->size;
        output->pos += cSize;
        ZSTDMT_releaseBuffer(mtctx->bufPool, mtctx->inBuff.buffer);  /* was allocated in initStream */
        mtctx->allJobsCompleted = 1;
        mtctx->frameEnded = 1;
        return 0;
    }

    /* fill input buffer */
    if ( (!mtctx->jobReady)
      && (input->size > input->pos) ) {   /* support NULL input */
        if (mtctx->inBuff.buffer.start == NULL) {
            mtctx->inBuff.buffer = ZSTDMT_getBuffer(mtctx->bufPool);  /* note : allocation can fail, in which case, no forward input progress */
            mtctx->inBuff.filled = 0;
            if ( (mtctx->inBuff.buffer.start == NULL)    /* allocation failure */
              && (mtctx->doneJobID == mtctx->nextJobID) ) {  /* and nothing to flush */
                return ERROR(memory_allocation);   /* no forward progress possible => output an error */
        }   }
        if (mtctx->inBuff.buffer.start != NULL) {
            size_t const toLoad = MIN(input->size - input->pos, mtctx->inBuffSize - mtctx->inBuff.filled);
            DEBUGLOG(5, "ZSTDMT_compressStream_generic: adding %u bytes on top of %u to buffer of size %u",
                        (U32)toLoad, (U32)mtctx->inBuff.filled, (U32)mtctx->inBuffSize);
            memcpy((char*)mtctx->inBuff.buffer.start + mtctx->inBuff.filled, (const char*)input->src + input->pos, toLoad);
            input->pos += toLoad;
            mtctx->inBuff.filled += toLoad;
            forwardInputProgress = toLoad>0;
        }
        if ((input->pos < input->size) && (endOp == ZSTD_e_end))
            endOp = ZSTD_e_flush;   /* can't end now : not all input consumed */
    }

    if ( (mtctx->jobReady)
      || (mtctx->inBuff.filled >= newJobThreshold)  /* filled enough : let's compress */
      || ((endOp != ZSTD_e_continue) && (mtctx->inBuff.filled > 0))  /* something to flush : let's go */
      || ((endOp == ZSTD_e_end) && (!mtctx->frameEnded)) ) {   /* must finish the frame with a zero-size block */
        size_t const jobSize = MIN(mtctx->inBuff.filled - mtctx->prefixSize, mtctx->targetSectionSize);
        CHECK_F( ZSTDMT_createCompressionJob(mtctx, jobSize, endOp==ZSTD_e_end) );
    }

    /* check for potential compressed data ready to be flushed */
    {   size_t const remainingToFlush = ZSTDMT_flushProduced(mtctx, output, !forwardInputProgress); /* block if there was no forward input progress */
        if (input->pos < input->size) return MAX(remainingToFlush, 1);  /* input not consumed : do not flush yet */
        return remainingToFlush;
    }
}


size_t ZSTDMT_compressStream(ZSTDMT_CCtx* zcs, ZSTD_outBuffer* output, ZSTD_inBuffer* input)
{
    CHECK_F( ZSTDMT_compressStream_generic(zcs, output, input, ZSTD_e_continue) );

    /* recommended next input size : fill current input buffer */
    return zcs->inBuffSize - zcs->inBuff.filled;   /* note : could be zero when input buffer is fully filled and no more availability to create new job */
}


static size_t ZSTDMT_flushStream_internal(ZSTDMT_CCtx* mtctx, ZSTD_outBuffer* output, unsigned endFrame)
{
    size_t const srcSize = mtctx->inBuff.filled - mtctx->prefixSize;
    DEBUGLOG(5, "ZSTDMT_flushStream_internal");

    if ( mtctx->jobReady     /* one job ready for a worker to pick up */
      || (srcSize > 0)       /* still some data within input buffer */
      || (endFrame && !mtctx->frameEnded)) {  /* need a last 0-size block to end frame */
           DEBUGLOG(5, "ZSTDMT_flushStream_internal : create a new job (%u bytes, end:%u)",
                        (U32)srcSize, endFrame);
        CHECK_F( ZSTDMT_createCompressionJob(mtctx, srcSize, endFrame) );
    }

    /* check if there is any data available to flush */
    return ZSTDMT_flushProduced(mtctx, output, 1 /* blockToFlush */);
}


size_t ZSTDMT_flushStream(ZSTDMT_CCtx* mtctx, ZSTD_outBuffer* output)
{
    DEBUGLOG(5, "ZSTDMT_flushStream");
    if (mtctx->singleBlockingThread)
        return ZSTD_flushStream(mtctx->cctxPool->cctx[0], output);
    return ZSTDMT_flushStream_internal(mtctx, output, 0 /* endFrame */);
}

size_t ZSTDMT_endStream(ZSTDMT_CCtx* mtctx, ZSTD_outBuffer* output)
{
    DEBUGLOG(4, "ZSTDMT_endStream");
    if (mtctx->singleBlockingThread)
        return ZSTD_endStream(mtctx->cctxPool->cctx[0], output);
    return ZSTDMT_flushStream_internal(mtctx, output, 1 /* endFrame */);
}
