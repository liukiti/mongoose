// Copyright (c) 2023 Cesanta Software Limited
// All rights reserved

#include "net.h"

// Authenticated user.
// A user can be authenticated by:
//   - a name:pass pair, passed in a header Authorization: Basic .....
//   - an access_token, passed in a header Cookie: access_token=....
// When a user is shown a login screen, she enters a user:pass. If successful,
// a server responds with a http-only access_token cookie set.
struct user {
  const char *name, *pass, *access_token;
};

// Settings
struct settings {
  bool log_enabled;
  int log_level;
  long brightness;
  char *device_name;
};

static struct settings s_settings = {true, 1, 57, NULL};

static const char *s_json_header =
    "Content-Type: application/json\r\n"
    "Cache-Control: no-cache\r\n";
static uint64_t s_boot_timestamp = 0;  // Updated by SNTP

// Certificate generation procedure:
// openssl ecparam -name prime256v1 -genkey -noout -out key.pem
// openssl req -new -key key.pem -x509 -nodes -days 3650 -out cert.pem
static const char *s_tls_cert =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBCTCBsAIJAK9wbIDkHnAoMAoGCCqGSM49BAMCMA0xCzAJBgNVBAYTAklFMB4X\n"
    "DTIzMDEyOTIxMjEzOFoXDTMzMDEyNjIxMjEzOFowDTELMAkGA1UEBhMCSUUwWTAT\n"
    "BgcqhkjOPQIBBggqhkjOPQMBBwNCAARzSQS5OHd17lUeNI+6kp9WYu0cxuEIi/JT\n"
    "jphbCmdJD1cUvhmzM9/phvJT9ka10Z9toZhgnBq0o0xfTQ4jC1vwMAoGCCqGSM49\n"
    "BAMCA0gAMEUCIQCe0T2E0GOiVe9KwvIEPeX1J1J0T7TNacgR0Ya33HV9VgIgNvdn\n"
    "aEWiBp1xshs4iz6WbpxrS1IHucrqkZuJLfNZGZI=\n"
    "-----END CERTIFICATE-----\n";

static const char *s_tls_key =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEICBz3HOkQLPBDtdknqC7k1PNsWj6HfhyNB5MenfjmqiooAoGCCqGSM49\n"
    "AwEHoUQDQgAEc0kEuTh3de5VHjSPupKfVmLtHMbhCIvyU46YWwpnSQ9XFL4ZszPf\n"
    "6YbyU/ZGtdGfbaGYYJwatKNMX00OIwtb8A==\n"
    "-----END EC PRIVATE KEY-----\n";

// This is for newlib and TLS (mbedTLS)
uint64_t mg_now(void) {
  return mg_millis() + s_boot_timestamp;
}

int ui_event_next(int no, struct ui_event *e) {
  if (no < 0 || no >= MAX_EVENTS_NO) return 0;

  srand(no);
  e->type = (uint8_t) rand() % 4;
  e->prio = (uint8_t) rand() % 3;
  e->timestamp =
      (unsigned long) (mg_now() - 86400 * 1000 /* one day back */ +
                       no * 300 * 1000 /* 5 mins between alerts */ +
                       1000 * (rand() % 300) /* randomize event time */) /
      1000;

  mg_snprintf(e->text, MAX_EVENT_TEXT_SIZE, "event#%d", no);
  return no + 1;
}

// SNTP connection event handler. When we get a response from an SNTP server,
// adjust s_boot_timestamp. We'll get a valid time from that point on
static void sfn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
  uint64_t *expiration_time = (uint64_t *) c->data;
  if (ev == MG_EV_OPEN) {
    *expiration_time = mg_millis() + 3000;  // Store expiration time in 3s
  } else if (ev == MG_EV_SNTP_TIME) {
    uint64_t t = *(uint64_t *) ev_data;
    s_boot_timestamp = t - mg_millis();
    c->is_closing = 1;
  } else if (ev == MG_EV_POLL) {
    if (mg_millis() > *expiration_time) c->is_closing = 1;
  }
  (void) fn_data;
}

static void timer_sntp_fn(void *param) {  // SNTP timer function. Sync up time
  mg_sntp_connect(param, "udp://time.google.com:123", sfn, NULL);
}

// Parse HTTP requests, return authenticated user or NULL
static struct user *authenticate(struct mg_http_message *hm) {
  // In production, make passwords strong and tokens randomly generated
  // In this example, user list is kept in RAM. In production, it can
  // be backed by file, database, or some other method.
  static struct user users[] = {
      {"admin", "admin", "admin_token"},
      {"user1", "user1", "user1_token"},
      {"user2", "user2", "user2_token"},
      {NULL, NULL, NULL},
  };
  char user[64], pass[64];
  struct user *u, *result = NULL;
  mg_http_creds(hm, user, sizeof(user), pass, sizeof(pass));
  MG_INFO(("user [%s] pass [%s]", user, pass));

  if (user[0] != '\0' && pass[0] != '\0') {
    // Both user and password is set, search by user/password
    for (u = users; result == NULL && u->name != NULL; u++)
      if (strcmp(user, u->name) == 0 && strcmp(pass, u->pass) == 0) result = u;
  } else if (user[0] == '\0') {
    // Only password is set, search by token
    for (u = users; result == NULL && u->name != NULL; u++)
      if (strcmp(pass, u->access_token) == 0) result = u;
  }
  return result;
}

static void handle_login(struct mg_connection *c, struct user *u) {
  char cookie[256];
  mg_snprintf(cookie, sizeof(cookie),
              "Set-Cookie: access_token=%s; Path=/; "
              "%sHttpOnly; SameSite=Lax; Max-Age=%d\r\n",
              u->access_token, c->is_tls ? "Secure; " : "", 3600 * 24);
  mg_http_reply(c, 200, cookie, "{%m:%m}", MG_ESC("user"), MG_ESC(u->name));
}

static void handle_logout(struct mg_connection *c) {
  char cookie[256];
  mg_snprintf(cookie, sizeof(cookie),
              "Set-Cookie: access_token=; Path=/; "
              "Expires=Thu, 01 Jan 1970 00:00:00 UTC; "
              "%sHttpOnly; Max-Age=0; \r\n",
              c->is_tls ? "Secure; " : "");
  mg_http_reply(c, 200, cookie, "true\n");
}

static void handle_debug(struct mg_connection *c, struct mg_http_message *hm) {
  int level = mg_json_get_long(hm->body, "$.level", MG_LL_DEBUG);
  mg_log_set(level);
  mg_http_reply(c, 200, "", "Debug level set to %d\n", level);
}

static size_t print_int_arr(void (*out)(char, void *), void *ptr, va_list *ap) {
  size_t len = 0, num = va_arg(*ap, size_t);  // Number of items in the array
  int *arr = va_arg(*ap, int *);              // Array ptr
  for (size_t i = 0; i < num; i++) {
    len += mg_xprintf(out, ptr, "%s%d", i == 0 ? "" : ",", arr[i]);
  }
  return len;
}

static void handle_stats_get(struct mg_connection *c) {
  int points[] = {21, 22, 22, 19, 18, 20, 23, 23, 22, 22, 22, 23, 22};
  mg_http_reply(c, 200, s_json_header, "{%m:%d,%m:%d,%m:[%M]}",
                MG_ESC("temperature"), 21,  //
                MG_ESC("humidity"), 67,     //
                MG_ESC("points"), print_int_arr,
                sizeof(points) / sizeof(points[0]), points);
}

static size_t print_events(void (*out)(char, void *), void *ptr, va_list *ap) {
  size_t len = 0;
  struct ui_event ev;
  int pageno = va_arg(*ap, unsigned);
  int no = (pageno - 1) * EVENTS_PER_PAGE;
  int end = no + EVENTS_PER_PAGE;

  while ((no = ui_event_next(no, &ev)) != 0 && no <= end) {
    len += mg_xprintf(out, ptr, "%s{%m:%lu,%m:%d,%m:%d,%m:%m}",  //
                      len == 0 ? "" : ",",                       //
                      MG_ESC("time"), ev.timestamp,              //
                      MG_ESC("type"), ev.type,                   //
                      MG_ESC("prio"), ev.prio,                   //
                      MG_ESC("text"), MG_ESC(ev.text));
  }

  return len;
}

static void handle_events_get(struct mg_connection *c,
                              struct mg_http_message *hm) {
  int pageno = mg_json_get_long(hm->body, "$.page", 1);
  mg_http_reply(c, 200, s_json_header, "{%m:[%M], %m:%d}", MG_ESC("arr"),
                print_events, pageno, MG_ESC("totalCount"), MAX_EVENTS_NO);
}

static void handle_settings_set(struct mg_connection *c, struct mg_str body) {
  struct settings settings;
  memset(&settings, 0, sizeof(settings));
  mg_json_get_bool(body, "$.log_enabled", &settings.log_enabled);
  settings.log_level = mg_json_get_long(body, "$.log_level", 0);
  settings.brightness = mg_json_get_long(body, "$.brightness", 0);
  char *s = mg_json_get_str(body, "$.device_name");
  if (s && strlen(s) < MAX_DEVICE_NAME) {
    free(settings.device_name);
    settings.device_name = s;
  } else {
    free(s);
  }

  // Save to the device flash
  s_settings = settings;
  bool ok = true;
  mg_http_reply(c, 200, s_json_header,
                "{%m:%s,%m:%m}",                          //
                MG_ESC("status"), ok ? "true" : "false",  //
                MG_ESC("message"), MG_ESC(ok ? "Success" : "Failed"));
}

static void handle_settings_get(struct mg_connection *c) {
  mg_http_reply(c, 200, s_json_header, "{%m:%s,%m:%hhu,%m:%hhu,%m:%m}",  //
                MG_ESC("log_enabled"),
                s_settings.log_enabled ? "true" : "false",    //
                MG_ESC("log_level"), s_settings.log_level,    //
                MG_ESC("brightness"), s_settings.brightness,  //
                MG_ESC("device_name"), MG_ESC(s_settings.device_name));
}

// HTTP request handler function
static void fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
  if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    struct user *u = authenticate(hm);

    if (mg_http_match_uri(hm, "/api/#") && u == NULL) {
      mg_http_reply(c, 403, "", "Not Authorised\n");
    } else if (mg_http_match_uri(hm, "/api/login")) {
      handle_login(c, u);
    } else if (mg_http_match_uri(hm, "/api/logout")) {
      handle_logout(c);
    } else if (mg_http_match_uri(hm, "/api/debug")) {
      handle_debug(c, hm);
    } else if (mg_http_match_uri(hm, "/api/stats/get")) {
      handle_stats_get(c);
    } else if (mg_http_match_uri(hm, "/api/events/get")) {
      handle_events_get(c, hm);
    } else if (mg_http_match_uri(hm, "/api/settings/get")) {
      handle_settings_get(c);
    } else if (mg_http_match_uri(hm, "/api/settings/set")) {
      handle_settings_set(c, hm->body);
    } else {
      struct mg_http_serve_opts opts;
      memset(&opts, 0, sizeof(opts));
#if MG_ENABLE_PACKED_FS
      opts.root_dir = "/web_root";
      opts.fs = &mg_fs_packed;
#else
      opts.root_dir = "web_root";
#endif
      mg_http_serve_dir(c, ev_data, &opts);
    }
    MG_DEBUG(("%lu %.*s %.*s -> %.*s", c->id, (int) hm->method.len,
              hm->method.ptr, (int) hm->uri.len, hm->uri.ptr, (int) 3,
              &c->send.buf[9]));
  }
  (void) fn_data;
}

void web_init(struct mg_mgr *mgr) {
  struct mg_tls_opts opts = {0};
  opts.server_cert = mg_str(s_tls_cert);
  opts.server_key = mg_str(s_tls_key);
  mg_tls_ctx_init(mgr, &opts);

  s_settings.device_name = strdup("My Device");

  mg_http_listen(mgr, HTTP_URL, fn, NULL);
  mg_http_listen(mgr, HTTPS_URL, fn, NULL);

  // mg_timer_add(c->mgr, 1000, MG_TIMER_REPEAT, timer_mqtt_fn, c->mgr);
  mg_timer_add(mgr, 3600 * 1000, MG_TIMER_RUN_NOW | MG_TIMER_REPEAT,
               timer_sntp_fn, mgr);
}
