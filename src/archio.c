/*
 * fsarchiver: Filesystem Archiver
 *
 * Copyright (C) 2008-2010 Francois Dupoux.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Homepage: http://www.fsarchiver.org
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <sys/statvfs.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>

#include "fsarchiver.h"
#include "syncthread.h"
#include "dico.h"
#include "common.h"
#include "options.h"
#include "archio.h"
#include "queue.h"
#include "comp_gzip.h"
#include "comp_bzip2.h"
#include "error.h"

carchio *archio_alloc(char *basepath)
{
    carchio *ai;

    if ((ai = calloc(1, sizeof(carchio))) == NULL)
        return NULL;

    strlist_init(&ai->vollist);

    ai->newarch = false;
    ai->curblock = 0;
    ai->archfd = -1;
    ai->archid = 0;
    ai->curvol = 0;

    snprintf(ai->basepath, sizeof(ai->basepath), "%s", basepath);
    snprintf(ai->volpath, sizeof(ai->volpath), "%s", basepath);

    return ai;
}

int archio_destroy(carchio *ai)
{
    strlist_destroy(&ai->vollist);
    return 0;
}

int archio_generate_id(carchio *ai)
{
    ai->archid = generate_random_u32_id();
    return 0;
}

int archio_incvolume(carchio *ai)
{
    ai->curvol++;
    get_path_to_volume(ai->volpath, PATH_MAX, ai->basepath, ai->curvol);
    return 0;
}

s64 archio_get_currentpos(carchio *ai)
{
    return (s64)lseek64(ai->archfd, 0, SEEK_CUR);
}

int archio_open_write(carchio *ai)
{
    struct stat64 st;
    long archflags=0;
    long archperm;
    ciohead head;
    int res;

    memset(&st, 0, sizeof(st));
    archflags = O_RDWR|O_CREAT|O_TRUNC|O_LARGEFILE;
    archperm = S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH;

    if ((res = stat64(ai->volpath, &st)) == 0 && !S_ISREG(st.st_mode))
    {
        errprintf("%s already exists, and is not a regular file.\n", ai->basepath);
        return -1;
    }
    else if ((g_options.overwrite==0) && (res==0) && S_ISREG(st.st_mode)) // archive exists and is a regular file
    {
        errprintf("%s already exists, please remove it first.\n", ai->basepath);
        return -1;
    }

    if ((ai->archfd = open64(ai->volpath, archflags, archperm)) < 0)
    {
        sysprintf ("cannot create archive %s\n", ai->volpath);
        return -1;
    }
    ai->newarch=true;

    strlist_add(&ai->vollist, ai->volpath);

    // write volume header
    memset(&head, 0, sizeof(head));
    head.magic = cpu_to_le32(FSA_MAGIC_IOH);
    head.archid = cpu_to_le32(ai->archid);
    head.type = cpu_to_le16(IOHEAD_VOLHEAD);
    head.data.volhead.volnum = cpu_to_le32(ai->curvol);
    head.data.volhead.minver = cpu_to_le64(FSA_VERSION_BUILD(0, 7, 0, 0));
    head.csum = cpu_to_le32(fletcher32((u8 *)&head.data, sizeof(head.data)));
    if (archio_write_low_level(ai, (char*)&head, sizeof(head)) != 0)
    {
        errprintf("archio_write_low_level() failed to write volume header\n");
        return -1;
    }

    return 0;
}

int archio_open_read(carchio *ai)
{
    struct stat64 st;
    ciohead volhead;
    ciohead volfoot;
    u32 volnum = 0;
    u32 archid = 0;
    u64 minver = 0;
    u64 curver = 0;
    bool validhead;
    u32 checksum;

    // 1. check that the volume exists and is a regular file
    while (regfile_exists(ai->volpath) != true)
    {
        // wait until the queue is empty so that the main thread does not pollute the screen
        while (queue_count(g_queue) > 0)
            usleep(5000);
        fflush(stdout);
        fflush(stderr);
        // ask path to the current volume
        msgprintf(MSG_FORCE, "File [%s] is not found, please type the path to volume %ld:\n", ai->volpath, (long)ai->curvol);
        fprintf(stdout, "New path:> ");
        scanf("%s", ai->volpath);
    }

    if ((ai->archfd = open64(ai->volpath, O_RDONLY|O_LARGEFILE)) < 0)
    {
        sysprintf ("Cannot open archive %s\n", ai->volpath);
        return -1;
    }

    if (fstat64(ai->archfd, &st) != 0)
    {
        sysprintf("cannot read file details: fstat64(%s) failed\n", ai->volpath);
        return -1;
    }

    if (!S_ISREG(st.st_mode))
    {
        errprintf("%s is not a regular file, cannot continue\n", ai->volpath);
        close(ai->archfd);
        return -1;
    }

    if (st.st_size < sizeof(volhead) + sizeof(volfoot))
    {
        errprintf("%s is not a valid fsarchiver volume: file is too small\n", ai->volpath);
        close(ai->archfd);
        return -1;
    }

    // 2. read volfoot (contains a duplicate of things that are in volhead)
    if (lseek64(ai->archfd, st.st_size - sizeof(volfoot), SEEK_SET) < 0)
    {
        sysprintf("lseek64() failed to go to the end of the volume to get the volfoot header\n");
        return -1;
    }

    if (archio_read_low_level(ai, &volfoot, sizeof(volfoot)) != 0)
    {
        errprintf("Failed to read volfoot volume header\n");
        return -1;
    }  

    // 3. read volhead
    if (lseek64(ai->archfd, 0, SEEK_SET) < 0)
    {
        sysprintf("lseek64() failed to go to the beginning of the volume to get the volhead header\n");
        return -1;
    }

    if (archio_read_low_level(ai, &volhead, sizeof(volhead)) != 0)
    {
        errprintf("Failed to read volhead volume header\n");
        return -1;
    }  

    // 4. check that at least one of volhead or volfoot is valid
    //    this way we don't loose the entire archive if one if corrupt
    validhead = false;

    checksum = fletcher32((u8 *)&volfoot.data, sizeof(volfoot.data));
    if ((le32_to_cpu(volfoot.magic) == FSA_MAGIC_IOH)
        && (le16_to_cpu(volfoot.type) == IOHEAD_VOLFOOT)
        && (le32_to_cpu(volfoot.csum) == checksum))
        {
            volnum = le32_to_cpu(volfoot.data.volfoot.volnum);
            minver = le64_to_cpu(volfoot.data.volfoot.minver);
            archid = le32_to_cpu(volfoot.archid);
            validhead = true;
        }
        else
        {
            errprintf("The volume footer is invalid\n");
        }

    checksum = fletcher32((u8 *)&volhead.data, sizeof(volhead.data));
    if ((le32_to_cpu(volhead.magic) == FSA_MAGIC_IOH)
        && (le16_to_cpu(volhead.type) == IOHEAD_VOLHEAD)
        && (le32_to_cpu(volhead.csum) == checksum))
        {
            volnum = le32_to_cpu(volhead.data.volhead.volnum);
            minver = le64_to_cpu(volhead.data.volhead.minver);
            archid = le32_to_cpu(volfoot.archid);
            validhead = true;
        }
        else
        {
            errprintf("The volume header is invalid\n");
        }

    if (validhead == false)
    {
        errprintf("Both the volume header and footer are invalid.\n"
            "This file is either corrupt or not compatible with this\n"
            "fsarchiver version.\n");
        return -1;
    }

    // 5. anaylse data found in the valid volhead/volfoot header

    // check volume number
    if (volnum != ai->curvol)
    {
        errprintf("Unexpected fsarchiver volume number: "
            "found=%d expected=%d\n", (int)volnum, (int)ai->curvol);
        return -1;
    }

    // check minimum version requirement
    curver = FSA_VERSION_BUILD(PACKAGE_VERSION_A, PACKAGE_VERSION_B, PACKAGE_VERSION_C, PACKAGE_VERSION_D);
    if (curver < minver)
    {
        errprintf("Cannot read volume header: wrong fsarchiver version:\n"
            "- current version: %d.%d.%d.%d\n- minimum version required: %d.%d.%d.%d\n", 
            (int)FSA_VERSION_GET_A(curver), (int)FSA_VERSION_GET_B(curver),
            (int)FSA_VERSION_GET_C(curver), (int)FSA_VERSION_GET_D(curver),
            (int)FSA_VERSION_GET_A(minver), (int)FSA_VERSION_GET_B(minver),
            (int)FSA_VERSION_GET_C(minver), (int)FSA_VERSION_GET_D(minver));
        return -1;
    }

    // 6. save or check the archive id
    if (volnum == 0)
    {
        ai->archid = archid;
    }
    else if (archid != ai->archid)
    {
        errprintf("Unexpected fsarchiver archive identifier: "
            "found=%.8x expected=%.8x\n", (int)archid, (int)ai->archid);
        return -1;
    }

    return 0;
}

int archio_close_read(carchio *ai)
{
    if (ai->archfd < 0)
        return -1;

    close(ai->archfd);
    ai->archfd = -1;

    return 0;
}

int archio_close_write(carchio *ai, bool lastvol)
{
    ciohead head;
    int ret = 0;

    if (ai->archfd < 0)
    {
        errprintf("Error: volume is not open\n");
        return -1;
    }

    memset(&head, 0, sizeof(head));
    head.magic = cpu_to_le32(FSA_MAGIC_IOH);
    head.archid = cpu_to_le32(ai->archid);
    head.type = cpu_to_le16(IOHEAD_VOLFOOT);
    head.data.volfoot.volnum = cpu_to_le32(ai->curvol);
    head.data.volfoot.minver = cpu_to_le64(FSA_VERSION_BUILD(0, 7, 0, 0));
    head.data.volfoot.lastvol = cpu_to_le8(lastvol);
    head.csum = cpu_to_le32(fletcher32((u8 *)&head.data, sizeof(head.data)));

    if (archio_write_low_level(ai, (char*)&head, sizeof(head)) != 0)
    {
        errprintf("archio_write_low_level() failed to write volume footer\n");
        ret = -1;
    }

    fsync(ai->archfd);
    close(ai->archfd);
    ai->archfd=-1;

    return ret;
}

int archio_delete_all(carchio *ai)
{
    char volpath[PATH_MAX];
    int count;
    int i;

    if (ai->archfd >= 0)
    {
        archio_close_write(ai, false);
    }

    if (ai->newarch == true)
    {
        count=strlist_count(&ai->vollist);
        for (i=0; i < count; i++)
        {
            if (strlist_getitem(&ai->vollist, i, volpath, sizeof(volpath))==0)
            {
                if (unlink(volpath) == 0)
                    msgprintf(MSG_FORCE, "removed %s\n", volpath);
                else
                    errprintf("cannot remove %s\n", volpath);
            }
        }
    }

    return 0;
}

int archio_write_block(carchio *ai, char *buffer, u32 bufsize, u32 bytesused)
{
    ciohead head;

    memset(&head, 0, sizeof(head));
    head.magic = cpu_to_le32(FSA_MAGIC_IOH);
    head.archid = cpu_to_le32(ai->archid);
    head.type = cpu_to_le16(IOHEAD_BLKHEAD);
    head.data.blkhead.blocknum = cpu_to_le64(ai->curblock++);
    head.data.blkhead.bytesused = cpu_to_le32(bytesused);
    head.csum = cpu_to_le32(fletcher32((u8 *)&head.data, sizeof(head.data)));

    // 1. close current volume if splitting enabled and current volume reached maxvolsize
    if (archio_split_check(ai, bufsize) == true)
    {
        archio_close_write(ai, false);
        archio_incvolume(ai);
    }

    // 2. create new volume if there is no current volume open
    if (ai->archfd == -1)
    {
        msgprintf(MSG_VERB2, "Creating volume %.3d: [%s]\n", (int)ai->curvol, ai->volpath);
        if (archio_open_write(ai)!=0)
        {
            msgprintf(MSG_STACK, "archio_open_write() failed\n");
            return -1;
        }
    }

    // 3. write blk header
    if (archio_write_low_level(ai, (char *)&head, sizeof(head)) != 0)
    {
        msgprintf(MSG_STACK, "archio_write_low_level(%ld) failed to write data\n", (long)bufsize);
        return -1;
    }

    // 4. write current buffer to the archive
    if (archio_write_low_level(ai, buffer, bufsize) != 0)
    {
        msgprintf(MSG_STACK, "archio_write_low_level(%ld) failed to write data\n", (long)bufsize);
        return -1;
    }

    return 0;
}

int archio_read_block(carchio *ai, char *buffer, u32 datsize, u32 *bytesused)
{
    ciohead head;
    u8 lastvol;
    u16 type;
    bool sumok;

    do
    {
        // 1. open volume if there is no current volume open
        if (ai->archfd == -1)
        {
            msgprintf(MSG_VERB2, "Opening volume %.3d: [%s]\n", (int)ai->curvol, ai->volpath);
            if (archio_open_read(ai) != 0)
            {
                msgprintf(MSG_STACK, "archio_open_read() failed\n");
                return -1;
            }
        }

        // 2. read low-level iohead
        if (archio_read_iohead(ai, &head, &sumok) != 0)
        {
            msgprintf(MSG_STACK, "archio_read_iohead() failed\n");
            return -1;
        }
        type = le16_to_cpu(head.type);

        // 3. handle volume splitting
        if (type == IOHEAD_VOLFOOT)
        {
            archio_close_read(ai);
            lastvol = le8_to_cpu(head.data.volfoot.lastvol);
            if (lastvol == true)
            {
                return FSAERR_ENDOFFILE;
            }
            else
            {
                archio_incvolume(ai);
            }
        }

        // 4. read data block
        if (type == IOHEAD_BLKHEAD)
        {
            if (archio_read_low_level(ai, buffer, datsize) != 0)
            {
                msgprintf(MSG_STACK, "archio_read_low_level(%ld) failed\n", (long)datsize);
                return -1;
            }
            *bytesused = le32_to_cpu(head.data.blkhead.bytesused);
        }
    }
    while (type != IOHEAD_BLKHEAD);

    return FSAERR_SUCCESS;
}

int archio_read_low_level(carchio *ai, void *data, u32 bufsize)
{
    long lres;

    errno = 0;
    if ((lres = read(ai->archfd, (char*)data, (long)bufsize)) != (long)bufsize)
    {
        if ((lres >= 0) && (lres < (long)bufsize))
        {
            sysprintf("read(size=%ld) failed: unexpected end of archive volume: lres=%ld\n", (long)bufsize, lres);
            return -1;
        }
        else
        {
            sysprintf("read(size=%ld) failed: lres=%ld\n", (long)bufsize, lres);
            return -1;
        }

        return -1;
    }

    return 0;
}

int archio_write_low_level(carchio *ai, char *buffer, u32 bufsize)
{
    struct statvfs64 statvfsbuf;
    char textbuf[128];
    long lres;

    errno = 0;
    if ((lres = write(ai->archfd, buffer, bufsize)) != bufsize)
    {
        errprintf("write(size=%ld) returned %ld\n", (long)bufsize, (long)lres);
        if ((lres >= 0) && (lres < (long)bufsize)) // probably "no space left"
        {
            if (fstatvfs64(ai->archfd, &statvfsbuf)!=0)
            {   sysprintf("fstatvfs(fd=%d) failed\n", ai->archfd);
                return -1;
            }
            
            u64 freebytes = statvfsbuf.f_bfree * statvfsbuf.f_bsize;
            errprintf("Can't write to the archive file. Space on device is %s. \n"
                "If the archive is being written to a FAT filesystem, you may have reached \n"
                "the maximum filesize that it can handle (in general 2 GB)\n", 
                format_size(freebytes, textbuf, sizeof(textbuf), 'h'));
            return -1;
        }
        else // another error
        {
            sysprintf("write(size=%ld) failed\n", (long)bufsize);
            return -1;
        }
    }

    return 0;
}

int archio_read_iohead(carchio *ai, ciohead *head, bool *csumok)
{
    ciohead temphead;
    u64 bytesignored = 0;
    s64 curpos = 0;
    bool valid = false;
    u32 checksum;

    // init
    memset(&temphead, 0, sizeof(temphead));
    memset(head, 0, sizeof(ciohead));

    // search for next read header marker and magic (it may be further if corruption in archive)
    if ((curpos = lseek64(ai->archfd, 0, SEEK_CUR)) < 0)
    {
        sysprintf("lseek64() failed to get the current position in archive\n");
        return -1;
    }

    // read until we found a valid io-header (skip rubbish if archive is corrupt)
    do
    {
        if (archio_read_low_level(ai, (char*)&temphead, sizeof(temphead)) != 0)
        {
            errprintf("failed to read io-header\n");
            return -1;
        }

        valid = ((le32_to_cpu(temphead.magic) == FSA_MAGIC_IOH) && (le32_to_cpu(temphead.archid) == ai->archid));

        if (valid == false)
        {
            if (lseek64(ai->archfd, curpos++, SEEK_SET) < 0)
            {
                sysprintf("lseek64(pos=%lld, SEEK_SET) failed\n", (long long)curpos);
                return -1;
            }
            bytesignored++;
        }
    }
    while (valid == false);

    if (valid == false)
    {
        return -1;
    }

    if (bytesignored > 0)
    {
        errprintf("skipped %lld bytes of data to find a valid low-level header\n", (long long)bytesignored);
    }

    checksum = fletcher32((u8 *)&temphead.data, sizeof(temphead.data));
    *csumok = (checksum == le32_to_cpu(temphead.csum));
    memcpy(head, &temphead, sizeof(temphead));

    return 0;
}

int archio_split_check(carchio *ai, u32 size)
{
    s64 cursize=0;

    if ((ai->archfd >= 0) && ((cursize = archio_get_currentpos(ai)) >= 0) && (g_options.splitsize > 0) && (cursize + size + sizeof(ciohead) > g_options.splitsize))
    {
        msgprintf(MSG_DEBUG2, "splitchk: YES --> archfd=%d, cursize=%lld, g_options.splitsize=%lld, cursize+size=%lld, wb->size=%lld\n",
            (int)ai->archfd, (long long)cursize, (long long)g_options.splitsize, (long long)cursize+size, (long long)size);
        return true;
    }
    else
    {
        msgprintf(MSG_DEBUG2, "splitchk: NO --> archfd=%d, cursize=%lld, g_options.splitsize=%lld, cursize+wb->size=%lld, wb->size=%lld\n",
            (int)ai->archfd, (long long)cursize, (long long)g_options.splitsize, (long long)cursize+size, (long long)size);
        return false;
    }
}

/*int archio_is_path_to_curvol(carchio *ai, char *path)
{
    assert(ai);
    assert(path);
    return strncmp(ai->volpath, path, PATH_MAX)==0 ? true : false;
}*/