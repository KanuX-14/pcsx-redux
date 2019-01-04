/***************************************************************************
 *   Copyright (C) 2007 PCSX-df Team                                       *
 *   Copyright (C) 2009 Wei Mingzhi                                        *
 *   Copyright (C) 2012 notaz                                              *
 *   Copyright (C) 2002-2011 Neill Corlett                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <process.h>
#define strcasecmp _stricmp
#else
#include <limits.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include <zlib.h>

#include <algorithm>

#include "core/cdriso.h"
#include "core/cdrom.h"
#include "core/plugins.h"
#include "core/ppf.h"
#include "core/psxemulator.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
}

unsigned int g_cdrIsoMultidiskCount;
unsigned int g_cdrIsoMultidiskSelect;

static File *s_cdHandle = NULL;
static File *s_subHandle = NULL;

static bool s_subChanMixed = false;
static bool s_subChanRaw = false;
static bool s_subChanMissing = false;

static bool s_multifile = false;
static bool s_isMode1ISO = false;  // TODO: use sector size/mode info from CUE also?

static unsigned char s_cdbuffer[PCSX::CDRom::CD_FRAMESIZE_RAW];
static unsigned char s_subbuffer[PCSX::CDRom::SUB_FRAMESIZE];

static bool s_playing = false;
static bool s_cddaBigEndian = false;
static unsigned int s_cddaCurPos = 0;

/* Frame offset into CD image where pregap data would be found if it was there.
 * If a game seeks there we must *not* return subchannel data since it's
 * not in the CD image, so that cdrom code can fake subchannel data instead.
 * XXX: there could be multiple pregaps but PSX dumps only have one? */
static unsigned int s_pregapOffset;

// compressed image stuff
static struct compr_img_t {
    unsigned char buff_raw[16][PCSX::CDRom::CD_FRAMESIZE_RAW];
    unsigned char buff_compressed[PCSX::CDRom::CD_FRAMESIZE_RAW * 16 + 100];
    unsigned int *index_table;
    unsigned int index_len;
    unsigned int block_shift;
    unsigned int current_block;
    unsigned int sector_in_blk;
} * compr_img;

static ssize_t (*s_cdimg_read_func)(File *f, unsigned int base, void *dest, int sector);

char *CALLBACK CDR__getDriveLetter(void);
long CALLBACK CDR__configure(void);
long CALLBACK CDR__test(void);
void CALLBACK CDR__about(void);
long CALLBACK CDR__setfilename(char *filename);
long CALLBACK CDR__getStatus(struct CdrStat *stat);

static void DecodeRawSubData(void);

class File {
  public:
    void close() {
        if (m_handle) fclose(m_handle);
    }
    ssize_t seek(ssize_t pos, int wheel) {
        if (m_handle) return fseek(m_handle, pos, wheel);
        if (!m_data) return -1;
        switch (wheel) {
            case SEEK_SET:
                m_ptr = pos;
                break;
            case SEEK_END:
                m_ptr = m_size - pos;
                break;
            case SEEK_CUR:
                m_ptr += pos;
                break;
        }
        m_ptr = std::min(std::max(m_ptr, m_size), 0);
        return m_ptr;
    }
    ssize_t tell() {
        if (m_handle) return ftell(m_handle);
        if (m_data) return m_ptr;
        return -1;
    }
    void flush() {
        if (m_handle) fflush(m_handle);
    }
    File(void *data, ssize_t size) {
        if (data) {
            m_data = static_cast<uint8_t *>(data);
        } else {
            assert(size == 1);
            m_data = &m_internalBuffer;
        }
        m_size = size;
    }
    File(const char *filename) { m_handle = fopen(filename, "rb"); }
    ssize_t read(void *dest, ssize_t size) {
        if (m_handle) return fread(dest, 1, size, m_handle);
        if (!m_data) return -1;
        size = std::max(m_size - m_ptr, size);
        if (size == 0) return -1;
        memcpy(dest, m_data + m_ptr, size);
        m_ptr += size;
        return size;
    }
    ssize_t write(const void *dest, size_t size) {
        assert(0);
        return -1;
    }
    int getc() {
        if (m_handle) return fgetc(m_handle);
        if (!m_data) return -1;
        if (m_size == m_ptr) return -1;
        return m_data[m_ptr++];
    }
    bool failed() { return m_ptr || m_data; }

  private:
    static const uint8_t m_internalBuffer = 0;
    FILE *m_handle = NULL;
    ssize_t m_ptr = 0;
    ssize_t m_size = 0;
    const uint8_t *m_data = NULL;
};

////////////////////////////////////////////////////////////////////////////////
//
// Sector types
//
// Mode 1
// -----------------------------------------------------
//        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
// 0000h 00 FF FF FF FF FF FF FF FF FF FF 00 [-ADDR-] 01
// 0010h [---DATA...
// ...
// 0800h                                     ...DATA---]
// 0810h [---EDC---] 00 00 00 00 00 00 00 00 [---ECC...
// ...
// 0920h                                      ...ECC---]
// -----------------------------------------------------
//
// Mode 2 (XA), form 1
// -----------------------------------------------------
//        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
// 0000h 00 FF FF FF FF FF FF FF FF FF FF 00 [-ADDR-] 02
// 0010h [--FLAGS--] [--FLAGS--] [---DATA...
// ...
// 0810h             ...DATA---] [---EDC---] [---ECC...
// ...
// 0920h                                      ...ECC---]
// -----------------------------------------------------
//
// Mode 2 (XA), form 2
// -----------------------------------------------------
//        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
// 0000h 00 FF FF FF FF FF FF FF FF FF FF 00 [-ADDR-] 02
// 0010h [--FLAGS--] [--FLAGS--] [---DATA...
// ...
// 0920h                         ...DATA---] [---EDC---]
// -----------------------------------------------------
//
// ADDR:  Sector address, encoded as minutes:seconds:frames in BCD
// FLAGS: Used in Mode 2 (XA) sectors describing the type of sector; repeated
//        twice for redundancy
// DATA:  Area of the sector which contains the actual data itself
// EDC:   Error Detection Code
// ECC:   Error Correction Code
//

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define ECM_HEADER_SIZE 4

uint32_t len_decoded_ecm_buffer = 0;  // same as decoded ECM file length or 2x size
uint32_t len_ecm_savetable = 0;       // same as sector count of decoded ECM file or 2x count

#ifdef ENABLE_ECM_FULL  // setting this makes whole ECM to be decoded in-memory meaning buffer could eat up to 700 MB of
                        // memory
uint32_t decoded_ecm_sectors = 1;  // initially sector 1 is always decoded
#else
uint32_t decoded_ecm_sectors = 0;  // disabled
#endif

bool ecm_file_detected = false;
uint32_t prevsector;

File *decoded_ecm = NULL;
void *decoded_ecm_buffer;

// Function that is used to read CD normally
ssize_t (*cdimg_read_func_o)(File *f, unsigned int base, void *dest, int sector) = NULL;

typedef struct ECMFILELUT {
    int32_t sector;
    int32_t filepos;
} ECMFILELUT;

ECMFILELUT *ecm_savetable = NULL;

static const size_t ECM_SECTOR_SIZE[4] = {1, 2352, 2336, 2336};

////////////////////////////////////////////////////////////////////////////////

static uint32_t get32lsb(const uint8_t *src) {
    return (((uint32_t)(src[0])) << 0) | (((uint32_t)(src[1])) << 8) | (((uint32_t)(src[2])) << 16) |
           (((uint32_t)(src[3])) << 24);
}

static void put32lsb(uint8_t *dest, uint32_t value) {
    dest[0] = (uint8_t)(value);
    dest[1] = (uint8_t)(value >> 8);
    dest[2] = (uint8_t)(value >> 16);
    dest[3] = (uint8_t)(value >> 24);
}

////////////////////////////////////////////////////////////////////////////////
//
// LUTs used for computing ECC/EDC
//
static uint8_t ecc_f_lut[256];
static uint8_t ecc_b_lut[256];
static uint32_t edc_lut[256];

static void eccedc_init(void) {
    size_t i;
    for (i = 0; i < 256; i++) {
        uint32_t edc = i;
        size_t j = (i << 1) ^ (i & 0x80 ? 0x11D : 0);
        ecc_f_lut[i] = j;
        ecc_b_lut[i ^ j] = i;
        for (j = 0; j < 8; j++) {
            edc = (edc >> 1) ^ (edc & 1 ? 0xD8018001 : 0);
        }
        edc_lut[i] = edc;
    }
}

////////////////////////////////////////////////////////////////////////////////
//
// Compute EDC for a block
//
static uint32_t edc_compute(uint32_t edc, const uint8_t *src, size_t size) {
    for (; size; size--) {
        edc = (edc >> 8) ^ edc_lut[(edc ^ (*src++)) & 0xFF];
    }
    return edc;
}

//
// Write ECC block (either P or Q)
//
static void ecc_writepq(const uint8_t *address, const uint8_t *data, size_t major_count, size_t minor_count,
                        size_t major_mult, size_t minor_inc, uint8_t *ecc) {
    size_t size = major_count * minor_count;
    size_t major;
    for (major = 0; major < major_count; major++) {
        size_t index = (major >> 1) * major_mult + (major & 1);
        uint8_t ecc_a = 0;
        uint8_t ecc_b = 0;
        size_t minor;
        for (minor = 0; minor < minor_count; minor++) {
            uint8_t temp;
            if (index < 4) {
                temp = address[index];
            } else {
                temp = data[index - 4];
            }
            index += minor_inc;
            if (index >= size) {
                index -= size;
            }
            ecc_a ^= temp;
            ecc_b ^= temp;
            ecc_a = ecc_f_lut[ecc_a];
        }
        ecc_a = ecc_b_lut[ecc_f_lut[ecc_a] ^ ecc_b];
        ecc[major] = (ecc_a);
        ecc[major + major_count] = (ecc_a ^ ecc_b);
    }
}

//
// Write ECC P and Q codes for a sector
//
static void ecc_writesector(const uint8_t *address, const uint8_t *data, uint8_t *ecc) {
    ecc_writepq(address, data, 86, 24, 2, 86, ecc);          // P
    ecc_writepq(address, data, 52, 43, 86, 88, ecc + 0xAC);  // Q
}

////////////////////////////////////////////////////////////////////////////////

static const uint8_t zeroaddress[4] = {0, 0, 0, 0};

////////////////////////////////////////////////////////////////////////////////
//
// Reconstruct a sector based on type
//
static void reconstruct_sector(uint8_t *sector,  // must point to a full 2352-byte sector
                               int8_t type) {
    //
    // Sync
    //
    sector[0x000] = 0x00;
    sector[0x001] = 0xFF;
    sector[0x002] = 0xFF;
    sector[0x003] = 0xFF;
    sector[0x004] = 0xFF;
    sector[0x005] = 0xFF;
    sector[0x006] = 0xFF;
    sector[0x007] = 0xFF;
    sector[0x008] = 0xFF;
    sector[0x009] = 0xFF;
    sector[0x00A] = 0xFF;
    sector[0x00B] = 0x00;

    switch (type) {
        case 1:
            //
            // Mode
            //
            sector[0x00F] = 0x01;
            //
            // Reserved
            //
            sector[0x814] = 0x00;
            sector[0x815] = 0x00;
            sector[0x816] = 0x00;
            sector[0x817] = 0x00;
            sector[0x818] = 0x00;
            sector[0x819] = 0x00;
            sector[0x81A] = 0x00;
            sector[0x81B] = 0x00;
            break;
        case 2:
        case 3:
            //
            // Mode
            //
            sector[0x00F] = 0x02;
            //
            // Flags
            //
            sector[0x010] = sector[0x014];
            sector[0x011] = sector[0x015];
            sector[0x012] = sector[0x016];
            sector[0x013] = sector[0x017];
            break;
    }

    //
    // Compute EDC
    //
    switch (type) {
        case 1:
            put32lsb(sector + 0x810, edc_compute(0, sector, 0x810));
            break;
        case 2:
            put32lsb(sector + 0x818, edc_compute(0, sector + 0x10, 0x808));
            break;
        case 3:
            put32lsb(sector + 0x92C, edc_compute(0, sector + 0x10, 0x91C));
            break;
    }

    //
    // Compute ECC
    //
    switch (type) {
        case 1:
            ecc_writesector(sector + 0xC, sector + 0x10, sector + 0x81C);
            break;
        case 2:
            ecc_writesector(zeroaddress, sector + 0x10, sector + 0x81C);
            break;
    }

    //
    // Done
    //
}

struct trackinfo {
    enum track_type_t { CLOSED = 0, DATA = 1, CDDA = 2 } type;
    uint8_t start[3];                                           // MSF-format
    uint8_t length[3];                                          // MSF-format
    File *handle;                                               // for multi-track images CDDA
    enum cddatype_t { NONE = 0, BIN = 1, CCDDA = 2 } cddatype;  // BIN, WAV, MP3, APE
    char *decoded_buffer;
    uint32_t len_decoded_buffer;
    char filepath[256];
    uint32_t start_offset;  // byte offset from start of above file
};

#define MAXTRACKS 100 /* How many tracks can a CD hold? */

static int numtracks = 0;
static struct trackinfo ti[MAXTRACKS];

// get a sector from a msf-array
unsigned int msf2sec(const uint8_t *msf) { return ((msf[0] * 60 + msf[1]) * 75) + msf[2]; }

void sec2msf(unsigned int s, uint8_t *msf) {
    msf[0] = s / 75 / 60;
    s = s - msf[0] * 75 * 60;
    msf[1] = s / 75;
    s = s - msf[1] * 75;
    msf[2] = s;
}

// divide a string of xx:yy:zz into m, s, f
static void tok2msf(char *time, char *msf) {
    char *token;

    token = strtok(time, ":");
    if (token) {
        msf[0] = atoi(token);
    } else {
        msf[0] = 0;
    }

    token = strtok(NULL, ":");
    if (token) {
        msf[1] = atoi(token);
    } else {
        msf[1] = 0;
    }

    token = strtok(NULL, ":");
    if (token) {
        msf[2] = atoi(token);
    } else {
        msf[2] = 0;
    }
}

static trackinfo::cddatype_t get_cdda_type(const char *str) {
    const size_t lenstr = strlen(str);
    if (strncmp((str + lenstr - 3), "bin", 3) == 0) {
        return trackinfo::BIN;
    } else {
        return trackinfo::CCDDA;
    }
    return trackinfo::BIN;  // no valid extension or no support; assume bin
}

int get_compressed_cdda_track_length(const char *filepath) {
    int seconds = -1;
    av_log_set_level(AV_LOG_QUIET);

    AVFormatContext *inAudioFormat = NULL;
    inAudioFormat = avformat_alloc_context();
    int errorCode = avformat_open_input(&inAudioFormat, filepath, NULL, NULL);
    avformat_find_stream_info(inAudioFormat, NULL);
    seconds = (int)ceil((double)inAudioFormat->duration / (double)AV_TIME_BASE);
    avformat_close_input(&inAudioFormat);
    return seconds;
}

int decode_packet(int *got_frame, AVPacket pkt, int audio_stream_idx, AVFrame *frame, AVCodecContext *audio_dec_ctx,
                  void *buf, int *size, SwrContext *swr) {
    int ret = 0;
    int decoded = pkt.size;
    *got_frame = 0;

    if (pkt.stream_index == audio_stream_idx) {
        // ret = avcodec_decode_audio4(audio_dec_ctx, frame, got_frame, &pkt);
        ret = avcodec_receive_frame(audio_dec_ctx, frame);
        if (ret == 0) *got_frame = 1;
        if (ret == AVERROR(EAGAIN)) ret = 0;
        if (ret == 0) ret = avcodec_send_packet(audio_dec_ctx, &pkt);
        if (ret == AVERROR(EAGAIN)) {
            ret = 0;
        } else if (ret < 0) {
            PCSX::g_system->SysPrintf(_("Error decoding audio frame\n"));
            return ret;
        } else {
            ret = pkt.size;
        }

        /* Some audio decoders decode only part of the packet, and have to be
         * called again with the remainder of the packet data.
         * Sample: fate-suite/lossless-audio/luckynight-partial.shn
         * Also, some decoders might over-read the packet. */

        decoded = FFMIN(ret, pkt.size);

        if (*got_frame) {
            size_t unpadded_linesize = frame->nb_samples * av_get_bytes_per_sample(AVSampleFormat(frame->format));
            swr_convert(swr, (uint8_t **)&buf, frame->nb_samples, (const uint8_t **)frame->data, frame->nb_samples);
            (*size) += (unpadded_linesize * 2);
        }
    }
    return decoded;
}

int open_codec_context(int *stream_idx, AVFormatContext *fmt_ctx, enum AVMediaType type) {
    int ret, stream_index;
    AVStream *st;
    AVDictionary *opts = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);

    if (ret < 0) {
        PCSX::g_system->SysPrintf(_("Could not find %s stream in input file\n"), av_get_media_type_string(type));
        return ret;
    } else {
        stream_index = ret;
        st = fmt_ctx->streams[stream_index];

        AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            PCSX::g_system->SysPrintf(_("Failed to find %s codec\n"), av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }

        AVCodecContext *dec_ctx = avcodec_alloc_context3(dec);
        if (!dec_ctx) {
            PCSX::g_system->SysPrintf(_("Failed to find %s codec\n"), av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }
        avcodec_parameters_to_context(dec_ctx, st->codecpar);

        /* Init the decoders, with or without reference counting */
        if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
            PCSX::g_system->SysPrintf(_("Failed to open %s codec\n"), av_get_media_type_string(type));
            avcodec_free_context(&dec_ctx);
            return ret;
        }
        avcodec_free_context(&dec_ctx);
        *stream_idx = stream_index;
    }
    return 0;
}

int decode_compressed_cdda_track(char *buf, char *src_filename, int *size) {
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *audio_dec_ctx = NULL;
    AVCodec *audio_codec = NULL;
    AVStream *audio_stream = NULL;
    int audio_stream_idx = -1;
    AVFrame *frame = NULL;
    AVPacket pkt;
    SwrContext *resample_context;
    int ret = 0, got_frame;

    if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
        PCSX::g_system->SysPrintf(_("Could not open source file %s\n"), src_filename);
        return -1;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        PCSX::g_system->SysPrintf(_("Could not find stream information\n"));
        ret = -1;
        goto end;
    }

    if (open_codec_context(&audio_stream_idx, fmt_ctx, AVMEDIA_TYPE_AUDIO) >= 0) {
        audio_stream = fmt_ctx->streams[audio_stream_idx];
    }

    if (!audio_stream) {
        PCSX::g_system->SysPrintf(_("Could not find audio stream in the input, aborting\n"));
        ret = -1;
        goto end;
    }

    audio_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);

    if (!audio_codec) {
        PCSX::g_system->SysPrintf(_("Could not find audio codec for the input, aborting\n"));
        ret = -1;
        goto end;
    }

    audio_dec_ctx = avcodec_alloc_context3(audio_codec);

    if (!audio_dec_ctx) {
        PCSX::g_system->SysPrintf(_("Could not allocate audio codec for the input, aborting\n"));
        ret = -1;
        goto end;
    }

    // init and configure resampler
    resample_context = swr_alloc();
    if (!resample_context) {
        PCSX::g_system->SysPrintf(_("Could not allocate resample context"));
        ret = -1;
        goto end;
    }
    av_opt_set_int(resample_context, "in_channel_layout", audio_dec_ctx->channel_layout, 0);
    av_opt_set_int(resample_context, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
    av_opt_set_int(resample_context, "in_sample_rate", audio_dec_ctx->sample_rate, 0);
    av_opt_set_int(resample_context, "out_sample_rate", 44100, 0);
    av_opt_set_sample_fmt(resample_context, "in_sample_fmt", audio_dec_ctx->sample_fmt, 0);
    av_opt_set_sample_fmt(resample_context, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    if (swr_init(resample_context) < 0) {
        PCSX::g_system->SysPrintf(_("Could not open resample context"));
        ret = -1;
        goto end;
    }

    frame = av_frame_alloc();
    if (!frame) {
        PCSX::g_system->SysPrintf(_("Could not allocate frame\n"));
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* initialize packet, set data to NULL, let the demuxer fill it */
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    /* read frames from the file */
    while (av_read_frame(fmt_ctx, &pkt) >= 0) {
        AVPacket orig_pkt = pkt;
        do {
            ret = decode_packet(&got_frame, pkt, audio_stream_idx, frame, audio_dec_ctx, buf + (*size), size,
                                resample_context);
            if (ret < 0) break;
            pkt.data += ret;
            pkt.size -= ret;
        } while (pkt.size > 0);
        av_packet_unref(&orig_pkt);
    }

    /* flush cached frames */
    pkt.data = NULL;
    pkt.size = 0;
    do {
        decode_packet(&got_frame, pkt, audio_stream_idx, frame, audio_dec_ctx, buf + (*size), size, resample_context);
    } while (got_frame);

end:
    swr_free(&resample_context);
    if (audio_dec_ctx) {
        avcodec_close(audio_dec_ctx);
        avcodec_free_context(&audio_dec_ctx);
    }
    avformat_close_input(&fmt_ctx);
    av_frame_free(&frame);
    return ret < 0;
}

int do_decode_cdda(struct trackinfo *tri, uint32_t tracknumber) {
    tri->decoded_buffer = (char *)malloc(tri->len_decoded_buffer);
    memset(tri->decoded_buffer, 0, tri->len_decoded_buffer - 1);

    if (tri->decoded_buffer == NULL) {
        PCSX::g_system->SysMessage(_("Could not allocate memory to decode CDDA TRACK: %s\n"), tri->filepath);
        tri->handle->close();  // encoded file handle not needed anymore
        delete tri->handle;
        tri->handle = new File(NULL, 1);  // change handle to decoded one
        tri->cddatype = trackinfo::BIN;
        return 0;
    }

    tri->handle->close();  // encoded file handle not needed anymore
    delete tri->handle;

    int ret;
    PCSX::g_system->SysPrintf(_("Decoding audio tr#%u (%s)..."), tracknumber, tri->filepath);

    int len = 0;

    if ((ret = decode_compressed_cdda_track(tri->decoded_buffer, tri->filepath, &len)) == 0) {
        if (len > tri->len_decoded_buffer) {
            PCSX::g_system->SysPrintf(_("Buffer overflow..."));
            PCSX::g_system->SysPrintf(_("Actual %i vs. %i estimated\n"), len, tri->len_decoded_buffer);
            len = tri->len_decoded_buffer;  // we probably segfaulted already, oh well...
        }

        tri->handle = new File(tri->decoded_buffer, len);  // change handle to decoded one
        PCSX::g_system->SysPrintf(_("OK\n"), tri->filepath);
    }
    tri->cddatype = trackinfo::BIN;
    return len;
}

// this function tries to get the .toc file of the given .bin
// the necessary data is put into the ti (trackinformation)-array
static int parsetoc(const char *isofile) {
    char tocname[MAXPATHLEN], filename[MAXPATHLEN], *ptr;
    FILE *fi;
    char linebuf[256], tmp[256], name[256];
    char *token;
    char time[20], time2[20];
    unsigned int t, sector_offs, sector_size;
    unsigned int current_zero_gap = 0;

    numtracks = 0;

    // copy name of the iso and change extension from .bin to .toc
    strncpy(tocname, isofile, sizeof(tocname));
    tocname[MAXPATHLEN - 1] = '\0';
    if (strlen(tocname) >= 4) {
        strcpy(tocname + strlen(tocname) - 4, ".toc");
    } else {
        return -1;
    }

    if ((fi = fopen(tocname, "r")) == NULL) {
        // try changing extension to .cue (to satisfy some stupid tutorials)
        strcpy(tocname + strlen(tocname) - 4, ".cue");
        if ((fi = fopen(tocname, "r")) == NULL) {
            // if filename is image.toc.bin, try removing .bin (for Brasero)
            strcpy(tocname, isofile);
            t = strlen(tocname);
            if (t >= 8 && strcmp(tocname + t - 8, ".toc.bin") == 0) {
                tocname[t - 4] = '\0';
                if ((fi = fopen(tocname, "r")) == NULL) {
                    return -1;
                }
            } else {
                return -1;
            }
        }
    }

    strcpy(filename, tocname);
    if ((ptr = strrchr(filename, '/')) == NULL) ptr = strrchr(filename, '\\');
    if (ptr == NULL)
        *ptr = 0;
    else
        *(ptr + 1) = 0;

    memset(&ti, 0, sizeof(ti));
    s_cddaBigEndian = true;  // cdrdao uses big-endian for CD Audio

    sector_size = PCSX::CDRom::CD_FRAMESIZE_RAW;
    sector_offs = 2 * 75;

    // parse the .toc file
    while (fgets(linebuf, sizeof(linebuf), fi) != NULL) {
        // search for tracks
        strncpy(tmp, linebuf, sizeof(linebuf));
        token = strtok(tmp, " ");

        if (token == NULL) continue;

        if (!strcmp(token, "TRACK")) {
            sector_offs += current_zero_gap;
            current_zero_gap = 0;

            // get type of track
            token = strtok(NULL, " ");
            numtracks++;

            if (!strncmp(token, "MODE2_RAW", 9)) {
                ti[numtracks].type = trackinfo::DATA;
                sec2msf(2 * 75, ti[numtracks].start);  // assume data track on 0:2:0

                // check if this image contains mixed subchannel data
                token = strtok(NULL, " ");
                if (token != NULL && !strncmp(token, "RW", 2)) {
                    sector_size = PCSX::CDRom::CD_FRAMESIZE_RAW + PCSX::CDRom::SUB_FRAMESIZE;
                    s_subChanMixed = true;
                    if (!strncmp(token, "RW_RAW", 6)) s_subChanRaw = true;
                }
            } else if (!strncmp(token, "AUDIO", 5)) {
                ti[numtracks].type = trackinfo::CDDA;
            }
        } else if (!strcmp(token, "DATAFILE")) {
            if (ti[numtracks].type == trackinfo::CDDA) {
                sscanf(linebuf, "DATAFILE \"%[^\"]\" #%d %8s", name, &t, time2);
                ti[numtracks].start_offset = t;
                t = t / sector_size + sector_offs;
                sec2msf(t, (uint8_t *)&ti[numtracks].start);
                tok2msf((char *)&time2, (char *)&ti[numtracks].length);
            } else {
                sscanf(linebuf, "DATAFILE \"%[^\"]\" %8s", name, time);
                tok2msf((char *)&time, (char *)&ti[numtracks].length);
                strcat(filename, name);
                ti[numtracks].handle = new File(filename);
            }
        } else if (!strcmp(token, "FILE")) {
            sscanf(linebuf, "FILE \"%[^\"]\" #%d %8s %8s", name, &t, time, time2);
            tok2msf((char *)&time, (char *)&ti[numtracks].start);
            t += msf2sec(ti[numtracks].start) * sector_size;
            ti[numtracks].start_offset = t;
            t = t / sector_size + sector_offs;
            sec2msf(t, (uint8_t *)&ti[numtracks].start);
            tok2msf((char *)&time2, (char *)&ti[numtracks].length);
        } else if (!strcmp(token, "ZERO") || !strcmp(token, "SILENCE")) {
            // skip unneeded optional fields
            while (token != NULL) {
                token = strtok(NULL, " ");
                if (strchr(token, ':') != NULL) break;
            }
            if (token != NULL) {
                tok2msf(token, tmp);
                current_zero_gap = msf2sec(reinterpret_cast<uint8_t *>(tmp));
            }
            if (numtracks > 1) {
                t = ti[numtracks - 1].start_offset;
                t /= sector_size;
                s_pregapOffset = t + msf2sec(ti[numtracks - 1].length);
            }
        } else if (!strcmp(token, "START")) {
            token = strtok(NULL, " ");
            if (token != NULL && strchr(token, ':')) {
                tok2msf(token, tmp);
                t = msf2sec(reinterpret_cast<uint8_t *>(tmp));
                ti[numtracks].start_offset += (t - current_zero_gap) * sector_size;
                t = msf2sec(ti[numtracks].start) + t;
                sec2msf(t, (uint8_t *)&ti[numtracks].start);
            }
        }
    }
    if (numtracks > 0) s_cdHandle = new File(filename);

    fclose(fi);

    return 0;
}

int handlearchive(const char *isoname, int32_t *accurate_length);
// this function tries to get the .cue file of the given .bin
// the necessary data is put into the ti (trackinformation)-array
static int parsecue(const char *isofile) {
    char cuename[MAXPATHLEN];
    char filepath[MAXPATHLEN];
    char *incue_fname;
    FILE *fi;
    char *token;
    char time[20];
    char *tmp;
    char linebuf[256], tmpb[256], dummy[256];
    unsigned int incue_max_len;
    unsigned int t, file_len, mode, sector_offs;
    unsigned int sector_size = 2352;

    numtracks = 0;

    // copy name of the iso and change extension from .bin to .cue
    strncpy(cuename, isofile, sizeof(cuename));
    cuename[MAXPATHLEN - 1] = '\0';
    if (strlen(cuename) >= 4) {
        strcpy(cuename + strlen(cuename) - 4, ".cue");
    } else {
        return -1;
    }

    if ((fi = fopen(cuename, "r")) == NULL) {
        return -1;
    }

    // Some stupid tutorials wrongly tell users to use cdrdao to rip a
    // "bin/cue" image, which is in fact a "bin/toc" image. So let's check
    // that...
    if (fgets(linebuf, sizeof(linebuf), fi) != NULL) {
        if (!strncmp(linebuf, "CD_ROM_XA", 9)) {
            // Don't proceed further, as this is actually a .toc file rather
            // than a .cue file.
            fclose(fi);
            return parsetoc(isofile);
        }
        fseek(fi, 0, SEEK_SET);
    }

    // build a path for files referenced in .cue
    strncpy(filepath, cuename, sizeof(filepath));
    tmp = strrchr(filepath, '/');
    if (tmp == NULL) tmp = strrchr(filepath, '\\');
    if (tmp != NULL)
        tmp++;
    else
        tmp = filepath;
    *tmp = 0;
    filepath[sizeof(filepath) - 1] = 0;
    incue_fname = tmp;
    incue_max_len = sizeof(filepath) - (tmp - filepath) - 1;

    memset(&ti, 0, sizeof(ti));

    file_len = 0;
    sector_offs = 2 * 75;

    while (fgets(linebuf, sizeof(linebuf), fi) != NULL) {
        strncpy(dummy, linebuf, sizeof(linebuf));
        token = strtok(dummy, " ");

        if (token == NULL) {
            continue;
        }

        if (!strcmp(token, "TRACK")) {
            numtracks++;

            sector_size = 0;
            if (strstr(linebuf, "AUDIO") != NULL) {
                ti[numtracks].type = trackinfo::CDDA;
                sector_size = PCSX::CDRom::CD_FRAMESIZE_RAW;
                // Check if extension is mp3, etc, for compressed audio formats
                if (s_multifile && (ti[numtracks].cddatype = get_cdda_type(filepath)) > trackinfo::BIN) {
                    int seconds = get_compressed_cdda_track_length(filepath) + 0;
                    const bool lazy_decode = true;  // TODO: config param

                    // TODO: get frame length for compressed audio as well
                    ti[numtracks].len_decoded_buffer = 44100 * (16 / 8) * 2 * seconds;
                    strcpy(ti[numtracks].filepath, filepath);
                    file_len = ti[numtracks].len_decoded_buffer / PCSX::CDRom::CD_FRAMESIZE_RAW;

                    // Send to decoder if not lazy decoding
                    if (!lazy_decode) {
                        PCSX::g_system->SysPrintf("\n");
                        file_len = do_decode_cdda(&(ti[numtracks]), numtracks) / PCSX::CDRom::CD_FRAMESIZE_RAW;
                    }
                }
            } else if (sscanf(linebuf, " TRACK %u MODE%u/%u", &t, &mode, &sector_size) == 3) {
                int32_t accurate_len;
                // TODO: if 2048 frame length -> recalculate file_len?
                ti[numtracks].type = trackinfo::DATA;
                // detect if ECM or compressed & get accurate length
                if (handleecm(filepath, s_cdHandle, &accurate_len) == 0 ||
                    handlearchive(filepath, &accurate_len) == 0) {
                    file_len = accurate_len;
                }
            } else {
                PCSX::g_system->SysPrintf(".cue: failed to parse TRACK\n");
                ti[numtracks].type = numtracks == 1 ? trackinfo::DATA : trackinfo::CDDA;
            }
            if (sector_size == 0)  // TODO s_isMode1ISO?
                sector_size = PCSX::CDRom::CD_FRAMESIZE_RAW;
        } else if (!strcmp(token, "INDEX")) {
            if (sscanf(linebuf, " INDEX %02d %8s", &t, time) != 2)
                PCSX::g_system->SysPrintf(".cue: failed to parse INDEX\n");
            tok2msf(time, (char *)&ti[numtracks].start);

            t = msf2sec(ti[numtracks].start);
            ti[numtracks].start_offset = t * sector_size;
            t += sector_offs;
            sec2msf(t, ti[numtracks].start);

            // default track length to file length
            t = file_len - ti[numtracks].start_offset / sector_size;
            sec2msf(t, ti[numtracks].length);

            if (numtracks > 1 && ti[numtracks].handle == NULL) {
                // this track uses the same file as the last,
                // start of this track is last track's end
                t = msf2sec(ti[numtracks].start) - msf2sec(ti[numtracks - 1].start);
                sec2msf(t, ti[numtracks - 1].length);
            }
            if (numtracks > 1 && s_pregapOffset == -1) s_pregapOffset = ti[numtracks].start_offset / sector_size;
        } else if (!strcmp(token, "PREGAP")) {
            if (sscanf(linebuf, " PREGAP %8s", time) == 1) {
                tok2msf(time, dummy);
                sector_offs += msf2sec(reinterpret_cast<uint8_t *>(dummy));
            }
            s_pregapOffset = -1;  // mark to fill track start_offset
        } else if (!strcmp(token, "FILE")) {
            t = sscanf(linebuf, " FILE \"%255[^\"]\"", tmpb);
            if (t != 1) sscanf(linebuf, " FILE %255s", tmpb);

            // absolute path?
            ti[numtracks + 1].handle = new File(tmpb);
            if (ti[numtracks + 1].handle == NULL) {
                // relative to .cue?
                tmp = strrchr(tmpb, '\\');
                if (tmp == NULL) tmp = strrchr(tmpb, '/');
                if (tmp != NULL)
                    tmp++;
                else
                    tmp = tmpb;
                strncpy(incue_fname, tmp, incue_max_len);
                ti[numtracks + 1].handle = new File(filepath);
            }

            // update global offset if this is not first file in this .cue
            if (numtracks + 1 > 1) {
                s_multifile = true;
                sector_offs += file_len;
            }

            file_len = 0;
            if (ti[numtracks + 1].handle == NULL) {
                PCSX::g_system->SysMessage(_("\ncould not open: %s\n"), filepath);
                continue;
            }

            // File length, compressed audio length will be calculated in AUDIO tag
            ti[numtracks + 1].handle->seek(0, SEEK_END);
            file_len = ti[numtracks + 1].handle->tell() / PCSX::CDRom::CD_FRAMESIZE_RAW;

            if (numtracks == 0 && strlen(isofile) >= 4 && strcmp(isofile + strlen(isofile) - 4, ".cue") == 0) {
                // user selected .cue as image file, use its data track instead
                s_cdHandle->close();
                delete s_cdHandle;
                s_cdHandle = new File(filepath);
            }
        }
    }

    fclose(fi);

    return 0;
}

// this function tries to get the .ccd file of the given .img
// the necessary data is put into the ti (trackinformation)-array
static int parseccd(const char *isofile) {
    char ccdname[MAXPATHLEN];
    FILE *fi;
    char linebuf[256];
    unsigned int t;

    numtracks = 0;

    // copy name of the iso and change extension from .img to .ccd
    strncpy(ccdname, isofile, sizeof(ccdname));
    ccdname[MAXPATHLEN - 1] = '\0';
    if (strlen(ccdname) >= 4) {
        strcpy(ccdname + strlen(ccdname) - 4, ".ccd");
    } else {
        return -1;
    }

    if ((fi = fopen(ccdname, "r")) == NULL) {
        return -1;
    }

    memset(&ti, 0, sizeof(ti));

    while (fgets(linebuf, sizeof(linebuf), fi) != NULL) {
        if (!strncmp(linebuf, "[TRACK", 6)) {
            numtracks++;
        } else if (!strncmp(linebuf, "MODE=", 5)) {
            sscanf(linebuf, "MODE=%d", &t);
            ti[numtracks].type = ((t == 0) ? trackinfo::CDDA : trackinfo::DATA);
        } else if (!strncmp(linebuf, "INDEX 1=", 8)) {
            sscanf(linebuf, "INDEX 1=%d", &t);
            sec2msf(t + 2 * 75, ti[numtracks].start);
            ti[numtracks].start_offset = t * 2352;

            // If we've already seen another track, this is its end
            if (numtracks > 1) {
                t = msf2sec(ti[numtracks].start) - msf2sec(ti[numtracks - 1].start);
                sec2msf(t, ti[numtracks - 1].length);
            }
        }
    }

    fclose(fi);

    // Fill out the last track's end based on size
    if (numtracks >= 1) {
        s_cdHandle->seek(0, SEEK_END);
        t = s_cdHandle->tell() / PCSX::CDRom::CD_FRAMESIZE_RAW - msf2sec(ti[numtracks].start) + 2 * 75;
        sec2msf(t, ti[numtracks].length);
    }

    return 0;
}

// this function tries to get the .mds file of the given .mdf
// the necessary data is put into the ti (trackinformation)-array
static int parsemds(const char *isofile) {
    char mdsname[MAXPATHLEN];
    FILE *fi;
    unsigned int offset, extra_offset, l, i;
    unsigned short s;

    numtracks = 0;

    // copy name of the iso and change extension from .mdf to .mds
    strncpy(mdsname, isofile, sizeof(mdsname));
    mdsname[MAXPATHLEN - 1] = '\0';
    if (strlen(mdsname) >= 4) {
        strcpy(mdsname + strlen(mdsname) - 4, ".mds");
    } else {
        return -1;
    }

    if ((fi = fopen(mdsname, "rb")) == NULL) {
        return -1;
    }

    memset(&ti, 0, sizeof(ti));

    // check if it's a valid mds file
    fread(&i, 1, sizeof(unsigned int), fi);
    i = SWAP_LE32(i);
    if (i != 0x4944454D) {
        // not an valid mds file
        fclose(fi);
        return -1;
    }

    // get offset to session block
    fseek(fi, 0x50, SEEK_SET);
    fread(&offset, 1, sizeof(unsigned int), fi);
    offset = SWAP_LE32(offset);

    // get total number of tracks
    offset += 14;
    fseek(fi, offset, SEEK_SET);
    fread(&s, 1, sizeof(unsigned short), fi);
    s = SWAP_LE16(s);
    numtracks = s;

    // get offset to track blocks
    fseek(fi, 4, SEEK_CUR);
    fread(&offset, 1, sizeof(unsigned int), fi);
    offset = SWAP_LE32(offset);

    // skip lead-in data
    while (1) {
        fseek(fi, offset + 4, SEEK_SET);
        if (fgetc(fi) < 0xA0) {
            break;
        }
        offset += 0x50;
    }

    // check if the image contains mixed subchannel data
    fseek(fi, offset + 1, SEEK_SET);
    s_subChanMixed = s_subChanRaw = (fgetc(fi) ? true : false);

    // read track data
    for (i = 1; i <= numtracks; i++) {
        fseek(fi, offset, SEEK_SET);

        // get the track type
        ti[i].type = ((fgetc(fi) == 0xA9) ? trackinfo::CDDA : trackinfo::DATA);
        fseek(fi, 8, SEEK_CUR);

        // get the track starting point
        ti[i].start[0] = fgetc(fi);
        ti[i].start[1] = fgetc(fi);
        ti[i].start[2] = fgetc(fi);

        fread(&extra_offset, 1, sizeof(unsigned int), fi);
        extra_offset = SWAP_LE32(extra_offset);

        // get track start offset (in .mdf)
        fseek(fi, offset + 0x28, SEEK_SET);
        fread(&l, 1, sizeof(unsigned int), fi);
        l = SWAP_LE32(l);
        ti[i].start_offset = l;

        // get pregap
        fseek(fi, extra_offset, SEEK_SET);
        fread(&l, 1, sizeof(unsigned int), fi);
        l = SWAP_LE32(l);
        if (l != 0 && i > 1) s_pregapOffset = msf2sec(ti[i].start);

        // get the track length
        fread(&l, 1, sizeof(unsigned int), fi);
        l = SWAP_LE32(l);
        sec2msf(l, ti[i].length);

        offset += 0x50;
    }

    fclose(fi);
    return 0;
}

static int handlepbp(const char *isofile) {
    struct {
        unsigned int sig;
        unsigned int dontcare[8];
        unsigned int psar_offs;
    } pbp_hdr;
    struct {
        unsigned char type;
        unsigned char pad0;
        unsigned char track;
        char index0[3];
        char pad1;
        char index1[3];
    } toc_entry;
    struct {
        unsigned int offset;
        unsigned int size;
        unsigned int dontcare[6];
    } index_entry;
    char psar_sig[11];
    unsigned int t, cd_length, cdimg_base;
    unsigned int offsettab[8], psisoimg_offs;
    const char *ext = NULL;
    int i, ret;

    if (strlen(isofile) >= 4) ext = isofile + strlen(isofile) - 4;
    if (ext == NULL || (strcmp(ext, ".pbp") != 0 && strcmp(ext, ".PBP") != 0)) return -1;

    s_cdHandle->seek(0, SEEK_SET);

    numtracks = 0;

    ret = s_cdHandle->read(&pbp_hdr, sizeof(pbp_hdr));
    if (ret != sizeof(pbp_hdr)) {
        PCSX::g_system->SysPrintf("failed to read pbp\n");
        goto fail_io;
    }

    ret = s_cdHandle->seek(pbp_hdr.psar_offs, SEEK_SET);
    if (ret != 0) {
        PCSX::g_system->SysPrintf("failed to seek to %x\n", pbp_hdr.psar_offs);
        goto fail_io;
    }

    psisoimg_offs = pbp_hdr.psar_offs;
    s_cdHandle->read(psar_sig, sizeof(psar_sig));
    psar_sig[10] = 0;
    if (strcmp(psar_sig, "PSTITLEIMG") == 0) {
        // multidisk image?
        ret = s_cdHandle->seek(pbp_hdr.psar_offs + 0x200, SEEK_SET);
        if (ret != 0) {
            PCSX::g_system->SysPrintf("failed to seek to %x\n", pbp_hdr.psar_offs + 0x200);
            goto fail_io;
        }

        if (s_cdHandle->read(&offsettab, sizeof(offsettab)) != sizeof(offsettab)) {
            PCSX::g_system->SysPrintf("failed to read offsettab\n");
            goto fail_io;
        }

        for (i = 0; i < sizeof(offsettab) / sizeof(offsettab[0]); i++) {
            if (offsettab[i] == 0) break;
        }
        g_cdrIsoMultidiskCount = i;
        if (g_cdrIsoMultidiskCount == 0) {
            PCSX::g_system->SysPrintf("multidisk eboot has 0 images?\n");
            goto fail_io;
        }

        if (g_cdrIsoMultidiskSelect >= g_cdrIsoMultidiskCount) g_cdrIsoMultidiskSelect = 0;

        psisoimg_offs += offsettab[g_cdrIsoMultidiskSelect];

        ret = s_cdHandle->seek(psisoimg_offs, SEEK_SET);
        if (ret != 0) {
            PCSX::g_system->SysPrintf("failed to seek to %x\n", psisoimg_offs);
            goto fail_io;
        }

        s_cdHandle->read(psar_sig, sizeof(psar_sig));
        psar_sig[10] = 0;
    }

    if (strcmp(psar_sig, "PSISOIMG00") != 0) {
        PCSX::g_system->SysPrintf("bad psar_sig: %s\n", psar_sig);
        goto fail_io;
    }

    // seek to TOC
    ret = s_cdHandle->seek(psisoimg_offs + 0x800, SEEK_SET);
    if (ret != 0) {
        PCSX::g_system->SysPrintf("failed to seek to %x\n", psisoimg_offs + 0x800);
        goto fail_io;
    }

    // first 3 entries are special
    s_cdHandle->seek(sizeof(toc_entry), SEEK_CUR);
    s_cdHandle->read(&toc_entry, sizeof(toc_entry));
    numtracks = PCSX::CDRom::btoi(toc_entry.index1[0]);

    s_cdHandle->read(&toc_entry, sizeof(toc_entry));
    cd_length = PCSX::CDRom::btoi(toc_entry.index1[0]) * 60 * 75 + PCSX::CDRom::btoi(toc_entry.index1[1]) * 75 +
                PCSX::CDRom::btoi(toc_entry.index1[2]);

    for (i = 1; i <= numtracks; i++) {
        s_cdHandle->read(&toc_entry, sizeof(toc_entry));

        ti[i].type = (toc_entry.type == 1) ? trackinfo::CDDA : trackinfo::DATA;

        ti[i].start_offset = PCSX::CDRom::btoi(toc_entry.index0[0]) * 60 * 75 +
                             PCSX::CDRom::btoi(toc_entry.index0[1]) * 75 + PCSX::CDRom::btoi(toc_entry.index0[2]);
        ti[i].start_offset *= 2352;
        ti[i].start[0] = PCSX::CDRom::btoi(toc_entry.index1[0]);
        ti[i].start[1] = PCSX::CDRom::btoi(toc_entry.index1[1]);
        ti[i].start[2] = PCSX::CDRom::btoi(toc_entry.index1[2]);

        if (i > 1) {
            t = msf2sec(ti[i].start) - msf2sec(ti[i - 1].start);
            sec2msf(t, ti[i - 1].length);
        }
    }
    t = cd_length - ti[numtracks].start_offset / 2352;
    sec2msf(t, ti[numtracks].length);

    // seek to ISO index
    ret = s_cdHandle->seek(psisoimg_offs + 0x4000, SEEK_SET);
    if (ret != 0) {
        PCSX::g_system->SysPrintf("failed to seek to ISO index\n");
        goto fail_io;
    }

    compr_img = (compr_img_t *)calloc(1, sizeof(*compr_img));
    if (compr_img == NULL) goto fail_io;

    compr_img->block_shift = 4;
    compr_img->current_block = (unsigned int)-1;

    compr_img->index_len = (0x100000 - 0x4000) / sizeof(index_entry);
    compr_img->index_table = (unsigned int *)malloc((compr_img->index_len + 1) * sizeof(compr_img->index_table[0]));
    if (compr_img->index_table == NULL) goto fail_io;

    cdimg_base = psisoimg_offs + 0x100000;
    for (i = 0; i < compr_img->index_len; i++) {
        ret = s_cdHandle->read(&index_entry, sizeof(index_entry));
        if (ret != sizeof(index_entry)) {
            PCSX::g_system->SysPrintf("failed to read index_entry #%d\n", i);
            goto fail_index;
        }

        if (index_entry.size == 0) break;

        compr_img->index_table[i] = cdimg_base + index_entry.offset;
    }
    compr_img->index_table[i] = cdimg_base + index_entry.offset + index_entry.size;

    return 0;

fail_index:
    free(compr_img->index_table);
    compr_img->index_table = NULL;
fail_io:
    if (compr_img != NULL) {
        free(compr_img);
        compr_img = NULL;
    }
    return -1;
}

static int handlecbin(const char *isofile) {
    struct {
        char magic[4];
        unsigned int header_size;
        unsigned long long total_bytes;
        unsigned int block_size;
        unsigned char ver;  // 1
        unsigned char align;
        unsigned char rsv_06[2];
    } ciso_hdr;
    const char *ext = NULL;
    unsigned int index = 0, plain;
    int i, ret;
    size_t read_len = 0;

    if (strlen(isofile) >= 5) ext = isofile + strlen(isofile) - 5;
    if (ext == NULL || (strcasecmp(ext + 1, ".cbn") != 0 && strcasecmp(ext, ".cbin") != 0)) return -1;

    s_cdHandle->seek(0, SEEK_SET);

    ret = s_cdHandle->read(&ciso_hdr, sizeof(ciso_hdr));
    if (ret != sizeof(ciso_hdr)) {
        PCSX::g_system->SysPrintf("failed to read ciso header\n");
        return -1;
    }

    if (strncmp(ciso_hdr.magic, "CISO", 4) != 0 || ciso_hdr.total_bytes <= 0 || ciso_hdr.block_size <= 0) {
        PCSX::g_system->SysPrintf("bad ciso header\n");
        return -1;
    }
    if (ciso_hdr.header_size != 0 && ciso_hdr.header_size != sizeof(ciso_hdr)) {
        ret = s_cdHandle->seek(ciso_hdr.header_size, SEEK_SET);
        if (ret != 0) {
            PCSX::g_system->SysPrintf("failed to seek to %x\n", ciso_hdr.header_size);
            return -1;
        }
    }

    compr_img = (compr_img_t *)calloc(1, sizeof(*compr_img));
    if (compr_img == NULL) goto fail_io;

    compr_img->block_shift = 0;
    compr_img->current_block = (unsigned int)-1;

    compr_img->index_len = ciso_hdr.total_bytes / ciso_hdr.block_size;
    compr_img->index_table = (unsigned int *)malloc((compr_img->index_len + 1) * sizeof(compr_img->index_table[0]));
    if (compr_img->index_table == NULL) goto fail_io;

    read_len = sizeof(compr_img->index_table[0]) * compr_img->index_len;
    ret = s_cdHandle->read(compr_img->index_table, read_len);
    if (ret != read_len) {
        PCSX::g_system->SysPrintf("failed to read index table\n");
        goto fail_index;
    }

    for (i = 0; i < compr_img->index_len + 1; i++) {
        index = compr_img->index_table[i];
        plain = index & 0x80000000;
        index &= 0x7fffffff;
        compr_img->index_table[i] = (index << ciso_hdr.align) | plain;
    }
    if ((int64_t)index << ciso_hdr.align >= 0x80000000ll) {
        PCSX::g_system->SysPrintf("warning: ciso img too large, expect problems\n");
    }

    return 0;

fail_index:
    free(compr_img->index_table);
    compr_img->index_table = NULL;
fail_io:
    if (compr_img != NULL) {
        free(compr_img);
        compr_img = NULL;
    }
    return -1;
}

// this function tries to get the .sub file of the given .img
static int opensubfile(const char *isoname) {
    char subname[MAXPATHLEN];

    // copy name of the iso and change extension from .img to .sub
    strncpy(subname, isoname, sizeof(subname));
    subname[MAXPATHLEN - 1] = '\0';

    if (strlen(subname) >= 4) {
        strcpy(subname + strlen(subname) - 4, ".sub");
    }

    s_subHandle = new File(subname);
    if (!s_subHandle->failed()) {
        return 0;
    }
    delete s_subHandle;

    if (strlen(subname) >= 8) {
        strcpy(subname + strlen(subname) - 8, ".sub");
    }

    s_subHandle = new File(subname);
    if (s_subHandle->failed()) {
        delete s_subHandle;
        s_subHandle = NULL;
        return -1;
    }

    return 0;
}

static int opensbifile(const char *isoname) {
    char sbiname[MAXPATHLEN];

    strncpy(sbiname, isoname, sizeof(sbiname));
    sbiname[MAXPATHLEN - 1] = '\0';
    if (strlen(sbiname) >= 4) {
        strcpy(sbiname + strlen(sbiname) - 4, ".sbi");
    } else {
        return -1;
    }

    return LoadSBI(sbiname);
}

static ssize_t cdread_normal(File *f, unsigned int base, void *dest, int sector) {
    f->seek(base + sector * PCSX::CDRom::CD_FRAMESIZE_RAW, SEEK_SET);
    return f->read(dest, PCSX::CDRom::CD_FRAMESIZE_RAW);
}

static ssize_t cdread_sub_mixed(File *f, unsigned int base, void *dest, int sector) {
    int ret;

    f->seek(base + sector * (PCSX::CDRom::CD_FRAMESIZE_RAW + PCSX::CDRom::SUB_FRAMESIZE), SEEK_SET);
    ret = f->read(dest, PCSX::CDRom::CD_FRAMESIZE_RAW);
    f->read(s_subbuffer, PCSX::CDRom::SUB_FRAMESIZE);

    if (s_subChanRaw) DecodeRawSubData();

    return ret;
}

static int uncompress2_internal(void *out, unsigned long *out_size, void *in, unsigned long in_size) {
    static z_stream z;
    int ret = 0;

    if (z.zalloc == NULL) {
        // XXX: one-time leak here..
        z.next_in = Z_NULL;
        z.avail_in = 0;
        z.zalloc = Z_NULL;
        z.zfree = Z_NULL;
        z.opaque = Z_NULL;
        ret = inflateInit2(&z, -15);
    } else
        ret = inflateReset(&z);
    if (ret != Z_OK) return ret;

    z.next_in = reinterpret_cast<Bytef *>(in);
    z.avail_in = in_size;
    z.next_out = reinterpret_cast<Bytef *>(out);
    z.avail_out = *out_size;

    ret = inflate(&z, Z_NO_FLUSH);
    // inflateEnd(&z);

    *out_size -= z.avail_out;
    return ret == 1 ? 0 : ret;
}

static int cdread_compressed(File *f, unsigned int base, void *dest, int sector) {
    unsigned long cdbuffer_size, cdbuffer_size_expect;
    unsigned int start_byte, size;
    int is_compressed;
    int ret, block;

    if (base) sector += base / 2352;

    block = sector >> compr_img->block_shift;
    compr_img->sector_in_blk = sector & ((1 << compr_img->block_shift) - 1);

    if (block == compr_img->current_block) {
        // printf("hit sect %d\n", sector);
        goto finish;
    }

    if (sector >= compr_img->index_len * 16) {
        PCSX::g_system->SysPrintf("sector %d is past img end\n", sector);
        return -1;
    }

    start_byte = compr_img->index_table[block] & 0x7fffffff;
    if (s_cdHandle->seek(start_byte, SEEK_SET) != 0) {
        PCSX::g_system->SysPrintf("seek error for block %d at %x: ", block, start_byte);
        perror(NULL);
        return -1;
    }

    is_compressed = !(compr_img->index_table[block] & 0x80000000);
    size = (compr_img->index_table[block + 1] & 0x7fffffff) - start_byte;
    if (size > sizeof(compr_img->buff_compressed)) {
        PCSX::g_system->SysPrintf("block %d is too large: %u\n", block, size);
        return -1;
    }

    if (s_cdHandle->read(is_compressed ? compr_img->buff_compressed : compr_img->buff_raw[0], size) != size) {
        PCSX::g_system->SysPrintf("read error for block %d at %x: ", block, start_byte);
        perror(NULL);
        return -1;
    }

    if (is_compressed) {
        cdbuffer_size_expect = sizeof(compr_img->buff_raw[0]) << compr_img->block_shift;
        cdbuffer_size = cdbuffer_size_expect;
        ret = uncompress2_internal(compr_img->buff_raw[0], &cdbuffer_size, compr_img->buff_compressed, size);
        if (ret != 0) {
            PCSX::g_system->SysPrintf("uncompress failed with %d for block %d, sector %d\n", ret, block, sector);
            return -1;
        }
        if (cdbuffer_size != cdbuffer_size_expect)
            PCSX::g_system->SysPrintf("cdbuffer_size: %lu != %lu, sector %d\n", cdbuffer_size, cdbuffer_size_expect,
                                      sector);
    }

    // done at last!
    compr_img->current_block = block;

finish:
    if (dest != s_cdbuffer)  // copy avoid HACK
        memcpy(dest, compr_img->buff_raw[compr_img->sector_in_blk], PCSX::CDRom::CD_FRAMESIZE_RAW);
    return PCSX::CDRom::CD_FRAMESIZE_RAW;
}

static ssize_t cdread_2048(File *f, unsigned int base, void *dest, int sector) {
    int ret;

    f->seek(base + sector * 2048, SEEK_SET);
    ret = f->read((char *)dest + 12 * 2, 2048);

    // not really necessary, fake mode 2 header
    memset(s_cdbuffer, 0, 12 * 2);
    sec2msf(sector + 2 * 75, (uint8_t *)&s_cdbuffer[12]);
    s_cdbuffer[12 + 3] = 1;

    return ret;
}

/* Adapted from ecm.c:unecmify() (C) Neill Corlett */
// TODO: move this func to ecm.h
static int cdread_ecm_decode(File *f, unsigned int base, void *dest, int sector) {
    uint32_t output_edc = 0, b = 0, writebytecount = 0, num;
    int32_t sectorcount = 0;
    int8_t type = 0;  // mode type 0 (META) or 1, 2 or 3 for CDROM type
    uint8_t sector_buffer[PCSX::CDRom::CD_FRAMESIZE_RAW];
    bool processsectors =
        (bool)decoded_ecm_sectors;          // this flag tells if to decode all sectors or just skip to wanted sector
    ECMFILELUT *pos = &(ecm_savetable[0]);  // points always to beginning of ECM DATA

    // If not pointing to ECM file but CDDA file or some other track
    if (f != s_cdHandle) {
        // printf("BASETR %i %i\n", base, sector);
        return cdimg_read_func_o(f, base, dest, sector);
    }
    // When sector exists in decoded ECM file buffer
    else if (decoded_ecm_sectors && sector < decoded_ecm_sectors) {
        // printf("ReadSector %i %i\n", sector, savedsectors);
        return cdimg_read_func_o(decoded_ecm, base, dest, sector);
    }
    // To prevent invalid seek
    /* else if (sector > len_ecm_savetable) {
            PCSX::g_system->SysPrintf("ECM: invalid sector requested\n");
            return -1;
    }*/
    // printf("SeekSector %i %i %i %i\n", sector, pos->sector, prevsector, base);

    if (sector <= len_ecm_savetable) {
        // get sector from LUT which points to wanted sector or close to
        // TODO: What would be optimal maximum to search near sector?
        //       Might cause slowdown if too small but too big also..
        for (sectorcount = sector; ((sectorcount > 0) && ((sector - sectorcount) <= 50000)); sectorcount--) {
            if (ecm_savetable[sectorcount].filepos >= ECM_HEADER_SIZE) {
                pos = &(ecm_savetable[sectorcount]);
                // printf("LUTSector %i %i %i %i\n", sector, pos->sector, prevsector, base);
                break;
            }
        }
        // if suitable sector was not found from LUT use last sector if less than wanted sector
        if (pos->filepos <= ECM_HEADER_SIZE && sector > prevsector) pos = &(ecm_savetable[prevsector]);
    }

    writebytecount = pos->sector * PCSX::CDRom::CD_FRAMESIZE_RAW;
    sectorcount = pos->sector;
    if (decoded_ecm_sectors) decoded_ecm->seek(writebytecount, SEEK_SET);  // rewind to last pos
    f->seek(/*base+*/ pos->filepos, SEEK_SET);
    while (sector >= sectorcount) {  // decode ecm file until we are past wanted sector
        int c = f->getc();
        int bits = 5;
        if (c == EOF) {
            goto error_in;
        }
        type = c & 3;
        num = (c >> 2) & 0x1F;
        // printf("ECM1 file; count %x\n", c);
        while (c & 0x80) {
            c = f->getc();
            // printf("ECM2 file; count %x\n", c);
            if (c == EOF) {
                goto error_in;
            }
            if ((bits > 31) || ((uint32_t)(c & 0x7F)) >= (((uint32_t)0x80000000LU) >> (bits - 1))) {
                // PCSX::g_system->SysMessage(_("Corrupt ECM file; invalid sector count\n"));
                goto error;
            }
            num |= ((uint32_t)(c & 0x7F)) << bits;
            bits += 7;
        }
        if (num == 0xFFFFFFFF) {
            // End indicator
            len_decoded_ecm_buffer = writebytecount;
            len_ecm_savetable = len_decoded_ecm_buffer / PCSX::CDRom::CD_FRAMESIZE_RAW;
            break;
        }
        num++;
        while (num) {
            if (!processsectors && sectorcount >= (sector - 1)) {  // ensure that we read the sector we are supposed to
                processsectors = true;
                // printf("Saving at %i\n", sectorcount);
            } else if (processsectors && sectorcount > sector) {
                // printf("Terminating at %i\n", sectorcount);
                break;
            }
            /*printf("Type %i Num %i SeekSector %i ProcessedSectors %i(%i) Bytecount %i Pos %li Write %u\n",
                            type, num, sector, sectorcount, pos->sector, writebytecount, ftell(f),
               processsectors);*/
            switch (type) {
                case 0:  // META
                    b = num;
                    if (b > sizeof(sector_buffer)) {
                        b = sizeof(sector_buffer);
                    }
                    writebytecount += b;
                    if (!processsectors) {
                        f->seek(b, SEEK_CUR);
                        break;
                    }  // seek only
                    if (f->read(sector_buffer, b) != b) {
                        goto error_in;
                    }
                    // output_edc = edc_compute(output_edc, sector_buffer, b);
                    if (decoded_ecm_sectors && decoded_ecm->write(sector_buffer, b) != b) {  // just seek or write also
                        goto error_out;
                    }
                    break;
                case 1:  // Mode 1
                    b = 1;
                    writebytecount += ECM_SECTOR_SIZE[type];
                    if (f->read(sector_buffer + 0x00C, 0x003) != 0x003) {
                        goto error_in;
                    }
                    if (f->read(sector_buffer + 0x010, 0x800) != 0x800) {
                        goto error_in;
                    }
                    if (!processsectors) break;  // seek only
                    reconstruct_sector(sector_buffer, type);
                    // output_edc = edc_compute(output_edc, sector_buffer, ECM_SECTOR_SIZE[type]);
                    if (decoded_ecm_sectors &&
                        decoded_ecm->write(sector_buffer, ECM_SECTOR_SIZE[type]) != ECM_SECTOR_SIZE[type]) {
                        goto error_out;
                    }
                    break;
                case 2:  // Mode 2 (XA), form 1
                    b = 1;
                    writebytecount += ECM_SECTOR_SIZE[type];
                    if (!processsectors) {
                        f->seek(0x804, SEEK_CUR);
                        break;
                    }  // seek only
                    if (f->read(sector_buffer + 0x014, 0x804) != 0x804) {
                        goto error_in;
                    }
                    reconstruct_sector(sector_buffer, type);
                    // output_edc = edc_compute(output_edc, sector_buffer + 0x10, ECM_SECTOR_SIZE[type]);
                    if (decoded_ecm_sectors &&
                        decoded_ecm->write(sector_buffer + 0x10, ECM_SECTOR_SIZE[type]) != ECM_SECTOR_SIZE[type]) {
                        goto error_out;
                    }
                    break;
                case 3:  // Mode 2 (XA), form 2
                    b = 1;
                    writebytecount += ECM_SECTOR_SIZE[type];
                    if (!processsectors) {
                        f->seek(0x918, SEEK_CUR);
                        break;
                    }  // seek only
                    if (f->read(sector_buffer + 0x014, 0x918) != 0x918) {
                        goto error_in;
                    }
                    reconstruct_sector(sector_buffer, type);
                    // output_edc = edc_compute(output_edc, sector_buffer + 0x10, ECM_SECTOR_SIZE[type]);
                    if (decoded_ecm_sectors &&
                        decoded_ecm->write(sector_buffer + 0x10, ECM_SECTOR_SIZE[type]) != ECM_SECTOR_SIZE[type]) {
                        goto error_out;
                    }
                    break;
            }
            sectorcount = ((writebytecount / PCSX::CDRom::CD_FRAMESIZE_RAW) - 0);
            num -= b;
        }
        if (type && sectorcount > 0 && ecm_savetable[sectorcount].filepos <= ECM_HEADER_SIZE) {
            ecm_savetable[sectorcount].filepos = f->tell() /*-base*/;
            ecm_savetable[sectorcount].sector = sectorcount;
            // printf("Marked %i at pos %i\n", ecm_savetable[sectorcount].sector,
            // ecm_savetable[sectorcount].filepos);
        }
    }

    if (decoded_ecm_sectors) {
        decoded_ecm->flush();
        decoded_ecm->seek(-1 * PCSX::CDRom::CD_FRAMESIZE_RAW, SEEK_CUR);
        num = decoded_ecm->read(sector_buffer, PCSX::CDRom::CD_FRAMESIZE_RAW);
        decoded_ecm_sectors = MAX(decoded_ecm_sectors, sectorcount);
    } else {
        num = PCSX::CDRom::CD_FRAMESIZE_RAW;
    }

    memcpy(dest, sector_buffer, PCSX::CDRom::CD_FRAMESIZE_RAW);
    prevsector = sectorcount;
    // printf("OK: Frame decoded %i %i\n", sectorcount-1, writebytecount);
    return num;

error_in:
error:
error_out:
    // memset(dest, 0x0, PCSX::CDRomCD_FRAMESIZE_RAW);
    PCSX::g_system->SysPrintf("Error decoding ECM image: WantedSector %i Type %i Base %i Sectors %i(%i) Pos %i(%li)\n",
                              sector, type, base, sectorcount, pos->sector, writebytecount, f->tell());
    return -1;
}

int handleecm(const char *isoname, File *cdh, int32_t *accurate_length) {
    // Rewind to start and check ECM header and filename suffix validity
    cdh->seek(0, SEEK_SET);
    if ((cdh->getc() == 'E') && (cdh->getc() == 'C') && (cdh->getc() == 'M') && (cdh->getc() == 0x00) &&
        (strncmp((isoname + strlen(isoname) - 5), ".ecm", 4))) {
        // Function used to read CD normally
        // TODO: detect if 2048 and use it
        cdimg_read_func_o = cdread_normal;

        // Function used to decode ECM data
        s_cdimg_read_func = cdread_ecm_decode;

        // Last accessed sector
        prevsector = 0;

        // Already analyzed during this session, use cached results
        if (ecm_file_detected) {
            if (accurate_length) *accurate_length = len_ecm_savetable;
            return 0;
        }

        PCSX::g_system->SysPrintf(_("\nDetected ECM file with proper header and filename suffix.\n"));

        // Init ECC/EDC tables
        eccedc_init();

        // Reserve maximum known sector ammount for LUT (80MIN CD)
        len_ecm_savetable = 75 * 80 * 60;  // 2*(accurate_length/PCSX::CDRomCD_FRAMESIZE_RAW);

        // Index 0 always points to beginning of ECM data
        ecm_savetable = (ECMFILELUT *)calloc(len_ecm_savetable, sizeof(ECMFILELUT));  // calloc returns nulled data
        ecm_savetable[0].filepos = ECM_HEADER_SIZE;

        if (accurate_length || decoded_ecm_sectors) {
            uint8_t tbuf1[PCSX::CDRom::CD_FRAMESIZE_RAW];
            len_ecm_savetable = 0;  // indicates to cdread_ecm_decode that no lut has been built yet
            cdread_ecm_decode(cdh, 0U, tbuf1, INT_MAX);  // builds LUT completely
            if (accurate_length) *accurate_length = len_ecm_savetable;
        }

        // Full image decoded? Needs fmemopen()
#ifdef ENABLE_ECM_FULL
        if (decoded_ecm_sectors) {
            len_decoded_ecm_buffer = len_ecm_savetable * PCSX::CDRomCD_FRAMESIZE_RAW;
            decoded_ecm_buffer = malloc(len_decoded_ecm_buffer);
            if (decoded_ecm_buffer) {
                // printf("Memory ok1 %u %p\n", len_decoded_ecm_buffer, decoded_ecm_buffer);
                decoded_ecm = fmemopen(decoded_ecm_buffer, len_decoded_ecm_buffer, "w+b");
                decoded_ecm_sectors = 1;
            } else {
                PCSX::g_system->SysMessage("Could not reserve memory for full ECM buffer. Only LUT will be used.");
                decoded_ecm_sectors = 0;
            }
        }
#endif

        ecm_file_detected = true;

        return 0;
    }
    return -1;
}

ssize_t (*cdimg_read_func_archive)(File *f, unsigned int base, void *dest, int sector) = NULL;
#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>

struct archive *a = NULL;
uint32_t len_uncompressed_buffer = 0;
void *cdimage_buffer_mem = NULL;
FILE *cdimage_buffer = NULL;  // s_cdHandle to store file

int aropen(FILE *fparchive, const char *_fn) {
    int32_t r;
    uint64_t length = 0, length_peek;
    bool use_temp_file = false;  // TODO make a config param
    static struct archive_entry *ae = NULL;
    struct archive_entry *ae_peek;

    if (a == NULL && cdimage_buffer == NULL) {
        // We open file twice. First to peek sizes. This nastyness due used interface.
        a = archive_read_new();
        //      r = archive_read_support_filter_all(a);
        r = archive_read_support_format_all(a);
        // r = archive_read_support_filter_all(a);
        // r = archive_read_support_format_raw(a);
        // r = archive_read_open_FILE(a, archive);
        archive_read_open_filename(a, _fn, 75 * PCSX::CDRomCD_FRAMESIZE_RAW);
        if (r != ARCHIVE_OK) {
            PCSX::g_system->SysPrintf("Archive open failed (%i).\n", r);
            archive_read_free(a);
            a = NULL;
            return -1;
        }
        // Get the biggest file in archive
        while ((r = archive_read_next_header(a, &ae_peek)) == ARCHIVE_OK) {
            length_peek = archive_entry_size(ae_peek);
            // printf("Entry canditate %s %i\n", archive_entry_pathname(ae_peek), length_peek);
            length = MAX(length_peek, length);
            ae = (ae == NULL ? ae_peek : ae);
        }
        archive_read_free(a);
        if (ae == NULL) {
            PCSX::g_system->SysPrintf("Archive entry read failed (%i).\n", r);
            a = NULL;
            return -1;
        }
        // Now really open the file
        a = archive_read_new();
        //      r = archive_read_support_compression_all(a);
        r = archive_read_support_format_all(a);
        archive_read_open_filename(a, _fn, 75 * PCSX::CDRomCD_FRAMESIZE_RAW);
        while ((r = archive_read_next_header(a, &ae)) == ARCHIVE_OK) {
            length_peek = archive_entry_size(ae);
            if (length_peek == length) {
                // ae = ae_peek;
                PCSX::g_system->SysPrintf(" -- Selected entry %s %i", archive_entry_pathname(ae), length);
                break;
            }
        }

        len_uncompressed_buffer = length ? length : 700 * 1024 * 1024;
    }

    if (use_temp_file && (cdimage_buffer == NULL || s_cdHandle != cdimage_buffer)) {
        cdimage_buffer = fopen("/tmp/pcsxr.tmp.bin", "w+b");
    } else if (!use_temp_file && (cdimage_buffer == NULL || s_cdHandle != cdimage_buffer)) {
        if (cdimage_buffer_mem == NULL && ((cdimage_buffer_mem = malloc(len_uncompressed_buffer)) == NULL)) {
            PCSX::g_system->SysMessage("Could not reserve enough memory for full image buffer.\n");
            exit(3);
        }
        // printf("Memory ok2 %u %p\n", len_uncompressed_buffer, cdimage_buffer_mem);
        cdimage_buffer = fmemopen(cdimage_buffer_mem, len_uncompressed_buffer, "w+b");
    } else {
    }

    if (s_cdHandle != cdimage_buffer) {
        fclose(s_cdHandle);  // opened thru archive so this not needed anymore
        s_cdHandle = cdimage_buffer;
    }

    return 0;
}

static int cdread_archive(FILE *f, unsigned int base, void *dest, int sector) {
    int32_t r;
    size_t size;
    size_t readsize;
    static off_t offset = 0;  // w/o read always or static/ftell
    const void *buff;

    // If not pointing to archive file but CDDA file or some other track
    if (f != s_cdHandle) {
        return cdimg_read_func_archive(f, base, dest, sector);
    }

    // Jump if already completely read
    if (a != NULL /*&& (ecm_file_detected || sector*PCSX::CDRomCD_FRAMESIZE_RAW <= len_uncompressed_buffer)*/) {
        readsize = (sector + 1) * PCSX::CDRomCD_FRAMESIZE_RAW;
        for (fseek(cdimage_buffer, offset, SEEK_SET); offset < readsize;) {
            r = archive_read_data_block(a, &buff, &size, &offset);
            offset += size;
            PCSX::g_system->SysPrintf("ReadArchive seek:%u(%u) cur:%u(%u)\r", sector, readsize / 1024,
                                      offset / PCSX::CDRomCD_FRAMESIZE_RAW, offset / 1024);
            fwrite(buff, size, 1, cdimage_buffer);
            if (r != ARCHIVE_OK) {
                // PCSX::g_system->SysPrintf("End of archive.\n");
                archive_read_free(a);
                a = NULL;
                readsize = offset;
                fflush(cdimage_buffer);
                fseek(cdimage_buffer, 0, SEEK_SET);
            }
        }
    } else {
        // PCSX::g_system->SysPrintf("ReadSectorArchSector: %u(%u)\n", sector,
        // sector*PCSX::CDRomCD_FRAMESIZE_RAW);
    }

    // TODO what causes req sector to be greater than CD size?
    r = cdimg_read_func_archive(cdimage_buffer, base, dest, sector);
    return r;
}
int handlearchive(const char *isoname, int32_t *accurate_length) {
    uint32_t read_size = accurate_length ? MSF2SECT(70, 70, 16) : MSF2SECT(0, 0, 16);
    int ret = -1;
    if ((ret = aropen(s_cdHandle, isoname)) == 0) {
        s_cdimg_read_func = cdread_archive;
        PCSX::g_system->SysPrintf("[+archive]");
        if (!ecm_file_detected) {
#ifndef ENABLE_ECM_FULL
            // Detect ECM inside archive
            cdimg_read_func_archive = cdread_normal;
            cdread_archive(s_cdHandle, 0, s_cdbuffer, read_size);
            if (handleecm("test.ecm", cdimage_buffer, accurate_length) != -1) {
                cdimg_read_func_archive = cdread_ecm_decode;
                s_cdimg_read_func = cdread_archive;
                PCSX::g_system->SysPrintf("[+ecm]");
            }
#endif
        } else {
            PCSX::g_system->SysPrintf("[+ecm]");
        }
    }
    return ret;
}
#else
int aropen(FILE *fparchive, const char *_fn) { return -1; }
static int cdread_archive(FILE *f, unsigned int base, void *dest, int sector) { return -1; }
int handlearchive(const char *isoname, int32_t *accurate_length) { return -1; }
#endif

static unsigned char *CALLBACK ISOgetBuffer_compr(void) { return compr_img->buff_raw[compr_img->sector_in_blk] + 12; }

static unsigned char *CALLBACK ISOgetBuffer(void) { return s_cdbuffer + 12; }

static void PrintTracks(void) {
    int i;

    for (i = 1; i <= numtracks; i++) {
        PCSX::g_system->SysPrintf(
            _("Track %.2d (%s) - Start %.2d:%.2d:%.2d, Length %.2d:%.2d:%.2d\n"), i,
            (ti[i].type == trackinfo::DATA ? "DATA" : ti[i].cddatype == trackinfo::CCDDA ? "CZDA" : "CDDA"),
            ti[i].start[0], ti[i].start[1], ti[i].start[2], ti[i].length[0], ti[i].length[1], ti[i].length[2]);
    }
}

// This function is invoked by the front-end when opening an ISO
// file for playback
static long CALLBACK ISOopen(void) {
    if (s_cdHandle != NULL) {
        return 0;  // it's already open
    }

    s_cdHandle = new File(GetIsoFile());
    if (s_cdHandle->failed()) {
        delete s_cdHandle;
        s_cdHandle = NULL;
        return -1;
    }

    PCSX::g_system->SysPrintf(_("Loaded CD Image: %s"), GetIsoFile());

    s_cddaBigEndian = false;
    s_subChanMixed = false;
    s_subChanRaw = false;
    s_pregapOffset = 0;
    g_cdrIsoMultidiskCount = 1;
    s_multifile = false;

    CDR_getBuffer = ISOgetBuffer;
    s_cdimg_read_func = cdread_normal;

    if (parsecue(GetIsoFile()) == 0) {
        PCSX::g_system->SysPrintf("[+cue]");
    } else if (parsetoc(GetIsoFile()) == 0) {
        PCSX::g_system->SysPrintf("[+toc]");
    } else if (parseccd(GetIsoFile()) == 0) {
        PCSX::g_system->SysPrintf("[+ccd]");
    } else if (parsemds(GetIsoFile()) == 0) {
        PCSX::g_system->SysPrintf("[+mds]");
    }
    // TODO Is it possible that cue/ccd+ecm? otherwise use else if below to supressn extra checks
    if (handlepbp(GetIsoFile()) == 0) {
        PCSX::g_system->SysPrintf("[pbp]");
        CDR_getBuffer = ISOgetBuffer_compr;
        s_cdimg_read_func = cdread_compressed;
    } else if (handlecbin(GetIsoFile()) == 0) {
        PCSX::g_system->SysPrintf("[cbin]");
        CDR_getBuffer = ISOgetBuffer_compr;
        s_cdimg_read_func = cdread_compressed;
    } else if ((handleecm(GetIsoFile(), s_cdHandle, NULL) == 0)) {
        PCSX::g_system->SysPrintf("[+ecm]");
    } else if (handlearchive(GetIsoFile(), NULL) == 0) {
    }

    if (!s_subChanMixed && opensubfile(GetIsoFile()) == 0) {
        PCSX::g_system->SysPrintf("[+sub]");
    }
    if (opensbifile(GetIsoFile()) == 0) {
        PCSX::g_system->SysPrintf("[+sbi]");
    }

    if (!ecm_file_detected) {
        // guess whether it is mode1/2048
        s_cdHandle->seek(0, SEEK_END);
        if (s_cdHandle->tell() % 2048 == 0) {
            unsigned int modeTest = 0;
            s_cdHandle->seek(0, SEEK_SET);
            s_cdHandle->read(&modeTest, 4);
            if (SWAP_LE32(modeTest) != 0xffffff00) {
                PCSX::g_system->SysPrintf("[2048]");
                s_isMode1ISO = true;
            }
        }
        s_cdHandle->seek(0, SEEK_SET);
    }

    PCSX::g_system->SysPrintf(".\n");

    PrintTracks();

    if (s_subChanMixed && (s_cdimg_read_func == cdread_normal))
        s_cdimg_read_func = cdread_sub_mixed;
    else if (s_isMode1ISO && (s_cdimg_read_func == cdread_normal))
        s_cdimg_read_func = cdread_2048;
    else if (s_isMode1ISO && (cdimg_read_func_archive == cdread_normal))
        cdimg_read_func_archive = cdread_2048;

    // make sure we have another handle open for cdda
    if (numtracks > 1 && ti[1].handle == NULL) {
        ti[1].handle = new File(GetIsoFile());
    }

    return 0;
}

static long CALLBACK ISOclose(void) {
    int i;

    if (s_cdHandle != NULL) {
        s_cdHandle->close();
        delete s_cdHandle;
        s_cdHandle = NULL;
        // cdimage_buffer = NULL;
    }
    if (s_subHandle != NULL) {
        s_subHandle->close();
        delete s_subHandle;
        s_subHandle = NULL;
    }

    if (compr_img != NULL) {
        free(compr_img->index_table);
        free(compr_img);
        compr_img = NULL;
    }

    for (i = 1; i <= numtracks; i++) {
        if (ti[i].handle != NULL) {
            ti[i].handle->close();
            delete ti[i].handle;
            ti[i].handle = NULL;
            if (ti[i].decoded_buffer != NULL) {
                free(ti[i].decoded_buffer);
            }
            ti[i].cddatype = trackinfo::NONE;
        }
    }
    numtracks = 0;
    ti[1].type = trackinfo::CLOSED;

    memset(s_cdbuffer, 0, sizeof(s_cdbuffer));
    CDR_getBuffer = ISOgetBuffer;

    return 0;
}

long CALLBACK ISOinit(void) {
    assert(s_cdHandle == NULL);
    assert(s_subHandle == NULL);
    assert(ecm_file_detected == false);
    assert(decoded_ecm_buffer == NULL);
    assert(decoded_ecm == NULL);

    return 0;  // do nothing
}

static long CALLBACK ISOshutdown(void) {
    ISOclose();

    // ECM LUT
    free(ecm_savetable);
    ecm_savetable = NULL;

    if (decoded_ecm != NULL) {
        decoded_ecm->close();
        delete decoded_ecm;
        free(decoded_ecm_buffer);
        decoded_ecm_buffer = NULL;
        decoded_ecm = NULL;
    }
    ecm_file_detected = false;

#ifdef HAVE_LIBARCHIVE
    if (cdimage_buffer != NULL) {
        // fclose(cdimage_buffer);
        free(cdimage_buffer_mem);
        cdimage_buffer_mem = NULL;
        cdimage_buffer = NULL;
        if (a) {
            archive_read_free(a);
            a = NULL;
        }
    }
#endif

    return 0;
}

// return Starting and Ending Track
// buffer:
//  byte 0 - start track
//  byte 1 - end track
static long CALLBACK ISOgetTN(unsigned char *buffer) {
    buffer[0] = 1;

    if (numtracks > 0) {
        buffer[1] = numtracks;
    } else {
        buffer[1] = 1;
    }

    return 0;
}

// return Track Time
// buffer:
//  byte 0 - frame
//  byte 1 - second
//  byte 2 - minute
static long CALLBACK ISOgetTD(unsigned char track, unsigned char *buffer) {
    if (track == 0) {
        unsigned int sect;
        unsigned char time[3];
        sect = msf2sec(ti[numtracks].start) + msf2sec(ti[numtracks].length);
        sec2msf(sect, (uint8_t *)time);
        buffer[2] = time[0];
        buffer[1] = time[1];
        buffer[0] = time[2];
    } else if (numtracks > 0 && track <= numtracks) {
        buffer[2] = ti[track].start[0];
        buffer[1] = ti[track].start[1];
        buffer[0] = ti[track].start[2];
    } else {
        buffer[2] = 0;
        buffer[1] = 2;
        buffer[0] = 0;
    }

    return 0;
}

// decode 'raw' subchannel data ripped by cdrdao
static void DecodeRawSubData(void) {
    unsigned char subQData[12];
    int i;

    memset(subQData, 0, sizeof(subQData));

    for (i = 0; i < 8 * 12; i++) {
        if (s_subbuffer[i] & (1 << 6)) {  // only subchannel Q is needed
            subQData[i >> 3] |= (1 << (7 - (i & 7)));
        }
    }

    memcpy(&s_subbuffer[12], subQData, 12);
}

// read track
// time: byte 0 - minute; byte 1 - second; byte 2 - frame
// uses bcd format
static long CALLBACK ISOreadTrack(unsigned char *time) {
    int sector =
        PCSX::CDRom::MSF2SECT(PCSX::CDRom::btoi(time[0]), PCSX::CDRom::btoi(time[1]), PCSX::CDRom::btoi(time[2]));
    long ret;

    if (s_cdHandle == NULL) {
        return -1;
    }

    if (s_pregapOffset) {
        s_subChanMissing = false;
        if (sector >= s_pregapOffset) {
            sector -= 2 * 75;
            if (sector < s_pregapOffset) s_subChanMissing = true;
        }
    }

    ret = s_cdimg_read_func(s_cdHandle, 0, s_cdbuffer, sector);
    if (ret < 0) return -1;

    if (s_subHandle != NULL) {
        s_subHandle->seek(sector * PCSX::CDRom::SUB_FRAMESIZE, SEEK_SET);
        s_subHandle->read(s_subbuffer, PCSX::CDRom::SUB_FRAMESIZE);

        if (s_subChanRaw) DecodeRawSubData();
    }

    return 0;
}

// plays cdda audio
// sector: byte 0 - minute; byte 1 - second; byte 2 - frame
// does NOT uses bcd format
static long CALLBACK ISOplay(unsigned char *time) {
    s_playing = true;
    return 0;
}

// stops cdda audio
static long CALLBACK ISOstop(void) {
    s_playing = false;
    return 0;
}

// gets subchannel data
static unsigned char *CALLBACK ISOgetBufferSub(void) {
    if ((s_subHandle != NULL || s_subChanMixed) && !s_subChanMissing) {
        return s_subbuffer;
    }

    return NULL;
}

long CALLBACK ISOgetStatus(struct CdrStat *stat) {
    uint32_t sect;

    CDR__getStatus(stat);

    if (s_playing) {
        stat->Type = 0x02;
        stat->Status |= 0x80;
    } else {
        // BIOS - boot ID (CD type)
        stat->Type = ti[1].type;
    }

    // relative -> absolute time
    sect = s_cddaCurPos;
    sec2msf(sect, (uint8_t *)stat->Time);

    return 0;
}

// read CDDA sector into buffer
long CALLBACK ISOreadCDDA(unsigned char m, unsigned char s, unsigned char f, unsigned char *buffer) {
    unsigned char msf[3] = {m, s, f};
    unsigned int file, track, track_start = 0;
    int ret;

    s_cddaCurPos = msf2sec(msf);

    // find current track index
    for (track = numtracks;; track--) {
        track_start = msf2sec(ti[track].start);
        if (track_start <= s_cddaCurPos) break;
        if (track == 1) break;
    }

    // data tracks play silent (or CDDA set to silent)
    if (ti[track].type != trackinfo::CDDA || PCSX::g_emulator.config().Cdda == PCSX::Emulator::CDDA_DISABLED) {
        memset(buffer, 0, PCSX::CDRom::CD_FRAMESIZE_RAW);
        return 0;
    }

    file = 1;
    if (s_multifile) {
        // find the file that contains this track
        for (file = track; file > 1; file--)
            if (ti[file].handle != NULL) break;
    }

    /* Need to decode audio track first if compressed still (lazy) */
    if (ti[file].cddatype > trackinfo::BIN) {
        do_decode_cdda(&(ti[file]), file);
    }

    ret = s_cdimg_read_func(ti[file].handle, ti[track].start_offset, buffer, s_cddaCurPos - track_start);
    if (ret != PCSX::CDRom::CD_FRAMESIZE_RAW) {
        memset(buffer, 0, PCSX::CDRom::CD_FRAMESIZE_RAW);
        return -1;
    }

    if (PCSX::g_emulator.config().Cdda == PCSX::Emulator::CDDA_ENABLED_BE || s_cddaBigEndian) {
        int i;
        unsigned char tmp;

        for (i = 0; i < PCSX::CDRom::CD_FRAMESIZE_RAW / 2; i++) {
            tmp = buffer[i * 2];
            buffer[i * 2] = buffer[i * 2 + 1];
            buffer[i * 2 + 1] = tmp;
        }
    }

    return 0;
}

void cdrIsoInit(void) {
    CDR_init = ISOinit;
    CDR_shutdown = ISOshutdown;
    CDR_open = ISOopen;
    CDR_close = ISOclose;
    CDR_getTN = ISOgetTN;
    CDR_getTD = ISOgetTD;
    CDR_readTrack = ISOreadTrack;
    CDR_getBuffer = ISOgetBuffer;
    CDR_play = ISOplay;
    CDR_stop = ISOstop;
    CDR_getBufferSub = ISOgetBufferSub;
    CDR_getStatus = ISOgetStatus;
    CDR_readCDDA = ISOreadCDDA;

    CDR_getDriveLetter = CDR__getDriveLetter;
    CDR_configure = CDR__configure;
    CDR_test = CDR__test;
    CDR_about = CDR__about;
    CDR_setfilename = CDR__setfilename;

    numtracks = 0;
}

int cdrIsoActive(void) { return (s_cdHandle != NULL || ecm_savetable != NULL || decoded_ecm != NULL); }
