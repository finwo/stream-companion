#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <libgen.h>
#include <pwd.h>
#include <unistd.h>
#endif

#include "finwo/fnet.h"
#include "finwo/http-parser.h"
#include "finwo/http-server.h"
#include "kgabis/parson.h"
#include "tinycthread/tinycthread.h"
#include "webview/webview.h"

#include "fs.h"

#ifndef DIRECTORY_SEPARATOR
#if defined(_WIN32) || defined(_WIN64)
#define DIRECTORY_SEPARATOR "\\"
#else
#define DIRECTORY_SEPARATOR "/"
#endif
#endif

typedef struct {
  int port;
  char *settings_file;
  struct http_server_opts *http_opts;
  webview_t w;
} context_t;

struct llistener {
  void *next;
  char *topic;
  struct fnet_t *conn;
};

struct llistener *listeners = NULL;

#define UNUSED(x) (void)x

char * get_html(const char *name) {
  if (!strcmp("control-ui", name)) {
    return
#include "../tool/control-ui/dist/index.bundled.h"
      ;
  }
  if (!strcmp("overlay-phasmo-tracker", name)) {
    return
#include "../tool/overlay-phasmo-tracker/dist/index.bundled.h"
      ;
  }
  return "";
}

void bound_getSettings(const char *seq, const char *req, void *arg) {
  context_t *context = arg;
  struct buf *settings_contents = file_get_contents(context->settings_file);
  webview_return(context->w, seq, 0, settings_contents->data);
  buf_clear(settings_contents);
  free(settings_contents);
}

void bound_setSettings(const char *seq, const char *req, void *arg) {
  context_t *context = arg;

  JSON_Value *req_parsed = json_parse_string(req);
  if (json_value_get_type(req_parsed) != JSONArray) {
    webview_return(context->w, seq, 1, "new Error('Call error')");
    json_value_free(req_parsed);
    return;
  }

  JSON_Value *req_i0 = json_array_get_value(json_array(req_parsed), 0);
  if (json_value_get_type(req_i0) != JSONObject) {
    json_value_free(req_parsed);
    webview_return(context->w, seq, 1, "new Error('Passed settings must be an object')");
    return;
  }

  json_serialize_to_file_pretty(req_i0, context->settings_file);
  webview_return(context->w, seq, 0, "null");
}

/* void bound_homedir(const char *seq, const char *req, void *arg) { */
/*   UNUSED(req); */
/*   context_t *context = arg; */
/*   JSON_Value *settings_file = json_value_init_string(context->settings_file); */
/*   char *response = json_serialize_to_string_pretty(settings_file); */
/*   webview_return(context->w, seq, 0, response); */
/*   json_free_serialized_string(response); */
/*   json_value_free(settings_file); */
/*   printf("Done...\n"); */
/* } */

void wv_test(const char *seq, const char *req, void *arg);

void onServing(char *addr, uint16_t port, void *udata) {
  printf("Serving at %s:%d\n", addr, port);
}

void onTick(void *udata) {
  struct http_server_opts *opts = udata;

  printf("Tick on %s:%d\n", opts->addr, opts->port);
}

void route_404(struct http_server_reqdata *reqdata) {
  http_parser_header_set(reqdata->reqres->response, "Content-Type", "text/plain");
  reqdata->reqres->response->status     = 404;
  reqdata->reqres->response->body       = calloc(1, sizeof(struct buf));
  reqdata->reqres->response->body->data = strdup("not found\n");
  reqdata->reqres->response->body->len  = strlen(reqdata->reqres->response->body->data);
  http_server_response_send(reqdata, true);
}

void route_get_html(struct http_server_reqdata *reqdata, const char *name) {
  http_parser_header_set(reqdata->reqres->response, "Content-Type", "text/html");
  reqdata->reqres->response->body       = calloc(1, sizeof(struct buf));
  reqdata->reqres->response->body->data = strdup(get_html(name));
  reqdata->reqres->response->body->len  = strlen(reqdata->reqres->response->body->data);
  http_server_response_send(reqdata, true);
}

void route_get_control_ui(struct http_server_reqdata *reqdata) {
  route_get_html(reqdata, "control-ui");
}
void route_get_overlay_phasmo_tracker(struct http_server_reqdata *reqdata) {
  route_get_html(reqdata, "overlay-phasmo-tracker");
}

// Generic GET topic route
void route_get_topic(struct http_server_reqdata *reqdata, const char *topic) {
  struct fnet_t              *conn     = reqdata->connection;
  struct http_parser_message *request  = reqdata->reqres->request;
  struct http_parser_message *response = reqdata->reqres->response;

  // Build response
  const char *origin = http_parser_header_get(request, "Origin");
  response->status = 200;
  http_parser_header_set(response, "Transfer-Encoding"           , "chunked"             );
  http_parser_header_set(response, "Content-Type"                , "application/x-ndjson");
  http_parser_header_set(response, "Access-Control-Allow-Origin" , origin ? origin : "*" );

  // Assign an empty body, we're not doing anything yet
  response->body = calloc(1, sizeof(struct buf));
  response->body->data = strdup("");
  response->body->len  = 0;
  response->body->cap  = 1;

  // Send response
  struct buf *response_buffer = http_parser_sprint_response(response);
  fnet_write(conn, response_buffer);
  buf_clear(response_buffer);
  free(response_buffer);

  // Add the connection to listener list
  struct llistener *listener = malloc(sizeof(struct llistener));
  listener->conn  = conn;
  listener->next  = listeners;
  listener->topic = strdup(topic);
  listeners       = listener;
}

// Generic POST topic route
void route_post_topic(struct http_server_reqdata *reqdata, const char *topic) {
  struct fnet_t              *conn          = reqdata->connection;
  struct http_parser_message *request       = reqdata->reqres->request;
  struct http_parser_message *response      = reqdata->reqres->response;
  struct llistener           *listener      = listeners;
  struct llistener           *listener_prev = NULL;

  // Ensure there's a newline
  buf_append(request->body, "\n", 1);

  // Pre-build chunk
  int chunksize = request->body->len + 64;
  char *chunk = calloc(1, chunksize);
  chunksize = snprintf(chunk, chunksize, "%lx\r\n%s\r\n", request->body->len, request->body->data);

  // Output to all listeners on the topic
  while(listener) {
    // Handle closed connections
    if (listener->conn->status & FNET_STATUS_CLOSED) {
      if (listener_prev) {
        listener_prev->next = listener->next;
        fnet_free(listener->conn);
        free(listener->topic);
        free(listener);
        listener = listener_prev->next;
        continue;
      } else {
        listeners = listener->next;
        fnet_free(listener->conn);
        free(listener->topic);
        free(listener);
        listener = listeners;
        continue;
      }
    }
    // Transmit to listener
    if (!strcmp(listener->topic, topic)) {
      fnet_write(listener->conn, &(struct buf){
          .data = chunk,
          .len  = chunksize,
          .cap  = chunksize,
      });
    }
    // Continue to next listener
    listener_prev = listener;
    listener      = listener->next;
  }

  // And we're done with this memory
  free(chunk);

  // Build response
  const char *origin = http_parser_header_get(request, "Origin");
  response->status = 200;
  http_parser_header_set(response, "Content-Type"                , "application/json"   );
  http_parser_header_set(response, "Access-Control-Allow-Origin" , origin ? origin : "*");
  response->body       = calloc(1, sizeof(struct buf));
  response->body->data = strdup("{\"ok\":true}\n");
  response->body->len  = strlen(response->body->data);
  response->body->cap  = response->body->len + 1;

  // Send response
  struct buf *response_buffer = http_parser_sprint_response(response);
  fnet_write(reqdata->connection, response_buffer);
  buf_clear(response_buffer);
  free(response_buffer);
  fnet_close(conn);
}

void route_get_topic_chat(struct http_server_reqdata *reqdata) {
  return route_get_topic(reqdata, "chat");
}
void route_post_topic_chat(struct http_server_reqdata *reqdata) {
  return route_post_topic(reqdata, "chat");
}

int thread_http(void *arg) {
  context_t *context = arg;

  struct http_server_events evs = {
    .serving  = onServing,
    .close    = NULL,
    .notFound = route_404,
    .tick     = NULL,
  };
  struct http_server_opts opts = {
    .evs  = &evs,
    .addr = "0.0.0.0",
    .port = context->port,
  };

  context->http_opts = &opts;

  http_server_route("GET" , "/control-ui"            , route_get_control_ui            );
  http_server_route("GET" , "/overlay/phasmo-tracker", route_get_overlay_phasmo_tracker);
  http_server_route("GET" , "/topic/chat"            , route_get_topic_chat            );
  http_server_route("POST", "/topic/chat"            , route_post_topic_chat           );
  http_server_main(&opts);
  printf("http server has shut down\n");
  fnet_shutdown();

  printf("http_thread finished\n");
  return 0;
}


int thread_window(void *arg) {
  context_t *context = arg;
  /* char *js = malloc(8192); */

  webview_t w = webview_create(1, NULL);
  context->w = w;
  webview_set_title(w, "Basic Example");
  webview_set_size(w, 480, 720, WEBVIEW_HINT_NONE);

  webview_eval(w, "document.location.href = 'http://localhost:38475/control-ui';");
  webview_bind(w, "_getSettings", bound_getSettings, arg);
  webview_bind(w, "_setSettings", bound_setSettings, arg);
  /* webview_set_html(w, get_html("control-ui")); */
  webview_run(w);
  webview_destroy(w);

  printf("Window has closed...\n");

  // Mark http as done (else it will restart listening)
  if (context->http_opts) {
    context->http_opts->shutdown = true;
  }

  fnet_shutdown();
  printf("wndw_thread finished\n");
  return 0;
}

void wv_test(const char *seq, const char *req, void *arg) {
  context_t *context = (context_t *)arg;
  UNUSED(seq);
  UNUSED(req);
  UNUSED(context);
  printf("Bound fn was called!\nseq: %s\nreq: %s\n", seq, req);
  printf("Old port: %d\n", context->port);
  context->port++;
  printf("New port: %d\n", context->port);
  webview_return(context->w, seq, 0, "null");
}

#ifndef WINTERM
#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR lpCmdLine, int nCmdShow) {
  UNUSED(hInst);
  UNUSED(hPrevInst);
  UNUSED(lpCmdLine);
  UNUSED(nCmdShow);
#else
int main() {
#endif
#else // no WINTERM
int main() {
#endif

  const char *settings_file_template =
    "%s"
    DIRECTORY_SEPARATOR
    ".config"
    DIRECTORY_SEPARATOR
    "finwo"
    DIRECTORY_SEPARATOR
    "stream-companion.json"
    ;

  int i;
  context_t context = {
    .port          = 38475,
    .settings_file = calloc(snprintf(NULL, 0, settings_file_template, homedir()) + 1, 1),
  };
  thrd_t threads[2];

  sprintf(context.settings_file, settings_file_template, homedir());


  /* thrd_create(&threads[0], thread_fnet  , NULL    ); */
  thrd_create(&threads[1], thread_http  , &context);

  // Launch the window on the main thread
  thread_window(&context);

  for(i = 1; i < 2 ; i++) {
    printf("Joining thread %d\n", i);
    thrd_join(threads[i], NULL);
  }

  printf("Main fn finished\n");
  return 0;
}

#ifdef WINTERM
#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR lpCmdLine, int nCmdShow) {
  UNUSED(hInst);
  UNUSED(hPrevInst);
  UNUSED(lpCmdLine);
  UNUSED(nCmdShow);

  return main();
}
#endif
#endif
