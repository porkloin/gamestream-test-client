/* What machine this is and how it can decode: `gamestream-test-client probe` prints it as one JSON
 * object, and --decode hw picks its VAAPI device from the same GPU list.
 *
 * Everything is read from the kernel and the drivers, not other tools: /proc and sysfs for the
 * machine and its links, nl80211 for a Wi-Fi link, libva for each render node's driver and what it
 * decodes (each render node this process can open), and the PCI database for GPUs the driver doesn't
 * name. With a sysfs root other than /sys (a host's, mounted into a container) the links are that
 * machine's, but Wi-Fi details aren't. */
#include <dirent.h>
#include <stdarg.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <linux/nl80211.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <va/va.h>
#include <va/va_drm.h>

#include "test_client.h"

/* snprintf that says whether it all fit: a cut-off path or name is no path or name. */
__attribute__((format(printf, 3, 4))) static bool fmt(char *out, size_t size, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	int n = vsnprintf(out, size, format, args);
	va_end(args);
	if (n < 0 || (size_t)n >= size) {
		out[0] = '\0';
		return false;
	}
	return true;
}

/* The first line of a file, without its newline (and NULs, for device-tree strings); "" if it
 * can't be read. */
static void read_line(const char *path, char *out, size_t size)
{
	out[0] = '\0';
	FILE *f = fopen(path, "r");
	if (f == NULL)
		return;
	size_t n = fread(out, 1, size - 1, f);
	fclose(f);
	out[n] = '\0';
	for (size_t i = 0; i < n; i++)
		if (out[i] == '\n' || out[i] == '\0') {
			out[i] = '\0';
			break;
		}
}

static long read_long(const char *path)
{
	char s[64];
	read_line(path, s, sizeof s);
	return s[0] ? strtol(s, NULL, 0) : -1;
}

/* Vendors leave these in DMI when they don't fill it in. */
static bool placeholder(const char *s)
{
	return s[0] == '\0' || strncmp(s, "To Be Filled", 12) == 0 || strcmp(s, "System Product Name") == 0 ||
	       strcmp(s, "Default string") == 0;
}

static void machine_model(const char *sysfs, char *out, size_t size)
{
	char path[512];
	fmt(path, sizeof path, "%s/devices/virtual/dmi/id/product_name", sysfs);
	read_line(path, out, size);
	if (!placeholder(out))
		return;
	fmt(path, sizeof path, "%s/devices/virtual/dmi/id/board_name", sysfs);
	read_line(path, out, size);
	if (!placeholder(out))
		return;
	fmt(path, sizeof path, "%s/firmware/devicetree/base/model", sysfs);
	read_line(path, out, size);
}

/* The value of the first "KEY<spaces>: value" line in /proc/cpuinfo or /proc/meminfo. */
static void proc_field(const char *path, const char *key, char *out, size_t size)
{
	out[0] = '\0';
	FILE *f = fopen(path, "r");
	if (f == NULL)
		return;
	char line[512];
	size_t k = strlen(key);
	while (fgets(line, sizeof line, f) != NULL) {
		if (strncmp(line, key, k) != 0 || (line[k] != ' ' && line[k] != '\t' && line[k] != ':'))
			continue;
		char *v = strchr(line, ':');
		if (v == NULL)
			continue;
		for (v++; *v == ' ' || *v == '\t'; v++)
			;
		fmt(out, size, "%s", v);
		out[strcspn(out, "\n")] = '\0';
		break;
	}
	fclose(f);
}

static void os_name(const char *os_release, char *out, size_t size)
{
	out[0] = '\0';
	FILE *f = fopen(os_release, "r");
	if (f == NULL)
		return;
	char line[512];
	while (fgets(line, sizeof line, f) != NULL) {
		if (strncmp(line, "PRETTY_NAME=", 12) != 0)
			continue;
		char *v = line + 12;
		v[strcspn(v, "\n")] = '\0';
		size_t n = strlen(v);
		if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) {
			v[n - 1] = '\0';
			v++;
		}
		fmt(out, size, "%s", v);
		break;
	}
	fclose(f);
}

/* The interface the default route leaves by, from /proc/net/route. */
static bool default_iface(char *out, size_t size)
{
	FILE *f = fopen("/proc/net/route", "r");
	if (f == NULL)
		return false;
	char line[512], iface[IF_NAMESIZE];
	unsigned long dest, mask;
	bool found = false;
	while (fgets(line, sizeof line, f) != NULL)
		if (sscanf(line, "%15s %lx %*x %*x %*d %*d %*d %lx", iface, &dest, &mask) == 3 && dest == 0 && mask == 0) {
			fmt(out, size, "%s", iface);
			found = true;
			break;
		}
	fclose(f);
	return found;
}

/* --- Wi-Fi, from nl80211 ----------------------------------------------------------------------- */

struct wifi {
	bool known;
	int freq_mhz, width_mhz, signal_dbm, standard; /* standard: 4 to 7 for Wi-Fi 4 to 7, 0 unknown */
	double tx_mbps;
	char error[160]; /* why there are no figures, when there aren't */
	/* The query in flight: its replies, the error a dump's last message carried, and the access point the scan
	 * results say the link is associated with. */
	int replies, done_error;
	bool have_ap;
	unsigned char ap[6];
};

static int width_mhz(int w)
{
	switch (w) {
	case NL80211_CHAN_WIDTH_20_NOHT:
	case NL80211_CHAN_WIDTH_20: return 20;
	case NL80211_CHAN_WIDTH_40: return 40;
	case NL80211_CHAN_WIDTH_80: return 80;
	case NL80211_CHAN_WIDTH_80P80:
	case NL80211_CHAN_WIDTH_160: return 160;
	case NL80211_CHAN_WIDTH_320: return 320;
	default: return 0;
	}
}

static int on_interface(struct nl_msg *msg, void *arg)
{
	struct wifi *w = arg;
	w->replies++;
	struct nlattr *a[NL80211_ATTR_MAX + 1];
	struct genlmsghdr *h = nlmsg_data(nlmsg_hdr(msg));
	nla_parse(a, NL80211_ATTR_MAX, genlmsg_attrdata(h, 0), genlmsg_attrlen(h, 0), NULL);
	if (a[NL80211_ATTR_WIPHY_FREQ])
		w->freq_mhz = (int)nla_get_u32(a[NL80211_ATTR_WIPHY_FREQ]);
	if (a[NL80211_ATTR_CHANNEL_WIDTH])
		w->width_mhz = width_mhz((int)nla_get_u32(a[NL80211_ATTR_CHANNEL_WIDTH]));
	return NL_SKIP;
}

static int on_station(struct nl_msg *msg, void *arg)
{
	struct wifi *w = arg;
	struct nlattr *a[NL80211_ATTR_MAX + 1], *sta[NL80211_STA_INFO_MAX + 1], *rate[NL80211_RATE_INFO_MAX + 1];
	struct genlmsghdr *h = nlmsg_data(nlmsg_hdr(msg));
	w->replies++;
	nla_parse(a, NL80211_ATTR_MAX, genlmsg_attrdata(h, 0), genlmsg_attrlen(h, 0), NULL);
	if (!a[NL80211_ATTR_STA_INFO] || nla_parse_nested(sta, NL80211_STA_INFO_MAX, a[NL80211_ATTR_STA_INFO], NULL))
		return NL_SKIP;
	w->known = true;
	if (sta[NL80211_STA_INFO_SIGNAL])
		w->signal_dbm = (int8_t)nla_get_u8(sta[NL80211_STA_INFO_SIGNAL]);
	if (sta[NL80211_STA_INFO_TX_BITRATE] &&
	    !nla_parse_nested(rate, NL80211_RATE_INFO_MAX, sta[NL80211_STA_INFO_TX_BITRATE], NULL)) {
		if (rate[NL80211_RATE_INFO_BITRATE32])
			w->tx_mbps = nla_get_u32(rate[NL80211_RATE_INFO_BITRATE32]) / 10.0;
		w->standard = rate[NL80211_RATE_INFO_EHT_MCS]   ? 7
			      : rate[NL80211_RATE_INFO_HE_MCS]  ? 6
			      : rate[NL80211_RATE_INFO_VHT_MCS] ? 5
			      : rate[NL80211_RATE_INFO_MCS]     ? 4
								: 0;
	}
	return NL_SKIP;
}

/* The scan result the link is associated with: its address, the MLD's for a multi-link (Wi-Fi 7) connection. */
static int on_scan(struct nl_msg *msg, void *arg)
{
	struct wifi *w = arg;
	struct nlattr *a[NL80211_ATTR_MAX + 1], *bss[NL80211_BSS_MAX + 1];
	struct genlmsghdr *h = nlmsg_data(nlmsg_hdr(msg));
	w->replies++;
	nla_parse(a, NL80211_ATTR_MAX, genlmsg_attrdata(h, 0), genlmsg_attrlen(h, 0), NULL);
	if (!a[NL80211_ATTR_BSS] || nla_parse_nested(bss, NL80211_BSS_MAX, a[NL80211_ATTR_BSS], NULL) ||
	    !bss[NL80211_BSS_STATUS] || nla_get_u32(bss[NL80211_BSS_STATUS]) != NL80211_BSS_STATUS_ASSOCIATED)
		return NL_SKIP;
	struct nlattr *addr = bss[NL80211_BSS_MLD_ADDR] ? bss[NL80211_BSS_MLD_ADDR] : bss[NL80211_BSS_BSSID];
	if (addr != NULL && nla_len(addr) == 6) {
		memcpy(w->ap, nla_data(addr), 6);
		w->have_ap = true;
	}
	return NL_SKIP;
}

/* A dump's last message carries the error that ended it, which libnl otherwise drops. */
static int on_done(struct nl_msg *msg, void *arg)
{
	struct wifi *w = arg;
	struct nlmsghdr *n = nlmsg_hdr(msg);
	if (nlmsg_datalen(n) >= (int)sizeof(int))
		w->done_error = *(int *)nlmsg_data(n);
	return NL_STOP;
}

/* One nl80211 request about the interface (with the station's address when MAC isn't NULL); false, with the reason in
 * w->error, when it failed. */
static bool nl80211_query(struct nl_sock *sk, int family, int cmd, int flags, unsigned ifindex, const unsigned char *mac,
			  nl_recvmsg_msg_cb_t cb, struct wifi *w, const char *what)
{
	struct nl_msg *msg = nlmsg_alloc();
	if (msg == NULL)
		return false;
	genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, family, 0, flags, (uint8_t)cmd, 0);
	nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifindex);
	if (mac != NULL)
		nla_put(msg, NL80211_ATTR_MAC, 6, mac);
	w->replies = 0;
	w->done_error = 0;
	nl_socket_modify_cb(sk, NL_CB_VALID, NL_CB_CUSTOM, cb, w);
	nl_socket_modify_cb(sk, NL_CB_FINISH, NL_CB_CUSTOM, on_done, w);
	int err = nl_send_auto(sk, msg);
	if (err >= 0)
		err = nl_recvmsgs_default(sk);
	nlmsg_free(msg);
	if (err < 0)
		fmt(w->error, sizeof w->error, "%s: %s", what, nl_geterror(err));
	else if (w->done_error < 0)
		fmt(w->error, sizeof w->error, "%s: %s", what, strerror(-w->done_error));
	else if (w->replies == 0)
		fmt(w->error, sizeof w->error, "%s: no replies", what);
	else
		return true;
	return false;
}

static struct wifi wifi_link(const char *iface)
{
	struct wifi w = {0};
	unsigned ifindex = if_nametoindex(iface);
	struct nl_sock *sk = nl_socket_alloc();
	int err;
	if (ifindex == 0 || sk == NULL || (err = genl_connect(sk)) < 0) {
		fmt(w.error, sizeof w.error, "%s", ifindex == 0 ? "no such interface" : sk == NULL ? "no netlink socket" : nl_geterror(err));
		nl_socket_free(sk);
		return w;
	}
	/* Each reply is read in full: no acknowledgements on top, or a query's would be left for the next to trip on. */
	nl_socket_disable_auto_ack(sk);
	int family = genl_ctrl_resolve(sk, "nl80211");
	if (family >= 0) {
		nl80211_query(sk, family, NL80211_CMD_GET_INTERFACE, 0, ifindex, NULL, on_interface, &w, "interface");
		/* The station dump, else as `iw link` does: the associated access point from the scan results, then that
		 * station by its address. */
		nl80211_query(sk, family, NL80211_CMD_GET_STATION, NLM_F_DUMP, ifindex, NULL, on_station, &w, "station dump");
		if (!w.known && nl80211_query(sk, family, NL80211_CMD_GET_SCAN, NLM_F_DUMP, ifindex, NULL, on_scan, &w, "scan dump")) {
			if (!w.have_ap)
				fmt(w.error, sizeof w.error, "no associated access point in the scan results");
			else
				nl80211_query(sk, family, NL80211_CMD_GET_STATION, 0, ifindex, w.ap, on_station, &w, "station");
		}
	} else {
		fmt(w.error, sizeof w.error, "nl80211: %s", nl_geterror(family));
	}
	nl_socket_free(sk);
	return w;
}

/* --- GPUs -------------------------------------------------------------------------------------- */

/* The PCI database's names for a device: "AMD" and "Radeon Graphics (Granite Ridge)" from
 * "Advanced Micro Devices, Inc. [AMD/ATI]" and "Granite Ridge [Radeon Graphics]". */
static bool pci_name(unsigned vendor, unsigned device, char *out, size_t size)
{
	static const char *const dbs[] = {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids"};
	FILE *f = NULL;
	for (size_t i = 0; i < sizeof dbs / sizeof *dbs && f == NULL; i++)
		f = fopen(dbs[i], "r");
	if (f == NULL)
		return false;
	char line[512], vname[256] = "";
	bool in_vendor = false, found = false;
	while (fgets(line, sizeof line, f) != NULL) {
		line[strcspn(line, "\n")] = '\0';
		unsigned id;
		if (line[0] != '\t' && line[0] != '#' && sscanf(line, "%4x", &id) == 1) {
			if (in_vendor)
				break;
			in_vendor = id == vendor;
			if (in_vendor)
				fmt(vname, sizeof vname, "%s", line + 6);
		} else if (in_vendor && line[0] == '\t' && line[1] != '\t' && sscanf(line + 1, "%4x", &id) == 1 &&
			   id == device) {
			/* The vendor's short name: the bracket's first part, else without "Corporation". */
			char *b = strchr(vname, '[');
			if (b != NULL) {
				memmove(vname, b + 1, strlen(b));
				vname[strcspn(vname, "/]")] = '\0';
			} else {
				char *c = strstr(vname, " Corporation");
				if (c != NULL)
					*c = '\0';
			}
			char *d = line + 7, *open = strchr(d, '['), *close = open ? strchr(open, ']') : NULL;
			if (open != NULL && close != NULL) {
				*close = '\0';
				while (open > d && open[-1] == ' ')
					open--;
				char chip[256];
				fmt(chip, sizeof chip, "%.*s", (int)(open - d), d);
				fmt(out, size, "%s %s%s%s%s", vname, strchr(open, '[') + 1, chip[0] ? " (" : "", chip,
					 chip[0] ? ")" : "");
			} else {
				fmt(out, size, "%s %s", vname, d);
			}
			found = true;
			break;
		}
	}
	fclose(f);
	return found;
}

/* libva's notes go nowhere. */
static void va_quiet(void *context, const char *message)
{
}

/* What the render node's VAAPI driver is called and decodes. Mesa says "Mesa Gallium driver 26.2.6
 * for AMD Radeon RX 6800 XT (radeonsi, navi21, ...)": the driver is "radeonsi Mesa 26.2.6" and,
 * unless it's the generic "AMD Radeon Graphics", that's the GPU's name. Intel's says "Intel iHD
 * driver for Intel(R) Gen Graphics - 26.3.5 ()": "iHD 26.3.5", and no name. */
static void va_probe(struct gpu *g)
{
	int fd = open(g->device, O_RDWR);
	if (fd < 0)
		return;
	VADisplay dpy = vaGetDisplayDRM(fd);
	vaSetInfoCallback(dpy, va_quiet, NULL);
	vaSetErrorCallback(dpy, va_quiet, NULL);
	int major, minor;
	if (dpy == NULL || vaInitialize(dpy, &major, &minor) != VA_STATUS_SUCCESS) {
		close(fd);
		return;
	}
	const char *vendor = vaQueryVendorString(dpy);
	char version[64], rest[256], who[128];
	if (sscanf(vendor, "Mesa Gallium driver %63s for %255[^\n]", version, rest) == 2) {
		char *paren = strstr(rest, " (");
		if (paren != NULL && sscanf(paren, " (%127[^,)]", who) == 1)
			fmt(g->va_driver, sizeof g->va_driver, "%s Mesa %s", who, version);
		if (paren != NULL)
			*paren = '\0';
		if (strcmp(rest, "AMD Radeon Graphics") != 0)
			fmt(g->name, sizeof g->name, "%s", rest);
	} else if (sscanf(vendor, "Intel iHD driver for %*[^-]- %63s", version) == 1) {
		fmt(g->va_driver, sizeof g->va_driver, "iHD %s", version);
	} else {
		fmt(g->va_driver, sizeof g->va_driver, "%s", vendor);
	}

	int n = vaMaxNumProfiles(dpy), count = 0;
	VAProfile *profiles = calloc((size_t)n, sizeof *profiles);
	VAEntrypoint entries[32];
	if (profiles != NULL && vaQueryConfigProfiles(dpy, profiles, &count) == VA_STATUS_SUCCESS)
		for (int i = 0; i < count; i++) {
			int e = 0;
			if (vaQueryConfigEntrypoints(dpy, profiles[i], entries, &e) != VA_STATUS_SUCCESS)
				continue;
			bool vld = false;
			for (int j = 0; j < e; j++)
				vld |= entries[j] == VAEntrypointVLD;
			if (!vld)
				continue;
			switch (profiles[i]) {
			case VAProfileH264ConstrainedBaseline:
			case VAProfileH264Main:
			case VAProfileH264High: g->decodes |= DECODES_H264; break;
			case VAProfileHEVCMain: g->decodes |= DECODES_HEVC; break;
			case VAProfileHEVCMain10: g->decodes |= DECODES_HEVC10; break;
			case VAProfileAV1Profile0: g->decodes |= DECODES_AV1 | DECODES_AV1_10; break;
			default: break;
			}
		}
	free(profiles);
	vaTerminate(dpy);
	close(fd);
}

int probe_gpus(const char *sysfs, struct gpu *out, int max)
{
	char cpu[256];
	proc_field("/proc/cpuinfo", "model name", cpu, sizeof cpu);
	char dir[512];
	fmt(dir, sizeof dir, "%s/class/drm", sysfs);
	DIR *d = opendir(dir);
	if (d == NULL)
		return 0;
	struct dirent *e;
	int n = 0;
	while ((e = readdir(d)) != NULL && n < max) {
		if (strncmp(e->d_name, "renderD", 7) != 0)
			continue;
		struct gpu *g = &out[n++];
		memset(g, 0, sizeof *g);
		fmt(g->device, sizeof g->device, "/dev/dri/%s", e->d_name);
		char path[768];
		fmt(path, sizeof path, "%s/%s/device/vendor", dir, e->d_name);
		long vendor = read_long(path);
		fmt(path, sizeof path, "%s/%s/device/device", dir, e->d_name);
		long device = read_long(path);
		if (vendor >= 0 && device >= 0)
			fmt(g->pci, sizeof g->pci, "%04lx:%04lx", vendor, device);
		fmt(path, sizeof path, "%s/%s/device/mem_info_vram_total", dir, e->d_name);
		long vram = read_long(path);
		g->vram_mb = vram > 0 ? vram >> 20 : 0;
		if (access(g->device, R_OK | W_OK) == 0)
			va_probe(g);
		/* An integrated GPU's marketing name is the CPU's: the PCI database names it better. */
		if (cpu[0] != '\0' && strcmp(g->name, cpu) == 0)
			g->name[0] = '\0';
		if (g->name[0] == '\0' && vendor >= 0)
			pci_name((unsigned)vendor, (unsigned)device, g->name, sizeof g->name);
	}
	closedir(d);
	/* By name, renderD128 first. */
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && strcmp(out[j - 1].device, out[j].device) > 0; j--) {
			struct gpu t = out[j];
			out[j] = out[j - 1];
			out[j - 1] = t;
		}
	return n;
}

const struct gpu *probe_pick_decoder(const struct gpu *gpus, int n, unsigned needs, const char *avoid_pci)
{
	const struct gpu *best = NULL;
	for (int i = 0; i < n; i++) {
		const struct gpu *g = &gpus[i];
		if ((g->decodes & needs) != needs)
			continue;
		/* Another GPU than the one to keep free, then the one with more video memory. */
		bool avoided = avoid_pci != NULL && strcmp(g->pci, avoid_pci) == 0;
		bool best_avoided = best != NULL && avoid_pci != NULL && strcmp(best->pci, avoid_pci) == 0;
		if (best == NULL || (best_avoided && !avoided) || (best_avoided == avoided && g->vram_mb > best->vram_mb))
			best = g;
	}
	return best;
}

/* --- JSON -------------------------------------------------------------------------------------- */

void json_string(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\')
			fprintf(f, "\\%c", c);
		else if (c < 0x20)
			fprintf(f, "\\u%04x", c);
		else
			fputc(c, f);
	}
	fputc('"', f);
}

static void json_field(FILE *f, const char *name, const char *value, bool *first)
{
	fprintf(f, "%s\"%s\":", *first ? "" : ",", name);
	json_string(f, value);
	*first = false;
}

void probe_write_gpu(FILE *f, const struct gpu *g)
{
	static const struct {
		unsigned bit;
		const char *name;
	} codecs[] = {{DECODES_H264, "h264"}, {DECODES_HEVC, "hevc"}, {DECODES_HEVC10, "hevc10"}, {DECODES_AV1, "av1"}};
	bool first = true;
	fputc('{', f);
	json_field(f, "device", g->device, &first);
	json_field(f, "pci", g->pci, &first);
	json_field(f, "name", g->name, &first);
	json_field(f, "va_driver", g->va_driver, &first);
	fprintf(f, ",\"vram_mb\":%ld,\"decodes\":[", g->vram_mb);
	bool any = false;
	for (size_t i = 0; i < sizeof codecs / sizeof *codecs; i++)
		if (g->decodes & codecs[i].bit) {
			fprintf(f, "%s\"%s\"", any ? "," : "", codecs[i].name);
			any = true;
		}
	fputs("]}", f);
}

/* One link: its kind, speed, and for Wi-Fi in this network namespace, the radio's figures. */
static void write_link(FILE *f, const char *sysfs, const char *iface, bool here)
{
	char path[512];
	fmt(path, sizeof path, "%s/class/net/%s/wireless", sysfs, iface);
	bool wireless = access(path, F_OK) == 0;
	fmt(path, sizeof path, "%s/class/net/%s/speed", sysfs, iface);
	long speed = read_long(path);
	bool first = true;
	fputc('{', f);
	json_field(f, "iface", iface, &first);
	json_field(f, "kind", wireless ? "wifi" : "wired", &first);
	if (speed > 0)
		fprintf(f, ",\"speed_mbps\":%ld", speed);
	if (wireless && here) {
		struct wifi w = wifi_link(iface);
		if (!w.known)
			fprintf(stderr, "probe: no Wi-Fi figures for %s: %s\n", iface, w.error);
		if (w.known) {
			fputs(",\"wifi\":{", f);
			fprintf(f, "\"standard\":%d,\"freq_mhz\":%d,\"width_mhz\":%d,\"signal_dbm\":%d,\"tx_mbps\":%.1f", w.standard,
				w.freq_mhz, w.width_mhz, w.signal_dbm, w.tx_mbps);
			fputc('}', f);
		}
	}
	fputc('}', f);
}

/* The default route's link when sysfs has it (it's this network namespace's, or a container's
 * copy of the host's), else every physical link that's up. */
static void write_network(FILE *f, const char *sysfs)
{
	bool here = strcmp(sysfs, "/sys") == 0;
	char iface[IF_NAMESIZE], path[512];
	if (default_iface(iface, sizeof iface)) {
		fmt(path, sizeof path, "%s/class/net/%s/device", sysfs, iface);
		if (access(path, F_OK) == 0) {
			write_link(f, sysfs, iface, here);
			return;
		}
	}
	fmt(path, sizeof path, "%s/class/net", sysfs);
	DIR *d = opendir(path);
	if (d == NULL)
		return;
	struct dirent *e;
	bool any = false;
	while ((e = readdir(d)) != NULL) {
		char p[768], state[32];
		fmt(p, sizeof p, "%s/%s/device", path, e->d_name);
		if (e->d_name[0] == '.' || access(p, F_OK) != 0)
			continue;
		fmt(p, sizeof p, "%s/%s/operstate", path, e->d_name);
		read_line(p, state, sizeof state);
		if (strcmp(state, "up") != 0)
			continue;
		if (any)
			fputc(',', f);
		write_link(f, sysfs, e->d_name, here);
		any = true;
	}
	closedir(d);
}

int probe_main(int argc, char **argv)
{
	const char *sysfs = "/sys", *os_release = "/etc/os-release";
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--sysfs") == 0 && i + 1 < argc)
			sysfs = argv[++i];
		else if (strcmp(argv[i], "--os-release") == 0 && i + 1 < argc)
			os_release = argv[++i];
		else {
			fprintf(stderr, "usage: gamestream-test-client probe [--sysfs DIR] [--os-release FILE]\n");
			return 2;
		}
	}
	char hostname[256], model[256], cpu[256], mem[64], os[256];
	struct utsname u;
	read_line("/proc/sys/kernel/hostname", hostname, sizeof hostname);
	machine_model(sysfs, model, sizeof model);
	proc_field("/proc/cpuinfo", "model name", cpu, sizeof cpu);
	if (cpu[0] == '\0')
		proc_field("/proc/cpuinfo", "Model", cpu, sizeof cpu);
	proc_field("/proc/meminfo", "MemTotal", mem, sizeof mem);
	os_name(os_release, os, sizeof os);
	uname(&u);

	FILE *f = stdout;
	bool first = true;
	fputc('{', f);
	json_field(f, "hostname", hostname, &first);
	json_field(f, "model", model, &first);
	json_field(f, "cpu", cpu, &first);
	if (mem[0])
		fprintf(f, ",\"memory_gb\":%.0f", strtod(mem, NULL) / 1048576.0);
	json_field(f, "os", os, &first);
	json_field(f, "kernel", u.release, &first);
	fputs(",\"network\":[", f);
	write_network(f, sysfs);
	fputs("],\"gpus\":[", f);
	struct gpu gpus[16];
	int n = probe_gpus(sysfs, gpus, 16);
	for (int i = 0; i < n; i++) {
		if (i)
			fputc(',', f);
		probe_write_gpu(f, &gpus[i]);
	}
	fputs("]}\n", f);
	return 0;
}
