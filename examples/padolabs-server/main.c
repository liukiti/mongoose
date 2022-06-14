// Copyright (c) 2020 Cesanta Software Limited
// All rights reserved
//
// Example Websocket server. See https://mongoose.ws/tutorials/websocket-server/

#include "mongoose.h"
#include "ip2str.h"
#include "time.h"

//https://stackoverflow.com/a/65955446/19124287
static const char *s_listen_on = "ws://0.0.0.0:8000";
static const char *s_web_root = ".";

// This RESTful server implements the following endpoints:
//   /websocket - upgrade to Websocket, and implement websocket echo server
//   /api/rest - respond with JSON string {"result": 123}
//   any other URI serves static files from s_web_root
static void fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
  struct mg_mgr *mgr = (struct mg_mgr *) fn_data;

  if (ev == MG_EV_OPEN) {
    // c->is_hexdumping = 1;
  } else if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    if (mg_http_match_uri(hm, "/websocket")) {
      // Upgrade to websocket. From now on, a connection is a full-duplex
      // Websocket connection, which will receive MG_EV_WS_MSG events.
      mg_ws_upgrade(c, hm, NULL);
    } else if (mg_http_match_uri(hm, "/rest")) {
      // Serve REST response
      mg_http_reply(c, 200, "", "{\"result\": %d}\n", 123);
    } else {
      // Serve static files
      struct mg_http_serve_opts opts = {.root_dir = s_web_root};
      mg_http_serve_dir(c, ev_data, &opts);
    }
  } else if (ev == MG_EV_WS_MSG) {
    // Got websocket frame. Received data is wm->data.
    struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;

    //Printing the Client IP address and the received message 
    ip4_addr_t ip_st;
    ip_st.addr = (c->rem.ip);

    time_t now = 0;
    time(&now);
    struct tm *timeinfo = localtime(&now);
    char hora[10];
    
    strftime(hora, sizeof(hora), "%X", timeinfo);
    printf("%.*s\t" IPSTR "\t%.*s\n\r", (int)strlen(hora), hora,IP2STR(&(ip_st)), (int)wm->data.len, 
            wm->data.ptr);
    
    // Broadcast the received message to all connected websocket 
    // clients except the sender.
    //https://mongoose.ws/tutorials/timers/
    for (struct mg_connection *g = mgr->conns; g != NULL; g = g->next) {
      if(g!=c)
        mg_ws_send(g, wm->data.ptr, wm->data.len, WEBSOCKET_OP_TEXT);
    }
  }
  (void) fn_data;
}

int main(void) {
  struct mg_mgr mgr;  // Event manager
  mg_mgr_init(&mgr);  // Initialise event manager
  printf("Starting WS listener on %s/websocket\n", s_listen_on);
  mg_http_listen(&mgr, s_listen_on, fn, &mgr);  // Create HTTP listener
  for (;;) mg_mgr_poll(&mgr, 1000);             // Infinite event loop
  mg_mgr_free(&mgr);
  return 0;
}
