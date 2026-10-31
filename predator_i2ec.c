// SPDX-License-Identifier: GPL-2.0-only
/*
 * predator_i2ec.c - Acer Predator Helios Neo 16 (PHN16-71) EC / Mode-LED tool
 *
 * EC: ITE IT5570E, accessed read-only through the Super I/O depth-2 "I2EC"
 * bridge at ports 0x2E/0x2F. Mode changes go through the kernel's acer-wmi
 * platform_profile interface, so the BIOS/EC run their full mode-switch
 * routine (fan tables, power limits and the Mode-button LED).
 *
 * Nothing in this tool writes EC RAM/GPIO. Writing EC RAM directly does not
 * run the EC's mode-switch code (so the LED will not follow) and can leave
 * fans/power limits inconsistent. The only write path is the opt-in
 * --led-raw / --led-restore, limited to the SMBus channel-D host registers.
 *
 *   --probe            Show the current mode (cross-checked from 4 sources)
 *   --watch            Live monitor: print every mode change (button or software)
 *   --mode <name|n>    Switch mode via platform_profile (eco|quiet|balanced|performance|turbo)
 *   --cycle [sec]      Step through every mode (default 5 s each), then restore
 *   --sniff [sec]      Log EC SMBus channel D (LED controller bus) state changes
 *   --dump <from> <to> Hex-dump EC XRAM range (hex addresses, read-only)
 *
 * Windows: port I/O goes through the inpoutx64 driver that PredatorSense
 * installs, and the platform profile is read/set through the same BIOS WMI
 * method acer-wmi uses (SetGamingMiscSetting 0x0B). Run as Administrator.
 *
 * Build: gcc -O2 -Wall -Wextra predator_i2ec.c -o predator_i2ec
 *   Windows (MinGW-w64): gcc -O2 -Wall -Wextra predator_i2ec.c -o predator_i2ec.exe -lole32 -loleaut32 -lwbemuuid
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <wbemidl.h>
#include <wchar.h>
#define usleep(us) Sleep((DWORD)((us) / 1000))
#define SUDO ""
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/io.h>
#include <unistd.h>
#define SUDO "sudo "
#endif

/* ---------- Port I/O (Windows) ---------- */

#ifdef _WIN32
/* inpoutx64.sys (Highresolution Enterprises) ships with PredatorSense. */
#define INPOUT_DEVICE          "\\\\.\\inpoutx64"
#define IOCTL_READ_PORT_UCHAR  CTL_CODE(40000, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_WRITE_PORT_UCHAR CTL_CODE(40000, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

static HANDLE io_dev = INVALID_HANDLE_VALUE;

static void io_fail(void)
{
    fprintf(stderr, "inpoutx64 port I/O failed (error %lu)\n", GetLastError());
    exit(2);
}

static void outb(uint8_t val, uint16_t port)
{
    uint8_t b[3] = { port & 0xFF, port >> 8, val };
    DWORD n;
    if (!DeviceIoControl(io_dev, IOCTL_WRITE_PORT_UCHAR, b, 3, NULL, 0, &n, NULL)) io_fail();
}

static uint8_t inb(uint16_t port)
{
    uint8_t b[2] = { port & 0xFF, port >> 8 };
    DWORD n;
    if (!DeviceIoControl(io_dev, IOCTL_READ_PORT_UCHAR, b, 2, b, 1, &n, NULL)) io_fail();
    return b[0];
}

/* HWiNFO, LibreHardwareMonitor etc. take this mutex around Super I/O
 * accesses, so their probing of 0x2E/0x2F can't land in the middle of ours. */
#define ISA_MUTEX "Global\\Access_ISABUS.HTP.Method"
static HANDLE isa_mutex;

static void bus_lock(void)
{
    if (!isa_mutex) return;
    DWORD w = WaitForSingleObject(isa_mutex, 5000);
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
        fprintf(stderr, "could not get the Super I/O mutex (another program holds it)\n");
        exit(2);
    }
}
static void bus_unlock(void) { if (isa_mutex) ReleaseMutex(isa_mutex); }
#else
static void bus_lock(void) {}
static void bus_unlock(void) {}
#endif

/* ---------- Super I/O depth-2 / I2EC access ---------- */

#define SIO_INDEX 0x2E
#define SIO_DATA  0x2F
#define SIO_D2ADR 0x2E
#define SIO_D2DAT 0x2F
#define D2_I2EC_ADDR_L 0x10
#define D2_I2EC_ADDR_H 0x11
#define D2_I2EC_DATA   0x12

static inline void sio_write(uint8_t reg, uint8_t val) { outb(reg, SIO_INDEX); outb(val, SIO_DATA); }
static inline uint8_t sio_read(uint8_t reg) { outb(reg, SIO_INDEX); return inb(SIO_DATA); }
static inline void d2_write(uint8_t r, uint8_t v) { sio_write(SIO_D2ADR, r); sio_write(SIO_D2DAT, v); }
static inline uint8_t d2_read(uint8_t r) { sio_write(SIO_D2ADR, r); return sio_read(SIO_D2DAT); }

static uint8_t ec_rd(uint16_t addr)
{
    bus_lock();
    d2_write(D2_I2EC_ADDR_H, addr >> 8);
    d2_write(D2_I2EC_ADDR_L, addr & 0xFF);
    uint8_t v = d2_read(D2_I2EC_DATA);
    bus_unlock();
    return v;
}

/* The ONLY write path: restricted to SMBus channel D host registers. */
static void ec_wr_smbd(uint16_t addr, uint8_t val)
{
    if (addr < 0x1C35 || addr > 0x1C3C) {
        fprintf(stderr, "BUG: refusing EC write outside SMBus-D (0x%04X)\n", addr);
        exit(2);
    }
    bus_lock();
    d2_write(D2_I2EC_ADDR_H, addr >> 8);
    d2_write(D2_I2EC_ADDR_L, addr & 0xFF);
    d2_write(D2_I2EC_DATA, val);
    bus_unlock();
}

/* ---------- Known EC XRAM locations (verified on BIOS V1.20) ---------- */

#define XR_CHIPID1      0x2000  /* 0x55 */
#define XR_CHIPID2      0x2001  /* 0x70 -> IT5570 */
#define XR_MODE_ACPI    0x0554  /* = ACPI EC offset 0x54, what acer-wmi reads */
#define XR_MODE_INT     0x016F  /* EC firmware's internal copy, same encoding */
#define XR_MODE_WMI     0x0AF4  /* last WMI SetGamingMiscSetting(0x0B) value */
#define XR_FAN_DUTY_A   0x1807  /* PWM DCR5 -> GPA5 (fan) */
#define XR_FAN_DUTY_B   0x1808  /* PWM DCR6 -> GPA6 (fan) */
/* SMBus channel D. Idle: polls a temp sensor at 0x4D. On a mode change the
 * EC talks to slave 0x67 (addr byte 0xCE); the last block byte it sends is
 * a 1..5 mode/LED code that stays latched in HOBDBD. */
#define XR_SMBD_BASE    0x1C35  /* HOSTAD */
#define XR_SMBD_HOSTA   0x1C35
#define XR_SMBD_HOCTL   0x1C36
#define XR_SMBD_HOCMD   0x1C37
#define XR_SMBD_TRASLA  0x1C38
#define XR_SMBD_D0      0x1C39
#define XR_SMBD_D1      0x1C3A
#define XR_SMBD_HOBDB   0x1C3B

#define LED_SLAVE_W     0xCE    /* 7-bit 0x67, write */
/* HOCTL bits (ITE): SRT=start, SMCD protocol in bits 4:2, KILL=abort */
#define HOCTL_SRT       0x40
#define HOCTL_KILL      0x02
#define SMCD_BYTEDATA   (0x2 << 2)
#define SMCD_WORDDATA   (0x3 << 2)
#define SMCD_BLOCK      (0x5 << 2)
/* HOSTA bits: 0 busy, 1 done, 2 dev err, 3 bus err, 4 failed, 7 byte-done */
#define HOSTA_BUSY      0x01
#define HOSTA_DONE      0x02
#define HOSTA_ERRMASK   0x1C
#define HOSTA_BDS       0x80

#ifdef _WIN32
#define PROFILE_SRC     "WMI GetGamingMiscSetting(0x0B)"
#define PROFILE_HINT    "run as Administrator?"
#else
#define PROFILE_SRC     "/sys/firmware/acpi/platform_profile"
#define PROFILE_HINT    "is acer_wmi loaded?"
#endif

struct mode_info {
    uint8_t ec;          /* value at EC 0x54 / XRAM 0x0554 and 0x016F */
    uint8_t wmi;         /* value at XRAM 0x0AF4 (WMI argument)        */
    uint8_t led_code;    /* byte sent to SMBus slave 0x67               */
    const char *name;    /* PredatorSense name                          */
    const char *sysfs;   /* platform_profile name                       */
    const char *color;   /* observed Mode-LED colour ("?" = unconfirmed)*/
};

static const struct mode_info modes[] = {
    { 0x04, 0x06, 1, "Eco",         "low-power",            "green" },
    { 0x01, 0x00, 2, "Quiet",       "quiet",                "white (yellowish)" },
    { 0x00, 0x01, 3, "Balanced",    "balanced",             "cyan" },
    { 0x02, 0x04, 4, "Performance", "balanced-performance", "purple" },
    { 0x03, 0x05, 5, "Turbo",       "performance",          "pink" },
    /* led_code 2 (Quiet) shows as plain white when sent by hand */
};
#define NMODES (sizeof(modes) / sizeof(modes[0]))

/* LED controller palette (codes observed by eye on the PHN16-71, full scan of 0-255
 * with windows/palette_scan.ps1). There are 128 entries: bit 7 is ignored, so code
 * 128+n shows the same colour as n. Most unnamed codes are shades of white. */
static const struct { uint8_t code; const char *name; } palette[] = {
    { 0, "off" },    { 1, "green" },  { 2, "white" },    { 3, "cyan" },
    { 4, "purple" }, { 5, "pink" },   { 6, "orange" },   { 7, "red" },
    { 8, "dim-cyan" }, { 9, "dim-white" },
    { 27, "dim-orange" }, { 37, "yellow" }, { 39, "salmon" }, { 43, "dim-blue" },
    { 46, "lemon" }, { 50, "dim-purple" }, { 80, "rose" }, { 81, "magenta" },
    { 83, "light-orange" }, { 86, "blue" }, { 123, "light-blue" },
};
#define NPALETTE (sizeof(palette) / sizeof(palette[0]))

static const char *palette_name(uint8_t code)
{
    code &= 0x7F;
    for (size_t i = 0; i < NPALETTE; i++) if (palette[i].code == code) return palette[i].name;
    if (code >= 10 && code <= 20) return "white (brighter with higher code)";
    return NULL;
}

/* Accepts a colour name or a number (0-255). Returns -1 if invalid. */
static int parse_code(const char *s)
{
    for (size_t i = 0; i < NPALETTE; i++) if (!strcasecmp(palette[i].name, s)) return palette[i].code;
    char *end;
    long n = strtol(s, &end, 0);
    if (*s && !*end && n >= 0 && n <= 255) return (int)n;
    return -1;
}

static void print_palette(void)
{
    fprintf(stderr, "Colours:");
    for (size_t i = 0; i < NPALETTE; i++) fprintf(stderr, " %s(%u)", palette[i].name, palette[i].code);
    fprintf(stderr, "; 10-20 = white, getting brighter; 128-255 repeat 0-127\n");
}

static const struct mode_info *mode_by_ec(uint8_t v)
{
    for (size_t i = 0; i < NMODES; i++) if (modes[i].ec == v) return &modes[i];
    return NULL;
}
static const struct mode_info *mode_by_wmi(uint8_t v)
{
    for (size_t i = 0; i < NMODES; i++) if (modes[i].wmi == v) return &modes[i];
    return NULL;
}
static const struct mode_info *mode_by_led(uint8_t v)
{
    for (size_t i = 0; i < NMODES; i++) if (modes[i].led_code == v) return &modes[i];
    return NULL;
}
static const struct mode_info *mode_by_sysfs(const char *s)
{
    for (size_t i = 0; i < NMODES; i++) if (!strcmp(modes[i].sysfs, s)) return &modes[i];
    return NULL;
}
static const struct mode_info *mode_by_arg(const char *s)
{
    char *end;
    long n = strtol(s, &end, 0);
    if (*s && !*end) {                       /* numeric: EC encoding */
        return (n >= 0 && n <= 0xFF) ? mode_by_ec((uint8_t)n) : NULL;
    }
    for (size_t i = 0; i < NMODES; i++)
        if (!strcasecmp(modes[i].name, s) || !strcasecmp(modes[i].sysfs, s)) return &modes[i];
    return NULL;
}
static const char *nm(const struct mode_info *m) { return m ? m->name : "UNKNOWN"; }

/* ---------- platform profile helpers (names are the sysfs ones on both OSes) ---------- */

#ifdef _WIN32
/* BIOS WMI class ROOT\WMI:AcerGamingFunction (GUID 7A4DDFE7-...), the same
 * methods acer-wmi and PredatorSense call. Misc setting 0x0B = thermal
 * profile; input = 0x0B | value << 8, output = status | value << 8. */
static IWbemServices *wmi;
static BSTR wmi_path;            /* __PATH of the AcerGamingFunction instance */

static int wmi_open(void)
{
    static int failed;
    if (wmi_path) return 0;
    if (failed) return -1;
    failed = 1;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { fprintf(stderr, "CoInitializeEx: 0x%08lX\n", (unsigned long)hr); return -1; }
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                         RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    IWbemLocator *loc = NULL;
    hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&loc);
    if (FAILED(hr)) { fprintf(stderr, "WbemLocator: 0x%08lX\n", (unsigned long)hr); return -1; }
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    hr = IWbemLocator_ConnectServer(loc, ns, NULL, NULL, NULL, 0, NULL, NULL, &wmi);
    SysFreeString(ns);
    IWbemLocator_Release(loc);
    if (FAILED(hr)) { fprintf(stderr, "WMI connect ROOT\\WMI: 0x%08lX\n", (unsigned long)hr); return -1; }

    IEnumWbemClassObject *en = NULL;
    IWbemClassObject *obj = NULL;
    ULONG got = 0;
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(L"SELECT * FROM AcerGamingFunction");
    hr = IWbemServices_ExecQuery(wmi, lang, q, WBEM_FLAG_FORWARD_ONLY, NULL, &en);
    SysFreeString(lang);
    SysFreeString(q);
    if (SUCCEEDED(hr)) hr = IEnumWbemClassObject_Next(en, WBEM_INFINITE, 1, &obj, &got);
    if (en) IEnumWbemClassObject_Release(en);
    if (FAILED(hr) || !got) {
        fprintf(stderr, "no AcerGamingFunction WMI instance (0x%08lX)%s\n", (unsigned long)hr,
                hr == (HRESULT)WBEM_E_ACCESS_DENIED ? ": run as Administrator" : "");
        return -1;
    }
    VARIANT v;
    VariantInit(&v);
    hr = IWbemClassObject_Get(obj, L"__PATH", 0, &v, NULL, NULL);
    if (SUCCEEDED(hr) && v.vt == VT_BSTR) wmi_path = SysAllocString(v.bstrVal);
    VariantClear(&v);
    IWbemClassObject_Release(obj);
    if (!wmi_path) return -1;
    failed = 0;
    return 0;
}

/* Call a gaming method with one integer in (gmInput) and one out (gmOutput). */
static int wmi_call(const wchar_t *method, uint64_t in, uint64_t *out)
{
    if (wmi_open()) return -1;
    IWbemClassObject *cls = NULL, *def = NULL, *args = NULL, *res = NULL;
    BSTR cname = SysAllocString(L"AcerGamingFunction"), mname = SysAllocString(method);
    HRESULT hr = IWbemServices_GetObject(wmi, cname, 0, NULL, &cls, NULL);
    if (SUCCEEDED(hr)) hr = IWbemClassObject_GetMethod(cls, method, 0, &def, NULL);
    if (SUCCEEDED(hr) && !def) hr = E_UNEXPECTED;
    if (SUCCEEDED(hr)) hr = IWbemClassObject_SpawnInstance(def, 0, &args);
    if (SUCCEEDED(hr)) {
        /* WMI carries uint64 as a decimal string and uint32 as VT_I4 */
        CIMTYPE t = 0;
        IWbemClassObject_Get(args, L"gmInput", 0, NULL, &t, NULL);
        VARIANT v;
        VariantInit(&v);
        if (t == CIM_UINT64) {
            wchar_t num[24];
            swprintf(num, 24, L"%llu", (unsigned long long)in);
            v.vt = VT_BSTR;
            v.bstrVal = SysAllocString(num);
        } else {
            v.vt = VT_I4;
            v.lVal = (LONG)(uint32_t)in;
        }
        hr = IWbemClassObject_Put(args, L"gmInput", 0, &v, 0);
        VariantClear(&v);
    }
    if (SUCCEEDED(hr)) hr = IWbemServices_ExecMethod(wmi, wmi_path, mname, 0, NULL, args, &res, NULL);
    if (SUCCEEDED(hr)) {
        VARIANT v;
        VariantInit(&v);
        hr = IWbemClassObject_Get(res, L"gmOutput", 0, &v, NULL, NULL);
        if (SUCCEEDED(hr)) {
            if (v.vt == VT_BSTR) *out = wcstoull(v.bstrVal, NULL, 10);
            else if (v.vt == VT_I4) *out = (uint32_t)v.lVal;
            else hr = E_UNEXPECTED;
        }
        VariantClear(&v);
    }
    if (res) IWbemClassObject_Release(res);
    if (args) IWbemClassObject_Release(args);
    if (def) IWbemClassObject_Release(def);
    if (cls) IWbemClassObject_Release(cls);
    SysFreeString(cname);
    SysFreeString(mname);
    if (FAILED(hr)) { fprintf(stderr, "WMI %ls failed: 0x%08lX\n", method, (unsigned long)hr); return -1; }
    return 0;
}

static int read_profile(char *buf, size_t len)
{
    uint64_t out;
    if (wmi_call(L"GetGamingMiscSetting", 0x0B, &out) || (out & 0xFF)) return -1;
    const struct mode_info *m = mode_by_wmi((out >> 8) & 0xFF);
    if (m) snprintf(buf, len, "%s", m->sysfs);
    else snprintf(buf, len, "0x%02X", (unsigned)((out >> 8) & 0xFF));
    return 0;
}

static int write_profile(const char *p)
{
    const struct mode_info *m = mode_by_sysfs(p);
    uint64_t out;
    if (!m) { fprintf(stderr, "unknown profile '%s'\n", p); return -1; }
    if (wmi_call(L"SetGamingMiscSetting", 0x0B | (uint64_t)m->wmi << 8, &out)) return -1;
    if (out & 0xFF) {
        fprintf(stderr, "SetGamingMiscSetting(%s) returned status 0x%02X\n", m->name, (unsigned)(out & 0xFF));
        return -1;
    }
    return 0;
}
#else
static int read_profile(char *buf, size_t len)
{
    FILE *f = fopen(PROFILE_SRC, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)len, f)) { fclose(f); return -1; }
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

static int write_profile(const char *p)
{
    FILE *f = fopen(PROFILE_SRC, "w");
    if (!f) { fprintf(stderr, "open %s: %s\n", PROFILE_SRC, strerror(errno)); return -1; }
    int ok = fputs(p, f) >= 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { fprintf(stderr, "write '%s' to %s failed: %s\n", p, PROFILE_SRC, strerror(errno)); return -1; }
    return 0;
}
#endif

/* ---------- EC access setup ---------- */

static int ec_ready = 0;

static int ec_open(void)
{
    if (ec_ready) return 0;
#ifdef _WIN32
    /* serialize with other instances of this tool (handle stays open until exit) */
    HANDLE inst = CreateMutexA(NULL, FALSE, "Global\\predator_i2ec");
    if (inst && GetLastError() == ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "another predator_i2ec instance is running\n");
        return -1;
    }
    io_dev = CreateFileA(INPOUT_DEVICE, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, 0, NULL);
    if (io_dev == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "cannot open %s (error %lu). Run as Administrator; the inpoutx64\n"
                        "driver is installed by PredatorSense (sc query inpoutx64).\n",
                INPOUT_DEVICE, GetLastError());
        return -1;
    }
    isa_mutex = CreateMutexA(NULL, FALSE, ISA_MUTEX);
    if (!isa_mutex) isa_mutex = OpenMutexA(SYNCHRONIZE, FALSE, ISA_MUTEX);
#else
    /* serialize with other instances of this tool */
    int lf = open("/run/predator_i2ec.lock", O_CREAT | O_RDWR, 0600);
    if (lf >= 0 && flock(lf, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "another predator_i2ec instance is running\n");
        return -1;
    }
    if (ioperm(SIO_INDEX, 2, 1) != 0) {
        perror("ioperm(0x2E) failed (run as root; kernel lockdown blocks this)");
        return -1;
    }
#endif
    uint8_t id1 = ec_rd(XR_CHIPID1), id2 = ec_rd(XR_CHIPID2);
    if (id1 != 0x55 || id2 != 0x70) {
        fprintf(stderr, "I2EC sanity check failed: chip ID %02X%02X (expected 5570)\n", id1, id2);
        return -1;
    }
    ec_ready = 1;
    return 0;
}

static double now_s(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
#endif
}

static void hms(char *buf, size_t n)
{
    time_t t = time(NULL);
    strftime(buf, n, "%H:%M:%S", localtime(&t));
}

/* ---------- commands ---------- */

static int cmd_probe(void)
{
    char sp[64] = "n/a";
    int have_sysfs = read_profile(sp, sizeof sp) == 0;
    const struct mode_info *ms = have_sysfs ? mode_by_sysfs(sp) : NULL;

    if (ec_open()) return 1;
    uint8_t acpi = ec_rd(XR_MODE_ACPI), intl = ec_rd(XR_MODE_INT), wmi = ec_rd(XR_MODE_WMI);
    uint8_t led = ec_rd(XR_SMBD_HOBDB);
    const struct mode_info *m = mode_by_ec(acpi);

    printf("--- Predator PHN16-71 (ITE IT5570E) ---\n");
    printf("Current mode          : %s", nm(m));
    if (m) printf("   (Mode LED: %s)", m->color);
    printf("\n\nSources:\n");
    printf("  EC 0x54  (XRAM 0x0554, ACPI)      : 0x%02X -> %s\n", acpi, nm(m));
    printf("  XRAM 0x016F (EC internal)         : 0x%02X -> %s\n", intl, nm(mode_by_ec(intl)));
    printf("  XRAM 0x0AF4 (last WMI request)    : 0x%02X -> %s\n", wmi, nm(mode_by_wmi(wmi)));
    printf("  SMBus-D HOBDB (code sent to 0x67) : 0x%02X -> %s\n", led, nm(mode_by_led(led)));
    printf("  %-33s : %s -> %s\n", PROFILE_SRC, sp, have_sysfs ? nm(ms) : "unavailable (" PROFILE_HINT ")");

    int consistent = m && mode_by_ec(intl) == m && mode_by_led(led) == m && (!have_sysfs || ms == m);
    printf("\nConsistency           : %s\n", consistent ? "OK" :
           "MISMATCH (normal for ~1 s after a mode change, while --led-daemon/--led-raw is active,\n"
           "                        or after --led-effect/--kbd-rgb: HOBDB then holds their last data byte)");
    printf("Fan PWM duty (DCR5/6) : 0x%02X / 0x%02X\n", ec_rd(XR_FAN_DUTY_A), ec_rd(XR_FAN_DUTY_B));
    return 0;
}

static volatile sig_atomic_t stop_flag = 0;
static void on_sigint(int s) { (void)s; stop_flag = 1; }

static int cmd_watch(void)
{
    if (ec_open()) return 1;
    signal(SIGINT, on_sigint);
    printf("Watching mode changes (press the Mode button or change profile). Ctrl+C to stop.\n\n");
    printf("%-8s  %-12s  %-6s  %-6s  %-6s  %-7s  %s\n",
           "time", "mode", "EC54", "WMI", "LEDcd", "fanA/B", "LED colour");

    int last_acpi = -1, last_led = -1;
    char t[16];
    while (!stop_flag) {
        uint8_t acpi = ec_rd(XR_MODE_ACPI);
        uint8_t led = ec_rd(XR_SMBD_HOBDB);
        if (acpi != last_acpi || led != last_led) {
            const struct mode_info *m = mode_by_ec(acpi);
            hms(t, sizeof t);
            printf("%-8s  %-12s  0x%02X    0x%02X    0x%02X    %02X/%02X    %s\n",
                   t, nm(m), acpi, ec_rd(XR_MODE_WMI), led,
                   ec_rd(XR_FAN_DUTY_A), ec_rd(XR_FAN_DUTY_B), m ? m->color : "?");
            fflush(stdout);
            last_acpi = acpi;
            last_led = led;
        }
        usleep(50000);
    }
    printf("\n");
    return 0;
}

static int cmd_mode(const char *arg)
{
    const struct mode_info *m = mode_by_arg(arg);
    if (!m) {
        fprintf(stderr, "unknown mode '%s'. Use: eco, quiet, balanced, performance, turbo\n", arg);
        return 1;
    }
    char before[64] = "?";
    read_profile(before, sizeof before);
    if (write_profile(m->sysfs)) {
        fprintf(stderr, "Hint: " PROFILE_HINT " Turbo usually needs AC power.\n");
        return 1;
    }
    usleep(300000);
    char after[64] = "?";
    read_profile(after, sizeof after);
    printf("platform_profile: %s -> %s  (%s, expected LED: %s)\n", before, after, m->name, m->color);
    if (ec_open() == 0) {
        uint8_t acpi = ec_rd(XR_MODE_ACPI);
        printf("EC confirms       : 0x%02X -> %s%s\n", acpi, nm(mode_by_ec(acpi)),
               mode_by_ec(acpi) == m ? "" : "  (MISMATCH - EC refused?)");
    }
    return 0;
}

static int cmd_cycle(double hold)
{
    char orig[64];
    if (read_profile(orig, sizeof orig)) {
        fprintf(stderr, "cannot read %s (" PROFILE_HINT ")\n", PROFILE_SRC);
        return 1;
    }
    signal(SIGINT, on_sigint);
    if (ec_open()) return 1;
    printf("Cycling every mode for %.0f s each; note the Mode-LED colour. Ctrl+C restores '%s'.\n\n", hold, orig);
    for (size_t i = 0; i < NMODES && !stop_flag; i++) {
        if (write_profile(modes[i].sysfs)) continue;
        usleep(300000);
        uint8_t acpi = ec_rd(XR_MODE_ACPI);
        printf("[%zu/%zu] %-12s EC54=0x%02X  LEDcode=%u  -> look at the LED now\n",
               i + 1, NMODES, modes[i].name, acpi, ec_rd(XR_SMBD_HOBDB));
        fflush(stdout);
        double t0 = now_s();
        while (!stop_flag && now_s() - t0 < hold) usleep(100000);
    }
    write_profile(orig);
    printf("\nRestored '%s'.\n", orig);
    return 0;
}

static int cmd_sniff(double secs)
{
    if (ec_open()) return 1;
    signal(SIGINT, on_sigint);
    printf("Sniffing EC SMBus channel D for %.1f s (temp-sensor polls to 0x4D hidden).\n", secs);
    printf("Change mode now (button or --mode in another terminal).\n");
    printf("  t[s]    HOSTA HOCTL HOCMD TRASLA D0   D1   HOBDB PEC  PCTL CTL2\n");
    fflush(stdout);   /* so a redirected, killed sniffer still shows it ran */
    uint8_t last[10] = {0}, cur[10];
    double t0 = now_s();
    while (!stop_flag && now_s() - t0 < secs) {
        for (int i = 0; i < 10; i++) cur[i] = ec_rd(XR_SMBD_BASE + i);
        if (memcmp(cur, last, sizeof cur)) {
            if (cur[3] != 0x9B && cur[3] != 0x9A) {
                printf("%8.4f ", now_s() - t0);
                for (int i = 0; i < 10; i++) printf(" %02X  ", cur[i]);
                if (cur[3] == 0xCE) printf(" <- slave 0x67");
                printf("\n");
                fflush(stdout);
            }
            memcpy(last, cur, sizeof cur);
        }
    }
    return 0;
}

/* ---------- EXPERIMENTAL: talk to the LED controller (SMBus-D slave 0x67) ----------
 * Replays the exact 4-transaction sequence the EC firmware sends on a mode change:
 *   1. write word  cmd 0x00 = 0xFC81
 *   2. block write cmd 0x03 len 1 [code]       (EC uses 1..5 = eco..turbo)
 *   3. write word  cmd 0x00 = 0xE080
 *   4. write byte  cmd 0x01 = brightness       (EC uses 0xCD)
 * The EC firmware owns this controller too, so we start right after its 1 Hz
 * temperature-sensor poll, when the bus is idle for ~1 s. */

/* Wait until >=20 ms after the EC's own polling burst finishes. */
static int smbd_wait_quiet(void)
{
    double t0 = now_s(), last_busy = 0;
    int seen = 0;
    while (now_s() - t0 < 3.0) {
        if (ec_rd(XR_SMBD_HOSTA) & HOSTA_BUSY) { seen = 1; last_busy = now_s(); }
        else if (seen && now_s() - last_busy > 0.020) return 0;
    }
    return -1;
}

static int verbose_xfer = 1;
static int led_send(uint8_t code, uint8_t bright);

static int smbd_xfer_addr(uint8_t slave, uint8_t smcd, uint8_t cmd, uint8_t d0, uint8_t d1, uint8_t blk);

static int smbd_xfer(uint8_t smcd, uint8_t cmd, uint8_t d0, uint8_t d1, uint8_t blk)
{
    return smbd_xfer_addr(LED_SLAVE_W, smcd, cmd, d0, d1, blk);
}

static int smbd_xfer_addr(uint8_t slave, uint8_t smcd, uint8_t cmd, uint8_t d0, uint8_t d1, uint8_t blk)
{
    if (ec_rd(XR_SMBD_HOSTA) & HOSTA_BUSY) { fprintf(stderr, "  bus busy, abort\n"); return -1; }
    ec_wr_smbd(XR_SMBD_HOSTA, 0xFE);                 /* clear status (write-1-to-clear) */
    ec_wr_smbd(XR_SMBD_TRASLA, slave);
    ec_wr_smbd(XR_SMBD_HOCMD, cmd);
    ec_wr_smbd(XR_SMBD_D0, d0);
    ec_wr_smbd(XR_SMBD_D1, d1);
    if (smcd == SMCD_BLOCK) ec_wr_smbd(XR_SMBD_HOBDB, blk);
    ec_wr_smbd(XR_SMBD_HOCTL, HOCTL_SRT | smcd);

    double t0 = now_s();
    int rc = 0;
    uint8_t seen = 0;
    for (;;) {
        uint8_t s = ec_rd(XR_SMBD_HOSTA);
        seen |= s;
        if (s & HOSTA_ERRMASK) { fprintf(stderr, "  SMBus error, HOSTA=0x%02X\n", s); rc = -1; break; }
        if (s & HOSTA_BDS) ec_wr_smbd(XR_SMBD_HOSTA, 0xFE);   /* block byte sent; count is 1 */
        else if (!(s & HOSTA_BUSY) && (s & HOSTA_DONE || now_s() - t0 > 0.002)) break;
        if (now_s() - t0 > 0.030) { fprintf(stderr, "  timeout, HOSTA=0x%02X\n", s); rc = -1; break; }
    }
    if (rc) ec_wr_smbd(XR_SMBD_HOCTL, HOCTL_KILL);
    ec_wr_smbd(XR_SMBD_HOSTA, 0xFE);
    if (verbose_xfer || rc)
        printf("  cmd 0x%02X proto %u: HOSTA flags seen 0x%02X, %.2f ms %s\n",
               cmd, smcd >> 2, seen, (now_s() - t0) * 1000, rc ? "FAIL" : "ok");
    return rc;
}

/* Multi-byte SMBus block write to slave 0x67 (ITE: next byte -> HOBDB, then clear BDS). */
static int smbd_block_write(uint8_t cmd, const uint8_t *data, int n)
{
    if (n < 1 || n > 32) return -1;
    if (ec_rd(XR_SMBD_HOSTA) & HOSTA_BUSY) { fprintf(stderr, "  bus busy, abort\n"); return -1; }
    ec_wr_smbd(XR_SMBD_HOSTA, 0xFE);
    ec_wr_smbd(XR_SMBD_TRASLA, LED_SLAVE_W);
    ec_wr_smbd(XR_SMBD_HOCMD, cmd);
    ec_wr_smbd(XR_SMBD_D0, (uint8_t)n);
    ec_wr_smbd(XR_SMBD_HOBDB, data[0]);
    ec_wr_smbd(XR_SMBD_HOCTL, HOCTL_SRT | SMCD_BLOCK);

    double t0 = now_s();
    int idx = 1, rc = 0;
    uint8_t seen = 0;
    for (;;) {
        uint8_t s = ec_rd(XR_SMBD_HOSTA);
        seen |= s;
        if (s & HOSTA_ERRMASK) { fprintf(stderr, "  SMBus error, HOSTA=0x%02X\n", s); rc = -1; break; }
        if (s & HOSTA_BDS) {
            if (idx < n) ec_wr_smbd(XR_SMBD_HOBDB, data[idx++]);
            ec_wr_smbd(XR_SMBD_HOSTA, HOSTA_BDS);
            continue;
        }
        if (!(s & HOSTA_BUSY) && (s & HOSTA_DONE || now_s() - t0 > 0.002)) break;
        if (now_s() - t0 > 0.080) { fprintf(stderr, "  timeout, HOSTA=0x%02X\n", s); rc = -1; break; }
    }
    if (!rc && idx < n) { fprintf(stderr, "  block ended after %d of %d bytes\n", idx, n); rc = -1; }
    if (rc) ec_wr_smbd(XR_SMBD_HOCTL, HOCTL_KILL);
    ec_wr_smbd(XR_SMBD_HOSTA, 0xFE);
    if (verbose_xfer || rc)
        printf("  cmd 0x%02X block[%d]: HOSTA flags seen 0x%02X, %.2f ms %s\n",
               cmd, n, seen, (now_s() - t0) * 1000, rc ? "FAIL" : "ok");
    return rc;
}

/* One lighting transaction step. */
struct lt_step { char kind; uint8_t cmd; uint16_t word; uint8_t data[32]; int n; };

/* Parse "w00:FC81"  (write word), "b03:07,00"  (block write), "c01:CD" (write byte). */
static int lt_parse(const char *s, struct lt_step *st)
{
    char k = s[0];
    if (k != 'w' && k != 'b' && k != 'c') return -1;
    char *end;
    unsigned long cmd = strtoul(s + 1, &end, 16);
    if (*end != ':' || cmd > 0xFF) return -1;
    st->kind = k;
    st->cmd = (uint8_t)cmd;
    st->n = 0;
    const char *p = end + 1;
    if (k == 'w' || k == 'c') {
        unsigned long v = strtoul(p, &end, 16);
        if (*end || v > (k == 'w' ? 0xFFFFUL : 0xFFUL)) return -1;
        st->word = (uint16_t)v;
        return 0;
    }
    while (*p) {
        unsigned long v = strtoul(p, &end, 16);
        if (end == p || v > 0xFF || st->n >= 32) return -1;
        st->data[st->n++] = (uint8_t)v;
        p = (*end == ',') ? end + 1 : end;
        if (*end && *end != ',') return -1;
    }
    return st->n ? 0 : -1;
}

/* Run steps back-to-back inside one idle window of the EC's bus. */
static int lt_run(const struct lt_step *st, int n)
{
    if (smbd_wait_quiet()) { fprintf(stderr, "could not find an idle window on SMBus-D\n"); return -1; }
    for (int i = 0; i < n; i++) {
        int rc;
        if (st[i].kind == 'w')
            rc = smbd_xfer(SMCD_WORDDATA, st[i].cmd, st[i].word & 0xFF, st[i].word >> 8, 0);
        else if (st[i].kind == 'c')
            rc = smbd_xfer(SMCD_BYTEDATA, st[i].cmd, (uint8_t)st[i].word, 0, 0);
        else
            rc = smbd_block_write(st[i].cmd, st[i].data, st[i].n);
        if (rc) return rc;
    }
    return 0;
}

static int cmd_lt(int argc, char **argv)
{
    struct lt_step st[64];   /* all run inside one ~1 s idle window of the bus */
    int n = 0, yes = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--yes")) { yes = 1; continue; }
        if (n >= 64 || lt_parse(argv[i], &st[n])) { fprintf(stderr, "bad step '%s'\n", argv[i]); return 1; }
        n++;
    }
    if (!n || !yes) {
        fprintf(stderr,
            "EXPERIMENTAL raw lighting transactions to SMBus 0x67.\n"
            "Usage: " SUDO "%s --lt <step> ... --yes\n"
            "  wCC:HHLL   write word  (cmd CC, value 0xHHLL)\n"
            "  bCC:aa,bb  block write (cmd CC, bytes)\n"
            "  cCC:VV     write byte  (cmd CC)\n"
            "  e.g. mode preset 7: --lt w00:FC81 b03:07 w00:E080 c01:CD --yes\n", argv[0]);
        return 1;
    }
    if (ec_open()) return 1;
    int rc = lt_run(st, n);
    printf(rc ? "FAILED\n" : "Done.\n");
    return rc ? 1 : 0;
}

static int parse_rgb(const char *s, uint8_t rgb[3])
{
    const char *p = s[0] == '#' ? s + 1 : s;
    char *end;
    if (strlen(p) != 6) return -1;
    unsigned long v = strtoul(p, &end, 16);
    if (*end) return -1;
    rgb[0] = v >> 16; rgb[1] = (v >> 8) & 0xFF; rgb[2] = v & 0xFF;
    return 0;
}

/* Keyboard 4-zone static colour, same sequence the BIOS (WMI method 20) makes the EC send. */
static int cmd_kbd_rgb(int argc, char **argv)
{
    int yes = 0, nz = 0, bright = 100;
    uint8_t zones[4][3];
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--yes")) { yes = 1; continue; }
        if (nz < 4 && parse_rgb(argv[i], zones[nz]) == 0) { nz++; continue; }
        if (nz > 0) { bright = atoi(argv[i]); continue; }
        fprintf(stderr, "bad argument '%s'\n", argv[i]);
        return 1;
    }
    if (!nz || !yes || bright < 0 || bright > 100) {
        fprintf(stderr, "Usage: " SUDO "%s --kbd-rgb RRGGBB [RRGGBB RRGGBB RRGGBB] [brightness 0-100] --yes\n"
                        "  one colour = all four zones\n", argv[0]);
        return 1;
    }
    for (int z = nz; z < 4; z++) memcpy(zones[z], zones[nz - 1], 3);
    struct lt_step st[6] = {
        { 'w', 0x00, 0xF280, {0}, 0 },
        { 'b', 0x03, 0, {0}, 13 },
        { 'w', 0x00, 0xFD81, {0}, 0 },
        { 'b', 0x03, 0, {0x01, 0x00}, 2 },
        { 'w', 0x00, 0xE080, {0}, 0 },
        { 'c', 0x01, 0xC2, {0}, 0 },
    };
    for (int z = 0; z < 4; z++) memcpy(&st[1].data[z * 3], zones[z], 3);
    st[1].data[12] = (uint8_t)bright;
    if (ec_open()) return 1;
    verbose_xfer = 0;
    int rc = lt_run(st, 6);
    printf(rc ? "FAILED\n" : "Keyboard set.\n");
    return rc ? 1 : 0;
}

/* Restore stock look: keyboard static cyan (BIOS default here) + current mode preset. */
static void restore_stock_lighting(void)
{
    const struct lt_step kb[6] = {
        { 'w', 0x00, 0xF280, {0}, 0 },
        { 'b', 0x03, 0, {0x00,0xAE,0xC7, 0x00,0xAE,0xC7, 0x00,0xAE,0xC7, 0x00,0xAE,0xC7, 100}, 13 },
        { 'w', 0x00, 0xFD81, {0}, 0 },
        { 'b', 0x03, 0, {0x01, 0x00}, 2 },
        { 'w', 0x00, 0xE080, {0}, 0 },
        { 'c', 0x01, 0xC2, {0}, 0 },
    };
    lt_run(kb, 6);
    const struct mode_info *cur = mode_by_ec(ec_rd(XR_MODE_ACPI));
    if (cur) led_send(cur->led_code, 0xCD);
}

/* User-driven opcode sweep: press Ctrl+C as soon as the Mode button changes colour. */
static int cmd_op_sweep(int argc, char **argv)
{
    int yes = 0;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--yes")) yes = 1;
    if (argc < 4 || !yes) {
        fprintf(stderr,
            "EXPERIMENTAL: fires execute opcodes <from>..<to> at the lighting chip.\n"
            "Unknown opcodes could do anything to the lighting chip, including\n"
            "firmware/erase commands. Press Ctrl+C the moment the MODE BUTTON changes colour.\n"
            "Usage: " SUDO "%s --op-sweep <from> <to> [seconds per step, default 2] --yes\n"
            "Already tested: 0x30-0x33, 0xC0-0xDF, 0xFF.\n", argv[0]);
        return 1;
    }
    unsigned from = strtoul(argv[2], NULL, 0), to = strtoul(argv[3], NULL, 0);
    double hold = (argc >= 5 && strcmp(argv[4], "--yes")) ? atof(argv[4]) : 2.0;
    if (to > 0xFF || from > to) { fprintf(stderr, "bad range\n"); return 1; }
    if (ec_open()) return 1;
    signal(SIGINT, on_sigint);
    verbose_xfer = 0;
    printf("Watch the MODE BUTTON. Each step asks for RED or GREEN; Ctrl+C when it changes.\n\n");
    int last = -1, step = 0;
    for (unsigned op = from; op <= to && !stop_flag; op++, step++) {
        if (op == 0xCD) continue;
        uint8_t code = (step & 1) ? 1 : 7;
        struct lt_step st[4] = {
            { 'w', 0x00, 0xFC81, {0}, 0 },
            { 'b', 0x03, 0, {code}, 1 },
            { 'w', 0x00, 0xE080, {0}, 0 },
            { 'c', 0x01, (uint16_t)op, {0}, 0 },
        };
        printf("  >>> opcode 0x%02X   (button would go %s)\n", op, code == 7 ? "RED" : "GREEN");
        fflush(stdout);
        last = (int)op;
        if (lt_run(st, 4)) { printf("  chip did not ACK opcode 0x%02X - stopping\n", op); break; }
        double t0 = now_s();
        while (!stop_flag && now_s() - t0 < hold) usleep(50000);
    }
    stop_flag = 0;
    printf("\nLast opcode sent: 0x%02X (if you pressed Ctrl+C right at a change, it is this one\n"
           "or the one before it). Restoring keyboard + button...\n", last);
    restore_stock_lighting();
    return 0;
}

/* Show each code in a range for a few seconds so its colour can be noted. */
static int cmd_led_scan(int argc, char **argv)
{
    int yes = 0;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--yes")) yes = 1;
    if (argc < 4 || !yes) {
        fprintf(stderr, "Usage: " SUDO "%s --led-scan <from> <to> [seconds] --yes\n", argv[0]);
        return 1;
    }
    unsigned from = strtoul(argv[2], NULL, 0), to = strtoul(argv[3], NULL, 0);
    double hold = (argc >= 5 && strcmp(argv[4], "--yes")) ? atof(argv[4]) : 4.0;
    if (to > 0xFF || from > to) { fprintf(stderr, "bad range\n"); return 1; }
    if (ec_open()) return 1;
    signal(SIGINT, on_sigint);
    verbose_xfer = 0;
    printf("Showing LED codes %u..%u, %.0f s each. Ctrl+C to stop early.\n", from, to, hold);
    for (unsigned c = from; c <= to && !stop_flag; c++) {
        const char *pn = palette_name((uint8_t)c);
        printf("  code %3u (0x%02X)  %-34s ", c, c, pn ? pn : "unknown - what colour?");
        fflush(stdout);
        int rc = led_send((uint8_t)c, 0xCD);
        printf("%s\n", rc ? "FAILED" : "");
        double t0 = now_s();
        while (!stop_flag && now_s() - t0 < hold) usleep(100000);
    }
    const struct mode_info *cur = mode_by_ec(ec_rd(XR_MODE_ACPI));
    if (cur) led_send(cur->led_code, 0xCD);
    printf("Restored normal colour for %s.\n", nm(cur));
    return 0;
}

/* Keep a custom code per mode: whenever the EC sends its own code (mode
 * change), immediately replace it with the user's choice. */
static int cmd_led_daemon(int argc, char **argv)
{
    int yes = 0;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--yes")) yes = 1;
    if (argc < 3 || !yes) {
        fprintf(stderr,
            "Usage: " SUDO "%s --led-daemon <mode=colour[:bright],...> --yes\n"
            "  e.g. --led-daemon turbo=red,performance=orange,quiet=white:0x40 --yes\n"
            "  Modes not listed keep the stock colour. Ctrl+C restores stock colour.\n", argv[0]);
        print_palette();
        return 1;
    }
    int map_code[NMODES], map_bright[NMODES];
    for (size_t i = 0; i < NMODES; i++) { map_code[i] = -1; map_bright[i] = 0xCD; }

    char *spec = strdup(argv[2]);
    for (char *tok = strtok(spec, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq) { fprintf(stderr, "bad entry '%s'\n", tok); return 1; }
        *eq = 0;
        const struct mode_info *m = mode_by_arg(tok);
        if (!m) { fprintf(stderr, "unknown mode '%s'\n", tok); return 1; }
        char *colon = strchr(eq + 1, ':');
        if (colon) *colon = 0;
        size_t idx = (size_t)(m - modes);
        map_code[idx] = parse_code(eq + 1);
        if (map_code[idx] < 0) { fprintf(stderr, "unknown colour/code '%s'\n", eq + 1); print_palette(); return 1; }
        if (colon) map_bright[idx] = (int)strtoul(colon + 1, NULL, 0) & 0xFF;
        const char *pn = palette_name((uint8_t)map_code[idx]);
        printf("  %-12s -> %s (code %d), brightness 0x%02X\n", m->name, pn ? pn : "?",
               map_code[idx], map_bright[idx]);
    }
    free(spec);

    if (ec_open()) return 1;
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    verbose_xfer = 0;
    printf("LED daemon running. Ctrl+C to stop.\n");

    int last_sent = -1;          /* code we last wrote; -1 = none */
    int pending = 1;             /* need to (re)apply the custom code */
    const struct mode_info *last_m = NULL;
    double t_change = now_s();
    char t[16];
    while (!stop_flag) {
        uint8_t latched = ec_rd(XR_SMBD_HOBDB);
        const struct mode_info *m = mode_by_ec(ec_rd(XR_MODE_ACPI));
        if (m != last_m) { pending = 1; last_m = m; t_change = now_s(); }
        /* EC re-sent its stock code without a mode change (resume, etc.) */
        if (m && last_sent >= 0 && latched != (uint8_t)last_sent && latched == m->led_code) pending = 1;

        if (m && pending) {
            size_t idx = (size_t)(m - modes);
            int want = map_code[idx];
            if (want < 0) {
                pending = 0;
                last_sent = -1;
            } else if (latched == m->led_code || now_s() - t_change > 1.5) {
                usleep(30000);   /* let the EC finish its 4-transaction burst */
                if (led_send((uint8_t)want, (uint8_t)map_bright[idx]) == 0) {
                    last_sent = want;
                    pending = 0;
                    hms(t, sizeof t);
                    const char *pn = palette_name((uint8_t)want);
                    printf("%s  %-12s -> %s (code %d), brightness 0x%02X\n", t, m->name,
                           pn ? pn : "?", want, map_bright[idx]);
                    fflush(stdout);
                }
            }
        }
        usleep(100000);
    }
    const struct mode_info *cur = mode_by_ec(ec_rd(XR_MODE_ACPI));
    if (cur) led_send(cur->led_code, 0xCD);
    printf("\nStopped; restored stock colour for %s.\n", nm(cur));
    return 0;
}

/* Read-only probe of the LED controller's registers: read-word for each cmd. */
static int cmd_led_read(int argc, char **argv)
{
    unsigned from = argc >= 3 ? strtoul(argv[2], NULL, 0) : 0x00;
    unsigned to   = argc >= 4 ? strtoul(argv[3], NULL, 0) : 0x0F;
    if (to > 0xFF || from > to) { fprintf(stderr, "bad range\n"); return 1; }
    if (ec_open()) return 1;
    verbose_xfer = 0;
    printf("SMBus read-word from slave 0x67, cmd 0x%02X..0x%02X\n", from, to);
    for (unsigned c = from; c <= to; c++) {
        if (((c - from) % 16) == 0 && smbd_wait_quiet()) { fprintf(stderr, "no idle window\n"); return 1; }
        int rc = smbd_xfer_addr(LED_SLAVE_W | 1, SMCD_WORDDATA, (uint8_t)c, 0, 0, 0);
        uint8_t lo = ec_rd(XR_SMBD_D0), hi = ec_rd(XR_SMBD_D1);
        if (rc) printf("  cmd 0x%02X: NAK/error\n", c);
        else    printf("  cmd 0x%02X: 0x%02X%02X  (lo %02X hi %02X)\n", c, hi, lo, lo, hi);
    }
    return 0;
}

static int led_send(uint8_t code, uint8_t bright)
{
    if (smbd_wait_quiet()) { fprintf(stderr, "could not find an idle window on SMBus-D\n"); return -1; }
    int rc = 0;
    rc |= smbd_xfer(SMCD_WORDDATA, 0x00, 0x81, 0xFC, 0);
    if (!rc) rc |= smbd_xfer(SMCD_BLOCK, 0x03, 0x01, 0x00, code);
    if (!rc) rc |= smbd_xfer(SMCD_WORDDATA, 0x00, 0x80, 0xE0, 0);
    if (!rc) rc |= smbd_xfer(SMCD_BYTEDATA, 0x01, bright, 0x00, 0);
    return rc;
}

static int cmd_led_raw(int argc, char **argv)
{
    int yes = 0;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--yes")) yes = 1;
    if (argc < 3 || !yes) {
        fprintf(stderr,
            "EXPERIMENTAL: sends commands directly to the Mode-LED controller (SMBus 0x67).\n"
            "Usage: " SUDO "%s --led-raw <colour|code 0-255> [apply opcode, default 0xCD] --yes\n"
            "       " SUDO "%s --led-restore --yes   (re-send the code for the current mode)\n"
            "Any mode change (button or --mode) makes the EC overwrite it; use --led-daemon to keep it.\n",
            argv[0], argv[0]);
        print_palette();
        return 1;
    }
    if (ec_open()) return 1;
    uint8_t code, bright = 0xCD;
    if (!strcmp(argv[1], "--led-restore")) {
        const struct mode_info *m = mode_by_ec(ec_rd(XR_MODE_ACPI));
        if (!m) { fprintf(stderr, "unknown current mode\n"); return 1; }
        code = m->led_code;
        printf("Re-sending code %u (%s), brightness 0x%02X\n", code, m->name, bright);
    } else {
        int pc = parse_code(argv[2]);
        if (pc < 0) { fprintf(stderr, "unknown colour/code '%s'\n", argv[2]); print_palette(); return 1; }
        code = (uint8_t)pc;
        if (argc >= 4 && strcmp(argv[3], "--yes")) bright = (uint8_t)strtoul(argv[3], NULL, 0);
        const char *pn = palette_name(code);
        printf("Sending LED code %u (%s), brightness 0x%02X\n", code, pn ? pn : "unknown", bright);
    }
    int rc = led_send(code, bright);
    printf(rc ? "FAILED (see above). Run --mode <current> to make the EC resync.\n"
              : "Done - check the Mode LED.\n");
    return rc ? 1 : 0;
}

/* Button effects 0xA0 + mode, sent after page FD [01, speed] + commit. They switch
 * without the 0xCD flash, in the colour the last 0xCD latched (the cycle runs its own
 * colours). Only 0xCD sets that colour; the EC's own 0xCD on a mode change also
 * returns the button to static. */
static int cmd_led_effect(int argc, char **argv)
{
    static const struct { const char *name; uint8_t op; } fx[] = {
        { "static", 0xA0 }, { "breathing", 0xA1 }, { "cycle", 0xA2 },
    };
    int yes = 0, idx = -1;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--yes")) yes = 1;
    if (argc >= 3)
        for (int i = 0; i < 3; i++) if (!strcasecmp(argv[2], fx[i].name)) idx = i;
    long speed = (argc >= 4 && strcmp(argv[3], "--yes")) ? strtol(argv[3], NULL, 0) : 0;
    if (idx < 0 || speed < 0 || speed > 9 || !yes) {
        fprintf(stderr,
            "Mode-button effect without the flash, in the colour last set by a preset\n"
            "(--led-raw, --led-daemon or a mode change).\n"
            "Usage: " SUDO "%s --led-effect static|breathing|cycle [speed 0-9] --yes\n"
            "  speed 0 = slowest (default; cycle ~8 s per round), 9 = fastest (~2.5 s)\n"
            "  static stops breathing/cycle. A mode change returns the button to static.\n", argv[0]);
        return 1;
    }
    struct lt_step st[4] = {
        { 'w', 0x00, 0xFD81, {0}, 0 },
        { 'b', 0x03, 0, {0x01, (uint8_t)speed}, 2 },
        { 'w', 0x00, 0xE080, {0}, 0 },
        { 'c', 0x01, fx[idx].op, {0}, 0 },
    };
    if (ec_open()) return 1;
    verbose_xfer = 0;
    if (lt_run(st, 4)) { printf("FAILED\n"); return 1; }
    printf("Mode button: %s, speed %ld\n", fx[idx].name, speed);
    return 0;
}

static int cmd_dump(const char *a, const char *b)
{
    unsigned long from = strtoul(a, NULL, 16), to = strtoul(b, NULL, 16);
    if (to <= from || to > 0x10000) { fprintf(stderr, "bad range\n"); return 1; }
    if (ec_open()) return 1;
    for (unsigned long x = from & ~0xFUL; x < to; x += 16) {
        printf("%04lX:", x);
        for (unsigned long y = x; y < x + 16; y++)
            if (y >= from && y < to) printf(" %02X", ec_rd((uint16_t)y));
            else printf("   ");
        printf("\n");
    }
    return 0;
}

static void usage(const char *p)
{
    printf("Predator PHN16-71 Mode / Mode-LED tool\n\n"
           "  " SUDO "%s --probe               current mode, cross-checked\n"
           "  " SUDO "%s --watch               live mode-change monitor\n"
           "  " SUDO "%s --mode <m>            eco | quiet | balanced | performance | turbo\n"
           "  " SUDO "%s --cycle [sec]         show every mode/LED colour, then restore\n"
           "  " SUDO "%s --sniff [sec]         log EC SMBus channel D (LED controller)\n"
           "  " SUDO "%s --dump <from> <to>    hex-dump EC XRAM (read-only), e.g. 0500 0600\n"
           "\nEXPERIMENTAL (writes to the LED controller over the EC's SMBus):\n"
           "  " SUDO "%s --led-raw <colour|code> [opcode] --yes\n  " SUDO "%s --kbd-rgb RRGGBB [x3 more] [0-100] --yes   (keyboard, any RGB, instant)\n  " SUDO "%s --lt <steps> --yes                    (raw transactions, see source)\n"
           "  " SUDO "%s --led-restore --yes\n"
           "  " SUDO "%s --led-scan <from> <to> [sec] --yes\n"
           "  " SUDO "%s --led-daemon turbo=red,performance=orange,... --yes\n"
           "  " SUDO "%s --led-effect static|breathing|cycle [speed 0-9] --yes   (no flash)\n"
           "\nColours: off green white cyan purple pink orange red dim-cyan dim-white yellow lemon blue\n"
           "         light-blue dim-blue salmon rose magenta light-orange dim-orange dim-purple\n"
           "         (or any code 0-127; 10-20: brighter white)\n",
           p, p, p, p, p, p, p, p, p, p, p, p, p);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 1; }
    const char *c = argv[1];
    if (!strcmp(c, "--probe")) return cmd_probe();
    if (!strcmp(c, "--watch")) return cmd_watch();
    if (!strcmp(c, "--mode") && argc >= 3) return cmd_mode(argv[2]);
    if (!strcmp(c, "--cycle")) return cmd_cycle(argc >= 3 ? atof(argv[2]) : 5.0);
    if (!strcmp(c, "--sniff")) return cmd_sniff(argc >= 3 ? atof(argv[2]) : 10.0);
    if (!strcmp(c, "--dump") && argc >= 4) return cmd_dump(argv[2], argv[3]);
    if (!strcmp(c, "--led-raw") || !strcmp(c, "--led-restore")) return cmd_led_raw(argc, argv);
    if (!strcmp(c, "--led-read")) return cmd_led_read(argc, argv);
    if (!strcmp(c, "--led-scan")) return cmd_led_scan(argc, argv);
    if (!strcmp(c, "--lt")) return cmd_lt(argc, argv);
    if (!strcmp(c, "--op-sweep")) return cmd_op_sweep(argc, argv);
    if (!strcmp(c, "--kbd-rgb")) return cmd_kbd_rgb(argc, argv);
    if (!strcmp(c, "--led-daemon")) return cmd_led_daemon(argc, argv);
    if (!strcmp(c, "--led-effect")) return cmd_led_effect(argc, argv);
    usage(argv[0]);
    return 1;
}
