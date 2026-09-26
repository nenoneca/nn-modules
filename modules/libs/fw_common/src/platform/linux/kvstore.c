/* SPDX-License-Identifier: Apache-2.0 */

/* Linux/POSIX kvstore implementation: one binary file per key under
 * ${NN_GW_STATE_DIR:-$HOME/.local/state/nn-gw/kv}/<key>.  Forward
 * slashes inside keys become subdirectories.
 *
 * Use cases (Linux gateway daemon):
 *   - persist X25519 device priv across restarts
 *   - persist hub pubkey learned via BLE
 *   - small enough fits in a directory tree, no atomic-write needed
 *     beyond rename().
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <fw_common/kvstore.h>
#include <fw_common/log.h>

LOG_MODULE_REGISTER(fw_kv, LOG_LEVEL_INF);

#define MAX_REGISTRATIONS 8
#define MAX_KEYLEN 96

struct fw_kv_slot {
	char         prefix[MAX_KEYLEN];
	fw_kv_load_cb cb;
	void         *user;
};
static struct fw_kv_slot g_slots[MAX_REGISTRATIONS];
static int g_slot_count;
static char g_root[PATH_MAX];

static int resolve_root(void)
{
	const char *env = getenv("NN_GW_STATE_DIR");
	if (env && env[0]) {
		snprintf(g_root, sizeof(g_root), "%s/kv", env);
	} else {
		const char *xdg = getenv("XDG_STATE_HOME");
		if (xdg && xdg[0]) {
			snprintf(g_root, sizeof(g_root), "%s/nn-gw/kv", xdg);
		} else {
			const char *home = getenv("HOME");
			if (!home) home = "/tmp";
			snprintf(g_root, sizeof(g_root),
				 "%s/.local/state/nn-gw/kv", home);
		}
	}
	return 0;
}

static int mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s", path);
	size_t n = strlen(tmp);
	if (n && tmp[n-1] == '/') tmp[n-1] = '\0';
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
				LOG_ERR("mkdir(%s): %s", tmp, strerror(errno));
				return -errno;
			}
			*p = '/';
		}
	}
	if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
		LOG_ERR("mkdir(%s): %s", tmp, strerror(errno));
		return -errno;
	}
	return 0;
}

int fw_kv_init(void)
{
	if (g_root[0] != '\0') return 0;
	resolve_root();
	return mkdir_p(g_root);
}

int fw_kv_register(const char *prefix, fw_kv_load_cb cb, void *user)
{
	if (!prefix || !cb) return -EINVAL;
	if (g_slot_count >= MAX_REGISTRATIONS) return -ENOMEM;
	struct fw_kv_slot *s = &g_slots[g_slot_count++];
	snprintf(s->prefix, sizeof(s->prefix), "%s", prefix);
	s->cb = cb;
	s->user = user;
	return 0;
}

/* Recursively walk root_dir/<prefix>/, calling cb with the suffix and
 * file content for every regular file found.  rel_suffix is the
 * caller-relative suffix accumulator. */
static int load_walk(const char *full_dir, const char *rel_suffix,
		     fw_kv_load_cb cb, void *user)
{
	DIR *d = opendir(full_dir);
	if (!d) {
		if (errno == ENOENT) return 0;
		LOG_WRN("opendir(%s): %s", full_dir, strerror(errno));
		return -errno;
	}

	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;

		char child[PATH_MAX];
		snprintf(child, sizeof(child), "%s/%s", full_dir, e->d_name);

		struct stat st;
		if (stat(child, &st) != 0) continue;

		if (S_ISDIR(st.st_mode)) {
			char next_suffix[MAX_KEYLEN];
			if (rel_suffix[0] == '\0') {
				snprintf(next_suffix, sizeof(next_suffix), "%s",
					 e->d_name);
			} else {
				snprintf(next_suffix, sizeof(next_suffix), "%s/%s",
					 rel_suffix, e->d_name);
			}
			load_walk(child, next_suffix, cb, user);
			continue;
		}
		if (!S_ISREG(st.st_mode)) continue;

		/* Read file contents and invoke callback. */
		int fd = open(child, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			LOG_WRN("open(%s): %s", child, strerror(errno));
			continue;
		}
		uint8_t buf[256];
		ssize_t n = read(fd, buf, sizeof(buf));
		close(fd);
		if (n < 0) continue;

		char suffix[MAX_KEYLEN];
		if (rel_suffix[0] == '\0') {
			snprintf(suffix, sizeof(suffix), "%s", e->d_name);
		} else {
			snprintf(suffix, sizeof(suffix), "%s/%s", rel_suffix,
				 e->d_name);
		}
		cb(suffix, buf, (size_t)n, user);
	}
	closedir(d);
	return 0;
}

int fw_kv_load_all(void)
{
	fw_kv_init();
	for (int i = 0; i < g_slot_count; i++) {
		char full[PATH_MAX];
		snprintf(full, sizeof(full), "%s/%s", g_root, g_slots[i].prefix);
		load_walk(full, "", g_slots[i].cb, g_slots[i].user);
	}
	return 0;
}

int fw_kv_save(const char *key, const void *data, size_t len)
{
	fw_kv_init();

	char full[PATH_MAX], tmpfile[PATH_MAX];
	snprintf(full,    sizeof(full),    "%s/%s",     g_root, key);
	snprintf(tmpfile, sizeof(tmpfile), "%s/%s.tmp", g_root, key);

	/* Make sure the parent dir exists. */
	char parent[PATH_MAX];
	snprintf(parent, sizeof(parent), "%s", full);
	char *slash = strrchr(parent, '/');
	if (slash) {
		*slash = '\0';
		mkdir_p(parent);
	}

	int fd = open(tmpfile, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		LOG_ERR("open(%s): %s", tmpfile, strerror(errno));
		return -errno;
	}
	ssize_t off = 0;
	while (off < (ssize_t)len) {
		ssize_t n = write(fd, (const uint8_t *)data + off, len - off);
		if (n < 0) {
			int e = errno;
			close(fd);
			unlink(tmpfile);
			return -e;
		}
		off += n;
	}
	close(fd);

	if (rename(tmpfile, full) != 0) {
		int e = errno;
		LOG_ERR("rename(%s -> %s): %s", tmpfile, full, strerror(e));
		unlink(tmpfile);
		return -e;
	}
	return 0;
}

int fw_kv_delete(const char *key)
{
	fw_kv_init();
	char full[PATH_MAX];
	snprintf(full, sizeof(full), "%s/%s", g_root, key);
	if (unlink(full) != 0) {
		if (errno == ENOENT) return -ENOENT;
		LOG_WRN("unlink(%s): %s", full, strerror(errno));
		return -errno;
	}
	return 0;
}
