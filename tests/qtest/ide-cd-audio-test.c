/*
 * ATAPI CD-DA tests: cue/bin layout, READ CD, PLAY and audio output
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

/* mac99: macio on the main Uni-North PCI bus, CD on the second ATA bus */
#define UNIN_CFG_ADDR   0xf2800000
#define UNIN_CFG_DATA   0xf2c00000
#define MACIO_BAR       0x80000000
#define IDE_BASE        (MACIO_BAR + 0x21000)
#define DBDMA_CH        (MACIO_BAR + 0x8000 + 26 * 0x80)

#define REG(n)          (IDE_BASE + ((n) << 4))
#define REG_DATA        REG(0)
#define REG_FEATURE     REG(1)
#define REG_NSECTOR     REG(2)
#define REG_LCYL        REG(4)
#define REG_HCYL        REG(5)
#define REG_DEVICE      REG(6)
#define REG_STATUS      REG(7)

#define ST_BSY  0x80
#define ST_DRQ  0x08
#define ST_ERR  0x01

/* image layout */
#define T1_SECTORS      20          /* MODE1/2352 */
#define T2_PREGAP       75          /* INDEX 00 stored in the file */
#define T2_SECTORS      300         /* AUDIO */
#define T3_PREGAP       150         /* PREGAP, not in the file */
#define T3_SECTORS      150         /* AUDIO, second file */
#define T2_START        (T1_SECTORS + T2_PREGAP)
#define T3_START        (T2_START + T2_SECTORS + T3_PREGAP)
#define LEADOUT         (T3_START + T3_SECTORS)

#define RAW             2352

typedef struct Image {
    char *dir;
    char *cue;
    uint8_t *bin1;      /* track 1 + track 2 */
    size_t bin1_len;
    uint8_t *bin2;      /* track 3 */
    size_t bin2_len;
} Image;

static Image img;

static uint8_t to_bcd(int v)
{
    return ((v / 10) << 4) | (v % 10);
}

static uint32_t lcg(uint32_t *state)
{
    *state = *state * 1103515245 + 12345;
    return *state >> 8;
}

static void make_image(void)
{
    g_autoptr(GError) err = NULL;
    g_autofree char *bin1 = NULL, *bin2 = NULL;
    uint32_t seed = 1;
    uint8_t *p;
    int i, k;

    img.dir = g_dir_make_tmp("ide-cd-audio-XXXXXX", &err);
    g_assert_no_error(err);

    img.bin1_len = (T1_SECTORS + T2_PREGAP + T2_SECTORS) * RAW;
    img.bin1 = g_malloc0(img.bin1_len);
    p = img.bin1;
    for (i = 0; i < T1_SECTORS; i++, p += RAW) {
        uint32_t a = i + 150;

        memset(p + 1, 0xff, 10);
        p[12] = to_bcd(a / 75 / 60);
        p[13] = to_bcd((a / 75) % 60);
        p[14] = to_bcd(a % 75);
        p[15] = 1;
        for (k = 0; k < 2048; k++) {
            p[16 + k] = i * 7 + k;
        }
        memset(p + 2064, 0xec, 288);
    }
    /* track 2: tone on the left, noise on the right */
    for (i = 0; i < (T2_PREGAP + T2_SECTORS) * 588; i++, p += 4) {
        int16_t l = (int16_t)(((i * 37) % 200) * 150 - 15000);
        int16_t r = (int16_t)(lcg(&seed) | 1);

        stw_le_p(p, l);
        stw_le_p(p + 2, r);
    }

    img.bin2_len = T3_SECTORS * RAW;
    img.bin2 = g_malloc0(img.bin2_len);
    for (i = 0, p = img.bin2; i < T3_SECTORS * 588; i++, p += 4) {
        stw_le_p(p, (int16_t)(i * 3 + 1));
        stw_le_p(p + 2, (int16_t)(lcg(&seed) | 1));
    }

    bin1 = g_build_filename(img.dir, "disc.bin", NULL);
    bin2 = g_build_filename(img.dir, "track3.bin", NULL);
    img.cue = g_build_filename(img.dir, "disc.cue", NULL);
    g_assert(g_file_set_contents(bin1, (char *)img.bin1, img.bin1_len, NULL));
    g_assert(g_file_set_contents(bin2, (char *)img.bin2, img.bin2_len, NULL));
    g_assert(g_file_set_contents(img.cue,
        "REM generated\n"
        "FILE \"disc.bin\" BINARY\n"
        "  TRACK 01 MODE1/2352\n"
        "    INDEX 01 00:00:00\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 00 00:00:20\n"
        "    INDEX 01 00:01:20\n"
        "FILE \"track3.bin\" BINARY\n"
        "  TRACK 03 AUDIO\n"
        "    PREGAP 00:02:00\n"
        "    INDEX 01 00:00:00\n", -1, NULL));
}

static void free_image(void)
{
    g_autofree char *b1 = g_build_filename(img.dir, "disc.bin", NULL);
    g_autofree char *b2 = g_build_filename(img.dir, "track3.bin", NULL);

    unlink(b1);
    unlink(b2);
    unlink(img.cue);
    rmdir(img.dir);
    g_free(img.cue);
    g_free(img.dir);
    g_free(img.bin1);
    g_free(img.bin2);
}

/* PCM of disc sector @lba, NULL for gap sectors */
static const uint8_t *audio_sector(uint32_t lba)
{
    if (lba >= T1_SECTORS && lba < T2_START + T2_SECTORS) {
        return img.bin1 + lba * RAW;
    }
    if (lba >= T3_START && lba < LEADOUT) {
        return img.bin2 + (lba - T3_START) * RAW;
    }
    return NULL;
}

/* PCI configuration through the Uni-North host bridge (little-endian) */
static uint32_t pci_cfg_readl(QTestState *qts, int devfn, int reg)
{
    qtest_writel(qts, UNIN_CFG_ADDR, bswap32(0x80000000 | devfn << 8 | reg));
    return bswap32(qtest_readl(qts, UNIN_CFG_DATA));
}

static void pci_cfg_writel(QTestState *qts, int devfn, int reg, uint32_t v)
{
    qtest_writel(qts, UNIN_CFG_ADDR, bswap32(0x80000000 | devfn << 8 | reg));
    qtest_writel(qts, UNIN_CFG_DATA, bswap32(v));
}

static void map_macio(QTestState *qts)
{
    int devfn;

    for (devfn = 0; devfn < 256; devfn += 8) {
        uint32_t id = pci_cfg_readl(qts, devfn, 0);

        if ((id & 0xffff) == 0x106b && (id >> 16) == 0x0022) {
            pci_cfg_writel(qts, devfn, 0x10, MACIO_BAR);
            pci_cfg_writel(qts, devfn, 0x04, 0x6);  /* memory, bus master */
            return;
        }
    }
    g_assert_not_reached();
}

static QTestState *start(const char *extra)
{
    QTestState *qts;

    /* A G4 CPU keeps the Uni-North layout under ppc64 too */
    qts = qtest_initf("-M mac99 -cpu 7447a -nodefaults "
                      "-drive if=none,id=cd,file=%s,format=cue,media=cdrom "
                      "-device ide-cd,drive=cd,bus=ide.1,unit=0%s",
                      img.cue, extra ? extra : "");
    map_macio(qts);
    return qts;
}

static uint8_t ide_wait(QTestState *qts)
{
    uint8_t st;
    int i;

    for (i = 0; i < 100000; i++) {
        st = qtest_readb(qts, REG_STATUS);
        if (!(st & ST_BSY)) {
            return st;
        }
        g_usleep(10);
    }
    g_assert_not_reached();
}

static void ide_write_data(QTestState *qts, const uint8_t *buf, int len)
{
    int i;

    for (i = 0; i < len; i += 2) {
        qtest_memwrite(qts, REG_DATA, buf + i, 2);
    }
}

/* Sense key and ASC of a failed command, 0 on success */
typedef struct Result {
    uint8_t key;
    uint8_t asc;
    int len;
} Result;

static Result request_sense(QTestState *qts);

/*
 * Send @cdb; transfer @out_len bytes from @out or up to @in_len bytes into
 * @in by PIO.
 */
static Result atapi_pio(QTestState *qts, const uint8_t *cdb,
                        const uint8_t *out, int out_len,
                        uint8_t *in, int in_len)
{
    uint8_t packet[12] = { 0 };
    Result r = { 0 };
    uint8_t st;

    memcpy(packet, cdb, 12);
    qtest_writeb(qts, REG_DEVICE, 0xa0);
    qtest_writeb(qts, REG_FEATURE, 0);
    qtest_writeb(qts, REG_LCYL, 0xfe);
    qtest_writeb(qts, REG_HCYL, 0xff);
    qtest_writeb(qts, REG_STATUS, 0xa0);   /* PACKET */
    st = ide_wait(qts);
    g_assert(st & ST_DRQ);
    ide_write_data(qts, packet, 12);

    for (;;) {
        int n, i;

        st = ide_wait(qts);
        if (!(st & ST_DRQ)) {
            break;
        }
        n = qtest_readb(qts, REG_LCYL) | qtest_readb(qts, REG_HCYL) << 8;
        if (out) {
            g_assert(!(qtest_readb(qts, REG_NSECTOR) & 2));
            g_assert_cmpint(n, ==, out_len);
            ide_write_data(qts, out, out_len);
            continue;
        }
        for (i = 0; i < n; i += 2) {
            uint8_t w[2];

            qtest_memread(qts, REG_DATA, w, 2);
            if (r.len < in_len) {
                in[r.len] = w[0];
            }
            if (r.len + 1 < in_len) {
                in[r.len + 1] = w[1];
            }
            r.len += 2;
        }
    }
    if (st & ST_ERR) {
        int len = r.len;

        r = request_sense(qts);
        r.len = len;
    }
    return r;
}

static Result request_sense(QTestState *qts)
{
    uint8_t cdb[12] = { 0x03, 0, 0, 0, 18 };
    uint8_t sense[18] = { 0 };
    Result r;

    r = atapi_pio(qts, cdb, NULL, 0, sense, sizeof(sense));
    g_assert_cmpint(r.key, ==, 0);
    r.key = sense[2] & 0xf;
    r.asc = sense[12];
    return r;
}

static Result atapi(QTestState *qts, const uint8_t *cdb, uint8_t *in,
                    int in_len)
{
    return atapi_pio(qts, cdb, NULL, 0, in, in_len);
}

/* Clear the unit attention of the newly inserted medium. */
static void ready(QTestState *qts)
{
    uint8_t tur[12] = { 0 };
    int i;

    for (i = 0; i < 4; i++) {
        if (atapi(qts, tur, NULL, 0).key == 0) {
            return;
        }
    }
    g_assert_not_reached();
}

static void read_cd_cdb(uint8_t *cdb, int type, uint32_t lba, uint32_t n,
                        uint8_t fields)
{
    memset(cdb, 0, 12);
    cdb[0] = 0xbe;
    cdb[1] = type << 2;
    stl_be_p(cdb + 2, lba);
    cdb[6] = n >> 16;
    cdb[7] = n >> 8;
    cdb[8] = n;
    cdb[9] = fields;
}

static void test_toc(void)
{
    QTestState *qts = start(NULL);
    uint8_t cdb[12] = { 0x43 };
    uint8_t buf[256];
    Result r;
    int i;
    static const struct {
        uint8_t ctrl;
        uint32_t lba;
    } expect[] = {
        { 0x14, 0 }, { 0x10, T2_START }, { 0x10, T3_START },
        { 0x10, LEADOUT },
    };

    ready(qts);

    /* format 0, LBA */
    stw_be_p(cdb + 7, sizeof(buf));
    r = atapi(qts, cdb, buf, sizeof(buf));
    g_assert_cmpint(r.key, ==, 0);
    g_assert_cmpint(r.len, ==, 4 + 4 * 8);
    g_assert_cmpint(lduw_be_p(buf), ==, 2 + 4 * 8);
    g_assert_cmpint(buf[2], ==, 1);
    g_assert_cmpint(buf[3], ==, 3);
    for (i = 0; i < 4; i++) {
        uint8_t *d = buf + 4 + i * 8;

        g_assert_cmphex(d[1], ==, expect[i].ctrl);
        g_assert_cmpint(d[2], ==, i < 3 ? i + 1 : 0xaa);
        g_assert_cmpint(ldl_be_p(d + 4), ==, expect[i].lba);
    }

    /* format 0, MSF, from track 2 */
    cdb[1] = 2;
    cdb[6] = 2;
    r = atapi(qts, cdb, buf, sizeof(buf));
    g_assert_cmpint(r.key, ==, 0);
    g_assert_cmpint(r.len, ==, 4 + 3 * 8);
    g_assert_cmpint(buf[4 + 2], ==, 2);
    g_assert_cmpint(buf[4 + 5], ==, (T2_START + 150) / 75 / 60);
    g_assert_cmpint(buf[4 + 6], ==, ((T2_START + 150) / 75) % 60);
    g_assert_cmpint(buf[4 + 7], ==, (T2_START + 150) % 75);

    /* start track past the last one */
    cdb[6] = 4;
    r = atapi(qts, cdb, buf, sizeof(buf));
    g_assert_cmpint(r.key, ==, 5);

    /* format 1: session */
    cdb[1] = 0;
    cdb[6] = 0;
    cdb[2] = 1;
    r = atapi(qts, cdb, buf, sizeof(buf));
    g_assert_cmpint(r.key, ==, 0);
    g_assert_cmpint(r.len, ==, 12);
    g_assert_cmphex(buf[5], ==, 0x14);
    g_assert_cmpint(buf[6], ==, 1);

    /* format 2: full TOC */
    cdb[2] = 2;
    r = atapi(qts, cdb, buf, sizeof(buf));
    g_assert_cmpint(r.key, ==, 0);
    g_assert_cmpint(r.len, ==, 4 + 6 * 11);
    g_assert_cmphex(buf[4 + 3], ==, 0xa0);
    g_assert_cmpint(buf[4 + 8], ==, 1);
    g_assert_cmphex(buf[15 + 3], ==, 0xa1);
    g_assert_cmpint(buf[15 + 8], ==, 3);
    g_assert_cmphex(buf[26 + 1], ==, 0x10);
    g_assert_cmphex(buf[26 + 3], ==, 0xa2);
    g_assert_cmpint(buf[26 + 8], ==, (LEADOUT + 150) / 75 / 60);
    g_assert_cmpint(buf[26 + 9], ==, ((LEADOUT + 150) / 75) % 60);
    g_assert_cmpint(buf[26 + 10], ==, (LEADOUT + 150) % 75);
    g_assert_cmphex(buf[37 + 1], ==, 0x14);
    g_assert_cmphex(buf[48 + 1], ==, 0x10);
    g_assert_cmpint(buf[59 + 3], ==, 3);

    /* capacity is the lead-out */
    memset(cdb, 0, 12);
    cdb[0] = 0x25;
    r = atapi(qts, cdb, buf, 8);
    g_assert_cmpint(r.key, ==, 0);
    g_assert_cmpint(ldl_be_p(buf), ==, LEADOUT - 1);

    qtest_quit(qts);
}

static void test_read_cd(void)
{
    QTestState *qts = start(NULL);
    const int n = 30;
    g_autofree uint8_t *buf = g_malloc(n * RAW);
    uint8_t cdb[12];
    Result r;
    int i;

    ready(qts);

    /* audio sectors across the track 2 pregap/index 1 boundary */
    read_cd_cdb(cdb, 1, T2_START - 5, n, 0x10);
    r = atapi(qts, cdb, buf, n * RAW);
    g_assert_cmpint(r.key, ==, 0);
    g_assert_cmpint(r.len, ==, n * RAW);
    g_assert(!memcmp(buf, img.bin1 + (T2_START - 5) * RAW, n * RAW));

    /* track 3, second file, any sector type */
    read_cd_cdb(cdb, 0, T3_START + 7, 3, 0xf8);
    r = atapi(qts, cdb, buf, 3 * RAW);
    g_assert_cmpint(r.key, ==, 0);
    g_assert(!memcmp(buf, img.bin2 + 7 * RAW, 3 * RAW));

    /* PREGAP of track 3 is silence */
    read_cd_cdb(cdb, 1, T3_START - 2, 2, 0x10);
    r = atapi(qts, cdb, buf, 2 * RAW);
    g_assert_cmpint(r.key, ==, 0);
    for (i = 0; i < 2 * RAW; i++) {
        g_assert_cmpint(buf[i], ==, 0);
    }

    /* raw data sectors come from the bin unchanged */
    read_cd_cdb(cdb, 2, 3, 2, 0xf8);
    r = atapi(qts, cdb, buf, 2 * RAW);
    g_assert_cmpint(r.key, ==, 0);
    g_assert_cmpint(r.len, ==, 2 * RAW);
    g_assert(!memcmp(buf, img.bin1 + 3 * RAW, 2 * RAW));

    /* user data only */
    read_cd_cdb(cdb, 0, 4, 2, 0x10);
    r = atapi(qts, cdb, buf, 2 * 2048);
    g_assert_cmpint(r.len, ==, 2 * 2048);
    g_assert(!memcmp(buf, img.bin1 + 4 * RAW + 16, 2048));
    g_assert(!memcmp(buf + 2048, img.bin1 + 5 * RAW + 16, 2048));

    /* header + user data with formatted Q sub-channel */
    read_cd_cdb(cdb, 1, T2_START + 1, 1, 0x10);
    cdb[10] = 2;
    r = atapi(qts, cdb, buf, RAW + 16);
    g_assert_cmpint(r.len, ==, RAW + 16);
    g_assert_cmphex(buf[RAW], ==, 0x10);
    g_assert_cmphex(buf[RAW + 1], ==, 0x02);
    g_assert_cmphex(buf[RAW + 2], ==, 0x01);
    g_assert_cmphex(buf[RAW + 5], ==, 0x01);

    /* READ CD MSF of audio */
    memset(cdb, 0, 12);
    cdb[0] = 0xb9;
    cdb[1] = 1 << 2;
    cdb[3] = (T2_START + 150) / 75 / 60;
    cdb[4] = ((T2_START + 150) / 75) % 60;
    cdb[5] = (T2_START + 150) % 75;
    cdb[6] = (T2_START + 152) / 75 / 60;
    cdb[7] = ((T2_START + 152) / 75) % 60;
    cdb[8] = (T2_START + 152) % 75;
    cdb[9] = 0x10;
    r = atapi(qts, cdb, buf, 2 * RAW);
    g_assert_cmpint(r.key, ==, 0);
    g_assert_cmpint(r.len, ==, 2 * RAW);
    g_assert(!memcmp(buf, img.bin1 + T2_START * RAW, 2 * RAW));

    /* sector type mismatches */
    read_cd_cdb(cdb, 2, T2_START, 1, 0x10);
    r = atapi(qts, cdb, buf, RAW);
    g_assert_cmpint(r.key, ==, 5);
    g_assert_cmphex(r.asc, ==, 0x64);
    read_cd_cdb(cdb, 1, 0, 1, 0x10);
    r = atapi(qts, cdb, buf, RAW);
    g_assert_cmphex(r.asc, ==, 0x64);

    /* READ(10) of audio fails, of data succeeds */
    memset(cdb, 0, 12);
    cdb[0] = 0x28;
    stl_be_p(cdb + 2, T2_START);
    cdb[8] = 1;
    r = atapi(qts, cdb, buf, 2048);
    g_assert_cmphex(r.asc, ==, 0x64);
    stl_be_p(cdb + 2, 9);
    r = atapi(qts, cdb, buf, 2048);
    g_assert_cmpint(r.key, ==, 0);
    g_assert(!memcmp(buf, img.bin1 + 9 * RAW + 16, 2048));

    qtest_quit(qts);
}

/* READ CD audio over DBDMA */
static void test_read_cd_dma(void)
{
    QTestState *qts = start(NULL);
    const int n = 40;
    const uint64_t buf_addr = 0x100000, cmd_addr = 0x10000;
    g_autofree uint8_t *buf = g_malloc(n * RAW);
    uint8_t packet[12];
    uint8_t desc[32] = { 0 };
    uint8_t st;
    int i;

    ready(qts);

    /* two INPUT descriptors, then STOP */
    stw_le_p(desc + 0, n * RAW / 2);
    stw_le_p(desc + 2, 0x2000);         /* INPUT_MORE */
    stl_le_p(desc + 4, buf_addr);
    stw_le_p(desc + 16, n * RAW / 2);
    stw_le_p(desc + 18, 0x3000);        /* INPUT_LAST */
    stl_le_p(desc + 20, buf_addr + n * RAW / 2);
    qtest_memwrite(qts, cmd_addr, desc, sizeof(desc));
    memset(desc, 0, 16);
    stw_le_p(desc + 2, 0x7000);         /* STOP */
    qtest_memwrite(qts, cmd_addr + 32, desc, 16);
    qtest_memset(qts, buf_addr, 0x55, n * RAW);

    qtest_writel(qts, DBDMA_CH + 0x0c, bswap32(cmd_addr));
    qtest_writel(qts, DBDMA_CH, bswap32(0x80008000));   /* RUN */

    read_cd_cdb(packet, 1, T2_START + 10, n, 0x10);
    qtest_writeb(qts, REG_DEVICE, 0xa0);
    qtest_writeb(qts, REG_FEATURE, 1);  /* DMA */
    qtest_writeb(qts, REG_LCYL, 0);
    qtest_writeb(qts, REG_HCYL, 0);
    qtest_writeb(qts, REG_STATUS, 0xa0);
    st = ide_wait(qts);
    g_assert(st & ST_DRQ);
    ide_write_data(qts, packet, 12);

    for (i = 0; i < 100000; i++) {
        st = qtest_readb(qts, REG_STATUS);
        if (!(st & (ST_BSY | ST_DRQ))) {
            break;
        }
        g_usleep(10);
    }
    g_assert_cmphex(st & ST_ERR, ==, 0);
    qtest_memread(qts, buf_addr, buf, n * RAW);
    g_assert(!memcmp(buf, img.bin1 + (T2_START + 10) * RAW, n * RAW));

    qtest_quit(qts);
}

typedef struct SubQ {
    uint8_t status;
    uint8_t track;
    uint8_t index;
    uint32_t abs;
    int32_t rel;
} SubQ;

static SubQ read_subchannel(QTestState *qts)
{
    uint8_t cdb[12] = { 0x42, 0, 0x40, 1, 0, 0, 0, 0, 16 };
    uint8_t buf[16];
    SubQ q;
    Result r;

    r = atapi(qts, cdb, buf, sizeof(buf));
    g_assert_cmpint(r.key, ==, 0);
    g_assert_cmpint(buf[4], ==, 1);
    q.status = buf[1];
    q.track = buf[6];
    q.index = buf[7];
    q.abs = ldl_be_p(buf + 8);
    q.rel = ldl_be_p(buf + 12);
    return q;
}

/* Advance the clock in audio timer periods so read-ahead keeps up. */
static void step_ms(QTestState *qts, int ms)
{
    int i;

    for (i = 0; i < ms; i += 10) {
        qtest_clock_step(qts, 10 * 1000 * 1000);
    }
}

static void play(QTestState *qts, uint32_t lba, uint32_t len)
{
    uint8_t cdb[12] = { 0xa5 };
    Result r;

    stl_be_p(cdb + 2, lba);
    stl_be_p(cdb + 6, len);
    r = atapi(qts, cdb, NULL, 0);
    g_assert_cmpint(r.key, ==, 0);
}

static void test_play_subchannel(gconstpointer data)
{
    bool with_audio = GPOINTER_TO_INT(data);
    QTestState *qts = start(with_audio ?
                            " -audiodev none,id=snd -global ide-cd.audiodev=snd"
                            : NULL);
    uint8_t cdb[12];
    SubQ q, q2;
    Result r;

    ready(qts);

    q = read_subchannel(qts);
    g_assert_cmphex(q.status, ==, 0x15);

    /* play of a data track is refused */
    memset(cdb, 0, 12);
    cdb[0] = 0x45;
    stl_be_p(cdb + 2, 2);
    cdb[8] = 10;
    r = atapi(qts, cdb, NULL, 0);
    g_assert_cmphex(r.asc, ==, 0x64);

    /* pause without play */
    memset(cdb, 0, 12);
    cdb[0] = 0x4b;
    r = atapi(qts, cdb, NULL, 0);
    g_assert_cmphex(r.asc, ==, 0x2c);

    play(qts, T2_START, 150);
    q = read_subchannel(qts);
    g_assert_cmphex(q.status, ==, 0x11);
    g_assert_cmpint(q.track, ==, 2);

    step_ms(qts, 1000);
    q = read_subchannel(qts);
    g_assert_cmphex(q.status, ==, 0x11);
    g_assert_cmpint(q.track, ==, 2);
    g_assert_cmpint(q.index, ==, 1);
    g_assert_cmpint(q.abs, >=, T2_START + 70);
    g_assert_cmpint(q.abs, <=, T2_START + 76);
    g_assert_cmpint(q.rel, ==, q.abs - T2_START);

    /* pause holds the position */
    memset(cdb, 0, 12);
    cdb[0] = 0x4b;
    r = atapi(qts, cdb, NULL, 0);
    g_assert_cmpint(r.key, ==, 0);
    q = read_subchannel(qts);
    g_assert_cmphex(q.status, ==, 0x12);
    step_ms(qts, 500);
    q2 = read_subchannel(qts);
    g_assert_cmpint(q2.abs, ==, q.abs);

    /* resume and run to the end */
    cdb[8] = 1;
    r = atapi(qts, cdb, NULL, 0);
    g_assert_cmpint(r.key, ==, 0);
    g_assert_cmphex(read_subchannel(qts).status, ==, 0x11);
    step_ms(qts, 2000);
    q = read_subchannel(qts);
    g_assert_cmphex(q.status, ==, 0x13);
    g_assert_cmpint(q.abs, ==, T2_START + 150);
    g_assert_cmphex(read_subchannel(qts).status, ==, 0x15);

    /* stop during play; pregap position reports index 0 */
    play(qts, T3_START - 100, 200);
    step_ms(qts, 100);
    q = read_subchannel(qts);
    g_assert_cmpint(q.track, ==, 3);
    g_assert_cmpint(q.index, ==, 0);
    g_assert_cmpint(q.rel, <, 0);
    memset(cdb, 0, 12);
    cdb[0] = 0x4e;
    r = atapi(qts, cdb, NULL, 0);
    g_assert_cmpint(r.key, ==, 0);
    g_assert_cmphex(read_subchannel(qts).status, ==, 0x15);

    qtest_quit(qts);
}

static void mode_select_volume(QTestState *qts, uint8_t lsel, uint8_t lvol,
                               uint8_t rsel, uint8_t rvol)
{
    uint8_t cdb[12] = { 0x55, 0x10 };
    uint8_t param[24] = { 0 };
    Result r;

    param[8] = 0x0e;
    param[9] = 14;
    param[10] = 0x04;
    param[16] = lsel;
    param[17] = lvol;
    param[18] = rsel;
    param[19] = rvol;
    stw_be_p(cdb + 7, sizeof(param));
    r = atapi_pio(qts, cdb, param, sizeof(param), NULL, 0);
    g_assert_cmpint(r.key, ==, 0);
}

/* Play @len sectors from @lba into a wav file and return its samples. */
static uint8_t *play_to_wav(uint32_t lba, uint32_t len, bool half_left,
                            size_t *size)
{
    g_autofree char *wav = g_build_filename(img.dir, "out.wav", NULL);
    g_autofree char *opts = g_strdup_printf(
        " -audiodev wav,id=snd,path=%s -global ide-cd.audiodev=snd", wav);
    QTestState *qts = start(opts);
    uint8_t cdb[12];
    gchar *data;
    gsize n;
    uint8_t *pcm;
    int i;

    ready(qts);
    if (half_left) {
        mode_select_volume(qts, 0x01, 0x80, 0x00, 0xff);
    }

    /* page 0x0e reads back */
    {
        uint8_t ms[12] = { 0x5a, 0, 0x0e, 0, 0, 0, 0, 0, 24 };
        uint8_t buf[24];

        memcpy(cdb, ms, 12);
        g_assert_cmpint(atapi(qts, cdb, buf, sizeof(buf)).key, ==, 0);
        g_assert_cmphex(buf[8], ==, 0x0e);
        g_assert_cmphex(buf[17], ==, half_left ? 0x80 : 0xff);
        g_assert_cmphex(buf[18], ==, half_left ? 0x00 : 0x02);
    }

    play(qts, lba, len);
    for (i = 0; i < 400 && read_subchannel(qts).status == 0x11; i++) {
        qtest_clock_step(qts, 10 * 1000 * 1000);
    }
    g_assert_cmphex(read_subchannel(qts).status, ==, 0x15);
    /* let the output buffer play out */
    step_ms(qts, 500);
    qtest_quit(qts);

    g_assert(g_file_get_contents(wav, &data, &n, NULL));
    unlink(wav);
    g_assert_cmpint(n, >, 44);
    /* drop the header and leading silence before the first read landed */
    for (i = 44; i + 4 <= n && !ldl_le_p(data + i); i += 4) {
        /* nothing */
    }
    *size = n - i;
    pcm = g_memdup2(data + i, *size);
    g_free(data);
    return pcm;
}

static bool pcm_matches(const uint8_t *pcm, size_t size, uint32_t lba,
                        uint32_t len)
{
    uint32_t i;

    if (size < len * RAW) {
        return false;
    }
    for (i = 0; i < len; i++) {
        const uint8_t *src = audio_sector(lba + i);

        if (!src || memcmp(pcm + i * RAW, src, RAW)) {
            return false;
        }
    }
    return true;
}

static void test_play_output(void)
{
    const uint32_t lba = T2_START + 33, len = 40;
    g_autofree uint8_t *pcm = NULL;
    g_autofree uint8_t *wrong = NULL;
    size_t size, wsize;

    pcm = play_to_wav(lba, len, false, &size);
    g_assert(pcm_matches(pcm, size, lba, len));

    /* positive control: a play one sector off must not match */
    wrong = play_to_wav(lba + 1, len, false, &wsize);
    g_assert(pcm_matches(wrong, wsize, lba + 1, len));
    g_assert(!pcm_matches(wrong, wsize, lba, len));
}

static void test_play_volume(void)
{
    const uint32_t lba = T3_START + 5, len = 20;
    g_autofree uint8_t *pcm = NULL;
    size_t size;
    uint32_t i;

    pcm = play_to_wav(lba, len, true, &size);
    g_assert_cmpint(size, >=, len * RAW);
    for (i = 0; i < len * 588; i++) {
        const uint8_t *src = audio_sector(lba + i / 588) + (i % 588) * 4;
        int16_t l = lduw_le_p(src);

        g_assert_cmpint((int16_t)lduw_le_p(pcm + i * 4), ==, l * 0x80 / 255);
        g_assert_cmpint((int16_t)lduw_le_p(pcm + i * 4 + 2), ==, 0);
    }
}

static int read_toc0(QTestState *qts, uint8_t *buf, int len)
{
    uint8_t cdb[12] = { 0x43 };
    Result r;

    stw_be_p(cdb + 7, len);
    ready(qts);
    r = atapi(qts, cdb, buf, len);
    g_assert_cmpint(r.key, ==, 0);
    return r.len;
}

static void change_medium(QTestState *qts, const char *file, const char *fmt)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'blockdev-change-medium', 'arguments': {"
        " 'device': 'cd', 'filename': %s, 'format': %s }}", file, fmt);
}

static void test_media_change(void)
{
    g_autofree char *iso = g_build_filename(img.dir, "plain.iso", NULL);
    g_autofree uint8_t *data = g_malloc0(64 * 2048);
    QTestState *qts = start(NULL);
    uint8_t buf[64];
    uint8_t cdb[12];
    Result r;
    int len;

    g_assert(g_file_set_contents(iso, (char *)data, 64 * 2048, NULL));

    len = read_toc0(qts, buf, sizeof(buf));
    g_assert_cmpint(len, ==, 4 + 4 * 8);

    /* a plain image keeps the single data track answer */
    change_medium(qts, iso, "raw");
    len = read_toc0(qts, buf, sizeof(buf));
    g_assert_cmpint(len, ==, 20);
    g_assert_cmpint(buf[3], ==, 1);
    g_assert_cmphex(buf[5], ==, 0x14);
    g_assert_cmphex(buf[13], ==, 0x16);
    g_assert_cmpint(ldl_be_p(buf + 16), ==, 64);
    play(qts, 0, 0);
    memset(cdb, 0, 12);
    cdb[0] = 0x45;
    cdb[8] = 1;
    r = atapi(qts, cdb, NULL, 0);
    g_assert_cmphex(r.asc, ==, 0x64);
    g_assert_cmphex(read_subchannel(qts).status, ==, 0x15);

    /* back to the cue sheet */
    change_medium(qts, img.cue, "cue");
    len = read_toc0(qts, buf, sizeof(buf));
    g_assert_cmpint(len, ==, 4 + 4 * 8);
    g_assert_cmpint(ldl_be_p(buf + 4 + 3 * 8 + 4), ==, LEADOUT);

    qtest_quit(qts);
    unlink(iso);
}

static void test_capabilities(void)
{
    QTestState *qts = start(NULL);
    uint8_t cdb[12] = { 0x46, 0x02, 0x01, 0x03, 0, 0, 0, 0, 64 };
    uint8_t buf[64];
    Result r;

    ready(qts);
    r = atapi(qts, cdb, buf, sizeof(buf));
    g_assert_cmpint(r.key, ==, 0);
    g_assert_cmpint(r.len, ==, 16);
    g_assert_cmphex(lduw_be_p(buf + 8), ==, 0x0103);
    g_assert_cmphex(buf[10] & 1, ==, 1);
    g_assert_cmpint(lduw_be_p(buf + 14), ==, 256);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine("mac99")) {
        g_test_skip("mac99 not available");
        return g_test_run();
    }

    make_image();
    qtest_add_func("ide-cd-audio/toc", test_toc);
    qtest_add_func("ide-cd-audio/read-cd", test_read_cd);
    qtest_add_func("ide-cd-audio/read-cd-dma", test_read_cd_dma);
    qtest_add_func("ide-cd-audio/capabilities", test_capabilities);
    qtest_add_func("ide-cd-audio/media-change", test_media_change);
    qtest_add_data_func("ide-cd-audio/play-subchannel/silent",
                        GINT_TO_POINTER(0), test_play_subchannel);
    qtest_add_data_func("ide-cd-audio/play-subchannel/audiodev",
                        GINT_TO_POINTER(1), test_play_subchannel);
    qtest_add_func("ide-cd-audio/play-output", test_play_output);
    qtest_add_func("ide-cd-audio/play-volume", test_play_volume);
    ret = g_test_run();
    free_image();
    return ret;
}
