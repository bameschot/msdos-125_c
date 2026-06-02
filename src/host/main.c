#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/kernel.h"
#include "../kernel/disk.h"
#include "../command/command.h"
#include "bios_host.h"

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s <disk.img> [<drive_b.img> ...]   -- run MS-DOS 1.25\n"
        "  %s --format <disk.img> [--720|--360|--180]  -- create blank disk\n"
        "\n"
        "Options:\n"
        "  --format   Create a blank FAT12 image (default: 720 KB)\n"
        "  --720      720 KB   (80 tracks, 2 heads, 9 sec/trk)\n"
        "  --360      360 KB   (40 tracks, 2 heads, 9 sec/trk)\n"
        "  --180      180 KB   (40 tracks, 1 head,  9 sec/trk)\n"
        "\n"
        "Example:\n"
        "  %s --format disk.img --720\n"
        "  %s disk.img\n",
        argv0, argv0, argv0, argv0);
}

static int do_format(const char *path, const char *size_flag)
{
    uint8_t  secs_per_track = 9;
    uint8_t  heads          = 2;
    uint16_t total_sectors  = 1440;   /* 720 KB default */
    uint8_t  secs_per_clus  = 2;

    if (size_flag && strcmp(size_flag, "--360") == 0) {
        total_sectors = 720; secs_per_track = 9; heads = 2; secs_per_clus = 2;
    } else if (size_flag && strcmp(size_flag, "--180") == 0) {
        total_sectors = 360; secs_per_track = 9; heads = 1; secs_per_clus = 1;
    } else if (!size_flag || strcmp(size_flag, "--720") == 0) {
        total_sectors = 1440; secs_per_track = 9; heads = 2; secs_per_clus = 2;
    }

    return host_bios_format(path, secs_per_track, heads,
                            total_sectors, 512, secs_per_clus);
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    /* --format mode */
    if (strcmp(argv[1], "--format") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        const char *path = argv[2];
        const char *size = (argc >= 4) ? argv[3] : "--720";
        return do_format(path, size) == 0 ? 0 : 1;
    }

    /* Normal run mode: collect disk images */
    int         n_drives   = argc - 1;
    const char **img_paths = (const char**)&argv[1];

    host_bios_t hb;
    if (host_bios_init(&hb, img_paths, n_drives) != 0) return 1;

    dos_t dos;
    if (dos_init(&dos, &hb.base) != 0) {
        host_bios_shutdown(&hb);
        return 1;
    }

    /* Set a basic fatal error handler that prints a message and ignores */
    dos.fatal_handler = NULL;

    command_run(&dos);

    disk_reset(&dos);
    host_bios_shutdown(&hb);
    return 0;
}
