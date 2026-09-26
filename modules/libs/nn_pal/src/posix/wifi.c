/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_pal wifi — POSIX/iwd backend.
 *
 * Counterpart to src/posix/ble.c: together they let a Linux camera be
 * provisioned by the same hub wizard as the ESP ones — BLE carries the
 * credentials in, this joins the network with them.
 *
 * Credentials are handed to iwd as a network provisioning file rather
 * than over D-Bus.  That is deliberate: iwd autoconnects to any network
 * it has a provisioning file for, so a write + a scan nudge is enough,
 * and — the part that matters here — the credentials SURVIVE A REBOOT
 * with no further help from us.  Provisioning happens once, but the
 * camera has to rejoin on every boot afterwards.
 *
 * State is polled rather than taken from iwd's D-Bus signals.  What the
 * caller actually waits for is an IPv4 address, which is DHCP's business
 * and can land well after iwd reports "connected"; polling the interface
 * reports the thing that is true rather than the thing iwd knows.
 */
#include <nn_pal/wifi.h>

#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_pal_wifi);

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define IWD_DIR      "/var/lib/iwd"
#define POLL_MS      1000

static nn_pal_wifi_cb_t s_cb;
static void            *s_user;
static pthread_t        s_thread;
static bool             s_running;
static pthread_mutex_t  s_lock = PTHREAD_MUTEX_INITIALIZER;

static char             s_ifname[IF_NAMESIZE];
static char             s_ssid[64];
static bool             s_connected;      /* has an IPv4 address */
static char             s_ipv4[16];

/* ── helpers ────────────────────────────────────────────────────────── */

/* First wireless interface, by the presence of a phy80211 link in sysfs. */
static bool find_wifi_iface(char *out, size_t cap)
{
    DIR *d = opendir("/sys/class/net");
    if (!d) return false;
    bool found = false;
    struct dirent *de;
    while (!found && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[320];
        snprintf(p, sizeof p, "/sys/class/net/%s/phy80211", de->d_name);
        struct stat st;
        if (stat(p, &st) == 0) {
            snprintf(out, cap, "%s", de->d_name);
            found = true;
        }
    }
    closedir(d);
    return found;
}

/* Current IPv4 of the interface, "" if none. */
static void iface_ipv4(const char *ifname, char *out, size_t cap)
{
    out[0] = '\0';
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) return;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(p->ifa_name, ifname) != 0) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)(void *)p->ifa_addr;
        inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)cap);
        break;
    }
    freeifaddrs(ifa);
}

/* RSSI from /proc/net/wireless (dBm column), 0 when unknown. */
static int8_t iface_rssi(const char *ifname)
{
    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f) return 0;
    char line[256];
    int8_t rssi = 0;
    /* two header lines */
    if (fgets(line, sizeof line, f) && fgets(line, sizeof line, f)) {
        while (fgets(line, sizeof line, f)) {
            char name[64];
            int status; float link, level;
            if (sscanf(line, " %63[^:]: %d %f %f", name, &status, &link,
                       &level) == 4 && strcmp(name, ifname) == 0) {
                rssi = (int8_t)level;
                break;
            }
        }
    }
    fclose(f);
    return rssi;
}

/*
 * iwd names a provisioning file "<SSID>.psk".  An SSID may legally hold
 * '/' or other bytes that cannot appear in a filename; iwd's escape for
 * that is "=<hex>.psk".  Use the plain form when it is safe and the hex
 * form otherwise, so odd SSIDs do not silently write the wrong file (or
 * escape the directory).
 */
static bool psk_path(const char *ssid, char *out, size_t cap)
{
    bool plain = *ssid != '\0';
    for (const char *p = ssid; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c >= 0x7f || c == '/' || c == '\\' || c == '=') {
            plain = false;
            break;
        }
    }
    if (plain)
        return (size_t)snprintf(out, cap, IWD_DIR "/%s.psk", ssid) < cap;

    char hex[2 * 64 + 1];
    size_t n = strlen(ssid);
    if (n > 64) return false;
    for (size_t i = 0; i < n; i++)
        snprintf(hex + 2 * i, 3, "%02x", (unsigned char)ssid[i]);
    return (size_t)snprintf(out, cap, IWD_DIR "/=%s.psk", hex) < cap;
}

/* Write the provisioning file atomically: a half-written .psk that iwd
 * picks up mid-write is a confusing failure to debug in the field. */
static int write_psk(const char *ssid, const char *psk)
{
    char path[512];
    if (!psk_path(ssid, path, sizeof path)) {
        NN_LOG_ERR("ssid too long / unrepresentable");
        return -EINVAL;
    }
    if (mkdir(IWD_DIR, 0700) != 0 && errno != EEXIST) {
        NN_LOG_ERR("mkdir %s: %d", IWD_DIR, errno);
        return -EIO;
    }

    char tmp[544];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -EIO;
    fchmod(fileno(f), 0600);          /* it holds the PSK */
    fprintf(f, "[Security]\n");
    if (psk && *psk) fprintf(f, "Passphrase=%s\n", psk);
    fprintf(f, "\n[Settings]\nAutoConnect=true\n");
    int rc = (fflush(f) == 0 && fsync(fileno(f)) == 0) ? 0 : -EIO;
    fclose(f);
    if (rc == 0 && rename(tmp, path) != 0) rc = -EIO;
    if (rc != 0) unlink(tmp);
    return rc;
}

/* ── poll thread ────────────────────────────────────────────────────── */

static void *poll_thread(void *arg)
{
    (void)arg;
    while (true) {
        pthread_mutex_lock(&s_lock);
        bool run = s_running;
        char ifname[IF_NAMESIZE];
        snprintf(ifname, sizeof ifname, "%s", s_ifname);
        pthread_mutex_unlock(&s_lock);
        if (!run) break;

        if (ifname[0] == '\0') {
            if (find_wifi_iface(ifname, sizeof ifname)) {
                pthread_mutex_lock(&s_lock);
                snprintf(s_ifname, sizeof s_ifname, "%s", ifname);
                pthread_mutex_unlock(&s_lock);
                NN_LOG_INF("wifi interface: %s", ifname);
            }
        }

        if (ifname[0]) {
            char ip[16];
            iface_ipv4(ifname, ip, sizeof ip);
            bool up = ip[0] != '\0';

            pthread_mutex_lock(&s_lock);
            bool was = s_connected;
            s_connected = up;
            snprintf(s_ipv4, sizeof s_ipv4, "%s", ip);
            nn_pal_wifi_cb_t cb = s_cb;
            void *user = s_user;
            pthread_mutex_unlock(&s_lock);

            if (cb && up != was) {
                nn_pal_wifi_state_t st;
                memset(&st, 0, sizeof st);
                snprintf(st.ipv4, sizeof st.ipv4, "%s", ip);
                st.rssi_dbm = iface_rssi(ifname);
                if (up) {
                    /* An address is what callers are actually waiting
                     * for, so report both edges of the same event. */
                    cb(NN_PAL_WIFI_EV_CONNECTED, &st, user);
                    cb(NN_PAL_WIFI_EV_IP_ASSIGNED, &st, user);
                    NN_LOG_INF("wifi up: %s ip=%s rssi=%d", ifname, ip,
                               st.rssi_dbm);
                } else {
                    cb(NN_PAL_WIFI_EV_DISCONNECTED, &st, user);
                    NN_LOG_WRN("wifi down: %s", ifname);
                }
            }
        }

        struct timespec ts = { POLL_MS / 1000, (POLL_MS % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* ── API ────────────────────────────────────────────────────────────── */

int nn_pal_wifi_init(nn_pal_wifi_cb_t cb, void *user)
{
    pthread_mutex_lock(&s_lock);
    if (s_running) { pthread_mutex_unlock(&s_lock); return 0; }
    s_cb = cb;
    s_user = user;
    s_ifname[0] = '\0';
    s_connected = false;
    s_ipv4[0] = '\0';
    s_running = true;
    pthread_mutex_unlock(&s_lock);

    if (!find_wifi_iface(s_ifname, sizeof s_ifname))
        NN_LOG_WRN("no wireless interface yet — will keep looking");

    if (pthread_create(&s_thread, NULL, poll_thread, NULL) != 0) {
        pthread_mutex_lock(&s_lock);
        s_running = false;
        pthread_mutex_unlock(&s_lock);
        return -EIO;
    }
    return 0;
}

int nn_pal_wifi_connect(const char *ssid, const char *psk)
{
    if (!ssid || !*ssid) return -EINVAL;

    int rc = write_psk(ssid, psk);
    if (rc != 0) return rc;

    pthread_mutex_lock(&s_lock);
    snprintf(s_ssid, sizeof s_ssid, "%s", ssid);
    pthread_mutex_unlock(&s_lock);

    /* iwd autoconnects to a network it has a provisioning file for, but
     * only once it has seen it in a scan.  Nudge it so provisioning does
     * not appear to hang for a scan interval; failure is not fatal. */
    if (s_ifname[0]) {
        char cmd[256];
        snprintf(cmd, sizeof cmd,
                 "iwctl station %s scan >/dev/null 2>&1", s_ifname);
        if (system(cmd) != 0)
            NN_LOG_DBG("scan nudge failed (autoconnect will still run)");
    }
    NN_LOG_INF("wifi credentials stored for '%s' — waiting for iwd", ssid);
    return 0;
}

int nn_pal_wifi_disconnect(void)
{
    if (!s_ifname[0]) return 0;
    char cmd[256];
    snprintf(cmd, sizeof cmd, "iwctl station %s disconnect >/dev/null 2>&1",
             s_ifname);
    return system(cmd) == 0 ? 0 : -EIO;
}

int nn_pal_wifi_get_state(nn_pal_wifi_state_t *out)
{
    if (!out) return -EINVAL;
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&s_lock);
    snprintf(out->ipv4, sizeof out->ipv4, "%s", s_ipv4);
    char ifname[IF_NAMESIZE];
    snprintf(ifname, sizeof ifname, "%s", s_ifname);
    pthread_mutex_unlock(&s_lock);
    if (ifname[0]) out->rssi_dbm = iface_rssi(ifname);
    return 0;
}

bool nn_pal_wifi_is_connected(void)
{
    pthread_mutex_lock(&s_lock);
    bool c = s_connected;
    pthread_mutex_unlock(&s_lock);
    return c;
}
