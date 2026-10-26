/*
- Companion app (runs as Luma's boot.3dsx).

 *
- Reads sdmc:/loader/tmp/tid.tmp ("<16 hex digits>:<sd|nand|game>\n"), deletes it,

- and launches that title.

 *
- Two launch methods, chosen at runtime:

- HOME Menu is running  -> aptSetChainloader() and return; libctru's aptExit()

-      performs the application jump (this is the proven path when started from HOME).

-      Do NOT call APT_DoApplicationJump yourself.

-   2. HOME Menu is NOT running (Luma autoboot launches us in place of HOME, and HOME

-      then only loads on demand) -> NS_RebootToTitle(). Luma's homebrew environment

-      grants ns:s, and this needs no HOME Menu.

 *
- With no tid.tmp we behave like hbmenu under autoboot: stay open instead of exiting

- during boot (an early exit ended in a system error on the test console).

 *
- UNTESTED: method 2 has only been compile-checked against libctru.

 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define TID_FILE      "sdmc:/loader/tmp/tid.tmp"
#define LOCK_FILE     "sdmc:/loader/tmp/tmp.lock"
#define HOME_WAIT_MS  3000

/* Create the lock file before rebooting/launching */
static void createLockFile(void)
{
    FILE *f = fopen(LOCK_FILE, "w");
    if (f) {
        fclose(f);
    }
}

/* Strict parse of "0004000000123400:sd". Returns true on success. */
static bool parseTidFile(const char *s, u64 *tid, FS_MediaType *media)
{
    u64 v = 0;
    for (int i = 0; i < 16; i++) {
        char c = s[i];
        int d;
        if      (c >= '0' && c <= '9')      d = c - '0';
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
- status line once per second. */
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

static void launchViaNs(u64 tid, FS_MediaType media)
{
    printf("HOME Menu is not running; rebooting into the title via NS...\n");

    Result rc = nsInit();
    if (R_SUCCEEDED(rc)) {
        createLockFile();

        rc = NS_RebootToTitle((u8)media, tid, defaultAppMemType());
        if (R_SUCCEEDED(rc)) {
            /* The system should reboot any moment now. */
            for (int i = 0; i < 100; i++) svcSleepThread(100 * 1000 * 1000LL);
        }
        nsExit();
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "NS reboot failed (result %08X).", (unsigned)rc);
    failAndExit(msg);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    char buf[64] = {0};
    FILE *f = fopen(TID_FILE, "r");

    if (!f) {
        /* Nothing to launch. Under autoboot HOME may not be up; stay open like hbmenu
- does (press HOME for the HOME Menu), rather than exiting during boot. */
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
        createLockFile();
        /* Let libctru do the jump when we return. */
        aptSetChainloader(tid, (u8)media);
    } else {
        launchViaNs(tid, media);
    }

    gfxExit();
    return 0;
}