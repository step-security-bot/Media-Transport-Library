/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#include <pthread.h>
#include <st_dpdk_api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct app_context {
  int idx;
  int16_t framebuff_idx;
  uint8_t ready_framebuff[3];
  uint8_t free_framebuff[3];
  int fb_send;
  void* handle;
  bool stop;
  int frame_size;
  pthread_t app_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;
};

static int notify_frame_done(void* priv, uint16_t frame_idx) {
  struct app_context* s = (struct app_context*)priv;
  pthread_mutex_lock(&s->wake_mutex);
  s->free_framebuff[frame_idx] = 1;
  pthread_cond_signal(&s->wake_cond);
  pthread_mutex_unlock(&s->wake_mutex);
  s->fb_send++;
  printf("send frame %d..........\n", s->fb_send);
  return 0;
}

static int tx_next_frame(void* priv, uint16_t* next_frame_idx) {
  struct app_context* s = (struct app_context*)priv;
  int i;
  pthread_mutex_lock(&s->wake_mutex);
  for (i = 0; i < 3; i++) {
    if (s->ready_framebuff[i] == 1) {
      s->framebuff_idx = i;
      s->ready_framebuff[i] = 0;
      break;
    }
  }
  pthread_cond_signal(&s->wake_cond);
  pthread_mutex_unlock(&s->wake_mutex);
  if (i == 3) return -1;
  *next_frame_idx = s->framebuff_idx;
  return 0;
}

static void* app_tx_video_frame_thread(void* arg) {
  struct app_context* s = arg;
  uint16_t i;
  while (!s->stop) {
    pthread_mutex_lock(&s->wake_mutex);
    // guarantee the sequence, let ready buf to be exhausted
    bool has_ready = false;
    for (i = 0; i < 3; i++) {
      if (s->ready_framebuff[i] == 1) {
        has_ready = true;
        break;
      }
    }
    if (has_ready) {
      if (!s->stop) pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }

    for (i = 0; i < 3; i++) {
      if (s->free_framebuff[i] == 1) {
        s->free_framebuff[i] = 0;
        break;
      }
    }
    if (i == 3) {
      if (!s->stop) pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    pthread_mutex_unlock(&s->wake_mutex);
    uint8_t* dst = st20_tx_get_framebuffer(s->handle, i);
    // feed the data, memset 0 as example
    memset(dst, 0, s->frame_size);

    pthread_mutex_lock(&s->wake_mutex);
    s->ready_framebuff[i] = 1;
    pthread_mutex_unlock(&s->wake_mutex);
  }

  return NULL;
}

int main() {
  struct st_init_params param;
  memset(&param, 0, sizeof(param));
  int session_num = 1;
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], "0000:af:00.1", ST_PORT_MAX_LEN);
  uint8_t ip[4] = {192, 168, 0, 2};
  memcpy(param.sip_addr[ST_PORT_P], ip, ST_IP_ADDR_LEN);
  param.flags = ST_FLAG_BIND_NUMA;      // default bind to numa
  param.log_level = ST_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                    // usr ctx pointer
  param.ptp_get_time_fn =
      NULL;  // user regist ptp func, if not regist, the internal pt p will be used
  param.tx_sessions_cnt_max = session_num;
  param.rx_sessions_cnt_max = 0;
  param.lcores =
      NULL;  // didnot indicate lcore; let lib decide to core or user could define it.
  st_handle dev_handle = st_init(&param);
  if (!dev_handle) {
    printf("st_init fail\n");
    return -1;
  }
  st20_tx_handle tx_handle[session_num];
  struct app_context* app[session_num];
  int ret;

  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct app_context*)malloc(sizeof(struct app_context));
    if (!app[i]) {
      printf(" app struct is not correctly malloc");
      return -1;
    }
    app[i]->idx = i;
    struct st20_tx_ops ops_tx;
    ops_tx.name = "st20_test";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.num_port = 1;
    uint8_t ip[4] = {239, 168, 0, 1};
    memcpy(ops_tx.dip_addr[ST_PORT_P], ip, ST_IP_ADDR_LEN);  // tx src ip like 239.0.0.1
    strncpy(ops_tx.port[ST_PORT_P], "0000:af:00.1",
            ST_PORT_MAX_LEN);  // send port interface like 0000:af:00.0
    ops_tx.udp_port[ST_PORT_P] =
        10000 + i;  // user could config the udp port in this interface.
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_tx.payload_type = 112;
    // aligned with frame_tx[3] in app_context, framebuf pool size is 3 in lib.
    ops_tx.framebuff_cnt = 3;
    // app regist non-block func, app could get a frame to send to lib
    ops_tx.get_next_frame = tx_next_frame;
    // app regist non-block func, app could get the frame tx done
    ops_tx.notify_frame_done = notify_frame_done;
    tx_handle[i] = st20_tx_create(dev_handle, &ops_tx);
    if (!tx_handle[i]) {
      printf(" tx_session is not correctly created");
      free(app[i]);
      return -1;
    }
    app[i]->handle = tx_handle[i];
    app[i]->stop = false;
    memset(app[i]->ready_framebuff, 0, sizeof(app[i]->ready_framebuff));
    memset(app[i]->free_framebuff, 1, sizeof(app[i]->free_framebuff));
    struct st20_pgroup st20_pg;
    ret = st20_get_pgroup(ops_tx.fmt, &st20_pg);
    app[i]->frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    pthread_mutex_init(&app[i]->wake_mutex, NULL);
    pthread_cond_init(&app[i]->wake_cond, NULL);
    ret = pthread_create(&app[i]->app_thread, NULL, app_tx_video_frame_thread, app[i]);
    if (ret < 0) {
      printf("%s(%d), app_thread create fail %d\n", __func__, ret, i);
      ret = st20_tx_free(tx_handle[i]);
      if (ret) {
        printf("session free failed\n");
      }
      free(app[i]);
      return -1;
    }
  }
  // start tx
  ret = st_start(dev_handle);
  // tx 120s
  sleep(120);
  // stop app thread
  for (int i = 0; i < session_num; i++) {
    app[i]->stop = true;
    pthread_join(app[i]->app_thread, NULL);
  }

  // stop tx
  ret = st_stop(dev_handle);

  // release session
  for (int i = 0; i < session_num; i++) {
    ret = st20_tx_free(tx_handle[i]);
    if (ret) {
      printf("session free failed\n");
    }
    pthread_mutex_destroy(&app[i]->wake_mutex);
    pthread_cond_destroy(&app[i]->wake_cond);

    free(app[i]);
  }

  // destroy device
  st_uninit(dev_handle);
  return 0;
}
