/*
 * QEMU PowerMac Awacs Screamer device support
 *
 * Copyright (c) 2016 Mark Cave-Ayland
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "audio/audio.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "audio/audio.h"
#include "hw/audio/screamer.h"
#include "hw/qdev-properties.h"
#include "qemu/timer.h"
#include "hw/ppc/mac_dbdma.h"
#include "sysemu/sysemu.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/typedefs.h"

/* debug screamer */
//#define DEBUG_SCREAMER

#ifdef DEBUG_SCREAMER
#define SCREAMER_DPRINTF(fmt, ...)                                  \
    do { printf("SCREAMER: " fmt , ## __VA_ARGS__); } while (0)
#else
#define SCREAMER_DPRINTF(fmt, ...)
#endif

/* chip registers */
#define SND_CTRL_REG   0x0
#define CODEC_CTRL_REG 0x1
#define CODEC_STAT_REG 0x2
#define CLIP_CNT_REG   0x3
#define BYTE_SWAP_REG  0x4
#define FRAME_CNT_REG  0x5

#define CODEC_CTRL_MASKECMD        (0x1 << 24)
#define CODEC_CTRL1_RECALIBRATE    0x4

#define CODEC_STAT_MANUFACTURER_CRYSTAL    0x100
#define CODEC_STAT_AWACS_REVISION          0x3000
#define CODEC_STAT_MASK_VALID              (0x1 << 22) 

/* Audio */
static const char *s_spk = "screamer";

static void pmac_screamer_tx_transfer(ScreamerState *s)
{
    DBDMA_io *io = &s->io;
    int samples;

    samples = MIN(io->len >> s->shift, s->samples - s->wpos);
    dma_memory_read(&address_space_memory, io->addr,
                    &s->mixbuf[s->wpos << s->shift], samples << s->shift,
                    MEMTXATTRS_UNSPECIFIED);

    SCREAMER_DPRINTF("DMA actually transferred 0x%x, wpos is %d\n", samples << s->shift, s->wpos);

    io->addr += (samples << s->shift);
    io->len -= (samples << s->shift);
    s->wpos += samples;

    /* Continue DBDMA if we have completed the transfer, otherwise defer */
    if (io->len == 0) {
        SCREAMER_DPRINTF("-> End of transfer\n");
        io->dma_end(io);
    }
}

static void pmac_screamer_tx(DBDMA_io *io)
{
    ScreamerState *s = io->opaque;

    SCREAMER_DPRINTF("DMA TX transfer: addr %" HWADDR_PRIx
                     " len: %x\n", io->addr, io->len);

    memcpy(&s->io, io, sizeof(DBDMA_io));
    //if (s->wpos + (s->io.len >> s->shift) > s->samples) {
    //    return;
    //}

    pmac_screamer_tx_transfer(s);
}

static void pmac_screamer_tx_flush(DBDMA_io *io)
{
    DBDMA_channel *ch = io->channel;
    dbdma_cmd *current = &ch->current;
    uint16_t cmd;

    SCREAMER_DPRINTF("DMA TX flush!\n");
#if 0
    cmd = le16_to_cpu(current->command) & COMMAND_MASK;
    if (cmd == OUTPUT_MORE || cmd == OUTPUT_LAST ||
        cmd == INPUT_MORE || cmd == INPUT_LAST) {
        current->xfer_status = cpu_to_le16(ch->regs[DBDMA_STATUS]);
        current->res_count = cpu_to_le16(io->len);

        dma_memory_write(&address_space_memory, ch->regs[DBDMA_CMDPTR_LO],
                         &ch->current, sizeof(dbdma_cmd),
                         MEMTXATTRS_UNSPECIFIED);
    }
#endif

    ch->io.processing = false;

    cmd = le16_to_cpu(current->command) & COMMAND_MASK;
    if (cmd == INPUT_MORE || cmd == INPUT_LAST) {
        current->xfer_status = cpu_to_le16(ch->regs[DBDMA_STATUS]);
        current->res_count = cpu_to_le16(io->len);

            dma_memory_write(&address_space_memory, ch->regs[DBDMA_CMDPTR_LO],
                         &ch->current, sizeof(dbdma_cmd),
                         MEMTXATTRS_UNSPECIFIED);
    }

}

static void pmac_screamer_rx(DBDMA_io *io)
{
    SCREAMER_DPRINTF("DMA RX transfer: addr %" HWADDR_PRIx
                     " len: %x\n", io->addr, io->len);

    //ScreamerState *s = io->opaque;
    DBDMA_channel *ch = io->channel;
        
    /* FIXME: stop channel after updating with status to stop MacOS 9 freezing */
    ch->regs[DBDMA_STATUS] &= ~RUN;

    io->dma_end(io);
}

static void pmac_screamer_rx_flush(DBDMA_io *io)
{
    DBDMA_channel *ch = io->channel;
    dbdma_cmd *current = &ch->current;
    uint16_t cmd;

    SCREAMER_DPRINTF("DMA RX flush!\n");
#if 1
    cmd = le16_to_cpu(current->command) & COMMAND_MASK;
    if (cmd == INPUT_MORE || cmd == INPUT_LAST) {
        current->xfer_status = cpu_to_le16(ch->regs[DBDMA_STATUS]);
        current->res_count = cpu_to_le16(io->len);

        dma_memory_write(&address_space_memory, ch->regs[DBDMA_CMDPTR_LO],
                         &ch->current, sizeof(dbdma_cmd),
                         MEMTXATTRS_UNSPECIFIED);
    }
#endif
}

void macio_screamer_register_dma(ScreamerState *s, void *dbdma, int txchannel, int rxchannel)
{
    s->dbdma = dbdma;
    DBDMA_register_channel(dbdma, txchannel, s->dma_tx_irq,
                           pmac_screamer_tx, pmac_screamer_tx_flush, s);
    DBDMA_register_channel(dbdma, rxchannel, s->dma_rx_irq,
                           pmac_screamer_rx, pmac_screamer_rx_flush, s);
}

static void screamerspk_callback(void *opaque, int free_b)
{
    ScreamerState *s = opaque;
    DBDMA_io *io = &s->io;
    int samples, generated;

    if (free_b == 0) {
        return;
    }

    if (s->wpos - s->rpos == 0) {
        return;
    }

    samples = MIN(s->samples, free_b >> s->shift);
    generated = MIN(samples, s->wpos - s->rpos);

    AUD_write(s->voice, s->mixbuf + (uintptr_t)(s->rpos << s->shift),
              generated << s->shift);

    SCREAMER_DPRINTF("  - generated %d, wpos %d, rpos %d\n", generated, s->wpos, s->rpos);
    
    s->regs[FRAME_CNT_REG] += generated;
    s->rpos += generated;
    if (s->rpos < s->wpos) {
        return;
    }

    s->wpos = 0;
    s->rpos = 0;

    if (io->len) {
        DBDMA_channel *ch = io->channel;
        uint32_t status = ch->regs[DBDMA_STATUS];

        SCREAMER_DPRINTF("Continue deferred transfer\n");

        /* Disable channel so we only complete the current transfer */
        ch->regs[DBDMA_STATUS] &= ~RUN;

        /* Perform deferred transfer */
        pmac_screamer_tx_transfer(s);

        /* Re-enable channel */
        ch->regs[DBDMA_STATUS] = status;

        /* Kick channel to continue */
        DBDMA_kick(container_of(ch, DBDMAState, channels[ch->channel]));
    }
}

static void screamer_update_settings(ScreamerState *s)
{
    struct audsettings as = { s->rate, 2, AUDIO_FORMAT_S16,
        1 };

    s->voice = AUD_open_out(&s->card, s->voice, s_spk, s, screamerspk_callback, &as);
    if (!s->voice) {
        AUD_log(s_spk, "Could not open voice\n");
        return;
    }

    s->shift = 2;
    s->samples = AUD_get_buffer_size_out(s->voice) >> s->shift;
    s->mixbuf = g_malloc0(s->samples << s->shift);

    AUD_set_active_out(s->voice, true);
}

static void screamer_update_volume(ScreamerState *s)
{
    uint8_t muted = s->codec_ctrl_regs[0x1] & 0x80 ? 1 : 0;
    uint8_t att_left = (s->codec_ctrl_regs[0x4] & 0xf);
    uint8_t att_right = (s->codec_ctrl_regs[0x4] & 0x3c0) >> 6;

    SCREAMER_DPRINTF("setting mute: %d, attenuation L: %d R: %d\n",
                     muted, att_left, att_right);

    AUD_set_volume_out(s->voice, muted, (0xf - att_left) << 4,
                       (0xf - att_right) << 4);
}

static void screamer_reset(DeviceState *dev)
{
    ScreamerState *s = SCREAMER(dev);
    
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->codec_ctrl_regs, 0, sizeof(s->codec_ctrl_regs));
    memset(&s->io, 0, sizeof(DBDMA_io));

    s->rate = 44100;
    screamer_update_settings(s);

    s->bpos = 0;
    s->ppos = 0;

    return;
}

static void screamer_realizefn(DeviceState *dev, Error **errp)
{
    ScreamerState *s = SCREAMER(dev);

    if (!AUD_register_card(s_spk, &s->card, errp)) {
        return;
    }

    s->rate = 44100;
    screamer_update_settings(s);
}

static void screamer_control_write(ScreamerState *s, uint32_t val)
{
    SCREAMER_DPRINTF("%s: val %" PRId32 "\n", __func__, val);
        
    /* Basic rate selection */
    switch ((val & 0x700) >> 8) {
    case 0x00:
        s->rate = 44100;
        break;
    case 0x1:
        s->rate = 29400;
        break;
    case 0x2:
        s->rate = 22050;
        break;
    case 0x3:
        s->rate = 17640;
        break;
    case 0x4:
        s->rate = 14700;
        break;
    case 0x5:
        s->rate = 11025;
        break;
    case 0x6:
        s->rate = 8820;
        break;
    case 0x7:
        s->rate = 7350;
        break;
    }

    SCREAMER_DPRINTF("basic rate: %d\n", s->rate);
    screamer_update_settings(s);
    
    s->regs[0] = val;
}

static void screamer_codec_write(ScreamerState *s, hwaddr addr, uint64_t val)
{
    //SCREAMER_DPRINTF("%s: addr " HWADDR_PRIx " val %" PRIx64 "\n", __func__, addr, val);

    switch (addr) {
    case 0x1:
        /* Clear recalibrate if set */
        val = val & ~CODEC_CTRL1_RECALIBRATE;    

        /* Update volume in case mute set */
        screamer_update_volume(s);
        break;

    case 0x4:
        /* Speaker attenuation */
        screamer_update_volume(s);
        break;
    }
    
    s->codec_ctrl_regs[addr] = val;
}

static uint64_t screamer_read(void *opaque, hwaddr addr, unsigned size)
{
    ScreamerState *s = opaque;
    uint32_t val;

    addr = addr >> 4;
    switch (addr) {
    case SND_CTRL_REG:
        val = s->regs[addr];
        break;
    case CODEC_CTRL_REG:
        val = s->regs[addr] & ~CODEC_CTRL_MASKECMD;
        break;
    case CODEC_STAT_REG:
        if (s->codec_ctrl_regs[7] & 1) {
            /* Read back mode */
            val = s->codec_ctrl_regs[(s->codec_ctrl_regs[7] >> 1) & 0xe];
        } else {
            /* Return status register */
            val = s->regs[addr] & ~0xff00;
            val |= CODEC_STAT_MANUFACTURER_CRYSTAL | CODEC_STAT_AWACS_REVISION |
                   CODEC_STAT_MASK_VALID;
        }
        break;
    case CLIP_CNT_REG:
    case BYTE_SWAP_REG:
    case FRAME_CNT_REG:
        val = s->regs[addr];
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                  "screamer: Unimplemented register read "
                  "reg 0x%" HWADDR_PRIx " size 0x%x\n",
                  addr, size);
        val = 0;
        break;
    }

    SCREAMER_DPRINTF("%s: addr " HWADDR_FMT_plx " -> %x\n", __func__, addr, val);

    return val;
}

static void screamer_write(void *opaque, hwaddr addr,
                           uint64_t val, unsigned size)
{
    ScreamerState *s = opaque;
    uint32_t codec_addr;

    addr = addr >> 4;

    SCREAMER_DPRINTF("%s: addr " HWADDR_FMT_plx " val %" PRIx64 "\n", __func__, addr, val);

    switch (addr) {
    case SND_CTRL_REG:
        screamer_control_write(s, val & 0xffffffff);
        break;
    case CODEC_CTRL_REG:
        s->regs[addr] = val & 0xffffffff;
        codec_addr = (val & 0x7fff) >> 12;
        screamer_codec_write(s, codec_addr, val & 0xfff);
        break;
    case CODEC_STAT_REG:
    case CLIP_CNT_REG:
    case BYTE_SWAP_REG:
        s->regs[addr] = val & 0xffffffff;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                  "screamer: Unimplemented register write "
                  "reg 0x%" HWADDR_PRIx " size 0x%x value 0x%" PRIx64 "\n",
                  addr, size, val);
        break;
    }

    return;
}

static const MemoryRegionOps screamer_ops = {
    .read = screamer_read,
    .write = screamer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    }
};

static void screamer_initfn(Object *obj)
{
    SysBusDevice *d = SYS_BUS_DEVICE(obj);
    ScreamerState *s = SCREAMER(obj);

    memory_region_init_io(&s->mem, obj, &screamer_ops, s, "screamer", 0x1000);
    sysbus_init_mmio(d, &s->mem);
    sysbus_init_irq(d, &s->irq);
    sysbus_init_irq(d, &s->dma_tx_irq);
    sysbus_init_irq(d, &s->dma_rx_irq);
}

static Property screamer_properties[] = {
    DEFINE_AUDIO_PROPERTIES(ScreamerState, card),
    DEFINE_PROP_END_OF_LIST()
};

static void screamer_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = screamer_realizefn;
    dc->reset = screamer_reset;
    device_class_set_props(dc, screamer_properties);
}

static const TypeInfo screamer_type_info = {
    .name = TYPE_SCREAMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ScreamerState),
    .instance_init = screamer_initfn,
    .class_init = screamer_class_init,
};

static void screamer_register_types(void)
{
    type_register_static(&screamer_type_info);
}

type_init(screamer_register_types)
