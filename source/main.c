/*
 * Companion app (runs as Luma's boot.3dsx).
 *
 * Reads sdmc:/loader/tmp/tid.tmp ("<16 hex digits>:<sd|nand|game>\n"), deletes it,
 * and launches that title.
 *
 * Two launch methods, chosen at runtime:
 *   1. HOME Menu is running  -> aptSetChainloader() and return; libctru's aptExit()
 *      performs the application jump (this is the proven path when started from HOME).
 *      Do NOT call APT_DoApplicationJump yourself.
 *   2. HOME Menu is NOT running (Luma autoboot launches us in place of HOME, and HOME
 *      then only loads on demand) -> NS_RebootToTitle(). Luma's homebrew environment
 *      grants ns:s, and this needs no HOME Menu.
 *
 * With no tid.tmp we behave like hbmenu under autoboot: stay open instead of exiting
 * during boot (an early exit ended in a system error on the test console).
 *
 * Scan mode: if sdmc:/loader/tmp/scan.req exists (written by the FIRM's "rescan" key), or
 * Y is held at start, enumerate installed applications, read their names from the SMDH
 * icon data, and write sdmc:/loader/titles.txt (the list the FIRM's menu shows). Then
 * reboot so the FIRM comes back up with the new list.
 *
 * UNTESTED: only compile-checked against libctru (the NS-reboot path and the whole scan
 * mode have never run on a console).
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TID_FILE      "sdmc:/loader/tmp/tid.tmp"
/* Must match LOCK_FILE in the FIRM's launcher_config.h. Written right before the NS
 * reboot so the FIRM, which the reboot passes back through, skips its menu. */
#define LOCK_FILE     "sdmc:/loader/tmp/tmp.lock"
/* Must match TITLES_FILE / SCAN_REQ_FILE in the FIRM's launcher_config.h. */
#define TITLES_FILE   "sdmc:/loader/titles.txt"
#define SCAN_REQ_FILE "sdmc:/loader/tmp/scan.req"

#define MAX_TITLES    256            /* must be <= TITLES_MAX in the FIRM's titles.h */
#define NAME_MAX_LEN  47             /* the FIRM keeps TITLE_NAME_MAX-1 = 47 characters */
#define SCAN_NAND     0              /* 1 = also list applications installed to NAND */
/* The Homebrew Launcher placeholder title Luma swaps this app into; never list it. */
#define SKIP_TID      0x000400000D921E00ULL
#define HOME_WAIT_MS  3000

/* Strict parse of "0004000000123400:sd". Returns true on success. */
static bool parseTidFile(const char *s, u64 *tid, FS_MediaType *media)
{
    u64 v = 0;
    for (int i = 0; i < 16; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | (u64)d;
    }
    if (s[16] != ':') return false;

    const char *m = s + 17;
    size_t n = 0;
    while (m[n] && m[n] != '\n' && m[n] != '\r' && m[n] != ' ') n++;

    if      (n == 2 && !strncmp(m, "sd", 2))   *media = MEDIATYPE_SD;
    else if (n == 4 && !strncmp(m, "nand", 4)) *media = MEDIATYPE_NAND;
    else if (n == 4 && !strncmp(m, "game", 4)) *media = MEDIATYPE_GAME_CARD;
    else return false;

    *tid = v;
    return true;
}

/* Wait until the user presses START. HOME presses are handled by aptMainLoop(). */
static void waitForStart(void)
{
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}

static void failAndExit(const char *msg)
{
    printf("\x1b[31m%s\x1b[0m\n\nPress START to exit.\n", msg);
    waitForStart();
    aptClearChainloader();   /* make sure exiting goes to HOME, not back to us */
}

/* Polls (10x per second) whether HOME Menu is registered with APT; prints a
 * status line once per second. */
static bool waitForHome(int timeoutMs)
{
    for (int waited = 0; waited <= timeoutMs; waited += 100) {
        bool registered = false;
        if (R_SUCCEEDED(APT_IsRegistered(aptGetMenuAppID(), &registered)) && registered)
            return true;
        if (waited % 1000 == 0)
            printf("Waiting for HOME Menu... %ds\n", waited / 1000);
        if (!aptMainLoop()) return false;
        svcSleepThread(100 * 1000 * 1000LL);
    }
    return false;
}

/* Same defaults Luma's own autoboot uses: 64 MB on Old 3DS, 124 MB on New 3DS. */
static u8 defaultAppMemType(void)
{
    bool isNew = false;
    if (R_SUCCEEDED(APT_CheckNew3DS(&isNew)) && isNew) return 6;
    return 0;
}


/* ---------------------------- title scan ---------------------------- */

typedef struct { u64 tid; FS_MediaType media; char name[NAME_MAX_LEN + 1]; } Entry;
static Entry entries[MAX_TITLES];
static int   entryCount;

/* SMDH: 8-byte header, then 16 language entries of 0x200 bytes (short 0x80, long 0x100,
 * publisher 0x80, all UTF-16LE). Index 1 is English. */
typedef struct { u16 shortDesc[0x40]; u16 longDesc[0x80]; u16 publisher[0x40]; } SmdhTitle;
typedef struct { char magic[4]; u16 version; u16 reserved; SmdhTitle titles[16]; } SmdhHeader;

/* The FIRM's font is ASCII only: fold Latin-1 letters, map a few typographic marks, '?' the rest. */
static char foldChar(u16 c)
{
    static const char latin1[] =   /* U+00C0..U+00FF */
        "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (c >= 0x20 && c < 0x7F) return (char)c;
    if (c >= 0xC0 && c <= 0xFF) return latin1[c - 0xC0];
    if (c == 0x2018 || c == 0x2019) return '\'';
    if (c == 0x201C || c == 0x201D) return '"';
    if (c == 0x2013 || c == 0x2014) return '-';
    if (c == 0x0A || c == 0x0D || c == 0x09 || c == 0xA0) return ' ';
    return '?';
}

static void utf16ToName(const u16 *src, size_t maxChars, char *dst)
{
    size_t n = 0;
    for (size_t i = 0; i < maxChars && src[i] && n < NAME_MAX_LEN; i++) {
        char c = foldChar(src[i]);
        if (c == ' ' && (n == 0 || dst[n - 1] == ' ')) continue;   /* trim / collapse */
        dst[n++] = c;
    }
    while (n > 0 && dst[n - 1] == ' ') n--;
    dst[n] = 0;
}

/* Diagnostics: every icon read that failed (first MAX_FAIL_LOG are logged).
 * step: 1 open archive, 2 open file, 3 read, 4 bad SMDH header, 5 no non-empty name */
#define MAX_FAIL_LOG 64
static struct { int step; Result rc; u64 tid; FS_MediaType media; } failLog[MAX_FAIL_LOG];
static int smdhFailures;

static void noteFail(int step, Result rc, u64 tid, FS_MediaType media)
{
    if (smdhFailures < MAX_FAIL_LOG) {
        failLog[smdhFailures].step = step;
        failLog[smdhFailures].rc = rc;
        failLog[smdhFailures].tid = tid;
        failLog[smdhFailures].media = media;
    }
    smdhFailures++;
}

/* Opens <title>'s ExeFS:/icon. Same sequence FBI uses: open the title's "save data and
 * content" archive with {idLow, idHigh, media, 0}, then open the file by binary path. */
static Result openIcon(u64 tid, FS_MediaType media, Handle *outFile, FS_Archive *outArchive, bool *haveArchive, int *step)
{
    u32 archiveData[4] = { (u32)tid, (u32)(tid >> 32), (u32)media, 0 };
    static const u32 filePathData[5] = { 0x00000000, 0x00000000, 0x00000002, 0x6E6F6369 /* "icon" */, 0x00000000 };
    FS_Path archivePath = { PATH_BINARY, sizeof(archiveData), archiveData };
    FS_Path filePath    = { PATH_BINARY, sizeof(filePathData), filePathData };

    *haveArchive = false;
    *step = 1;
    Result rc = FSUSER_OpenArchive(outArchive, ARCHIVE_SAVEDATA_AND_CONTENT, archivePath);
    if (R_SUCCEEDED(rc)) {
        *haveArchive = true;
        *step = 2;
        rc = FSUSER_OpenFile(outFile, *outArchive, filePath, FS_OPEN_READ, 0);
        if (R_SUCCEEDED(rc)) return rc;
    }

    /* Second attempt: the one-call variant. */
    Result rc2 = FSUSER_OpenFileDirectly(outFile, ARCHIVE_SAVEDATA_AND_CONTENT, archivePath, filePath, FS_OPEN_READ, 0);
    return R_SUCCEEDED(rc2) ? rc2 : rc;
}

static bool readSmdhName(u64 tid, FS_MediaType media, char *out)
{
    static SmdhHeader smdh;                       /* ~8 KB: keep it off the stack */
    Handle h = 0;
    FS_Archive archive = 0;
    bool haveArchive = false;
    int step = 1;

    Result rc = openIcon(tid, media, &h, &archive, &haveArchive, &step);
    if (R_FAILED(rc)) {
        if (haveArchive) FSUSER_CloseArchive(archive);
        noteFail(step, rc, tid, media);
        return false;
    }

    u32 got = 0;
    rc = FSFILE_Read(h, &got, 0, &smdh, sizeof(smdh));
    FSFILE_Close(h);
    if (haveArchive) FSUSER_CloseArchive(archive);

    if (R_FAILED(rc)) { noteFail(3, rc, tid, media); return false; }
    if (got < sizeof(smdh.magic) + 4 + 2 * sizeof(SmdhTitle) || memcmp(smdh.magic, "SMDH", 4) != 0) {
        noteFail(4, (Result)got, tid, media);
        return false;
    }

    static const int order[] = { 1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        utf16ToName(smdh.titles[order[i]].shortDesc, 0x40, out);
        if (out[0]) return true;
    }
    noteFail(5, 0, tid, media);
    return false;
}


/* ---------------- names from the previous titles.txt ----------------
 * If an icon can't be read, keep whatever name the existing list already has for that title
 * (including names you edited by hand), instead of overwriting it with a product code. */

static Entry oldEntries[MAX_TITLES];
static int   oldCount;

static int hexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Same line format as the FIRM's parser: <16 hex>:<sd|nand|game>[:<name>] */
static int parseOldTitles(const char *text, size_t len, Entry *out, int max)
{
    int n = 0;
    size_t i = 0;
    while (i < len && n < max) {
        size_t start = i;
        while (i < len && text[i] != '\n') i++;
        size_t end = i;
        if (i < len) i++;
        if (end > start && text[end - 1] == '\r') end--;

        const char *l = text + start;
        size_t ll = end - start;
        if (ll < 19 || l[0] == '#') continue;

        u64 tid = 0;
        bool ok = true;
        for (int k = 0; k < 16; k++) {
            int d = hexDigit(l[k]);
            if (d < 0) { ok = false; break; }
            tid = (tid << 4) | (u64)d;
        }
        if (!ok || l[16] != ':') continue;

        size_t p = 17;
        FS_MediaType media;
        if      (ll - p >= 2 && !strncmp(l + p, "sd", 2))   { media = MEDIATYPE_SD;        p += 2; }
        else if (ll - p >= 4 && !strncmp(l + p, "nand", 4)) { media = MEDIATYPE_NAND;      p += 4; }
        else if (ll - p >= 4 && !strncmp(l + p, "game", 4)) { media = MEDIATYPE_GAME_CARD; p += 4; }
        else continue;

        Entry *e = &out[n];
        e->tid = tid;
        e->media = media;
        e->name[0] = 0;
        if (p < ll) {
            if (l[p] != ':') continue;
            p++;
            while (p < ll && l[p] == ' ') p++;
            size_t nl = 0;
            while (p < ll && nl < NAME_MAX_LEN) {
                char c = l[p++];
                e->name[nl++] = (c >= 0x20 && c < 0x7F) ? c : '?';
            }
            while (nl > 0 && e->name[nl - 1] == ' ') nl--;
            e->name[nl] = 0;
        }
        n++;
    }
    return n;
}

static void loadOldTitles(void)
{
    static char text[24 * 1024];
    oldCount = 0;
    FILE *f = fopen(TITLES_FILE, "r");
    if (!f) return;
    size_t got = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    oldCount = parseOldTitles(text, got, oldEntries, MAX_TITLES);
}

/* A name worth keeping: present and not just the title ID. */
static const char *oldNameFor(u64 tid, FS_MediaType media)
{
    for (int i = 0; i < oldCount; i++) {
        if (oldEntries[i].tid != tid || oldEntries[i].media != media) continue;
        const char *n = oldEntries[i].name;
        if (!n[0]) return NULL;
        bool allHex = strlen(n) == 16;
        for (int k = 0; allHex && k < 16; k++) allHex = hexDigit(n[k]) >= 0;
        return allHex ? NULL : n;
    }
    return NULL;
}

static int namedFromSmdh, namedFromCode, namedFromOld;

/* Fallback when the icon data can't be read: the cartridge-style product code
 * (e.g. "CTR-P-AQEE") is more useful on screen than a raw title ID. */
static bool productCodeName(FS_MediaType media, u64 tid, char *out)
{
    char pc[32] = {0};
    if (R_FAILED(AM_GetTitleProductCode(media, tid, pc))) return false;
    size_t n = 0;
    for (size_t i = 0; i < sizeof(pc) - 1 && pc[i] && n < NAME_MAX_LEN; i++)
        out[n++] = (pc[i] >= 0x20 && pc[i] < 0x7F) ? pc[i] : '?';
    out[n] = 0;
    return n > 0;
}

static void scanMedia(FS_MediaType media)
{
    u32 count = 0;
    if (R_FAILED(AM_GetTitleCount(media, &count)) || count == 0) return;   /* e.g. no card */

    u64 *ids = malloc((size_t)count * sizeof(u64));
    if (!ids) return;

    u32 read = 0;
    if (R_SUCCEEDED(AM_GetTitleList(&read, media, count, ids))) {
        for (u32 i = 0; i < read && entryCount < MAX_TITLES; i++) {
            if ((ids[i] >> 32) != 0x00040000ULL) continue;   /* applications only */
            if (ids[i] == SKIP_TID) continue;

            Entry *e = &entries[entryCount];
            e->tid = ids[i];
            e->media = media;
            e->name[0] = 0;
            if (readSmdhName(ids[i], media, e->name)) {
                namedFromSmdh++;
            } else {
                char code[NAME_MAX_LEN + 1] = {0};
                productCodeName(media, ids[i], code);
                const char *old = oldNameFor(ids[i], media);
                if (old && strcmp(old, code) != 0) {      /* a real or hand-edited name: keep it */
                    strncpy(e->name, old, NAME_MAX_LEN);
                    e->name[NAME_MAX_LEN] = 0;
                    namedFromOld++;
                } else if (code[0]) {
                    strcpy(e->name, code);
                    namedFromCode++;
                }
            }
            entryCount++;
            printf("\r%d titles...", entryCount);
        }
    }
    free(ids);
}

static int cmpEntries(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    /* titles without a name sort last */
    if (!x->name[0] != !y->name[0]) return x->name[0] ? -1 : 1;
    for (size_t i = 0;; i++) {
        int cx = x->name[i], cy = y->name[i];
        if (cx >= 'A' && cx <= 'Z') cx += 32;
        if (cy >= 'A' && cy <= 'Z') cy += 32;
        if (cx != cy) return cx - cy;
        if (!cx) return x->tid < y->tid ? -1 : x->tid > y->tid;
    }
}

static const char *mediaStr(FS_MediaType m)
{
    return m == MEDIATYPE_SD ? "sd" : m == MEDIATYPE_GAME_CARD ? "game" : "nand";
}


static void writeScanLog(void)
{
    FILE *f = fopen("sdmc:/loader/scan.log", "w");
    if (!f) return;
    fprintf(f, "# companion scan log\n");
    fprintf(f, "titles: %d | names from icon data: %d, kept from old list: %d, product code only: %d, unnamed: %d\n",
            entryCount, namedFromSmdh, namedFromOld, namedFromCode,
            entryCount - namedFromSmdh - namedFromOld - namedFromCode);
    fprintf(f, "icon read failures: %d\n", smdhFailures);
    fprintf(f, "step 1=open archive 2=open file 3=read 4=bad header 5=no name\n\n");
    for (int i = 0; i < smdhFailures && i < MAX_FAIL_LOG; i++) {
        Result rc = failLog[i].rc;
        fprintf(f, "step=%d result=%08X (level %d, summary %d, module %d, description %d) title=%08X%08X media=%s\n",
                failLog[i].step, (unsigned)rc, (int)R_LEVEL(rc), (int)R_SUMMARY(rc), (int)R_MODULE(rc),
                (int)R_DESCRIPTION(rc), (unsigned)(failLog[i].tid >> 32), (unsigned)failLog[i].tid,
                mediaStr(failLog[i].media));
    }
    fclose(f);
}

static bool writeTitlesFile(void)
{
    static char text[24 * 1024];
    size_t n = 0;

    n += (size_t)snprintf(text + n, sizeof(text) - n, "# generated by the companion's scan\n");
    for (int i = 0; i < entryCount; i++) {
        char line[128];
        int l = snprintf(line, sizeof(line), "%08X%08X:%s:%s\n", (unsigned)(entries[i].tid >> 32),
                         (unsigned)entries[i].tid, mediaStr(entries[i].media), entries[i].name);
        if (l <= 0 || n + (size_t)l >= sizeof(text)) break;
        memcpy(text + n, line, (size_t)l);
        n += (size_t)l;
    }

    mkdir("sdmc:/loader", 0777);                 /* ignore "already exists" */
    const char *tmp = TITLES_FILE ".new";
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = fwrite(text, 1, n, f) == n;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp); return false; }
    remove(TITLES_FILE);                         /* rename won't overwrite */
    return rename(tmp, TITLES_FILE) == 0;
}

static void rebootSystem(void)
{
    /* Both are best-effort and untested: ptm:sysm (clean reboot) first, then apt. */
    if (R_SUCCEEDED(ptmSysmInit())) {
        Result rc = PTMSYSM_RebootAsync(5000000000ULL);
        if (R_SUCCEEDED(rc))
            for (int i = 0; i < 100; i++) svcSleepThread(100 * 1000 * 1000LL);
        ptmSysmExit();
    }
    if (R_SUCCEEDED(APT_HardwareResetAsync()))
        for (int i = 0; i < 100; i++) svcSleepThread(100 * 1000 * 1000LL);
}

static void runScan(void)
{
    printf("Scanning installed titles...\n");

    Result rc = amInit();
    if (R_FAILED(rc)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "amInit failed (%08X).", (unsigned)rc);
        failAndExit(msg);
        return;
    }

    loadOldTitles();
    scanMedia(MEDIATYPE_SD);
    scanMedia(MEDIATYPE_GAME_CARD);
#if SCAN_NAND
    scanMedia(MEDIATYPE_NAND);
#endif
    amExit();

    qsort(entries, (size_t)entryCount, sizeof(Entry), cmpEntries);

    if (!writeTitlesFile()) {
        failAndExit("Could not write " TITLES_FILE);
        return;
    }

    writeScanLog();

    printf("\n\nWrote %d titles to titles.txt.\n", entryCount);
    printf("Names: %d icon data, %d kept, %d product code,\n       %d unnamed.\n",
           namedFromSmdh, namedFromOld, namedFromCode,
           entryCount - namedFromSmdh - namedFromOld - namedFromCode);

    if (smdhFailures) {
        Result rc = failLog[0].rc;
        printf("\x1b[33m\n%d icon reads failed. First one:\n step %d  result %08X\n"
               " lvl %d sum %d mod %d desc %d\n title %08X%08X\n"
               "(1 archive, 2 file, 3 read, 4 header, 5 name)\n"
               "Full list: sdmc:/loader/scan.log\x1b[0m\n",
               smdhFailures, failLog[0].step, (unsigned)rc,
               (int)R_LEVEL(rc), (int)R_SUMMARY(rc), (int)R_MODULE(rc), (int)R_DESCRIPTION(rc),
               (unsigned)(failLog[0].tid >> 32), (unsigned)failLog[0].tid);
        printf("\nPress START to reboot.\n");
        waitForStart();
    } else {
        printf("Rebooting...\n");
    }

    rebootSystem();

    failAndExit("Done, but could not reboot automatically.");
}

static bool writeLock(void)
{
    FILE *f = fopen(LOCK_FILE, "w");
    if (!f) return false;
    bool ok = fputs("1\n", f) >= 0;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

static void launchViaNs(u64 tid, FS_MediaType media)
{
    printf("HOME Menu is not running; rebooting into the title via NS...\n");

    /* If the lock can't be written we still reboot; you'll just see the FIRM's menu once. */
    if (!writeLock())
        printf("Warning: could not write %s\n", LOCK_FILE);

    Result rc = nsInit();
    if (R_SUCCEEDED(rc)) {
        rc = NS_RebootToTitle((u8)media, tid, defaultAppMemType());
        if (R_SUCCEEDED(rc)) {
            /* The system should reboot any moment now. */
            for (int i = 0; i < 100; i++) svcSleepThread(100 * 1000 * 1000LL);
        }
        nsExit();
    }

    /* We're still running, so no reboot is coming: don't leave a lock behind. */
    remove(LOCK_FILE);

    char msg[64];
    snprintf(msg, sizeof(msg), "NS reboot failed (result %08X).", (unsigned)rc);
    failAndExit(msg);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    /* Scan request: from the FIRM's rescan key (scan.req) or by holding Y at start. */
    bool scan = false;
    {
        struct stat st;
        if (stat(SCAN_REQ_FILE, &st) == 0) { scan = true; remove(SCAN_REQ_FILE); }
        hidScanInput();
        if (hidKeysHeld() & KEY_Y) scan = true;
    }
    if (scan) {
        runScan();
        gfxExit();
        return 0;
    }

    char buf[64] = {0};
    FILE *f = fopen(TID_FILE, "r");

    if (!f) {
        /* Nothing to launch. Under autoboot HOME may not be up; stay open like hbmenu
         * does (press HOME for the HOME Menu), rather than exiting during boot. */
        aptClearChainloader();
        if (!waitForHome(HOME_WAIT_MS)) {
            printf("\nNothing to launch.\nPress HOME for the HOME Menu.\n");
            waitForStart();
        }
        gfxExit();
        return 0;
    }

    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    remove(TID_FILE);        /* always delete, so a bad file can't loop forever */
    buf[got] = 0;

    u64 tid = 0;
    FS_MediaType media = MEDIATYPE_SD;
    if (!parseTidFile(buf, &tid, &media)) {
        failAndExit("tid.tmp is malformed.");
        gfxExit();
        return 0;
    }

    printf("Launching %08X%08X (media %d)...\n", (unsigned)(tid >> 32), (unsigned)tid, (int)media);

    if (waitForHome(HOME_WAIT_MS)) {
        /* Let libctru do the jump when we return. */
        aptSetChainloader(tid, (u8)media);
    } else {
        launchViaNs(tid, media);
    }

    gfxExit();
    return 0;
}