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

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <gcrypt.h>

#include "fsarchiver.h"
#include "archio.h"
#include "options.h"
#include "syncthread.h"
#include "iobuffer.h"
#include "common.h"
#include "error.h"
#include "queue.h"
#include "fec.h"

struct s_fecmainhead;
typedef struct s_fecmainhead cfecmainhead;

struct s_fecblockhead;
typedef struct s_fecblockhead cfecblockhead;

struct __attribute__ ((__packed__)) s_fecmainhead
{
    u32 magic; // always set to FSA_MAGIC_FEC
    u16 version; // allow modifications in later versions
    char md5sum[16]; // checksum of the data stored in that header
    union
    {
        struct {u16 fec_value_n;} __attribute__ ((__packed__)) fecv1;
        char maxsize[4074]; // reserve a fixed size for specific data
    }
    data;
};

struct __attribute__ ((__packed__)) s_fecblockhead
{
    char md5sum[16];
};

void *thread_iobuffer_writer_fct(void *args)
{
    char buffer_fec[FSA_FEC_MAXVAL_N * (FSA_FEC_PACKET_SIZE+sizeof(cfecblockhead))];
    char buffer_raw[FSA_FEC_VALUE_K * FSA_FEC_PACKET_SIZE];
    void *fec_src_pkt[FSA_FEC_VALUE_K];
    void *fec_handle = NULL;
    cfecmainhead fecmainhead;
    cfecblockhead fecchksum;
    int fec_value_n;
    char archive[PATH_MAX];
    carchio *ai = NULL;
    u32 curoffset;
    u32 blocksize;
    u32 bytesused;
    u64 blocknum;
    int res;
    int i;

    // initializations
    blocknum = 0;
    inc_secthreads();
    blocksize = iobuffer_get_block_size(g_iobuffer);
    assert(blocksize == sizeof(buffer_raw));
    assert(sizeof(cfecblockhead) == 16);
    assert(sizeof(cfecmainhead) == 4096);

    // initializes archive
    path_force_extension(archive, sizeof(archive), g_archive, ".fsa");
    if ((ai = archio_alloc(archive)) == NULL)
    {
        errprintf("archio_alloc() failed()\n");
        goto thread_iobuffer_writer_error;
    }
    archio_generate_id(ai);

    // prepares FEC main header
    fec_value_n = FSA_FEC_VALUE_K + g_options.ecclevel;
    memset(&fecmainhead, 0, sizeof(cfecmainhead));
    fecmainhead.magic = cpu_to_le32(FSA_MAGIC_FEC);
    fecmainhead.version = cpu_to_le16(1);
    fecmainhead.data.fecv1.fec_value_n = cpu_to_le16(fec_value_n);
    //printf("DEBUG: gcry_md_hash_buffer(GCRY_MD_MD5, fecmainhead.md5sum, &fecmainhead.data, sizeof(fecmainhead.data)=%d);\n", (int)sizeof(fecmainhead.data));
    gcry_md_hash_buffer(GCRY_MD_MD5, fecmainhead.md5sum, &fecmainhead.data, sizeof(fecmainhead.data));

    // initializes FEC
    msgprintf(MSG_DEBUG1, "fec_new(k=%d, n=%d)\n", (int)FSA_FEC_VALUE_K, (int)fec_value_n);
    if ((fec_handle = fec_new(FSA_FEC_VALUE_K, fec_value_n)) == NULL)
    {
        errprintf("fec_new(k=%d, n=%d) failed\n", (int)FSA_FEC_VALUE_K, (int)fec_value_n);
        goto thread_iobuffer_writer_error;
    }
    for (i=0; i < FSA_FEC_VALUE_K; i++)
    {
        fec_src_pkt[i] = &buffer_raw[i * FSA_FEC_PACKET_SIZE];
    }

    // write two copies of the FEC main header (because loosing it would be a disaster)
    for (i=0; i < FSA_FEC_MAINHEAD_COPIES; i++)
    {
        if (archio_write_block(ai, (char*)&fecmainhead, sizeof(cfecmainhead), sizeof(cfecmainhead)) != 0)
        {
            msgprintf(MSG_STACK, "cannot write FEC header: archio_write_block() failed\n");
            goto thread_iobuffer_writer_error;
        }
    }

    // main loop
    while ((res = iobuffer_read_fec_block(g_iobuffer, buffer_raw, blocksize, &bytesused)) == FSAERR_SUCCESS)
    {
        /*
        char debugsum[16];
        memset(debugsum, 0, 16);
        gcry_md_hash_buffer(GCRY_MD_MD5, debugsum, buffer_raw, blocksize);
        u64 *temp1 = (u64 *)(debugsum+0);
        u64 *temp2 = (u64 *)(debugsum+8);
        printf("FDEBUG-SAVE: blocksize=%d blocknum=%d bytesused=%d md5=[%.16llx%.16llx]\n", (int)blocksize, (int)blocknum, (int)bytesused, (long long)*temp1, (long long)*temp2);
        */

        curoffset = 0;
        for (i=0; i < fec_value_n; i++)
        {
            fec_encode(fec_handle, fec_src_pkt, &buffer_fec[curoffset], i, FSA_FEC_PACKET_SIZE);
            gcry_md_hash_buffer(GCRY_MD_MD5, fecchksum.md5sum, &buffer_fec[curoffset], FSA_FEC_PACKET_SIZE);
            memcpy(&buffer_fec[curoffset + FSA_FEC_PACKET_SIZE], &fecchksum, sizeof(cfecblockhead));
            curoffset += FSA_FEC_PACKET_SIZE + sizeof(cfecblockhead);
        }

        //printf("FDEBUG-SAVE: archio_write_block(size=%ld, bytesused=%d)\n", (long)curoffset, (int)bytesused);
        if (archio_write_block(ai, buffer_fec, curoffset, bytesused) != 0)
        {
            msgprintf(MSG_STACK, "cannot write block to archive: archio_write_block() failed\n");
            goto thread_iobuffer_writer_error;
        }
        
        blocknum++;
    }

    if (res != FSAERR_ENDOFFILE)
    {
        errprintf("archio_read_block() failed with res=%d\n", res);
        goto thread_iobuffer_writer_error;
    }

    fec_free(fec_handle);
    archio_close_write(ai, true);
    msgprintf(MSG_DEBUG1, "THREAD-IOBUF-WRITER: exit success\n");
    archio_destroy(ai);
    dec_secthreads();
    return NULL;

thread_iobuffer_writer_error:
    msgprintf(MSG_DEBUG1, "THREAD-IOBUF-WRITER: exit remove\n");
    set_stopfillqueue(); // say to the main thread it must stop
    fec_free(fec_handle);
    archio_close_write(ai, false);
    archio_delete_all(ai);
    archio_destroy(ai);
    dec_secthreads();
    return NULL;
}

void *thread_iobuffer_reader_fct(void *args)
{
    char buffer_fec[FSA_FEC_MAXVAL_N * (FSA_FEC_PACKET_SIZE + sizeof(cfecblockhead))];
    char buffer_raw[FSA_FEC_VALUE_K * FSA_FEC_PACKET_SIZE];
    char buffer_dec[FSA_FEC_VALUE_K * FSA_FEC_PACKET_SIZE];
    void *fec_src_pkt[FSA_FEC_VALUE_K];
    int fec_indexes[FSA_FEC_VALUE_K];
    cfecmainhead fecmainhead;
    cfecmainhead fectemphead;
    bool mainheader_found;
    void *fec_handle = NULL;
    int fec_value_n = 0;
    carchio *ai = NULL;
    char md5sum[16];
    int curoffset;
    int goodpkts;
    int badpkts;
    u32 encodedsize;
    u32 blocksize;
    u32 bytesused;
    u64 blocknum;
    int res;
    int i;

    // initializations
    memset(&fecmainhead, 0, sizeof(fecmainhead));
    blocknum = 0;
    inc_secthreads();
    blocksize = iobuffer_get_block_size(g_iobuffer);
    assert(blocksize == sizeof(buffer_raw));
    assert(sizeof(cfecblockhead) == 16);
    assert(sizeof(cfecmainhead) == 4096);

    // initializes archive
    if ((ai = archio_alloc(g_archive)) == NULL)
    {
        errprintf("archio_alloc() failed\n");
        goto thread_iobuffer_reader_error;
    }

    // read all copies of the main FEC header
    for (i=0, mainheader_found = false; i < FSA_FEC_MAINHEAD_COPIES; i++)
    {
        // read header from archive file into a temp structure
        if (archio_read_block(ai, (char*)&fectemphead, sizeof(cfecmainhead), &bytesused) != FSAERR_SUCCESS)
        {
            errprintf("iobuffer_write_fec_block() failed to read the main FEC header\n");
            goto thread_iobuffer_reader_error;
        }
        // use that copy if it is valid (magic and md5 checksum are correct)
        if (le32_to_cpu(fectemphead.magic) == FSA_MAGIC_FEC)
        {
            gcry_md_hash_buffer(GCRY_MD_MD5, md5sum, &fectemphead.data, sizeof(fectemphead.data));
            if (memcmp(md5sum, fectemphead.md5sum, sizeof(md5sum)) == 0)
            {
                mainheader_found = true;
                memcpy(&fecmainhead, &fectemphead, sizeof(cfecmainhead));
            }
        }
    }

    // alalyse data from the main FEC header
    if (mainheader_found == false)
    {
        errprintf("cannot read the main FEC header from the archive: all copies have corruptions\n");
        goto thread_iobuffer_reader_error;
    }
    if (le16_to_cpu(fecmainhead.version) != 1)
    {
        errprintf("unsupported version in the main FEC header\n");
        goto thread_iobuffer_reader_error;
    }
    fec_value_n = le16_to_cpu(fecmainhead.data.fecv1.fec_value_n);
    if ((fec_value_n < FSA_FEC_VALUE_K) || (fec_value_n > FSA_FEC_MAXVAL_N))
    {
        errprintf("invalid value for fec_value_n found in the main FEC header: %d\n", (int)fec_value_n);
        goto thread_iobuffer_reader_error;
    }

    // initializes FEC
    msgprintf(MSG_DEBUG1, "fec_new(k=%d, n=%d)\n", (int)FSA_FEC_VALUE_K, (int)fec_value_n);
    if ((fec_handle = fec_new(FSA_FEC_VALUE_K, fec_value_n)) == NULL)
    {
        errprintf("fec_new(k=%d, n=%d) failed\n", (int)FSA_FEC_VALUE_K, (int)fec_value_n);
        goto thread_iobuffer_reader_error;
    }

    // read all fec encoded blocks from archive (one fec encoded block = N packets)
    encodedsize = fec_value_n * (FSA_FEC_PACKET_SIZE + sizeof(cfecblockhead));
    while ((res = archio_read_block(ai, buffer_fec, encodedsize, &bytesused)) == FSAERR_SUCCESS)
    {
        goodpkts = 0;
        badpkts = 0;
        curoffset = 0;

        // try to read K good packets from the list of N packets
        for (i=0; (goodpkts < FSA_FEC_VALUE_K) && (i < fec_value_n); i++)
        {
            // simulate corruptions in the low-level block from the archive
            /*struct timeval now;
            gettimeofday(&now, NULL);
            if ((now.tv_usec % 11) == 0)
            {
                 memset(&buffer_fec[curoffset], 0xFF, FSA_FEC_PACKET_SIZE);
            }*/

            gcry_md_hash_buffer(GCRY_MD_MD5, md5sum, &buffer_fec[curoffset], FSA_FEC_PACKET_SIZE);
            if (memcmp(md5sum, &buffer_fec[curoffset + FSA_FEC_PACKET_SIZE], sizeof(cfecblockhead)) == 0)
            {
                fec_src_pkt[goodpkts] = &buffer_raw[goodpkts * FSA_FEC_PACKET_SIZE];
                memcpy(fec_src_pkt[goodpkts], &buffer_fec[curoffset], FSA_FEC_PACKET_SIZE);
                fec_indexes[goodpkts] = i;
                goodpkts++;
            }
            else
            {
                badpkts++;
            }
            curoffset += FSA_FEC_PACKET_SIZE + sizeof(cfecblockhead);
        }

        if (goodpkts == FSA_FEC_VALUE_K) // enough good packets found
        {
            res = fec_decode(fec_handle, fec_src_pkt, fec_indexes, FSA_FEC_PACKET_SIZE);
            for (i=0; i < FSA_FEC_VALUE_K; i++)
            {
                memcpy(&buffer_dec[i * FSA_FEC_PACKET_SIZE], fec_src_pkt[i], FSA_FEC_PACKET_SIZE);
            }

            /*
            char debugsum[16]; 
            memset(debugsum, 0, 16);
            gcry_md_hash_buffer(GCRY_MD_MD5, debugsum, buffer_dec, blocksize);
            u64 *temp1 = (u64 *)(debugsum+0);
            u64 *temp2 = (u64 *)(debugsum+8);
            printf("FDEBUG-REST: blocksize=%d blocknum=%d md5=[%.16llx%.16llx] blocksize=%d, bytesused=%d\n", (int)blocksize, (int)blocknum, (long long)*temp1, (long long)*temp2, (int)blocksize, (int)bytesused);
            */

            if (iobuffer_write_fec_block(g_iobuffer, buffer_dec, blocksize, bytesused) != 0)
            {
                errprintf("iobuffer_write_fec_block() failed\n");
                goto thread_iobuffer_reader_error;
            }

            if (badpkts > 0) // if errors have been found in the FEC packets
            {
                errprintf("the error-correction-code has fixed all corruptions in archive block %lld: %d bad packets out of %d packets\n", (long long)blocknum, (int)badpkts, (int)fec_value_n);
            }
        }
        else if (goodpkts < FSA_FEC_VALUE_K) // too many bad packets
        {
            errprintf("cannot fix corruptions in archive block %lld: too many bad packets (%d bad packets out of %d packets)\n", (long long)blocknum, (int)badpkts, (int)fec_value_n);
        }

        blocknum++;
    }

    if (res != FSAERR_ENDOFFILE)
    {
        errprintf("archio_read_block() failed with res=%d\n", res);
        goto thread_iobuffer_reader_error;
    }

    msgprintf(MSG_DEBUG1, "THREAD-IOBUF-READER: exit success\n");
    fec_free(fec_handle);
    iobuffer_set_end_of_queue(g_iobuffer, true);
    archio_destroy(ai);
    dec_secthreads();
    return NULL;

thread_iobuffer_reader_error:
    msgprintf(MSG_DEBUG1, "THREAD-IOBUF-READER: queue_set_end_of_queue(g_queue, true)\n");
    fec_free(fec_handle);
    iobuffer_set_end_of_queue(g_iobuffer, true);
    queue_set_end_of_queue(g_queue, true); // don't wait for more data from this thread
    dec_secthreads();
    msgprintf(MSG_DEBUG1, "THREAD-IOBUF-READER: exit\n");
    return NULL;
}