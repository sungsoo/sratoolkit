/*===========================================================================
 *
 *                            PUBLIC DOMAIN NOTICE
 *               National Center for Biotechnology Information
 *
 *  This software/database is a "United States Government Work" under the
 *  terms of the United States Copyright Act.  It was written as part of
 *  the author's official duties as a United States Government employee and
 *  thus cannot be copyrighted.  This software/database is freely available
 *  to the public for use. The National Library of Medicine and the U.S.
 *  Government have not placed any restriction on its use or reproduction.
 *
 *  Although all reasonable efforts have been taken to ensure the accuracy
 *  and reliability of the software and data, the NLM and the U.S.
 *  Government do not and cannot warrant the performance or results that
 *  may be obtained by using this software or data. The NLM and the U.S.
 *  Government disclaim all warranties, express or implied, including
 *  warranties of performance, merchantability or fitness for any particular
 *  purpose.
 *
 *  Please cite the author in any work or product based on this material.
 *
 * ===========================================================================
 *
 */

#include <vfs/extern.h>

#include <vfs/manager.h>
#include <vfs/path.h>
#include <vfs/path-priv.h>
#include <vfs/manager-priv.h> /* VFSManagerMakeFromKfg */

#include "path-priv.h"

#include <krypto/key.h>
#include <krypto/encfile.h>
#include <krypto/wgaencrypt.h>
#include <krypto/ciphermgr.h>

#include <kfg/config.h>
#include <kfg/repository.h>
#include <kfg/keystore.h>
#include <kfg/keystore-priv.h>
#include <kfg/kfg-priv.h>

#include <vfs/resolver.h>
#include <sra/srapath.h>

#include <kfs/directory.h>
#include <kfs/file.h>
#include <kfs/sra.h>
#include <kfs/tar.h>
#include <kfs/dyload.h>
#include <kfs/kfs-priv.h>
#include <kfs/nullfile.h>
#include <kfs/buffile.h>
#include <kfs/quickmount.h>
#include <kfs/cacheteefile.h>
#include <kfs/lockfile.h>

#include <kns/curl-file.h>
#include <kxml/xml.h>
#include <klib/refcount.h>
#include <klib/log.h>
#include <klib/rc.h>
#include <klib/printf.h>

#include <strtol.h>

#include <sysalloc.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>


#ifdef _DEBUGGING
#define MGR_DEBUG(msg) DBGMSG(DBG_VFS,DBG_FLAG(DBG_VFS_MGR), msg)
#else
#define MGR_DEBUG(msg)
#endif


#define DEFAULT_CACHE_BLOCKSIZE ( 32768 * 4 )
#define DEFAULT_CACHE_CLUSTER 1

#define VFS_KRYPTO_PASSWORD_MAX_SIZE 4096

/*--------------------------------------------------------------------------
 * VFSManager
 */

/* currently expected to be a singleton and not use a vtable but
 * be fully fleshed out here */
struct VFSManager
{
    KRefcount refcount;

    /* the current directory in the eyes of the O/S when created */
    KDirectory * cwd;

    /* configuration manager */
    KConfig * cfg;

    /* krypto's cipher manager */
    KCipherManager * cipher;

    /* SRAPath will be replaced with a VResolver */
    struct VResolver * resolver;

    /* path to a global password file */
    char *pw_env;
    
    /* encryption key storage */ 
    struct KKeyStore* keystore;
};

static const char kfsmanager_classname [] = "VFSManager";

static 
VFSManager * singleton = NULL;


/* Destroy
 *  destroy file
 */
static rc_t VFSManagerDestroy ( VFSManager *self )
{
    if ( self == NULL )
        return RC ( rcVFS, rcFile, rcDestroying, rcSelf, rcNull );

    KRefcountWhack (&self->refcount, kfsmanager_classname);

    KDirectoryRelease (self->cwd);

    KConfigRelease (self->cfg);

    KCipherManagerRelease (self->cipher);

    VResolverRelease ( self->resolver );

    free ( self -> pw_env );
    
    KKeyStoreRelease( self->keystore );

    free (self);

    singleton = NULL;
    return 0;
}

/* AddRef
 *  creates a new reference
 *  ignores NULL references
 */
LIB_EXPORT rc_t CC VFSManagerAddRef ( const VFSManager *self )
{
    if (self != NULL)
    {
        switch (KRefcountAdd (&self->refcount, kfsmanager_classname))
        {
        case krefOkay:
            break;
        case krefZero:
            return RC (rcVFS, rcMgr, rcAttaching, rcRefcount, rcIncorrect);
        case krefLimit:
            return RC (rcVFS, rcMgr, rcAttaching, rcRefcount, rcExhausted);
        case krefNegative:
            return RC (rcVFS, rcMgr, rcAttaching, rcRefcount, rcInvalid);
        default:
            return RC (rcVFS, rcMgr, rcAttaching, rcRefcount, rcUnknown);
        }
    }
    return 0;
}

/* Release
 *  discard reference to file
 *  ignores NULL references
 */
LIB_EXPORT rc_t CC VFSManagerRelease ( const VFSManager *self )
{
    rc_t rc = 0;
    if (self != NULL)
    {
        switch (KRefcountDrop (&self->refcount, kfsmanager_classname))
        {
        case krefOkay:
        case krefZero:
            break;
        case krefWhack:
            rc = VFSManagerDestroy ((VFSManager*)self);
            break;
        case krefNegative:
            return RC (rcVFS, rcMgr, rcAttaching, rcRefcount, rcInvalid);
        default:
            rc = RC (rcVFS, rcMgr, rcAttaching, rcRefcount, rcUnknown);
            break;            
        }
    }
    return rc;
}



/*--------------------------------------------------------------------------
 * VFSManagerMakeCurlFile
 */
static
rc_t VFSManagerMakeCurlFile( const VFSManager * self, const KFile **cfp,
                             const char * url, const char * cache_location )
{
    rc_t rc = KCurlFileMake ( cfp, url, false );
    if ( rc == 0 )
    {
        const KFile *temp_file;
        rc_t rc2;
        if ( cache_location == NULL )
        {
            /* there is no cache_location! just wrap the remote file in a buffer */
            rc2 = KBufFileMakeRead ( & temp_file, * cfp, 128 * 1024 * 1024 );
        }
        else
        {
            /* we do have a cache_location! wrap the remote file in a cacheteefile */
            rc2 = KDirectoryMakeCacheTee ( self->cwd, &temp_file, *cfp, NULL,
                                           DEFAULT_CACHE_BLOCKSIZE, DEFAULT_CACHE_CLUSTER,
                                           false, "%s", cache_location );
        }
        if ( rc2 == 0 )
        {
            KFileRelease ( * cfp );
            * cfp = temp_file;
        }
    }
    return rc;
}

static rc_t CC VFSManagerGetConfigPWFile (const VFSManager * self, char * b, size_t bz, size_t * pz)
{
    const char * env;
    const KConfigNode * node;
    size_t oopsy;
    size_t z = 0;
    rc_t rc;

    if (pz)
        *pz = 0;

    env = getenv (ENV_KRYPTO_PWFILE);
    if (!env)
        env = self->pw_env;
    if (env)
    {
        z = string_copy (b, bz, env, string_size (env));
    
        /* force a NUL that string_copy might have omitted 
         * even if this truncates the path */
        b[bz-1] = '\0';

        if (pz)
            *pz = z;
       
        return 0;
    }
    
    { /* If we are in a protected area, there may be an area-specific key file */
        const KRepositoryMgr *repoMgr;
        rc = KConfigMakeRepositoryMgrRead ( self->cfg, &repoMgr );
        if (rc == 0)
        {
            const KRepository* prot;
            rc = KRepositoryMgrCurrentProtectedRepository ( repoMgr, &prot );
            if (rc == 0)
            {
                rc = KRepositoryEncryptionKeyFile ( prot, b, bz, pz);            
                KRepositoryRelease(prot);
            }
            KRepositoryMgrRelease(repoMgr);
        }
        if (GetRCState(rc) == rcNotFound)
            rc = RC (rcVFS, rcMgr, rcOpening, rcEncryptionKey, rcNotFound);
    }

    if (rc != 0)
    {   /* fall back on an old-style global password file*/
        rc = KConfigOpenNodeRead (self->cfg, &node, KFG_KRYPTO_PWFILE);
        if (rc)
        {
            /* if not found, change object from path to encryption key */
            if (GetRCState(rc) == rcNotFound)
                rc = RC (rcVFS, rcMgr, rcOpening, rcEncryptionKey, rcNotFound);
        }
        else
        {
            rc = KConfigNodeRead (node, 0, b, bz-1, &z, &oopsy);
            if (rc == 0)
            {
                if (oopsy != 0)
                    rc = RC (rcKrypto, rcMgr, rcReading, rcBuffer, rcInsufficient);
                else
                {
                    b[z] = '\0';
                    *pz = z;
                }
            }
            KConfigNodeRelease (node);
        }
    }
    
    return rc;
}

static
rc_t GetEncryptionKey(const VFSManager * self, const VPath * vpath, char* obuff, size_t buf_size, size_t *pwd_size)
{
    /* -----
     * #if 0
     * first check the insecure password on the command line hack 
     * #endif 
     *
     * then check the option for pwfile in the VPath
     * then check the option for pwfd
     * then check the keystore. if necessary, keystore will 
     *          check the environment      
     *          check the configuration
     */

#if 0
    /* obviously not used yet */
    if (VPathOption (vpath, vpopt_temporary_pw_hack, obuff, buf_size, &z) == 0)
    {
        if (z < 1)
            rc = RC (rcVFS, rcPath, rcConstructing, rcParam, rcInvalid);
        else
        {
            size_t x = 0;
            size_t y = 0;
            int ch, h, l;

            while (x < z)
            {
                h = tolower(obuff[x++]);
                l = tolower(obuff[x++]);

                if (!isxdigit(h) || !isxdigit(l))
                    rc = RC (rcVFS, rcPath, rcConstructing, rcParam, rcInvalid);

                if (isdigit(h))
                    ch = (h - '0') << 4;
                else
                    ch = (h + 10 - 'a') << 4;
                if (isdigit(l))
                    ch |= (l - '0');
                else
                    ch |= (l + 10 - 'a');

                /* added for compatibility with other passwords */
                if ((ch == '\r') || (ch == '\n'))
                    break;
                obuff[y++] = (char)ch;
            }
            obuff[y] = '\0';
            assert (z == x);
            assert (z/2 == y);
            z = y;
            * pwd_size = z;
        }
    }
#endif    

    rc_t rc = 0;
    rc_t rc2;
    size_t z;

    if (VPathOption (vpath, vpopt_pwpath, obuff, buf_size - 1, &z) == 0)
    {
        const KFile * pwfile;
        obuff [z] = '\0';
        rc = KDirectoryOpenFileRead(self->cwd, &pwfile, obuff);
        if (rc == 0)
        {
            rc = KKeyStoreSetTemporaryKeyFromFile(self->keystore, pwfile);
            rc2 = KFileRelease(pwfile);
            if (rc == 0)
                rc = rc2;
        }
    }
    else if (VPathOption (vpath, vpopt_pwfd, obuff, buf_size - 1, &z) == 0)
    {
        /* -----
         * pwfd is not fully a VPath at this point: we 
         * should obsolete it
         */
        const KFile * pwfile;
        obuff [z] = '\0';
        rc = KFileMakeFDFileRead (&pwfile, atoi (obuff));
        if (rc == 0)
        {
            rc = KKeyStoreSetTemporaryKeyFromFile(self->keystore, pwfile);
            rc2 = KFileRelease(pwfile);
            if (rc == 0)
                rc = rc2;
        }
    }
    
    if (rc == 0)
    {
        KEncryptionKey* enc_key;
        rc = KKeyStoreGetKey(self->keystore, NULL, &enc_key); /* here, we are only interested in global keys - at least for now */
        if (rc == 0)
        {
            *pwd_size = string_copy(obuff, buf_size, enc_key->value.addr, enc_key->value.size);
            if (*pwd_size != enc_key->value.size)
                rc = RC(rcVFS, rcPath, rcReading, rcBuffer, rcInsufficient);
            rc2 = KEncryptionKeyRelease(enc_key);
            if (rc == 0)
                rc = rc2;
        }
    }
    
    rc2 = KKeyStoreSetTemporaryKeyFromFile(self->keystore, NULL); /* forget the temp key if set */
    if (rc == 0)
        rc = rc2;
    return rc;
}

/*
 * This is still hack - must match VFSManagerResolvePathRelativeDir()
 */
LIB_EXPORT rc_t CC VFSManagerWGAValidateHack (const VFSManager * self, 
                                              const KFile * file,
                                              const char * path) /* we'll move this to a vpath */
{
    VPath * vpath;
    rc_t rc = 0;

    rc = VPathMake (&vpath, path);
    if (rc == 0)
    {
        size_t z;
        char obuff [VFS_KRYPTO_PASSWORD_MAX_SIZE + 2]; /* 1 for over-read and 1 for NUL */
        rc = GetEncryptionKey(self, vpath, obuff, sizeof(obuff), &z);

        if (rc == 0)
        {
            rc = WGAEncValidate (file, obuff, z);
        }
    }
    return rc;
}



/* ResolvePath
 *
 * take a VPath and resolve to a final form apropriate for KDB
 *
 * that is take a relative path and resolve it against the CWD
 * or take an accession and resolve into the local or remote 
 * VResolver file based on config. It is just a single resolution percall
 */
static rc_t VFSManagerResolvePathResolver (const VFSManager * self,
                                           uint32_t flags,
                                           const VPath * in_path,
                                           VPath ** out_path)
{
    rc_t rc = 0;

    *out_path = NULL;

    /*
     * this RC perculates up for ncbi-acc: schemes but not for
     * no scheme uris
     */
    if ((flags & vfsmgr_rflag_no_acc) == vfsmgr_rflag_no_acc)
    {
        /* hack */
        if ( VPathGetUri_t ( in_path ) == vpuri_none )
            rc = SILENT_RC (rcVFS, rcMgr, rcResolving, rcSRA, rcNotAvailable);
        else
            rc = RC (rcVFS, rcMgr, rcResolving, rcSRA, rcNotAvailable);
    }
    else
    {
        bool not_done = true;

        /*
         * cast because we seem to have the restriction on the output from
         * VResolver that seems too restrictive
         */
        if ((flags & vfsmgr_rflag_no_acc_local) == 0)
        {
            rc = VResolverLocal (self->resolver, in_path, (const VPath **)out_path);
            if (rc == 0)
                not_done = false;
        }
            
        if (not_done && ((flags & vfsmgr_rflag_no_acc_remote) == 0))
        {
            rc = VResolverRemote (self->resolver, eProtocolHttp,
                in_path, (const VPath **)out_path, NULL);
        }
    }
    return rc;
}


static rc_t VFSManagerResolvePathInt (const VFSManager * self,
                                      uint32_t flags,
                                      const KDirectory * base_dir,
                                      const VPath * in_path,
                                      VPath ** out_path)
{
    rc_t rc;
    char * pc;
    VPUri_t uri_type;

    assert (self);
    assert (in_path);
    assert (out_path);

    uri_type = VPathGetUri_t ( in_path );
    switch ( uri_type )
    {
    default:
        rc = RC (rcVFS, rcMgr, rcResolving, rcPath, rcInvalid);
        break;

    case vpuri_not_supported:
    case vpuri_ncbi_legrefseq:
        rc = RC (rcVFS, rcMgr, rcResolving, rcPath, rcUnsupported);
        break;

    case vpuri_ncbi_acc:
        rc = VFSManagerResolvePathResolver (self, flags, in_path, out_path);
        break;

    case vpuri_none:
        /* for KDB purposes, no scheme might be an accession */
        if (flags & vfsmgr_rflag_kdb_acc)
        {
             /* no '/' is permitted in an accession */
            pc = string_chr (in_path->path.addr, in_path->path.size, '/');
            if (pc == NULL)
            {
                rc = VFSManagerResolvePathResolver (self, flags, in_path, out_path);
                if (rc == 0)
                    break;
            }
        }
        /* Fall through */
    case vpuri_ncbi_vfs:
    case vpuri_file:
        /* check for relative versus full path : assumes no 'auth' not starting with '/' */
        if (in_path->path.addr[0] == '/')
        {
            rc = VPathAddRef (in_path);
            if (rc == 0)
                *out_path = (VPath *)in_path; /* oh these const ptr are annoying */
        }
        else
        {
            /* not 'properly' handling query, fragment etc. for relative path
             * assumes path within VPath is ASCIZ
             */
            size_t s;
            VPath * v;
            char u [32 * 1024];

            switch ( uri_type )
            {
            default:
                rc = RC (rcVFS, rcMgr, rcResolving, rcFunction, rcInvalid);
                break;

            case vpuri_ncbi_vfs:
                string_printf ( u, sizeof u, & s, "%S:", & in_path -> scheme );
                rc = KDirectoryResolvePath ( base_dir, true, & u [ s ], sizeof u - s,
                    "%.*s", ( int ) in_path -> path . size, in_path -> path . addr );
                if ( rc == 0 )
                {
                    s = string_size ( u );
                    rc = string_printf ( & u [ s ], sizeof u - s, NULL,
                        "%S%S", & in_path -> query, & in_path -> fragment );
                }
                if (rc == 0)
                    rc = VPathMake (&v, u);
                break;

            case vpuri_none:
            case vpuri_file:
                rc = KDirectoryResolvePath ( base_dir, true, u, sizeof u,
                    "%.*s", ( int ) in_path -> path . size, in_path -> path . addr );
                rc = VPathMake (&v, u);
                break;
            }
            if (rc == 0)
                *out_path = v;
        }
        break;

        /* these are considered fully resolved already */
    case vpuri_http:
    case vpuri_ftp:
        rc = VPathAddRef (in_path);
        if (rc == 0)
            *out_path = (VPath*)in_path;
        break;

    }
    return rc;
}


LIB_EXPORT rc_t CC VFSManagerResolvePath (const VFSManager * self,
                                          uint32_t flags,
                                          const VPath * in_path,
                                          VPath ** out_path)
{
    if (out_path == NULL)
        return RC (rcVFS, rcMgr, rcResolving, rcParam, rcNull);

    *out_path = NULL;

    if (self == NULL)
        return RC (rcVFS, rcMgr, rcResolving, rcSelf, rcNull);

    if (in_path == NULL)
        return RC (rcVFS, rcMgr, rcResolving, rcParam, rcNull);

    return VFSManagerResolvePathInt (self, flags, self->cwd, in_path, out_path);
}

LIB_EXPORT rc_t CC VFSManagerResolvePathRelative (const VFSManager * self,
                                                  uint32_t flags,
                                                  const struct  VPath * base_path,
                                                  const struct  VPath * in_path,
                                                  struct VPath ** out_path)
{
    const KDirectory * dir;
    rc_t rc;

    if (out_path == NULL)
        rc = RC (rcVFS, rcMgr, rcResolving, rcParam, rcNull);

    *out_path = NULL;

    if (self == NULL)
        return RC (rcVFS, rcMgr, rcResolving, rcSelf, rcNull);

    if (in_path == NULL)
        return RC (rcVFS, rcMgr, rcResolving, rcParam, rcNull);

    rc = VFSManagerOpenDirectoryRead (self, &dir, base_path);
    if (rc == 0)
        rc = VFSManagerResolvePathInt (self, flags, dir, in_path, out_path);

    return rc;
}

/*
 * This is still hack - must match VFSManagerGetEncryptionKey()
 */

LIB_EXPORT rc_t CC VFSManagerResolvePathRelativeDir (const VFSManager * self,
                                                     uint32_t flags,
                                                     const KDirectory * base_dir,
                                                     const VPath * in_path,
                                                     VPath ** out_path)
{
    if (out_path == NULL)
        return RC (rcVFS, rcMgr, rcResolving, rcParam, rcNull);

    *out_path = NULL;

    if (self == NULL)
        return RC (rcVFS, rcMgr, rcResolving, rcSelf, rcNull);

    if (in_path == NULL)
        return RC (rcVFS, rcMgr, rcResolving, rcParam, rcNull);

    return VFSManagerResolvePathInt (self, flags, base_dir, in_path, out_path);
}


/* OpenFileRead
 *  opens an existing file with read-only access
 *
 *  "f" [ OUT ] - return parameter for newly opened file
 *
 *  "path" [ IN ] - NUL terminated string in directory-native
 *  character set denoting target file
 */
static
rc_t VFSManagerOpenFileReadDecryption (const VFSManager *self,
                                       const KDirectory * dir,
                                       const KFile ** f,
                                       const KFile * file,
                                       const VPath * path,
                                       bool force_decrypt,
                                       bool * was_encrypted)
{
    rc_t rc = 0;
    size_t z;
    char obuff [VFS_KRYPTO_PASSWORD_MAX_SIZE + 2]; /* 1 for over-read and 1 for NUL */
    bool has_enc_opt;

    if (was_encrypted)
        *was_encrypted = false;

    /* -----
     * at this point we have no fatal errors and we have the
     * file opened but we have not seen if we have to decrypt
     * or use other query options
     */
    has_enc_opt = (VPathOption (path, vpopt_encrypted, obuff,
                                sizeof obuff, &z) == 0);

    if ((has_enc_opt == false) &&
        (force_decrypt == false))
    {
        /* if we are not told to decrypt, don't and we are done */
        KFileAddRef (file);
        *f = file;
    }

    else /* we are told to decrypt if possible */
    {
        /* -----
         * pre-read 4kb from the 'encrypted file'
         */
        rc = KFileRandomAccess (file);
        if (rc == 0)
            ;
        /* most common and easiest option is it has random
         * access - a no-op here
         */
        else if (GetRCState(rc) == rcUnsupported)
        {
            const KFile * buffile;

            rc = KBufFileMakeRead (&buffile, file, 32 * 2 * 1024);
            if (rc)
                ;
            else
            {
                /* there is an extra reference to file now, but
                 * it gets removed after this function returns
                 */
                file = buffile;
            }
        }
        
        if (rc == 0)
        {
            size_t tz;
            char tbuff [4096];

            /* we now have a file from which we can pre-read the
             * possible encrypted format header */
            rc = KFileReadAll (file, 0, tbuff, sizeof tbuff, &tz);
            if (rc == 0)
            {
                /* 
                 * we've successfully read 4KB from the file,
                 * now decide if is actually an encrypted file
                 * format we support
                 */
                const KFile * encfile;

                /* is this the header of an ecnrypted file? */
                if (KFileIsEnc (tbuff, tz) == 0)
                {
                    if (was_encrypted)
                        *was_encrypted = true;
                    rc = GetEncryptionKey(self, path, obuff, sizeof(obuff), &z);
                    if (rc == 0)
                    {
                        KKey key;

                        /* create the AES Key */
                        rc = KKeyInitRead (&key, kkeyAES128, obuff, z);
                        if (rc)
                            ;
                        else
                        {
                            rc = KEncFileMakeRead (&encfile, file, &key);
                            if (rc)
                                ;
                            else
                            {
                                const KFile * buffile;

                                /*
                                 * TODO: make the bsize a config item not a hard constant
                                 */
                                rc = KBufFileMakeRead (&buffile, encfile,
                                                       256 * 1024 * 1024);
                                if (rc == 0)
                                {
                                    *f = buffile;
                                    /* *f keeps a reference to encfile, can release it here */
                                    KFileRelease (encfile);
                                    return 0;
                                }
                                KFileRelease (encfile);
                            }
                        }
                    }
                }
                else if (KFileIsWGAEnc (tbuff, tz) == 0)
                {
                    if (was_encrypted)
                        *was_encrypted = true;
                    rc = GetEncryptionKey(self, path, obuff, sizeof(obuff), &z);
                    if (rc == 0)
                    {
                        rc = KFileMakeWGAEncRead (&encfile, file, obuff, z);
                        if (rc)
                            ;
                        else
                        {
                            /* we'll release anextra reference to file
                             * after this function returns
                             */
                            *f = encfile;
                            return 0;
                        }
                    }
                }
                else
                {
                    /* -----
                     * not encrypted in a manner we can decrypt so 
                     * give back the raw file (possibly buffered
                     *
                     * since file is released in the caller
                     * we need another reference
                     */
                    KFileAddRef (file);
                    *f = file;
                    return 0;
                }
            }
        }
    }
    return rc;
}


/*
 * try to open the file as a regular file
 */
static
rc_t VFSManagerOpenFileReadRegularFile (char * pbuff, size_t z,
                                        KFile const ** file,
                                        const KDirectory * dir)
{
    rc_t rc;
    char rbuff [8192];

    assert ((pbuff) && (pbuff[0]));
    assert (*file == NULL);

    rc = KDirectoryResolvePath (dir, true, rbuff, sizeof rbuff,
                                pbuff);
    if (rc)
        ; /* log? */
    else
    {
        /* validate that the file system agrees the path refers
         * to a regular file (even if through a link */
        uint32_t type;

        type = KDirectoryPathType (dir, rbuff);
        switch (type & ~kptAlias)
        {
        case kptNotFound:
            rc = RC (rcVFS, rcMgr, rcOpening, rcFile,
                     rcNotFound);
            break;

        case kptBadPath:
            rc = RC (rcVFS, rcMgr, rcOpening, rcFile,
                     rcInvalid);
            break;

        case kptDir:
        case kptCharDev:
        case kptBlockDev:
        case kptFIFO:
        case kptZombieFile:
            rc = RC (rcVFS, rcMgr, rcOpening, rcFile,
                     rcIncorrect);
            break;

        default:
            rc = RC (rcVFS, rcMgr, rcOpening, rcFile, rcUnknown);
            break;

        case kptFile:
            /*
             * this is the good/successful path: open the file 
             * as a const KFile
             */
            rc = KDirectoryOpenFileRead (dir, file, rbuff);
            break;
        }
    }

    return rc;
}

/*
 * if successful set *file to a usable KFile * and return 0
 * if unsuccessful but without error, set *file to NULL and return 0
 * if an error encountered set *file to NULL and return non-zero.
 */
static
rc_t VFSManagerOpenFileReadSpecial (char * pbuff, size_t z, KFile const ** file)
{
    rc_t rc;
    static const char dev [] = "/dev/";
    static const char dev_stdin [] = "/dev/stdin";
    static const char dev_null [] = "/dev/null";

    assert (pbuff);
    assert (z);
    assert (file);

    *file = NULL;

    /*
     * Handle a few special case path names that are pre-opened
     * 'file descriptors'
     *
     * This probably needs to be system specific eventually
     *
     * First check for the path being in the 'dev' directory in
     * posix/unix terms
     */
    if (string_cmp (dev, sizeof dev - 1, pbuff, z, sizeof dev - 1) != 0)
        rc = 0; /* we're done */

    else
    {
        if (strcmp (dev_stdin, pbuff) == 0)
            rc = KFileMakeStdIn (file);

        else if (strcmp (dev_null, pbuff) == 0)
            rc = KFileMakeNullRead (file);

        else if (strncmp ("/dev/fd/", pbuff, sizeof "/dev/fd/" - 1) == 0)
        {
            char * pc;
            size_t ix;

            pc = pbuff + sizeof "/dev/fd/" - 1;

            for (ix = 0; isdigit (pc[ix]); ++ix)
                assert (ix <= z);

            if ((ix > 0)&&(pc[ix] == '\0'))
            {
                int fd;

                fd = atoi (pc);
                rc = KFileMakeFDFileRead (file, fd);
            }
        }
    }

    return rc;
}

static
rc_t VFSManagerOpenFileReadInt (const VFSManager *self,
                                const KDirectory * dir,
                                KFile const **f,
                                const VPath * path,
                                bool force_decrypt,
                                bool * was_encrypted)
{
    /* -----
     * this is a first pass that only opens files directory referenced from 
     * the ced or have a sysdir root; that is it uses KSysDir and KSysFile
     * only.
     */
    const KFile * file = NULL;
    size_t num_read;
    char pbuff [4096];
    rc_t rc;

    rc = VPathReadPath (path, pbuff, sizeof pbuff, &num_read);
    if (rc)
        ; /* log? */
    else
    {
        /* -----
         * try to open path as a special file if requested
         *
         * *file will be set or a usable file or to NULL and rc will reflect
         * any error
         */
        rc = VFSManagerOpenFileReadSpecial (pbuff, num_read, &file);

        if (rc == 0)
        {
            /* -----
             * If we didn't open the file using the special
             * logic above for special paths open the file and have no error,
             * continue
             */
            if (file == NULL)
                rc = VFSManagerOpenFileReadRegularFile (pbuff, num_read,
                                                        &file, dir);
            /*
             * we either have an rc to return with or we have an open KFile:
             * check for possible encryption that we are told to decrypt
             */
            if (rc == 0)
            {
                rc = VFSManagerOpenFileReadDecryption (self, dir, f, file, path,
                                                       force_decrypt, was_encrypted);
            }
            /* release file if we are here and it is open */
            KFileRelease (file);
        }
    }
    return rc;
}


static
rc_t VFSManagerOpenFileReadDirectoryRelativeInt (const VFSManager *self,
                                                 const KDirectory * dir,
                                                 KFile const **f,
                                                 const VPath * path,
                                                 bool force_decrypt,
                                                 bool * was_encrypted)
{
    rc_t rc;

    if (f == NULL)
        rc = RC (rcVFS, rcMgr, rcOpening, rcParam, rcNull);

    else
    {
        *f = NULL;

        if ((f == NULL) || (path == NULL))
            rc = RC (rcVFS, rcMgr, rcOpening, rcParam, rcNull);

        else if (self == NULL)
            rc = RC (rcVFS, rcMgr, rcOpening, rcSelf, rcNull);

        else
        {

            rc = VFSManagerOpenFileReadInt (self, dir, f, path, force_decrypt, was_encrypted);
        }
    }
    return rc;
}


/* we will create a KFile from a http or ftp url... */
static rc_t VFSManagerOpenCurlFile ( const VFSManager *self,
                                     KFile const **f,
                                     const VPath * path )
{
    rc_t rc;
/*    const char * url; */
    const String * uri = NULL;

    if ( f == NULL )
        return RC( rcVFS, rcMgr, rcOpening, rcParam, rcNull );
    *f = NULL;
    if ( self == NULL )
        return RC( rcVFS, rcMgr, rcOpening, rcSelf, rcNull );
    if ( path == NULL )
        return RC( rcVFS, rcMgr, rcOpening, rcParam, rcNull );

/*    url = path->path.addr; */
    rc = VPathMakeString ( path, &uri );
    if ( rc == 0 )
    {
        if ( self->resolver != NULL )
        {
            const VPath * local_cache;
            /* find cache - vresolver call */
            rc = VResolverCache ( self->resolver, path, &local_cache, 0 );
            if ( rc == 0 )
                /* we did find a place for local cache --> use it! */
                rc = VFSManagerMakeCurlFile( self, f, uri->addr, local_cache->path.addr );
            else
                /* we did NOT find a place for local cache --> we are not caching! */
                rc = VFSManagerMakeCurlFile( self, f, uri->addr, NULL );
        }
        else
            rc = VFSManagerMakeCurlFile( self, f, uri->addr, NULL );
        free( ( void * )uri );
    }
    return rc;
}


LIB_EXPORT
rc_t CC VFSManagerOpenFileReadDirectoryRelative (const VFSManager *self,
                                                 const KDirectory * dir,
                                                 KFile const **f,
                                                 const VPath * path)
{
    return VFSManagerOpenFileReadDirectoryRelativeInt (self, dir, f, path, false, NULL);
}

LIB_EXPORT
rc_t CC VFSManagerOpenFileReadDirectoryRelativeDecrypt (const VFSManager *self,
                                                        const KDirectory * dir,
                                                        KFile const **f,
                                                        const VPath * path) /*,
                                                        bool force_decrypt) */
{
    return VFSManagerOpenFileReadDirectoryRelativeInt (self, dir, f, path, true, NULL);
}


static rc_t ResolveVPathByVResolver( struct VResolver * resolver, const VPath ** path )
{
    rc_t rc;

    if ( resolver == NULL )
        rc = RC ( rcVFS, rcFile, rcOpening, rcSRA, rcUnsupported );
    else
    {
        const VPath * tpath;
        rc = VResolverLocal ( resolver, *path, &tpath );
        if ( rc == 0 )
        {
            VPathRelease ( *path );
            *path = tpath;
        }
    }
    return rc;
}

static rc_t ResolveVPathBySRAPath( const VPath ** path )
{
    * path = NULL;
    return RC ( rcVFS, rcFile, rcOpening, rcSRA, rcUnsupported );
}


LIB_EXPORT rc_t CC VFSManagerOpenFileRead ( const VFSManager *self,
                                            KFile const **f,
                                            const VPath * path_ )
{
    rc_t rc;

    if ( f == NULL )
        rc = RC (rcVFS, rcMgr, rcOpen, rcParam, rcNull);
    else
    {
        *f = NULL;

        if  (self == NULL )
            rc = RC ( rcVFS, rcMgr, rcOpen, rcSelf, rcNull );
        else if ( f == NULL )
            rc = RC ( rcVFS, rcMgr, rcOpen, rcParam, rcNull );
        else
        {
            rc = VPathAddRef ( path_ );
            if ( rc == 0 )
            {
                const VPath * path = path_;
                VPUri_t uri_type = VPathGetUri_t ( path );

                switch ( uri_type )
                {
                default:
                case vpuri_invalid:
                    rc = RC (rcVFS, rcFile, rcOpening, rcPath, rcInvalid);
                    break;

                case vpuri_not_supported:
                    rc = RC (rcVFS, rcFile, rcOpening, rcPath, rcUnsupported);
                    break;

                case vpuri_ncbi_acc:
                    if ( self->resolver != NULL )
                        rc = ResolveVPathByVResolver( self->resolver, &path );
                    else
                        rc = ResolveVPathBySRAPath( &path );

                    if ( rc != 0 )
                        break;

                /* !!! fall through !!! */

                case vpuri_none:
                case vpuri_ncbi_vfs:
                case vpuri_file:
                    rc = VFSManagerOpenFileReadDirectoryRelativeInt ( self, self->cwd, f, path, false, NULL );
                    break;

                case vpuri_ncbi_legrefseq:
                    rc = RC ( rcVFS, rcFile, rcOpening, rcPath, rcIncorrect );
                    break;

                case vpuri_http:
                case vpuri_ftp:
                    rc = VFSManagerOpenCurlFile ( self, f, path );
                    break;
                }
                VPathRelease (path);
            }
        }
    }
    return rc;
}


LIB_EXPORT rc_t CC VFSManagerOpenFileReadDecrypt (const VFSManager *self,
                                                  KFile const **f,
                                                  const VPath * path)
{
    return VFSManagerOpenFileReadDirectoryRelativeInt ( self, self->cwd, f, path, true, NULL );
}

LIB_EXPORT
rc_t CC VFSManagerOpenDirectoryUpdateDirectoryRelative (const VFSManager *self,
                                                        const KDirectory * dir,
                                                        KDirectory **d,
                                                        const VPath * path)
{
    rc_t rc;
    VPUri_t uri_type;

    if ((d == NULL) || (path == NULL))
        return RC (rcVFS, rcMgr, rcOpening, rcParam, rcNull);

    *d = NULL;

    if (self == NULL)
        return RC (rcVFS, rcMgr, rcOpening, rcSelf, rcNull);

    uri_type = VPathGetUri_t ( path );
    switch ( uri_type )
    {
    case vpuri_http :
    case vpuri_ftp :
        return RC( rcVFS, rcMgr, rcOpening, rcParam, rcWrongType );

    default :
        {
            uint32_t type;

            /* WHY NOT JUST TRY TO OPEN THE DIRECTORY,
               AND LET KFS TELL US WHAT'S WRONG? */

            type = KDirectoryPathType (dir, "%.*s", ( int ) path -> path . size, path -> path . addr );
            switch (type & ~kptAlias)
            {
            case kptNotFound:
                rc = RC (rcVFS, rcMgr, rcOpening, rcDirectory, rcNotFound);
                break;

            case kptFile:
                rc = RC (rcVFS, rcMgr, rcOpening, rcDirectory, rcReadonly);
                break;

            case kptBadPath:
                rc = RC (rcVFS, rcMgr, rcOpening, rcDirectory, rcInvalid);
                break;

            case kptDir:
                rc = KDirectoryOpenDirUpdate ((KDirectory*)dir, d, false, "%.*s", ( int ) path -> path . size, path -> path . addr);
                return rc;

            case kptCharDev:
            case kptBlockDev:
            case kptFIFO:
            case kptZombieFile:
                rc = RC (rcVFS, rcMgr, rcOpening, rcDirectory, rcIncorrect);
                break;

            default:
                rc = RC (rcVFS, rcMgr, rcOpening, rcDirectory, rcUnknown);
                break;
            }
        }
    }
    return rc;
}


LIB_EXPORT rc_t CC VFSManagerOpenDirectoryUpdate (const VFSManager *self,
                                                  KDirectory **d,
                                                  const VPath * path)
{
    return VFSManagerOpenDirectoryUpdateDirectoryRelative (self, self->cwd, d, path);
}


static
rc_t TransformFileToDirectory(const KDirectory * dir,
                              const KFile * file,
                              KDirectory const **d,
                              const char *path_str,
                              bool was_encrypted)
{
    rc_t rc;

    rc = KFileRandomAccess( file );
    if (rc)
        PLOGERR(klogErr,(klogErr, rc, "Can not use files without random access"
                         " as database archives '$(P)'", "P=%s", path_str));
    else
    {
        size_t tz;
        char tbuff [4096];

        rc = KFileReadAll (file, 0, tbuff, sizeof tbuff, &tz);
        if ( rc )
            LOGERR (klogErr, rc, "Error reading the head of an archive to use as a database object");
        else
        {
            /* we only use KAR/SRA or tar files as archives so try to identify
             * as our KAR/SRA file.
             IT IS NOT TRUE ANYMORE ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ */
            if ( KFileIsSRA( tbuff, tz ) == 0 )
                /* if it was open it as a directory */
                rc = KDirectoryOpenSraArchiveReadUnbounded_silent_preopened( dir, d, false, file, path_str );

            else
            {
                rc = KDirectoryOpenTarArchiveRead_silent_preopened( dir, d, false, file, path_str );

                /*
                 * if RC here we did not have an SRA and did not have
                 * a tar file we could use; assume the problem was:
                 * - decryption if the file was encrypted
                 * - or it is not an archive
                 */
                if (rc != 0) {
                    if ( was_encrypted ) {
                     /* the following RC update is not correct anymore but:
                        TODO: check tools/libraries
                            that expect this returned code and fix them
                        rc = RC(rcVFS, rcEncryptionKey, rcOpening, rcEncryption,
                             rcIncorrect ); */
                        PLOGERR (klogErr, (klogErr, rc,
                            "could not use '$(P)' as an "
                            "archive it was encrypted so the password"
                            " was possibly wrong or it is not SRA or"
                            " TAR file", "P=%s", path_str));
                    }
                    else {
                        PLOGERR (klogInfo, (klogInfo, rc,
                            "could not use '$(P)' as an "
                            "archive not identified as SRA or"
                            " TAR file", "P=%s", path_str));
                    }
                }
            }
        }
    }
    return rc;
}

/* also handles ftp - if it cant we'll need another function */
static
rc_t VFSManagerOpenDirectoryReadHttp (const VFSManager *self,
                                      const KDirectory * dir,
                                      KDirectory const **d,
                                      const VPath * path,
                                      bool force_decrypt)
{
    rc_t rc;
    const KFile * file = NULL;

    rc = VFSManagerOpenCurlFile ( self, &file, path );
    if ( rc != 0 )
    {
        PLOGERR ( klogErr, ( klogErr, rc, "error with curl open '$(U)'",
                             "U=%S:%S", & path -> scheme, & path -> path ) );
    }
    else
    {
        const char mountpointpath[] = "/";
        const KDirectory * mountpoint;

        rc = KQuickMountDirMake (self->cwd, &mountpoint, file,
                                 mountpointpath, sizeof mountpointpath - 1, 
                                 path->path.addr, path->path.size);
        if (rc)
        {
            PLOGERR (klogErr, (klogErr, rc, "error creating mount "
                               "'$(M)' for '$(F)", "M=%s,F=%S",
                               mountpointpath, &path->path));
        }
        else
        {
            const KFile * f;
            bool was_encrypted = false;

            rc = VFSManagerOpenFileReadDecryption (self, mountpoint, &f,
                                                   file, path,
                                                   force_decrypt,
                                                   &was_encrypted);
            if (rc == 0)
            {
                    
                rc = TransformFileToDirectory (mountpoint, f, d, 
                                               path->path.addr,
                                               was_encrypted);
                /* hacking in the fragment bit */
                if ((rc == 0) && (path->fragment . size > 1 ) )
                {
                    const KDirectory * tempd = * d;
                    const char * fragment = path -> fragment . addr + 1;
                    int frag_size = ( int ) path -> fragment . size - 1;

                    assert ( fragment [ -1 ] == '#' );
                        
                    rc = KDirectoryOpenDirRead (tempd, d, false, "%.*s", frag_size, fragment );
                    
                    KDirectoryRelease (tempd);
                }
                KFileRelease (f);
            }
            KDirectoryRelease (mountpoint);
        }
        KFileRelease (file);
    }
    return rc;
}


static
rc_t VFSManagerOpenDirectoryReadKfs (const VFSManager *self,
                                     const KDirectory * dir,
                                     KDirectory const **d,
                                     const VPath * path,
                                     bool force_decrypt)
{
    const KFile * file = NULL;
    char rbuff[ 4096 ]; /* resolved path buffer */
    rc_t rc;

    assert (self);
    assert (dir);
    assert (d);
    assert (path);
    assert ((force_decrypt == false) || (force_decrypt == true));
    assert (*d == NULL);

    file = NULL;

    rc = KDirectoryResolvePath( dir, true, rbuff, sizeof rbuff, "%.*s", ( int ) path -> path . size, path -> path . addr );
    if ( rc == 0 )
    {
        uint32_t type;
        bool was_encrypted;

        type = KDirectoryPathType( dir, rbuff );
        switch (type & ~kptAlias)
        {
        case kptNotFound:
            rc = RC( rcVFS, rcMgr, rcOpening, rcDirectory, rcNotFound );
            break;

        case kptFile:
            rc = VFSManagerOpenFileReadDirectoryRelativeInt (self, dir, 
                                                             &file, path, 
                                                             force_decrypt,
                                                             &was_encrypted);
            if (rc == 0)
                rc = TransformFileToDirectory (dir, file, d, rbuff,
                                               was_encrypted);
            break;

        case kptBadPath:
            rc = RC( rcVFS, rcMgr, rcOpening, rcDirectory, rcInvalid );
            break;

        case kptDir:
            rc = KDirectoryOpenDirRead( dir, d, false, rbuff );
            return rc;

        case kptCharDev:
        case kptBlockDev:
        case kptFIFO:
        case kptZombieFile:
            rc = RC( rcVFS, rcMgr, rcOpening, rcDirectory, rcIncorrect );
            break;

        default:
            rc = RC( rcVFS, rcMgr, rcOpening, rcDirectory, rcUnknown );
            break;
        }

        /* hacking in the fragment bit */
        /* the C grammar specifies order of precedence... */
        if ((rc == 0) && (path->fragment.size > 1 ))
        {
            const KDirectory * tempd = * d;
            const char * fragment = path -> fragment . addr + 1;
            int frag_size = ( int ) path -> fragment . size - 1;

            assert ( fragment [ -1 ] == '#' );
            
            rc = KDirectoryOpenDirRead (tempd, d, false, "%.*s", frag_size, fragment );
            
            KDirectoryRelease (tempd);
        }
    }

    KFileRelease(file);

    return rc;
}


static
rc_t VFSManagerOpenDirectoryReadLegrefseq (const VFSManager *self,
                                           const KDirectory * dir,
                                           KDirectory const **d,
                                           const VPath * path,
                                           bool force_decrypt)
{
    const KFile * file;
    const KDirectory * dd;
    size_t num_read;
    char pbuff [4096]; /* path buffer */
    rc_t rc;

    assert (self);
    assert (dir);
    assert (d);
    assert (path);
    assert ((force_decrypt == false) || (force_decrypt == true));
    assert (*d == NULL);

    file = NULL;
    dd = NULL;

    /* hier part only */
    rc = VPathReadPath (path, pbuff, sizeof pbuff, &num_read);
    if ( rc == 0 )
    {
        char rbuff[ 4096 ]; /* resolved path buffer */
        rc = KDirectoryResolvePath( dir, true, rbuff, sizeof rbuff, pbuff );
        if ( rc == 0 )
        {
            uint32_t type;
            bool was_encrypted;

            type = KDirectoryPathType( dir, rbuff );
            switch (type & ~kptAlias)
            {
            case kptNotFound:
                rc = RC( rcVFS, rcMgr, rcOpening, rcDirectory, rcNotFound );
                break;

            case kptFile:
                rc = VFSManagerOpenFileReadDirectoryRelativeInt (self, dir, 
                                                                 &file, path, 
                                                                 force_decrypt,
                                                                 &was_encrypted);
                if (rc == 0)
                    rc = TransformFileToDirectory (dir, file, &dd, rbuff,
                                                   was_encrypted);
                break;

            case kptBadPath:
                rc = RC( rcVFS, rcMgr, rcOpening, rcDirectory, rcInvalid );
                break;

            case kptDir:
                rc = KDirectoryOpenDirRead( dir, &dd, false, rbuff );
                break;

            case kptCharDev:
            case kptBlockDev:
            case kptFIFO:
            case kptZombieFile:
                rc = RC( rcVFS, rcMgr, rcOpening, rcDirectory, rcIncorrect );
                break;

            default:
                rc = RC( rcVFS, rcMgr, rcOpening, rcDirectory, rcUnknown );
                break;
            }

            if (rc == 0)
            {
                if ( path -> fragment . size < 2 )
                    rc = RC( rcVFS, rcMgr, rcOpening, rcPath, rcIncorrect );
                else
                {
                    const char *fragment = path -> fragment . addr + 1;
                    int frag_size = ( int ) path -> fragment . size - 1;
                    assert ( fragment [ -1 ] == '#' );

                    rc = KDirectoryOpenDirRead (dd, d, false, "%.*s", frag_size, fragment );

                    KDirectoryRelease (dd);
                }
            }
        }
    }
    return rc;
}


static
rc_t VFSManagerOpenDirectoryReadDirectoryRelativeInt (const VFSManager *self,
                                                      const KDirectory * dir,
                                                      KDirectory const **d,
                                                      const VPath * path_,
                                                      bool force_decrypt)
{
    rc_t rc;
    do 
    {
        if (d == NULL)
        {
            rc =  RC (rcVFS, rcDirectory, rcOpening, rcParam, rcNull);
            break;
        }

        *d = NULL;

        if (self == NULL)
        {
            rc = RC (rcVFS, rcDirectory, rcOpening, rcSelf, rcNull);
            break;
        }

        if ((dir == NULL) || (path_ == NULL))
        {
            rc = RC (rcVFS, rcDirectory, rcOpening, rcParam, rcNull);
            break;
        }

        if ((force_decrypt != false) && (force_decrypt != true))
        {
            rc = RC (rcVFS, rcDirectory, rcOpening, rcParam, rcInvalid);
            break;
        }

        rc = VPathAddRef (path_);
        if ( rc )
            break;
        else
        {
            const VPath *path = path_;
            VPUri_t uri_type = VPathGetUri_t ( path );

            switch ( uri_type )
            {
            default:
            case vpuri_invalid:
                rc = RC (rcVFS, rcDirectory, rcOpening, rcPath, rcInvalid);
                break;


            case vpuri_not_supported:
                rc = RC (rcVFS, rcDirectory, rcOpening, rcPath, rcUnsupported);
                break;

            case vpuri_ncbi_acc:
                if ( self->resolver != NULL )
                    rc = ResolveVPathByVResolver( self->resolver, &path );
                else
                    rc = ResolveVPathBySRAPath( &path );
                if ( rc != 0 )
                    break;

            /* !!! fall through !!! */

            case vpuri_none:
            case vpuri_ncbi_vfs:
            case vpuri_file:
                rc = VFSManagerOpenDirectoryReadKfs ( self, dir, d, path, force_decrypt );
                break;

            case vpuri_ncbi_legrefseq:
                rc = VFSManagerOpenDirectoryReadLegrefseq ( self, dir, d, path, force_decrypt );
                break;

            case vpuri_http:
            case vpuri_ftp:
                rc = VFSManagerOpenDirectoryReadHttp ( self, dir, d, path, force_decrypt );
                break;
            }
            VPathRelease ( path ); /* same as path_ if not uri */
        }
    } while (0);
    return rc;
}


LIB_EXPORT 
rc_t CC VFSManagerOpenDirectoryReadDirectoryRelative (const VFSManager *self,
                                                      const KDirectory * dir,
                                                      KDirectory const **d,
                                                      const VPath * path)
{
    return VFSManagerOpenDirectoryReadDirectoryRelativeInt (self, dir, d, path, false);
}


LIB_EXPORT 
rc_t CC VFSManagerOpenDirectoryReadDirectoryRelativeDecrypt (const VFSManager *self,
                                                             const KDirectory * dir,
                                                             KDirectory const **d,
                                                             const VPath * path)
{
    return VFSManagerOpenDirectoryReadDirectoryRelativeInt (self, dir, d, path, true);
}


LIB_EXPORT rc_t CC VFSManagerOpenDirectoryReadDecrypt (const VFSManager *self,
                                                       KDirectory const **d,
                                                       const VPath * path)
{
    return VFSManagerOpenDirectoryReadDirectoryRelativeInt (self, self->cwd, d, path, true);
}


LIB_EXPORT rc_t CC VFSManagerOpenDirectoryRead (const VFSManager *self,
                                                KDirectory const **d,
                                                const VPath * path)
{
    return VFSManagerOpenDirectoryReadDirectoryRelativeInt (self, self->cwd, d, path, false);
}


/* OpenFileWrite
 *  opens an existing file with write access
 *
 *  "f" [ OUT ] - return parameter for newly opened file
 *
 *  "update" [ IN ] - if true, open in read/write mode
 *  otherwise, open in write-only mode
 *
 *  "path" [ IN ] - NUL terminated string in directory-native
 *  character set denoting target file
 */
LIB_EXPORT rc_t CC VFSManagerOpenFileWrite (const VFSManager *self,
                                            KFile **f, bool update,
                                            const VPath * path )
{
    /* -----
     * this is a first pass that only opens files directory referenced from 
     * the ced or have a sysdir root; that is it uses KSysDir and KSysFile
     * only.
     */
    KFile * file = NULL;
    size_t num_read;
    char pbuff [4096];
    rc_t rc;

    if ((f == NULL) || (path == NULL))
        return RC (rcVFS, rcMgr, rcOpening, rcParam, rcNull);

    *f = NULL;

    if (self == NULL)
        return RC (rcVFS, rcMgr, rcOpening, rcSelf, rcNull);

    rc = VPathReadPath (path, pbuff, sizeof pbuff, &num_read);
    if (rc == 0)
    {
        /* handle a few special case path names
         * This probably needs to be system specifica eventually
         */
        if (strncmp ("/dev/", pbuff, sizeof "/dev/" - 1) == 0)
        {

            if (strcmp ("/dev/stdout", pbuff) == 0)
                rc = KFileMakeStdOut (&file);
            else if (strcmp ("/dev/stderr", pbuff) == 0)
                rc = KFileMakeStdErr (&file);
            else if (strcmp ("/dev/null", pbuff) == 0)
                rc = KFileMakeNullUpdate (&file);
            else if (strncmp ("/dev/fd/", pbuff, sizeof "/dev/fd/" - 1) == 0)
            {
                char * pc;
                size_t ix;

                pc = pbuff + sizeof "/dev/fd/" - 1;

                for (ix = 0; isdigit (pc[ix]); ++ix)
                    ;

                if ((ix > 0)&&(pc[ix] == '\0'))
                {
                    int fd = atoi (pc);

                    rc = KFileMakeFDFileWrite (&file, update, fd);
                }
            }
        }
        if ((rc == 0)&&(file == NULL))
        {
            char rbuff [4096];

            rc = KDirectoryResolvePath (self->cwd, true, rbuff, sizeof rbuff, pbuff);
            if (rc == 0)
            {
                uint32_t type;

                type = KDirectoryPathType (self->cwd, rbuff);
                switch (type & ~kptAlias)
                {
                case kptNotFound:
                    rc = RC (rcVFS, rcMgr, rcOpening, rcFile, rcNotFound);
                    break;

                case kptFile:
                    rc = KDirectoryOpenFileWrite (self->cwd, &file, update, rbuff);
                    break;

                case kptBadPath:
                    rc = RC (rcVFS, rcMgr, rcOpening, rcFile, rcInvalid);
                    break;
                case kptDir:
                case kptCharDev:
                case kptBlockDev:
                case kptFIFO:
                case kptZombieFile:
                    rc = RC (rcVFS, rcMgr, rcOpening, rcFile, rcIncorrect);
                    break;

                default:
                    rc = RC (rcVFS, rcMgr, rcOpening, rcFile, rcUnknown);
                    break;
                }
            }
        }
    }
    if (rc == 0)
    {
        size_t z;
        char obuff [VFS_KRYPTO_PASSWORD_MAX_SIZE+2];

        if (VPathOption (path, vpopt_encrypted, obuff, sizeof obuff, &z) == 0)
        {
            rc = GetEncryptionKey(self, path, obuff, sizeof(obuff), &z);
            if (rc == 0)
            {
                KKey key;
                KFile * encfile;
            
                rc = KKeyInitUpdate (&key, kkeyAES128, obuff, z);
                if (rc == 0)
                {
                    rc = KEncFileMakeWrite (&encfile, file, &key);
                    if (rc == 0)
                    {
                        KFileRelease (file); /* owned by encfile now */
                        *f = encfile;
                        return 0;
                    }
                }
            }
            if (rc)
                KFileRelease (file);
        }
        else
        {
            *f = file;
            return 0;
        }
    }
    return rc;
}


/* CreateFile
 *  opens a file with write access
 *
 *  "f" [ OUT ] - return parameter for newly opened file
 *
 *  "update" [ IN ] - if true, open in read/write mode
 *  otherwise, open in write-only mode
 *
 *  "access" [ IN ] - standard Unix access mode, e.g. 0664
 *
 *  "mode" [ IN ] - a creation mode ( see explanation above ).
 *
 *  "path" [ IN ] VPath representing the path, URL or URN of the desired file
 */
LIB_EXPORT rc_t CC VFSManagerCreateFile ( const VFSManager *self, KFile **f,
                                          bool update, uint32_t access, KCreateMode mode, const VPath * path )
{
    /* -----
     * this is a first pass that only opens files directory referenced from 
     * the ced or have a sysdir root; that is it uses KSysDir and KSysFile
     * only.
     */
    KFile * file = NULL;
    size_t num_read;
    rc_t rc;
    bool file_created = false;
    char pbuff [4096];
    char rbuff [4096];

    if ((f == NULL) || (path == NULL))
        return RC (rcVFS, rcMgr, rcOpening, rcParam, rcNull);

    *f = NULL;

    if (self == NULL)
        return RC (rcVFS, rcMgr, rcOpening, rcSelf, rcNull);

    rc = VPathReadPath (path, pbuff, sizeof pbuff, &num_read);
    if (rc == 0)
    {

        /* handle a few special case path names
         * This probably needs to be system specifica eventually
         */
        if (strncmp ("/dev/", pbuff, sizeof "/dev/" - 1) == 0)
        {

            if (strcmp ("/dev/stdout", pbuff) == 0)
                rc = KFileMakeStdOut (&file);
            else if (strcmp ("/dev/stderr", pbuff) == 0)
                rc = KFileMakeStdErr (&file);
            else if (strcmp ("/dev/null", pbuff) == 0)
                rc = KFileMakeNullUpdate (&file);
            else if (strncmp ("/dev/fd/", pbuff, sizeof "/dev/fd/" - 1) == 0)
            {
                char * pc;
                size_t ix;

                pc = pbuff + sizeof "/dev/fd/" - 1;

                for (ix = 0; isdigit (pc[ix]); ++ix)
                    ;

                if ((ix > 0)&&(pc[ix] == '\0'))
                {
                    int fd = atoi (pc);

                    rc = KFileMakeFDFileWrite (&file, update, fd);
                }
            }
        }
        if ((rc == 0)&&(file == NULL))
        {
            rc = KDirectoryResolvePath (self->cwd, true, rbuff, sizeof rbuff, pbuff);
            if (rc == 0)
            {
                uint32_t type;

                type = KDirectoryPathType (self->cwd, rbuff);
                switch (type & ~kptAlias)
                {
                case kptNotFound:
                case kptFile:
                    rc = KDirectoryCreateFile (self->cwd, &file, update, access, mode,
                                               rbuff);
                    if (rc == 0)
                        file_created = true;
                    break;

                case kptBadPath:
                    rc = RC (rcVFS, rcMgr, rcOpening, rcFile, rcInvalid);
                    break;
                case kptDir:
                case kptCharDev:
                case kptBlockDev:
                case kptFIFO:
                case kptZombieFile:
                    rc = RC (rcVFS, rcMgr, rcOpening, rcFile, rcIncorrect);
                    break;

                default:
                    rc = RC (rcVFS, rcMgr, rcOpening, rcFile, rcUnknown);
                    break;
                }
            }
        }
    }
    if (rc == 0)
    {
        size_t z;
        char obuff [VFS_KRYPTO_PASSWORD_MAX_SIZE+2];

        if (VPathOption (path, vpopt_encrypted, obuff, sizeof obuff, &z) == 0)
        {
            rc = GetEncryptionKey(self, path, obuff, sizeof(obuff), &z);
            if (rc == 0)
            {
                KKey key;
                KFile * encfile;
                rc = KKeyInitUpdate (&key, kkeyAES128, obuff, z);

                obuff[z] = '\0';

                rc = KEncFileMakeWrite (&encfile, file, &key);
                if (rc == 0)
                {
                    KFileRelease (file); /* now owned by encfile */
                    *f = encfile;
                    return 0;   
                }
            }
            if (rc)
                KFileRelease (file);
        }
        else
        {
            *f = file;
            return 0;
        }
    }
    if (rc && file_created)
        KDirectoryRemove (self->cwd, true, rbuff);
    return rc;
}


/* Remove
 *  remove an accessible object from its directory
 *
 *  "force" [ IN ] - if true and target is a directory,
 *  remove recursively
 *
 *  "path" [ IN ] - NUL terminated string in directory-native
 *  character set denoting target object
 */
LIB_EXPORT rc_t CC VFSManagerRemove ( const VFSManager *self, bool force,
                                      const VPath * path )
{
    /* -----
     * this is a first pass that only opens files directory referenced from 
     * the ced or have a sysdir root; that is it uses KSysDir and KSysFile
     * only.
     */
    size_t num_read;
    char pbuff [4096];
    rc_t rc;

    if (path == NULL)
        return RC (rcVFS, rcMgr, rcOpening, rcParam, rcNull);

    if (self == NULL)
        return RC (rcVFS, rcMgr, rcOpening, rcSelf, rcNull);

    rc = VPathReadPath (path, pbuff, sizeof pbuff, &num_read);
    if (rc == 0)
    {
        char rbuff [4096];
    
        rc = KDirectoryResolvePath (self->cwd, true, rbuff, sizeof rbuff, pbuff);
        if (rc == 0)
        {
            uint32_t type;

            type = KDirectoryPathType (self->cwd, rbuff);
            switch (type & ~kptAlias)
            {
            case kptNotFound:
                break;

            case kptFile:
            case kptDir:
            case kptCharDev:
            case kptBlockDev:
            case kptFIFO:
            case kptZombieFile:
                rc = KDirectoryRemove (self->cwd, force, rbuff);
                break;

            case kptBadPath:
                rc = RC (rcVFS, rcMgr, rcOpening, rcFile, rcInvalid);
                break;
/*                 rc = RC (rcVFS, rcMgr, rcOpening, rcFile, rcIncorrect); */
/*                 break; */

            default:
                rc = RC (rcVFS, rcMgr, rcOpening, rcFile, rcUnknown);
                break;
            }
        }
    }
    return rc;
}

/* Make
 */
LIB_EXPORT rc_t CC VFSManagerMake ( VFSManager ** pmanager ) {
    return VFSManagerMakeFromKfg(pmanager, NULL);
}

/* Make
 */
LIB_EXPORT rc_t CC VFSManagerMakeFromKfg ( struct VFSManager ** pmanager,
    struct KConfig * cfg)
{
    rc_t rc;

    if (pmanager == NULL)
        return RC (rcVFS, rcMgr, rcConstructing, rcParam, rcNull);

    *pmanager = singleton;
    if (singleton != NULL)
    {
        rc = VFSManagerAddRef ( singleton );
        if ( rc != 0 )
            *pmanager = NULL;
    }
    else
    {
        VFSManager * obj;

        obj = calloc (1, sizeof (*obj));
        if (obj == NULL)
            rc = RC (rcVFS, rcMgr, rcConstructing, rcMemory, rcExhausted);
        else
        {
            KRefcountInit (&obj->refcount, 1, kfsmanager_classname, "init", 
                           kfsmanager_classname);

            rc = KDirectoryNativeDir (&obj->cwd);
            if (rc == 0)
            {
                {
                    if (cfg == NULL) {
                        rc = KConfigMake (&obj->cfg, NULL);
                    }
                    else {
                        rc = KConfigAddRef(cfg);
                        if (rc == 0) {
                            obj->cfg = cfg;
                        }
                    }
                    if ( rc == 0 )
                    {
                        rc = KCipherManagerMake (&obj->cipher);
                        if ( rc == 0 )
                        {
                            rc = KKeyStoreMake ( &obj->keystore, obj->cfg );
                            if ( rc == 0 )
                            {
                                rc = VFSManagerMakeResolver ( obj, &obj->resolver, obj->cfg );
                                if ( rc != 0 )
                                {
                                    LOGERR ( klogWarn, rc, "could not build vfs-resolver" );
                                    rc = 0;
                                }

                                *pmanager = singleton = obj;
                                return rc;
                            }
                        }
                    }
                }
            }
        }
        VFSManagerDestroy (obj);
    }
    return rc;
}


LIB_EXPORT rc_t CC VFSManagerGetCWD (const VFSManager * self, KDirectory ** cwd)
{
    rc_t rc;

    if (cwd == NULL)
        return RC (rcVFS, rcMgr, rcAccessing, rcParam, rcNull);

    *cwd = NULL;

    if (self == NULL)
        return RC (rcVFS, rcMgr, rcAccessing, rcSelf, rcNull);

    rc = KDirectoryAddRef ( self->cwd );
    if (rc)
        return rc;

    *cwd = self->cwd;

    return 0;
}


LIB_EXPORT rc_t CC VFSManagerGetResolver ( const VFSManager * self, struct VResolver ** resolver )
{
    if ( resolver == NULL )
        return RC ( rcVFS, rcMgr, rcAccessing, rcParam, rcNull );

    *resolver = NULL;

    if ( self == NULL )
        return RC ( rcVFS, rcMgr, rcAccessing, rcSelf, rcNull );

    if ( self->resolver )
    {
        rc_t rc = VResolverAddRef ( self->resolver );
        if ( rc != 0 )
            return rc;
    }
    *resolver = self->resolver;
    return 0;
}


LIB_EXPORT rc_t CC VFSManagerGetKryptoPassword (const VFSManager * self,
                                                char * password,
                                                size_t max_size,
                                                size_t * size)
{
    rc_t rc;

    if (self == NULL)
        rc = RC (rcVFS, rcMgr, rcAccessing, rcSelf, rcNull);

    else if ((password == NULL) || (max_size == 0) || (size == NULL))
        rc = RC (rcVFS, rcMgr, rcAccessing, rcParam, rcNull);

    else
    {
        size_t z;
        char obuff [4096 + 16];

        rc = VFSManagerGetConfigPWFile(self, obuff, sizeof obuff, &z);
        if (rc == 0)
        {
            VPath * vpath;
            rc_t rc2;
            rc = VPathMake (&vpath, obuff);
            if (rc == 0)
                rc = GetEncryptionKey(self, vpath, password, max_size, size);
            rc2 = VPathRelease (vpath);
            if (rc == 0)
                rc = rc2;
        }
    }
    return rc;
}

LIB_EXPORT rc_t CC VFSManagerUpdateKryptoPassword (const VFSManager * self, 
                                                   const char * password,
                                                   size_t size,
                                                   char * pwd_dir,
                                                   size_t pwd_dir_size)
{
    static const char temp_extension [] = ".tmp";
    rc_t rc;

    if (self == NULL)
        rc = RC (rcVFS, rcEncryptionKey, rcUpdating, rcSelf, rcNull);

    else if ((password == NULL) || (size == 0))
        rc = RC (rcVFS, rcEncryptionKey, rcUpdating, rcParam, rcNull);

    else if (size > VFS_KRYPTO_PASSWORD_MAX_SIZE)
        rc = RC (rcVFS, rcEncryptionKey, rcUpdating, rcSize, rcExcessive);

    else if ((string_chr (password, size, '\n') != NULL) ||
             (string_chr (password, size, '\r') != NULL))
        rc = RC (rcVFS, rcEncryptionKey, rcUpdating, rcEncryptionKey, rcInvalid);

    else
    {
        size_t old_password_file_size;
        char old_password_file [8193];
        
        rc = VFSManagerGetConfigPWFile (self, old_password_file,
                                        sizeof old_password_file - 1,
                                        &old_password_file_size);
        if (rc) {
            if (rc ==
                SILENT_RC(rcKrypto, rcMgr, rcReading, rcBuffer, rcInsufficient))
            {
                rc =
                    RC(rcVFS, rcEncryptionKey, rcUpdating, rcPath, rcExcessive);
            }
            LOGERR (klogErr, rc, "failed to obtain configured path for password file");
        }

        else if (old_password_file_size >= (sizeof old_password_file - 1))
        {
            rc = RC (rcVFS, rcEncryptionKey, rcUpdating, rcPath, rcExcessive);
            PLOGERR (klogErr,
                     (klogErr, rc, "configured path too long for function "
                      "'$(P)' '${F}'", "P=%s,F=%s",
                      old_password_file, __func__));
        }
        else
        {
            KPathType ftype;
            bool old_exists;

            old_password_file[old_password_file_size] = '\0';
            ftype = KDirectoryPathType (self->cwd, old_password_file);

            switch (ftype)
            {
            case kptNotFound:
                old_exists = false;
                break;

            case kptBadPath:
                rc = RC (rcVFS, rcEncryptionKey, rcUpdating, rcPath, rcInvalid);
                break;

            case kptFile:
                old_exists = true;
                break;

            case kptDir:
            case kptCharDev:
            case kptBlockDev:
            case kptFIFO:
            case kptZombieFile:
            case kptDataset:
            case kptDatatype:
                rc = RC (rcVFS, rcEncryptionKey, rcUpdating, rcPath, rcIncorrect);
                break;

            default:
                rc = RC (rcVFS, rcEncryptionKey, rcUpdating, rcPath, rcCorrupt);
                break;
            }

            if (rc)
                PLOGERR (klogErr,
                         (klogErr, rc, "cannot use configured path for "
                          "password file '$(P)'", "P=%s", old_password_file));

            else
            {
                VPath * vold;
                size_t new_password_file_size;
                char new_password_file [sizeof old_password_file + sizeof temp_extension];
                size_t password_dir_size;
                char password_dir [sizeof old_password_file];
/*                 bool save_old_password; */
                char * pc;

                memcpy (password_dir, old_password_file, old_password_file_size);
                memcpy (new_password_file, old_password_file, old_password_file_size);
                memcpy (new_password_file + old_password_file_size, temp_extension, sizeof temp_extension);
                new_password_file_size = old_password_file_size + sizeof temp_extension - 1;

                pc = string_rchr (password_dir, old_password_file_size, '/');
                if (pc == NULL)
                {
                    password_dir[0] = '.';
                    pc = password_dir+1;
                }
                *pc = '\0';
                password_dir_size = pc - password_dir;

                if (pwd_dir && pwd_dir_size) {
                    size_t n = string_copy(pwd_dir, pwd_dir_size,
                                           password_dir, password_dir_size + 1);
                    if (n >= pwd_dir_size) {
                        int i = 0;
                        size_t p = pwd_dir_size - 1;
                        pwd_dir[p] = '\0';
                        for (i = 0; i < 3; ++i) {
                            if (p == 0)
                            {   break; }
                            pwd_dir[--p] = '.';
                        }
                        if (p != 0)
                        {   pwd_dir[--p] = ' '; }
                    }
                }

                rc = VPathMake (&vold, old_password_file);
                if (rc)
                    PLOGERR (klogErr,
                             (klogErr, rc, "could not create vpath for "
                              "password file '$(P)'", "P=%s",
                              old_password_file));

                else
                {
                    VPath * vnew;

                    rc = VPathMake (&vnew, new_password_file);
                    if (rc)
                        PLOGERR (klogErr,
                                 (klogErr, rc, "could not create vpath for "
                                  "password file '$(P)'", "P=%s",
                                  new_password_file));

                    else
                    {
                        const KFile * fold = NULL;
                        KFile * fnew = NULL;

                        if (old_exists)
                        {
                            rc = VFSManagerOpenFileRead (self, &fold, vold);

                            if (rc)
                                PLOGERR (klogErr,
                                         (klogErr, rc, "unable to open existing "
                                          "password file '$(P)'", "P=%s",
                                          old_password_file));
                        }
                        

                        if (rc == 0)
                        {
                            rc = VFSManagerCreateFile (self, &fnew, false, 0600,
                                                       kcmInit|kcmParents,
                                                       vnew);
                            if (rc)
                                PLOGERR (klogErr,
                                         (klogErr, rc, "unable to open temporary "
                                          "password file '$(P)'", "P=%s",
                                          new_password_file));

                            else
                            {
                                uint64_t writ;
                                size_t this_writ;

                                rc = KFileWriteAll (fnew, 0, password, size, &this_writ);
                                if (rc)
                                    PLOGERR (klogErr,
                                             (klogErr, rc, "unable to write "
                                              "password to temporary password "
                                              "file '$(P)'", "P=%s",
                                              new_password_file));

                                else if (this_writ != size)
                                {
                                    rc = RC (rcVFS, rcEncryptionKey, rcWriting,
                                             rcFile, rcInsufficient);
                                    PLOGERR (klogErr,
                                             (klogErr, rc, "unable to write complete "
                                              "password to temporary password "
                                              "file '$(P)'", "P=%s",
                                              new_password_file));
                                }

                                else
                                {
                                    writ = this_writ;

                                    rc = KFileWriteAll (fnew, this_writ, "\n", 1, &this_writ);
                                    if (rc)
                                        PLOGERR (klogErr,
                                                 (klogErr, rc, "unable to write "
                                                  "password to temporary password "
                                                  "file '$(P)'", "P=%s",
                                                  new_password_file));

                                    else if (this_writ != 1)
                                    {
                                        rc = RC (rcVFS, rcEncryptionKey, rcWriting,
                                                 rcFile, rcInsufficient);
                                        PLOGERR (klogErr,
                                                 (klogErr, rc, "unable to write complete "
                                                  "password to temporary password "
                                                  "file '$(P)'", "P=%s",
                                                  new_password_file));
                                    }

                                    else
                                    {
                                        bool do_rename;

                                        do_rename = true;
                                        ++writ;

                                        if (old_exists)
                                        {
                                            uint64_t read;
                                            size_t this_read;
                                            char buffer [VFS_KRYPTO_PASSWORD_MAX_SIZE+4];

                                            rc = KFileReadAll (fold, 0, buffer,
                                                               sizeof buffer, &this_read);
                                            if (rc)
                                                ;

                                            else
                                            {
                                                read = this_read;
                                                /* look for duplicated password */
                                                if (read > size)
                                                {
                                                    char cc;

                                                    cc = buffer[size];
                                                    if (((cc == '\n') || (cc == '\r')) &&
                                                        (memcmp (buffer, password, size) == 0))
                                                    {
                                                        do_rename = false;
                                                    }
                                                }
                                                if (read)
                                                    rc = KFileWriteAll (fnew, writ, buffer, read, &this_writ);

                                                if (rc)
                                                    ;
                                                else if (do_rename)
                                                {
                                                    writ += this_writ;

                                                    do
                                                    {
                                                        rc = KFileReadAll (fold, read, buffer,
                                                                           sizeof buffer, &this_read);
                                                        if (rc)
                                                            ;

                                                        else if (this_read == 0)
                                                            break;

                                                        else
                                                        {
                                                            rc = KFileWriteAll (fnew, writ, buffer,
                                                                                this_read, &this_writ);
                                                            if (rc)
                                                                ;

                                                            else if (this_read != this_writ)
                                                            {
                                                                rc = RC (rcVFS, rcEncryptionKey, rcWriting,
                                                                         rcFile, rcInsufficient);
                                                                PLOGERR (klogErr,
                                                                         (klogErr, rc,
                                                                          "unable to write complete "
                                                                          "password to temporary password "
                                                                          "file '$(P)'", "P=%s",
                                                                          new_password_file));
                                                            }

                                                            else
                                                            {
                                                                read += this_read;
                                                                writ += this_writ;
                                                            }
                                                        }
                                                    } while (rc == 0);
                                                }
                                            }
                                            KFileRelease (fold);
                                            fold = NULL;
                                        }

                                        KFileRelease (fnew);
                                        fnew = NULL;

                                        if (rc == 0)
                                        {
                                            if (do_rename)
                                            {
                                                rc = KDirectoryRename (self->cwd, true, 
                                                                       new_password_file,
                                                                       old_password_file);
                                            }
                                            else
                                            {
                                                KDirectoryRemove (self->cwd, true, new_password_file);
                                            }

#if !WINDOWS
                                            if (rc == 0)
                                            {
                                                uint32_t access;

                                                rc = KDirectoryAccess (self->cwd,
                                                                       &access, password_dir);
                                                if (rc)
                                                    ;

                                                else
                                                {
                                                    if (access & 0027)
                                                        rc = RC (rcVFS, rcEncryptionKey, rcUpdating, rcDirectory, rcExcessive);
                                                }
                                            }
#endif
                                        }
                                    }
                                }
                                KFileRelease (fnew);
                            }
                            KFileRelease (fold);
                        }
                        VPathRelease (vold);
                    }
                    VPathRelease (vnew);
                }
            }
        }
    }
    return rc;
}

/*--------------------------------------------------------------------------
 * KConfig
 *  placing some KConfig code that relies upon VFS here
 */


/* ReadVPath
 *  read a VPath node value
 *
 * self [ IN ] - KConfig object
 * path [ IN ] - path to the node
 * result [ OUT ] - return value (rc != 0 if cannot be converted)
 *
 */
LIB_EXPORT rc_t CC KConfigReadVPath ( struct KConfig const* self, const char* path, struct VPath** result )
{
    rc_t rc;

    if ( result == NULL )
        rc = RC ( rcKFG, rcNode, rcReading, rcParam, rcNull );
    else
    {
        struct KConfigNode const *n;
        rc = KConfigOpenNodeRead ( self, & n, path );
        if ( rc == 0 )
        {
            rc = KConfigNodeReadVPath ( n, result );
            KConfigNodeRelease ( n );
            return rc;
        }

        * result = NULL;
    }

    return rc;
}

/* ReadVPath
 *  read a VPath node value
 *
 * self [ IN ] - KConfigNode object
 * result [ OUT ] - return value (rc != 0 if cannot be converted)
 *
 */
LIB_EXPORT rc_t CC KConfigNodeReadVPath ( struct KConfigNode const *self, struct VPath** result )
{
    rc_t rc;

    if ( result == NULL )
        rc = RC ( rcKFG, rcNode, rcReading, rcParam, rcNull );
    else
    {
        char buffer [ 4096 ];
        size_t num_read, to_read;
        rc = KConfigNodeRead ( self, 0, buffer, sizeof buffer, & num_read, & to_read );
        if ( rc == 0 )
        {
            char *p;

            if ( to_read == 0 && num_read < sizeof buffer )
            {
                buffer [ num_read ] = 0;
                return VPathMake ( result, buffer );
            }

            p = malloc ( num_read + to_read + 1 );
            if ( p == NULL )
                rc = RC ( rcKFG, rcNode, rcReading, rcMemory, rcExhausted );
            else
            {
                rc = KConfigNodeRead ( self, 0, p, num_read + to_read + 1, & num_read, & to_read );
                if ( rc == 0 )
                {
                    p [ num_read ] = 0;
                    rc = VPathMake ( result, p );
                }

                free ( p );
                return rc;
            }
        }

        * result = NULL;
    }

    return rc;
}


static rc_t VFSManagerResolveAcc( const VFSManager * self,
                                  const struct VPath * source,
                                  struct VPath ** path_to_build,
                                  const struct KFile ** remote_file,
                                  const struct VPath ** local_cache )
{
    rc_t rc;
    const VPath * local, * remote;
    
    assert (self);
    assert (source);
    assert (path_to_build);
    assert (remote_file);
    assert (local_cache);

#if 1
    rc = VResolverQuery ( self -> resolver, eProtocolHttp, source, & local, & remote, local_cache );
    if ( rc == 0 )
    {
        assert ( local != NULL || remote != NULL );
        assert ( local == NULL || remote == NULL );
        * path_to_build = ( VPath* ) ( ( local != NULL ) ? local : remote );
    }
#else

    /* first try to find it localy */
    rc = VResolverLocal ( self->resolver, source, (const VPath **)path_to_build );
    if ( GetRCState( rc ) == rcNotFound )
    {
        /* if not found localy, try to find it remotely */
        rc = VResolverRemote ( self->resolver, eProtocolHttp,
            source, (const VPath **)path_to_build, remote_file );
        if ( rc == 0 && remote_file != NULL && local_cache != NULL )
        {
            /* if found and the caller wants to know the location of a local cache file... */
            uint64_t size_of_remote_file = 0;
            if ( *remote_file != NULL )
                rc = KFileSize ( *remote_file, &size_of_remote_file );
            if ( rc ==  0 )
                rc = VResolverCache ( self->resolver, *path_to_build, local_cache, size_of_remote_file );
        }
    }

#endif
    return rc;
}


static rc_t VFSManagerResolveLocal( const VFSManager * self,
                                    const char * local_path,
                                    struct VPath ** path_to_build )
{
    assert ( self != NULL );
    assert ( local_path != NULL && local_path [ 0 ] != 0 );
    assert ( path_to_build != NULL );

    return VFSManagerMakePath ( self, path_to_build, "ncbi-file:%s", local_path );
}

static rc_t VFSManagerResolvePathOrAcc( const VFSManager * self,
                                        const struct VPath * source,
                                        struct VPath ** path_to_build,
                                        const struct KFile ** remote_file,
                                        const struct VPath ** local_cache,
                                        bool resolve_acc )
{
    char buffer[ 4096 ];
    size_t num_read;
    rc_t rc = VPathReadPath ( source, buffer, sizeof buffer, &num_read );
    if ( rc == 0 && num_read > 0 )
    {
        char * pos_of_slash = string_chr ( buffer, string_size( buffer ), '/' );
        if ( pos_of_slash != NULL )
        {
            /* we can now assume that the source is a filesystem-path :
               we build a new VPath and prepend with 'ncbi-file:' */
            rc = VFSManagerResolveLocal( self, buffer, path_to_build );
        }
        else if ( resolve_acc )
        {
            /* we assume the source is an accession! */
            rc = VFSManagerResolveAcc( self, source, path_to_build, remote_file, local_cache );
            if ( GetRCState( rc ) == rcNotFound )
            {
                /* if we were not able to find the source as accession, we assume it is a local path */
                rc = VFSManagerResolveLocal( self, buffer, path_to_build );
            }
        }
        else
        {
            rc = RC ( rcVFS, rcMgr, rcAccessing, rcParam, rcInvalid );
        }
    }
    return rc;
}


static rc_t VFSManagerResolveRemote( const VFSManager * self,
                                     struct VPath ** source,
                                     struct VPath ** path_to_build,
                                     const struct KFile ** remote_file,
                                     const struct VPath ** local_cache )
{
    rc_t rc = 0;
    *path_to_build = *source;
    if ( local_cache != NULL && remote_file != NULL && self->resolver != NULL )
    {

/*        VFS_EXTERN rc_t CC VPathMakeString ( const VPath * self, const String ** uri ); */
        char full_url[ 4096 ];
        size_t num_read;
        rc = VPathReadPath ( *source, full_url, sizeof full_url, &num_read );
        if ( rc == 0 && num_read > 0 )
        {
            rc = KCurlFileMake ( remote_file, full_url, false );
            if ( rc == 0 )
            {
                uint64_t size_of_remote_file = 0;
                rc = KFileSize ( *remote_file, &size_of_remote_file );
                if ( rc == 0 )
                    rc = VResolverCache ( self->resolver, *source, local_cache, size_of_remote_file );
            }
        }
    }
    *source = NULL;
    return rc;
}

/* DEPRECATED */
LIB_EXPORT rc_t CC VFSManagerResolveSpec ( const VFSManager * self,
                                           const char * spec,
                                           struct VPath ** path_to_build,
                                           const struct KFile ** remote_file,
                                           const struct VPath ** local_cache,
                                           bool resolve_acc )
{
    rc_t rc = 0;
    if ( self == NULL )
        rc = RC ( rcVFS, rcMgr, rcAccessing, rcSelf, rcNull );
    else if ( spec == NULL || path_to_build == NULL )
        rc = RC ( rcVFS, rcMgr, rcAccessing, rcParam, rcNull );
    else if ( spec[ 0 ] == 0 )
        rc = RC ( rcVFS, rcMgr, rcAccessing, rcParam, rcEmpty );
    else
    {
        VPath * temp;
        *path_to_build = NULL;
        if ( local_cache != NULL )
            *local_cache = NULL;
        if ( remote_file != NULL ) 
            *remote_file = NULL;
        rc = VPathMake ( &temp, spec );
        if ( rc == 0 )
        {
            VPUri_t uri_type;
            rc = VPathGetScheme_t( temp, &uri_type );
            if ( rc == 0 )
            {
                switch ( uri_type )
                {
                default                  : /* !! fall through !! */
                case vpuri_invalid       : rc = RC ( rcVFS, rcMgr, rcAccessing, rcParam, rcInvalid );
                                           break;

                case vpuri_none          : /* !! fall through !! */
                case vpuri_not_supported : rc = VFSManagerResolvePathOrAcc( self, temp, path_to_build, remote_file, local_cache, resolve_acc );
                                           break;

                case vpuri_ncbi_vfs      : /* !! fall through !! */
                case vpuri_file          : *path_to_build = temp;
                                           temp = NULL;
                                           break;

                case vpuri_ncbi_acc      : if ( resolve_acc )
                                                rc = VFSManagerResolveAcc( self, temp, path_to_build, remote_file, local_cache );
                                           else
                                                rc = RC ( rcVFS, rcMgr, rcAccessing, rcParam, rcInvalid );
                                           break;

                case vpuri_http          : /* !! fall through !! */
                case vpuri_ftp           : rc = VFSManagerResolveRemote( self, &temp, path_to_build, remote_file, local_cache );
                                           break;

                case vpuri_ncbi_legrefseq: /* ??? */
                                           break;
                }
            }
            if ( temp != NULL )
                VPathRelease ( temp );
        }
    }
    return rc;
}

LIB_EXPORT const struct KConfig* CC VFSManagerGetConfig(const struct VFSManager * self)
{
    if ( self == NULL )
        return NULL;
    return self->cfg;
}

/*
 * Object Id / Object name bindings for accessions and dbGaP files
 */

#define MAX_OBJID_SIZE 20
#define MAX_NAME_SIZE 4096

LIB_EXPORT void VFSManagerSetBindingsFile(struct VFSManager * self, const char* path)
{
    if (self != NULL)
        KKeyStoreSetBindingsFile( self->keystore, path);
}

LIB_EXPORT const char* VFSManagerGetBindingsFile(struct VFSManager * self)
{
    if (self == NULL)
        return NULL;
    return KKeyStoreGetBindingsFile(self->keystore);
}

LIB_EXPORT rc_t CC VFSManagerRegisterObject(struct VFSManager* self, uint32_t oid, const struct VPath* obj)
{
    rc_t rc = 0;
    if ( self == NULL )
        rc = RC ( rcVFS, rcMgr, rcRegistering, rcSelf, rcNull );
    else if ( obj == NULL )
        rc = RC ( rcVFS, rcMgr, rcRegistering, rcParam, rcNull );
    else
    {
        const String* newName;
        rc = VPathMakeString (obj, &newName);
        if (rc == 0)
        {
            rc = KKeyStoreRegisterObject(self->keystore, oid, newName);
            StringWhack(newName);
        }
    }
    return rc;
}

LIB_EXPORT rc_t CC VFSManagerGetObject(const struct VFSManager* self, uint32_t oid, struct VPath** obj)
{
    rc_t rc = 0;
    if ( self == NULL )
        rc = RC ( rcVFS, rcMgr, rcRetrieving, rcSelf, rcNull );
    else if ( obj == NULL )
        rc = RC ( rcVFS, rcMgr, rcRetrieving, rcParam, rcNull );
    else
    {
        const String* objName;
        rc = KKeyStoreGetObjectName(self->keystore, oid, &objName);
        if (rc == 0)
        {
            rc = VFSManagerMakePath (self, obj, "%S", objName);
            StringWhack(objName);
        }
    }
    return rc;
}

LIB_EXPORT rc_t CC VFSManagerGetObjectId(const struct VFSManager* self, const struct VPath* obj, uint32_t* oid)
{
    rc_t rc = 0;
    if ( self == NULL )
        rc = RC ( rcVFS, rcMgr, rcRetrieving, rcSelf, rcNull );
    else if ( obj == NULL || oid == NULL)
        rc = RC ( rcVFS, rcMgr, rcRetrieving, rcParam, rcNull );
    else
    {
        const String* pathString;
        rc = VPathMakeString(obj, &pathString);
        if (rc == 0)
        {
            rc = VKKeyStoreGetObjectId(self->keystore, pathString, oid);
            StringWhack(pathString);
        }
    }
    return rc;
}

