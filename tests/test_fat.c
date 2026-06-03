/* tests/test_fat.c — Unit tests for the FAT12 filesystem layer.
 *
 * Covers: fat_unpack/fat_pack, fat_fndclus, fat_geteof,
 *         fat_release/fat_relblks, fat_allocate, disk_figrec,
 *         and the fat12_is_eof/fat12_is_free predicates.
 *
 * Does not require a BIOS or disk image: all operations work on
 * in-memory FAT buffers via fat_buf_t + dpb_t stubs.  Functions that
 * accept dos_t* but never dereference it (marked (void)dos) receive NULL.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../src/kernel/types.h"
#include "../src/kernel/bios.h"
#include "../src/kernel/dos.h"
#include "../src/kernel/fat.h"
#include "../src/kernel/disk.h"

/* ------------------------------------------------------------------ */
/* Minimal test harness                                                  */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail;

#define CHECK(cond, msg) \
    do { \
        if (cond) { printf("PASS  " msg "\n"); g_pass++; } \
        else      { printf("FAIL  " msg "  [line %d]\n", __LINE__); g_fail++; } \
    } while (0)

/* ------------------------------------------------------------------ */
/* Helpers: set up dpb_t backed by fat_buf_t, build chains              */
/* ------------------------------------------------------------------ */

static void dpb_init(fat_buf_t *fb, dpb_t *dp,
                     uint16_t maxclus, uint8_t clusmsk, uint16_t firrec)
{
    memset(fb, 0, sizeof *fb);
    /* dpb->fat points to fb->data; fat[-1]==fb->fat_dirty, fat[-2]==fb->dev_dirty */
    dp->fat      = fb->data;
    dp->maxclus  = maxclus;
    dp->clusmsk  = clusmsk;
    dp->clusshft = 0;
    dp->firrec   = firrec;
    dp->firfat   = 1;
    dp->firdir   = 3;
    dp->secsiz   = 512;
    dp->fatsiz   = 1;
    dp->fatcnt   = 1;
    dp->maxent   = 16;
    dp->drvnum   = 0;
    dp->devnum   = 0;
}

/* Build sequential chain: start → start+1 → … → end → EOF */
static void chain_make(dpb_t *dp, uint16_t start, uint16_t end)
{
    for (uint16_t c = start; c < end; c++)
        fat_pack(dp->fat, c, c + 1);
    fat_pack(dp->fat, end, FAT12_EOF);
}

/* Make an fcb_t with given first cluster and no cache */
static fcb_t fcb_make(uint16_t firclus)
{
    fcb_t f;
    memset(&f, 0, sizeof f);
    f.firclus = firclus;
    f.recsiz  = 128;
    return f;
}

/* ------------------------------------------------------------------ */
/* fat12_is_eof / fat12_is_free predicates                              */
/* ------------------------------------------------------------------ */

static void test_predicates(void)
{
    printf("\n--- fat12 predicates ---\n");

    CHECK( fat12_is_eof(0xFF8), "0xFF8 is EOF (min threshold)");
    CHECK( fat12_is_eof(0xFFF), "0xFFF is EOF (canonical)");
    CHECK( fat12_is_eof(0xFFB), "0xFFB is EOF (mid-range)");
    CHECK(!fat12_is_eof(0xFF7), "0xFF7 (BAD) is not EOF");
    CHECK(!fat12_is_eof(0x000), "0x000 (FREE) is not EOF");
    CHECK(!fat12_is_eof(0x005), "0x005 is not EOF");

    CHECK( fat12_is_free(0x000), "0x000 is FREE");
    CHECK(!fat12_is_free(0x001), "0x001 is not FREE");
    CHECK(!fat12_is_free(0xFF8), "0xFF8 is not FREE");
    CHECK(!fat12_is_free(0xFFF), "0xFFF is not FREE");
}

/* ------------------------------------------------------------------ */
/* fat_unpack / fat_pack                                                 */
/* ------------------------------------------------------------------ */

static void test_pack_unpack(void)
{
    printf("\n--- fat_unpack / fat_pack ---\n");
    uint8_t buf[64];

    /* All-zero FAT: every cluster reads as FREE */
    memset(buf, 0, sizeof buf);
    CHECK(fat_unpack(buf, 2)  == FAT12_FREE, "all-zero: cluster 2 == FREE");
    CHECK(fat_unpack(buf, 3)  == FAT12_FREE, "all-zero: cluster 3 == FREE");
    CHECK(fat_unpack(buf, 10) == FAT12_FREE, "all-zero: cluster 10 == FREE");

    /* Standard FAT12 header bytes: 0xF8 0xFF 0xFF → clus0=0xFF8, clus1=0xFFF */
    memset(buf, 0, sizeof buf);
    buf[0] = 0xF8; buf[1] = 0xFF; buf[2] = 0xFF;
    CHECK(fat_unpack(buf, 0) == 0xFF8, "header cluster 0 == 0xFF8 (media byte)");
    CHECK(fat_unpack(buf, 1) == 0xFFF, "header cluster 1 == 0xFFF");

    /* Round-trip: even cluster */
    memset(buf, 0, sizeof buf);
    fat_pack(buf, 2, 0xABC);
    CHECK(fat_unpack(buf, 2) == 0xABC, "round-trip even cluster 2");

    /* Round-trip: odd cluster */
    memset(buf, 0, sizeof buf);
    fat_pack(buf, 3, 0xDEF);
    CHECK(fat_unpack(buf, 3) == 0xDEF, "round-trip odd cluster 3");

    /* Special values */
    memset(buf, 0, sizeof buf);
    fat_pack(buf, 4, FAT12_FREE);
    CHECK(fat_unpack(buf, 4) == FAT12_FREE, "pack/unpack FREE (0x000)");

    fat_pack(buf, 5, FAT12_EOF);
    CHECK(fat_unpack(buf, 5) == FAT12_EOF, "pack/unpack EOF (0xFFF)");

    fat_pack(buf, 6, FAT12_BAD);
    CHECK(fat_unpack(buf, 6) == FAT12_BAD, "pack/unpack BAD (0xFF7)");

    /* Neighbor isolation: adjacent clusters share one byte.
     * Packing cluster N must not corrupt cluster N±1. */
    memset(buf, 0, sizeof buf);
    fat_pack(buf, 4, 0x456);       /* even */
    fat_pack(buf, 5, 0x123);       /* odd  — shares a byte with cluster 4 */
    CHECK(fat_unpack(buf, 4) == 0x456, "neighbor isolation: cluster 4 not corrupted by cluster 5 write");
    CHECK(fat_unpack(buf, 5) == 0x123, "neighbor isolation: cluster 5 not corrupted by cluster 4 write");

    memset(buf, 0, sizeof buf);
    fat_pack(buf, 6, 0xAAA);       /* even */
    fat_pack(buf, 7, 0xBBB);       /* odd  — shares a byte with cluster 6 */
    CHECK(fat_unpack(buf, 6) == 0xAAA, "neighbor isolation: even after odd write");
    CHECK(fat_unpack(buf, 7) == 0xBBB, "neighbor isolation: odd after even write");

    /* Overwrite: second pack must replace the first */
    memset(buf, 0, sizeof buf);
    fat_pack(buf, 8, 0x111);
    fat_pack(buf, 8, 0x222);
    CHECK(fat_unpack(buf, 8) == 0x222, "overwrite even cluster");

    memset(buf, 0, sizeof buf);
    fat_pack(buf, 9, 0x333);
    fat_pack(buf, 9, 0x444);
    CHECK(fat_unpack(buf, 9) == 0x444, "overwrite odd cluster");

    /* High cluster numbers: no out-of-bounds reads */
    memset(buf, 0, sizeof buf);
    fat_pack(buf, 20, 0xCCC);
    CHECK(fat_unpack(buf, 20) == 0xCCC, "high even cluster 20");

    fat_pack(buf, 21, 0xDDD);
    CHECK(fat_unpack(buf, 21) == 0xDDD, "high odd cluster 21");
    CHECK(fat_unpack(buf, 20) == 0xCCC, "cluster 20 intact after cluster 21 write");
}

/* ------------------------------------------------------------------ */
/* fat_fndclus                                                           */
/* ------------------------------------------------------------------ */

static void test_fndclus(void)
{
    printf("\n--- fat_fndclus ---\n");
    fat_buf_t fb;
    dpb_t     dp;
    dpb_init(&fb, &dp, 20, 0, 5);  /* maxclus=20, 1 sec/clus */

    /* Build chain: 2→3→4→5→EOF */
    chain_make(&dp, 2, 5);

    /* Empty file: firclus==0 returns EOF sentinel */
    {
        fcb_t f = fcb_make(0);
        uint16_t pos;
        uint16_t r = fat_fndclus(NULL, &dp, &f, 0, &pos);
        CHECK(fat12_is_eof(r), "empty file (firclus=0): returns EOF sentinel");
    }

    /* count=0, no cache: returns first cluster at pos 0 */
    {
        fcb_t f = fcb_make(2);
        uint16_t pos;
        uint16_t r = fat_fndclus(NULL, &dp, &f, 0, &pos);
        CHECK(r == 2 && pos == 0, "count=0: returns first cluster");
    }

    /* count=1: one step forward */
    {
        fcb_t f = fcb_make(2);
        uint16_t pos;
        uint16_t r = fat_fndclus(NULL, &dp, &f, 1, &pos);
        CHECK(r == 3 && pos == 1, "count=1: returns cluster 3");
    }

    /* count=3: reaches last cluster in chain */
    {
        fcb_t f = fcb_make(2);
        uint16_t pos;
        uint16_t r = fat_fndclus(NULL, &dp, &f, 3, &pos);
        CHECK(r == 5 && pos == 3, "count=3: reaches last cluster");
    }

    /* count > chain length: stops at EOF, returns EOF value */
    {
        fcb_t f = fcb_make(2);
        uint16_t pos;
        uint16_t r = fat_fndclus(NULL, &dp, &f, 10, &pos);
        CHECK(fat12_is_eof(r), "count > chain length: returns EOF val");
    }

    /* Cache hit: lstclus=3 at cluspos=1, want count=2 → walk 1 more step */
    {
        fcb_t f = fcb_make(2);
        f.lstclus = 3;
        f.cluspos = 1;
        uint16_t pos;
        uint16_t r = fat_fndclus(NULL, &dp, &f, 2, &pos);
        CHECK(r == 4 && pos == 2, "cache hit forward: resumes from lstclus");
    }

    /* Cache exact hit: lstclus=4 at cluspos=2, want count=2 → return lstclus */
    {
        fcb_t f = fcb_make(2);
        f.lstclus = 4;
        f.cluspos = 2;
        uint16_t pos;
        uint16_t r = fat_fndclus(NULL, &dp, &f, 2, &pos);
        CHECK(r == 4 && pos == 2, "cache exact hit: returns lstclus directly");
    }

    /* Cache miss (backwards): lstclus=4 at cluspos=2, want count=1 → restart */
    {
        fcb_t f = fcb_make(2);
        f.lstclus = 4;
        f.cluspos = 2;
        uint16_t pos;
        uint16_t r = fat_fndclus(NULL, &dp, &f, 1, &pos);
        CHECK(r == 3 && pos == 1, "cache miss backwards: restarts from firclus");
    }

    /* Single-cluster file: count=0 → returns that cluster, count=1 → EOF */
    {
        memset(fb.data, 0, sizeof fb.data);
        fat_pack(dp.fat, 7, FAT12_EOF);
        fcb_t f = fcb_make(7);
        uint16_t pos;
        CHECK(fat_fndclus(NULL, &dp, &f, 0, &pos) == 7, "single-cluster: count=0 returns it");
        CHECK(fat12_is_eof(fat_fndclus(NULL, &dp, &f, 1, &pos)),
              "single-cluster: count=1 returns EOF");
    }
}

/* ------------------------------------------------------------------ */
/* fat_geteof                                                            */
/* ------------------------------------------------------------------ */

static void test_geteof(void)
{
    printf("\n--- fat_geteof ---\n");
    fat_buf_t fb;
    dpb_t     dp;
    dpb_init(&fb, &dp, 20, 0, 5);

    /* Single cluster: last == that cluster */
    memset(fb.data, 0, sizeof fb.data);
    fat_pack(dp.fat, 2, FAT12_EOF);
    CHECK(fat_geteof(dp.fat, 2) == 2, "single cluster: last == 2");

    /* Chain 2→3→4→EOF: last == 4 */
    memset(fb.data, 0, sizeof fb.data);
    chain_make(&dp, 2, 4);
    CHECK(fat_geteof(dp.fat, 2) == 4, "chain 2-4: last == 4");

    /* Chain starting mid-range: 6→7→8→EOF */
    memset(fb.data, 0, sizeof fb.data);
    chain_make(&dp, 6, 8);
    CHECK(fat_geteof(dp.fat, 6) == 8, "chain 6-8: last == 8");
}

/* ------------------------------------------------------------------ */
/* fat_release / fat_relblks                                             */
/* ------------------------------------------------------------------ */

static void test_release(void)
{
    printf("\n--- fat_release / fat_relblks ---\n");
    fat_buf_t fb;
    dpb_t     dp;
    dpb_init(&fb, &dp, 20, 0, 5);

    /* Release single cluster: becomes FREE, dirty flag set */
    {
        memset(fb.data, 0, sizeof fb.data);
        fat_pack(dp.fat, 2, FAT12_EOF);
        fb.fat_dirty = 0;
        fat_release(NULL, &dp, 2);
        CHECK(fat_unpack(dp.fat, 2) == FAT12_FREE, "release single cluster: freed");
        CHECK(fb.fat_dirty != 0, "release: sets FAT dirty flag");
    }

    /* Release chain 2→3→4→EOF: all three become FREE */
    {
        memset(fb.data, 0, sizeof fb.data);
        chain_make(&dp, 2, 4);
        fb.fat_dirty = 0;
        fat_release(NULL, &dp, 2);
        CHECK(fat_unpack(dp.fat, 2) == FAT12_FREE, "release chain: cluster 2 freed");
        CHECK(fat_unpack(dp.fat, 3) == FAT12_FREE, "release chain: cluster 3 freed");
        CHECK(fat_unpack(dp.fat, 4) == FAT12_FREE, "release chain: cluster 4 freed");
        CHECK(fb.fat_dirty != 0, "release chain: sets FAT dirty flag");
    }

    /* fat_relblks(bx, EOF): truncation — bx gets EOF mark, its successors freed.
     * Chain 2→3→4→5→EOF; call relblks(3, 0xFFF) → cluster 3=EOF, 4=FREE, 5=FREE */
    {
        memset(fb.data, 0, sizeof fb.data);
        chain_make(&dp, 2, 5);
        fb.fat_dirty = 0;
        fat_relblks(NULL, &dp, 3, FAT12_EOF);
        CHECK(fat12_is_eof(fat_unpack(dp.fat, 3)), "relblks EOF: bx marked as new EOF");
        CHECK(fat_unpack(dp.fat, 4) == FAT12_FREE, "relblks EOF: successor freed");
        CHECK(fat_unpack(dp.fat, 5) == FAT12_FREE, "relblks EOF: tail freed");
        /* Predecessor link (cluster 2 → 3) is untouched */
        CHECK(fat_unpack(dp.fat, 2) == 3, "relblks EOF: predecessor not modified");
        CHECK(fb.fat_dirty != 0, "relblks: sets FAT dirty flag");
    }

    /* fat_relblks(bx, FREE): same as fat_release (first entry also freed) */
    {
        memset(fb.data, 0, sizeof fb.data);
        chain_make(&dp, 5, 7);
        fat_relblks(NULL, &dp, 5, FAT12_FREE);
        CHECK(fat_unpack(dp.fat, 5) == FAT12_FREE, "relblks FREE: first cluster freed");
        CHECK(fat_unpack(dp.fat, 6) == FAT12_FREE, "relblks FREE: middle freed");
        CHECK(fat_unpack(dp.fat, 7) == FAT12_FREE, "relblks FREE: last freed");
    }
}

/* ------------------------------------------------------------------ */
/* fat_allocate                                                          */
/* ------------------------------------------------------------------ */

static void test_allocate(void)
{
    printf("\n--- fat_allocate ---\n");
    fat_buf_t fb;
    dpb_t     dp;
    dpb_init(&fb, &dp, 10, 0, 5);  /* small disk: valid clusters 2..10 */

    /* Allocate 1 cluster for a brand-new file (tail=0) */
    {
        memset(fb.data, 0, sizeof fb.data);
        fb.fat_dirty = 0;
        fcb_t f = fcb_make(0);
        uint16_t r = fat_allocate(NULL, &dp, &f, 0, 0, 1);
        CHECK(r >= 2 && r <= 10, "alloc 1: returns valid cluster number");
        CHECK(fat12_is_eof(fat_unpack(dp.fat, r)), "alloc 1: cluster marked EOF");
        CHECK(f.firclus == r, "alloc 1: fcb->firclus set to first cluster");
        CHECK(fb.fat_dirty != 0, "alloc 1: marks FAT dirty");
    }

    /* Allocate 3 clusters: must form a proper chain ending at EOF */
    {
        memset(fb.data, 0, sizeof fb.data);
        fcb_t f = fcb_make(0);
        uint16_t r = fat_allocate(NULL, &dp, &f, 0, 0, 3);
        CHECK(r != 0, "alloc 3: non-zero return");
        CHECK(f.firclus == r, "alloc 3: fcb->firclus == first allocated");
        uint16_t c2 = fat_unpack(dp.fat, r);
        uint16_t c3 = fat_unpack(dp.fat, c2);
        CHECK(!fat12_is_eof(c2) && c2 != FAT12_FREE,
              "alloc 3: first cluster links forward");
        CHECK(!fat12_is_eof(c3) && c3 != FAT12_FREE,
              "alloc 3: second cluster links forward");
        CHECK(fat12_is_eof(fat_unpack(dp.fat, c3)),
              "alloc 3: third cluster is EOF");
    }

    /* Extend an existing chain: tail=2 already in use */
    {
        memset(fb.data, 0, sizeof fb.data);
        fat_pack(dp.fat, 2, FAT12_EOF);   /* existing single-cluster file */
        fcb_t f = fcb_make(2);
        uint16_t r = fat_allocate(NULL, &dp, &f, 2, 0, 1);
        CHECK(r != 0, "extend: non-zero return");
        CHECK(fat_unpack(dp.fat, 2) == r, "extend: old tail now links to new cluster");
        CHECK(fat12_is_eof(fat_unpack(dp.fat, r)), "extend: new cluster is EOF");
        CHECK(f.firclus == 2, "extend: fcb->firclus unchanged");
    }

    /* Allocate 0: returns tail unchanged, no modification */
    {
        memset(fb.data, 0, sizeof fb.data);
        fat_pack(dp.fat, 2, FAT12_EOF);
        fb.fat_dirty = 0;
        fcb_t f = fcb_make(2);
        uint16_t r = fat_allocate(NULL, &dp, &f, 2, 0, 0);
        CHECK(r == 2, "alloc 0: returns tail");
        CHECK(fb.fat_dirty == 0, "alloc 0: FAT not dirtied");
    }

    /* Disk full: all clusters occupied → returns 0 */
    {
        memset(fb.data, 0, sizeof fb.data);
        for (uint16_t c = 2; c <= 10; c++)
            fat_pack(dp.fat, c, FAT12_EOF);
        fcb_t f = fcb_make(0);
        uint16_t r = fat_allocate(NULL, &dp, &f, 0, 0, 1);
        CHECK(r == 0, "disk full: returns 0");
    }

    /* Request more clusters than available (3 free, need 5) → returns 0 */
    {
        memset(fb.data, 0, sizeof fb.data);
        for (uint16_t c = 5; c <= 10; c++)
            fat_pack(dp.fat, c, FAT12_EOF);
        fcb_t f = fcb_make(0);
        uint16_t r = fat_allocate(NULL, &dp, &f, 0, 0, 5);
        CHECK(r == 0, "partial disk: returns 0 when not enough clusters");
    }

    /* Allocate from fragmented disk: free clusters scattered */
    {
        memset(fb.data, 0, sizeof fb.data);
        /* Occupy clusters 3, 5, 7 — free are 2, 4, 6, 8, 9, 10 */
        fat_pack(dp.fat, 3, FAT12_EOF);
        fat_pack(dp.fat, 5, FAT12_EOF);
        fat_pack(dp.fat, 7, FAT12_EOF);
        fcb_t f = fcb_make(0);
        uint16_t r = fat_allocate(NULL, &dp, &f, 0, 0, 3);
        CHECK(r != 0, "fragmented disk: allocates 3 clusters");
        /* Walk the chain: should find exactly 3 linked clusters */
        uint16_t n1 = fat_unpack(dp.fat, r);
        uint16_t n2 = fat_unpack(dp.fat, n1);
        CHECK(!fat12_is_eof(n1) && n1 != FAT12_FREE,
              "fragmented: chain link 1 valid");
        CHECK(fat12_is_eof(fat_unpack(dp.fat, n2)),
              "fragmented: chain ends at EOF after 3 clusters");
        /* Allocated clusters should not overlap with occupied ones */
        CHECK(r != 3 && r != 5 && r != 7, "fragmented: first alloc not occupied");
        CHECK(n1 != 3 && n1 != 5 && n1 != 7, "fragmented: second alloc not occupied");
        CHECK(n2 != 3 && n2 != 5 && n2 != 7, "fragmented: third alloc not occupied");
    }
}

/* ------------------------------------------------------------------ */
/* disk_figrec                                                           */
/* ------------------------------------------------------------------ */

static void test_figrec(void)
{
    printf("\n--- disk_figrec ---\n");
    dpb_t dp;
    memset(&dp, 0, sizeof dp);

    /* 1 sector/cluster (clusmsk=0): cluster 2 maps to firrec */
    dp.firrec  = 5;
    dp.clusmsk = 0;
    CHECK(disk_figrec(&dp, 2, 0) == 5, "1spc: cluster 2 sec 0 -> firrec");
    CHECK(disk_figrec(&dp, 3, 0) == 6, "1spc: cluster 3 sec 0 -> firrec+1");
    CHECK(disk_figrec(&dp, 4, 0) == 7, "1spc: cluster 4 sec 0 -> firrec+2");
    CHECK(disk_figrec(&dp, 10, 0) == 13, "1spc: cluster 10 sec 0 -> firrec+8");

    /* 2 sectors/cluster (clusmsk=1) */
    dp.clusmsk = 1;
    CHECK(disk_figrec(&dp, 2, 0) == 5, "2spc: cluster 2 sec 0");
    CHECK(disk_figrec(&dp, 2, 1) == 6, "2spc: cluster 2 sec 1");
    CHECK(disk_figrec(&dp, 3, 0) == 7, "2spc: cluster 3 sec 0");
    CHECK(disk_figrec(&dp, 3, 1) == 8, "2spc: cluster 3 sec 1");
    CHECK(disk_figrec(&dp, 4, 0) == 9, "2spc: cluster 4 sec 0");

    /* 4 sectors/cluster (clusmsk=3) */
    dp.firrec  = 10;
    dp.clusmsk = 3;
    CHECK(disk_figrec(&dp, 2, 0) == 10, "4spc: cluster 2 sec 0");
    CHECK(disk_figrec(&dp, 2, 3) == 13, "4spc: cluster 2 sec 3");
    CHECK(disk_figrec(&dp, 3, 0) == 14, "4spc: cluster 3 sec 0");
    CHECK(disk_figrec(&dp, 3, 3) == 17, "4spc: cluster 3 sec 3");

    /* firrec=0 edge case (though unlikely in practice) */
    dp.firrec  = 0;
    dp.clusmsk = 0;
    CHECK(disk_figrec(&dp, 2, 0) == 0, "firrec=0: cluster 2 sec 0 -> 0");
    CHECK(disk_figrec(&dp, 3, 0) == 1, "firrec=0: cluster 3 sec 0 -> 1");
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== FAT12 unit tests ===\n");

    test_predicates();
    test_pack_unpack();
    test_fndclus();
    test_geteof();
    test_release();
    test_allocate();
    test_figrec();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
