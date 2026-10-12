/*
 * addon_xtream.h — Xtream Codes IPTV provider client for EVO Player (#93).
 */
#ifndef ADDON_XTREAM_H
#define ADDON_XTREAM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "evo_addon.h"
#include "evo_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xtream_config {
    char host[128];
    int  port;
    char username[64];
    char password[64];
    bool use_https;
    char stream_format[16]; /* "m3u8" (default) or "ts" */
    bool is_connected;
    char status[32];        /* "Active", etc. */
    char exp_date[32];
    char server_name[64];
} xtream_config_t;

/* Initialize Xtream client and load persistent credentials from xtream.conf */
int  xtream_init(void);

/* Save Xtream configuration to disk */
int  xtream_save_config(void);

/* Retrieve active configuration pointer */
xtream_config_t *xtream_get_config(void);

/* Update server connection settings */
void xtream_set_server(const char *host, int port, const char *username, const char *password, bool use_https);

/* Set preferred stream format ("m3u8" or "ts") */
void xtream_set_stream_format(const char *fmt);

typedef void (*xtream_auth_cb)(int success, const char *msg, void *userdata);

/* Authenticate with Xtream server asynchronously */
int  xtream_connect_async(xtream_auth_cb callback, void *userdata);

extern const evo_provider_t evo_provider_xtream;

/* 1 when this stream URL is the series episode resolved last. */
int  xtream_is_playing_episode(const char *url);
void xtream_note_playing(const char *item_id, const char *url);
/* The episode before / after it (dir -1 / +1), into the next / previous season.
 * 0 found, -1 none, -3 the series is being fetched - ask again shortly. */
int  xtream_episode_neighbor(int dir, char *item_id, size_t id_sz, char *title, size_t title_sz);

/* Settings > Clear cache: the lists kept on disk. */
void xtream_clear_cache(void);

/* Several accounts: how many there are, and forgetting one ("acct:<n>" rows). */
int  xtream_account_count(void);
int  xtream_remove_account(int id);

/* TV guide grid: a live channel's guide id and days of catch-up.
 * 0 when item_id is a live channel. */
int  xtream_channel_info(const char *item_id, char *epg_id, size_t esz, int *archive_days);

#ifdef __cplusplus
}
#endif

#endif /* ADDON_XTREAM_H */
