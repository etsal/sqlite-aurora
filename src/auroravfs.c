/*
 * Modified by Emil Tsalapatis on 2023-06-01.
 * Created by Rakeeb Hossain on 2023-04-08.
 */

/*
**
 * This is an in-memory VFS implementation that uses an application-supplied
 * virtual memory address. This can be mmap'd ahead of time.
**
** Shared memory is implemented using the usual os_unix VFS, so WAL is enabled
** and can be used.
**
** USAGE:
**
**    sqlite3_open_v2("file:/whatever?ptr=0xf05538&sz=14336&max=65536", &db,
**                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI,
**                    "auroravfs");
**
** These are the query parameters:
**
**    ptr=          The address of the memory buffer that holds the database.
**
**    sz=           The current size the database file
**
**    maxsz=        The maximum size of the database.  In other words, the
**                  amount of space allocated for the ptr= buffer.
**
**    freeonclose=  If true, then sqlite3_free() is called on the ptr=
**                  value when the connection closes.
**
** The ptr= and sz= query parameters are required.  If maxsz= is omitted,
** then it defaults to the sz= value.  Parameter values can be in either
** decimal or hexadecimal.  The filename in the URI is ignored.
*/
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sls_wal.h>

/*
** Forward declaration of objects used by this utility
*/
typedef struct sqlite3_vfs AuroraVfs;
typedef struct AuroraFile AuroraFile;

/* Access to a lower-level VFS that (might) implement dynamic loading,
** access to randomness, etc.
*/
#define ORIGVFS(p) ((sqlite3_vfs*)((p)->pAppData))
#define ORIGFILE(p) ((sqlite3_file*)(((AuroraFile*)(p))+1))

static char *mainDbName = NULL;

/* An open file */
struct AuroraFile {
    sqlite3_file base;              /* IO methods */
    sqlite3_int64 sz;               /* Size of the file */
    sqlite3_int64 szMax;            /* Space allocated to aData */
    unsigned char *aData;           /* content of the file */
    sqlite3_file *pReal;            /* The real underlying file */
    int isAurMmap;                  /* Should we use Aurora methods or fallback to underlying VFS? */
    char *fileName;                 /* Name of file */
    sqlite_int64 szWritten;	    /* Bytes written since last snapshot */
    sqlite_uint64 szThreshold;	    /* Checkpointing threshold */
    bool bCkptOnSync;	    	    /* Checkpoint on xSync()? */
    int fd;                         /* Aurora SAS fd */
};

/*
** Methods for AuroraFile
*/
static int auroraClose(sqlite3_file*);
static int auroraRead(sqlite3_file*, void*, int iAmt, sqlite3_int64 iOfst);
static int auroraWrite(sqlite3_file*,const void*,int iAmt, sqlite3_int64 iOfst);
static int auroraTruncate(sqlite3_file*, sqlite3_int64 size);
static int auroraSync(sqlite3_file*, int flags);
static int auroraFileSize(sqlite3_file*, sqlite3_int64 *pSize);
static int auroraLock(sqlite3_file*, int);
static int auroraUnlock(sqlite3_file*, int);
static int auroraCheckReservedLock(sqlite3_file*, int *pResOut);
static int auroraFileControl(sqlite3_file*, int op, void *pArg);
static int auroraSectorSize(sqlite3_file*);
static int auroraDeviceCharacteristics(sqlite3_file*);
static int auroraShmMap(sqlite3_file*, int iPg, int pgsz, int, void volatile**);
static int auroraShmLock(sqlite3_file*, int offset, int n, int flags);
static void auroraShmBarrier(sqlite3_file*);
static int auroraShmUnmap(sqlite3_file*, int deleteFlag);
static int auroraFetch(sqlite3_file*, sqlite3_int64 iOfst, int iAmt, void **pp);
static int auroraUnfetch(sqlite3_file*, sqlite3_int64 iOfst, void *p);

/*
** Methods for AuroraVfs
*/
static int auroraOpen(sqlite3_vfs*, const char *, sqlite3_file*, int , int *);
static int auroraDelete(sqlite3_vfs*, const char *zName, int syncDir);
static int auroraAccess(sqlite3_vfs*, const char *zName, int flags, int *);
static int auroraFullPathname(sqlite3_vfs*, const char *zName, int, char *zOut);
static void *auroraDlOpen(sqlite3_vfs*, const char *zFilename);
static void auroraDlError(sqlite3_vfs*, int nByte, char *zErrMsg);
static void (*auroraDlSym(sqlite3_vfs *pVfs, void *p, const char*zSym))(void);
static void auroraDlClose(sqlite3_vfs*, void*);
static int auroraRandomness(sqlite3_vfs*, int nByte, char *zOut);
static int auroraSleep(sqlite3_vfs*, int microseconds);
static int auroraCurrentTime(sqlite3_vfs*, double*);
static int auroraGetLastError(sqlite3_vfs*, int, char *);
static int auroraCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64*);

static sqlite3_vfs aurora_vfs = {
        2,                           /* iVersion */
        0,                           /* szOsFile (set when registered) */
        1024,                        /* mxPathname */
        0,                           /* pNext */
        "auroravfs",                    /* zName */
        0,                           /* pAppData (set when registered) */
        auroraOpen,                     /* xOpen */
        auroraDelete,                   /* xDelete */
        auroraAccess,                   /* xAccess */
        auroraFullPathname,             /* xFullPathname */
        auroraDlOpen,                   /* xDlOpen */
        auroraDlError,                  /* xDlError */
        auroraDlSym,                    /* xDlSym */
        auroraDlClose,                  /* xDlClose */
        auroraRandomness,               /* xRandomness */
        auroraSleep,                    /* xSleep */
        auroraCurrentTime,              /* xCurrentTime */
        auroraGetLastError,             /* xGetLastError */
        auroraCurrentTimeInt64          /* xCurrentTimeInt64 */
};

static const sqlite3_io_methods aurora_io_methods = {
        3,                              /* iVersion */
        auroraClose,                      /* xClose */
        auroraRead,                       /* xRead */
        auroraWrite,                      /* xWrite */
        auroraTruncate,                   /* xTruncate */
        auroraSync,                       /* xSync */
        auroraFileSize,                   /* xFileSize */
        auroraLock,                       /* xLock */
        auroraUnlock,                     /* xUnlock */
        auroraCheckReservedLock,          /* xCheckReservedLock */
        auroraFileControl,                /* xFileControl */
        auroraSectorSize,                 /* xSectorSize */
        auroraDeviceCharacteristics,      /* xDeviceCharacteristics */
        auroraShmMap,                     /* xShmMap */
        auroraShmLock,                    /* xShmLock */
        auroraShmBarrier,                 /* xShmBarrier */
        auroraShmUnmap,                   /* xShmUnmap */
        auroraFetch,                      /* xFetch */
        auroraUnfetch                     /* xUnfetch */
};


/*
** Close an aurora-file.
**
** The pData pointer is owned by the application, so there is nothing
** to free.
*/
static int auroraClose(sqlite3_file *pFile){
    AuroraFile *p = (AuroraFile *)pFile;
    if (!p->isAurMmap)
	    return p->pReal->pMethods->xClose(p->pReal);

    return SQLITE_OK;
}

/*
** Read data from an aurora-file.
*/
static int auroraRead(
        sqlite3_file *pFile,
        void *zBuf,
        int iAmt,
        sqlite_int64 iOfst
){
    AuroraFile *p = (AuroraFile *)pFile;
    if (!p->isAurMmap)
        return p->pReal->pMethods->xRead(p->pReal, zBuf, iAmt, iOfst);

    memcpy(zBuf, p->aData + iOfst, iAmt);
    return SQLITE_OK;
}

/*
** Write data to an aurora-file.
*/
static int auroraWrite(
        sqlite3_file *pFile,
        const void *z,
        int iAmt,
        sqlite_int64 iOfst
){
    const size_t szEnd = iOfst + iAmt;
    int rc;

    AuroraFile *p = (AuroraFile *)pFile;
    if (!p->isAurMmap)
        return p->pReal->pMethods->xWrite(p->pReal, z, iAmt, iOfst);

    if (szEnd > p->szMax)
    	return SQLITE_FULL;

    /* Copy in the data and possibly adjust the file size. */
    p->sz = szEnd > p->sz ? szEnd : p->sz;
    memcpy(p->aData + iOfst, z, iAmt);

    /* Check if we went over the checkpointing threshold. */
    p->szWritten += iAmt;

    /* A 0 threshold turns off xWrite() checkpointing. */
    if (p->szThreshold == 0)
	    return SQLITE_OK;

    if (p->szWritten > p->szThreshold) {
	
	rc = sas_trace_commit(p->fd);
	if (rc < 0)
		return SQLITE_ERROR_SNAPSHOT;

	p->szWritten = 0;
    }

    return SQLITE_OK;
}

/*
** Truncate an aurora-file.
*/
static int auroraTruncate(sqlite3_file *pFile, sqlite_int64 size){
    AuroraFile *p = (AuroraFile *)pFile;
    if (!p->isAurMmap)
        return p->pReal->pMethods->xTruncate(p->pReal, size);

    if (size > p->sz) {
    	if (size > p->szMax)
		return SQLITE_FULL;

    	memset(p->aData+p->sz, 0, size-p->sz);
    }

    p->sz = size;
    return SQLITE_OK;
}

/*
** Sync an aurora-file.
*/
static int auroraSync(sqlite3_file *pFile, int flags){
    int rc;

    AuroraFile *p = (AuroraFile *)pFile;
    if (!p->isAurMmap)
        return p->pReal->pMethods->xSync(p->pReal, flags);

    if (!p->bCkptOnSync || p->szWritten == 0)
	    return SQLITE_OK;

    rc = sas_trace_commit(p->fd);
    if (rc < 0)
	return SQLITE_ERROR_SNAPSHOT;

    p->szWritten = 0;

    return SQLITE_OK;
}

/*
** Return the current file-size of an aurora-file.
*/
static int auroraFileSize(sqlite3_file *pFile, sqlite_int64 *pSize){
    AuroraFile *p = (AuroraFile *)pFile;
    if (!p->isAurMmap)
        return p->pReal->pMethods->xFileSize(p->pReal, pSize);

    *pSize = p->sz;
    return SQLITE_OK;
}

/*
** Lock an aurora-file.
*/
static int auroraLock(sqlite3_file *pFile, int eLock){
    AuroraFile *p = (AuroraFile *)pFile;
    if (!p->isAurMmap)
        p->pReal->pMethods->xLock(p->pReal, eLock);
    
    return SQLITE_OK;
}

/*
** Unlock an aurora-file.
*/
static int auroraUnlock(sqlite3_file *pFile, int eLock){
    AuroraFile *p = (AuroraFile *)pFile;
    if (!p->isAurMmap)
	p->pReal->pMethods->xUnlock(p->pReal, eLock);
	
    return SQLITE_OK;
}

/*
** Check if another file-handle holds a RESERVED lock on an aurora-file.
*/
static int auroraCheckReservedLock(sqlite3_file *pFile, int *pResOut){
    AuroraFile *p = (AuroraFile *)pFile;
    if (!p->isAurMmap)
        return p->pReal->pMethods->xCheckReservedLock(p->pReal, pResOut);

    *pResOut = 0;
    return SQLITE_OK;
}

/*
** File control method. For custom operations on an aurora-file.
*/
static int auroraFileControl(sqlite3_file *pFile, int op, void *pArg){
    AuroraFile *p = (AuroraFile *)pFile;
    int rc;

    if (!p->isAurMmap)
        return p->pReal->pMethods->xFileControl(p->pReal, op, pArg);

    rc = SQLITE_NOTFOUND;
    if (op == SQLITE_FCNTL_VFSNAME) {
        *(char**)pArg = sqlite3_mprintf("aurora(%p,%lld)", p->aData, p->sz);
        rc = SQLITE_OK;
    }

    return rc;
}

/*
** Return the sector-size in bytes for an aurora-file.
*/
static int auroraSectorSize(sqlite3_file *pFile){
    AuroraFile *p = (AuroraFile *)pFile;
    if (!p->isAurMmap)
        return p->pReal->pMethods->xSectorSize(p->pReal);

    return 1024;
}

/*
** Return the device characteristic flags supported by an aurora-file.
*/
static int auroraDeviceCharacteristics(sqlite3_file *pFile){
    AuroraFile *p = (AuroraFile *)pFile;
    if (!p->isAurMmap)
        return p->pReal->pMethods->xDeviceCharacteristics(p->pReal);

    return SQLITE_IOCAP_ATOMIC |
           SQLITE_IOCAP_POWERSAFE_OVERWRITE |
           SQLITE_IOCAP_SAFE_APPEND |
           SQLITE_IOCAP_SEQUENTIAL;
}

/* Create a shared memory file mapping */
static int auroraShmMap(
        sqlite3_file *pFile,
        int iPg,
        int pgsz,
        int bExtend,
        void volatile **pp
){
    AuroraFile *p = (AuroraFile *)pFile;
    if (!p->isAurMmap)
        return p->pReal->pMethods->xShmMap(p->pReal, iPg, pgsz, bExtend, pp);

    /* 
     * XXX Implement this; returning a pointer 
     * into the mapped region should be fine.
     */
    return SQLITE_IOERR_SHMMAP;
}

/* Perform locking on a shared-memory segment */
static int auroraShmLock(sqlite3_file *pFile, int offset, int n, int flags){
    AuroraFile *p = (AuroraFile *)pFile;
    if (!p->isAurMmap)
        return p->pReal->pMethods->xShmLock(p->pReal, offset, n, flags);

    /* 
     * XXX Implement this; using a no-op if fine right now 
     * because we do not need to open the database from
     * multiple processes.
     */
    return SQLITE_IOERR_SHMLOCK;
}

/* Memory barrier operation on shared memory */
static void auroraShmBarrier(sqlite3_file *pFile){
    AuroraFile *p = (AuroraFile *)pFile;
    if (!p->isAurMmap)
    	p->pReal->pMethods->xShmBarrier(p->pReal);

    /* XXX Find a way to call this function. */
    //sqlite3MemoryBarrier();
}

/* Unmap a shared memory segment */
static int auroraShmUnmap(sqlite3_file *pFile, int deleteFlag){
    AuroraFile *p = (AuroraFile *)pFile;
    if (!p->isAurMmap)
        return p->pReal->pMethods->xShmUnmap(p->pReal, deleteFlag);

    return SQLITE_OK;
}

/* Fetch a page of a memory-mapped file */
static int auroraFetch(
        sqlite3_file *pFile,
        sqlite3_int64 iOfst,
        int iAmt,
        void **pp
){
    AuroraFile *p = (AuroraFile *)pFile;
    if (p->isAurMmap) {
        *pp = (void*)(p->aData + iOfst);
        return SQLITE_OK;
    } else {
        return p->pReal->pMethods->xFetch(p->pReal, iOfst, iAmt, pp);
    }
}

/* Release a memory-mapped page */
static int auroraUnfetch(sqlite3_file *pFile, sqlite3_int64 iOfst, void *pPage){
    AuroraFile *p = (AuroraFile *)pFile;
    if (p->isAurMmap) {
        return SQLITE_OK;
    } else {
        return p->pReal->pMethods->xUnfetch(p->pReal, iOfst, pPage);
    }
}

/*
** Open an aurora file handle.
*/
static int auroraOpen(
        sqlite3_vfs *pVfs,
        const char *zName,
        sqlite3_file *pFile,
        int flags,
        int *pOutFlags
){
    AuroraFile *p = (AuroraFile*)pFile;
    memset(p, 0, sizeof(*p));
    int rc = SQLITE_OK;

    p->pReal = (sqlite3_file*)&p[1];
    int isAurMmap = (flags & SQLITE_OPEN_MAIN_DB);
    p->isAurMmap = isAurMmap;

    if (isAurMmap) {
        p->aData = (unsigned char*)sqlite3_uri_int64(zName,"ptr", 0);
        if (p->aData == 0)
		return SQLITE_CANTOPEN;

        p->sz = sqlite3_uri_int64(zName, "sz", -1);
        if (p->sz < 0)
		return SQLITE_CANTOPEN;

        p->szMax = sqlite3_uri_int64(zName, "max", p->sz);
        if (p->szMax < p->sz)
		return SQLITE_CANTOPEN;

        p->fd = sqlite3_uri_int64(zName, "fd", 0);
        if (p->fd == 0)
		return SQLITE_CANTOPEN;

	if (sas_trace_start(p->fd) != 0)
		return SQLITE_INTERNAL;
	/* 
	 * Threshold can be 0, in which case xWrite() 
	 * does not trigger checkpointing at all.
	 */
        p->szThreshold = sqlite3_uri_int64(zName, "threshold", 0);

	/*
	 * Configure triggering checkpointing on xSync.
	 * The default is to keep checkpointing on.
	 */
        p->bCkptOnSync = sqlite3_uri_int64(zName, "ckptOnSync", 1) > 0;

        mainDbName = sqlite3_malloc(strlen(zName));
        strcpy(mainDbName, zName);

        // Create the file, but don't do anything with it
        rc = ORIGVFS(pVfs)->xOpen(ORIGVFS(pVfs), zName, p->pReal, flags, pOutFlags);
    } else {
        rc = ORIGVFS(pVfs)->xOpen(ORIGVFS(pVfs), zName, p->pReal, flags, pOutFlags);
    }
    p->fileName = sqlite3_malloc(strlen(zName));
    strcpy(p->fileName, zName);

    if (rc == 0) {
        pFile->pMethods = &aurora_io_methods;
    }
    return rc;
}

/*
** Delete the file located at zPath. If the dirSync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int auroraDelete(sqlite3_vfs *pVfs, const char *zPath, int dirSync){
    return ORIGVFS(pVfs)->xDelete(ORIGVFS(pVfs), zPath, dirSync);
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int auroraAccess(
        sqlite3_vfs *pVfs,
        const char *zPath,
        int flags,
        int *pResOut
){
    if (mainDbName != NULL && strcmp(zPath, mainDbName) == 0) {
        *pResOut = 1;
        return SQLITE_OK;
    } else {
        return ORIGVFS(pVfs)->xAccess(ORIGVFS(pVfs), zPath, flags, pResOut);
    }
}

/*
** Populate buffer zOut with the full canonical pathname corresponding
** to the pathname in zPath. zOut is guaranteed to point to a buffer
** of at least (INST_MAX_PATHNAME+1) bytes.
*/
static int auroraFullPathname(
        sqlite3_vfs *pVfs,
        const char *zPath,
        int nOut,
        char *zOut
){
    sqlite3_snprintf(nOut, zOut, "%s", zPath);
    return SQLITE_OK;
}

/*
** Open the dynamic library located at zPath and return a handle.
*/
static void *auroraDlOpen(sqlite3_vfs *pVfs, const char *zPath){
    return ORIGVFS(pVfs)->xDlOpen(ORIGVFS(pVfs), zPath);
}

/*
** Populate the buffer zErrMsg (size nByte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated
** with dynamic libraries.
*/
static void auroraDlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg){
    ORIGVFS(pVfs)->xDlError(ORIGVFS(pVfs), nByte, zErrMsg);
}

/*
** Return a pointer to the symbol zSymbol in the dynamic library pHandle.
*/
static void (*auroraDlSym(sqlite3_vfs *pVfs, void *p, const char *zSym))(void){
    return ORIGVFS(pVfs)->xDlSym(ORIGVFS(pVfs), p, zSym);
}

/*
** Close the dynamic library handle pHandle.
*/
static void auroraDlClose(sqlite3_vfs *pVfs, void *pHandle){
    ORIGVFS(pVfs)->xDlClose(ORIGVFS(pVfs), pHandle);
}

/*
** Populate the buffer pointed to by zBufOut with nByte bytes of
** random data.
*/
static int auroraRandomness(sqlite3_vfs *pVfs, int nByte, char *zBufOut){
    return ORIGVFS(pVfs)->xRandomness(ORIGVFS(pVfs), nByte, zBufOut);
}

/*
** Sleep for nMicro microseconds. Return the number of microseconds
** actually slept.
*/
static int auroraSleep(sqlite3_vfs *pVfs, int nMicro){
    return ORIGVFS(pVfs)->xSleep(ORIGVFS(pVfs), nMicro);
}

/*
** Return the current time as a Julian Day number in *pTimeOut.
*/
static int auroraCurrentTime(sqlite3_vfs *pVfs, double *pTimeOut){
    return ORIGVFS(pVfs)->xCurrentTime(ORIGVFS(pVfs), pTimeOut);
}

static int auroraGetLastError(sqlite3_vfs *pVfs, int a, char *b){
    return ORIGVFS(pVfs)->xGetLastError(ORIGVFS(pVfs), a, b);
}
static int auroraCurrentTimeInt64(sqlite3_vfs *pVfs, sqlite3_int64 *p){
    return ORIGVFS(pVfs)->xCurrentTimeInt64(ORIGVFS(pVfs), p);
}

/*
** This routine is called when the extension is loaded.
** Register the new VFS.
*/
int sqlite3_auroravfs_init(
        sqlite3 *db,
        char **pzErrMsg,
        const sqlite3_api_routines *pApi
){
    sqlite3_vfs *pOrig;
    int rc = SQLITE_OK;

    SQLITE_EXTENSION_INIT2(pApi);

    /* Loads default VFS into pAppData. */
    pOrig = sqlite3_vfs_find(0);
    if (pOrig == 0)
	    return SQLITE_ERROR;

    aurora_vfs.pAppData = pOrig;
    aurora_vfs.szOsFile = pOrig->szOsFile + sizeof(AuroraFile);
    rc = sqlite3_vfs_register(&aurora_vfs, 1);
    if (rc == SQLITE_OK)
	    rc = SQLITE_OK_LOAD_PERMANENTLY;

    return rc;
}
