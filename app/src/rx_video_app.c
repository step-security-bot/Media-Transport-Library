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

#include "rx_video_app.h"

static int app_rx_video_enqueue_frame(struct st_app_rx_video_session* s, void* frame) {
  int idx = s->idx;

  if (s->st21_dst_qp_idx == s->st21_dst_qc_idx) {
    err("%s(%d), queue is busy idx %d\n", __func__, idx, s->st21_dst_qp_idx);
    return -EBUSY;
  }

  s->st21_frames_dst_queue[s->st21_dst_qp_idx] = frame;
  s->st21_dst_qp_idx++;
  if (s->st21_dst_qp_idx >= s->st21_dst_q_size) s->st21_dst_qp_idx = 0;
  return 0;
}

static void* app_rx_video_frame_thread(void* arg) {
  struct st_app_rx_video_session* s = arg;
  int idx = s->idx;
  int consumer_idx;
  void* frame;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st21_app_thread_stop) {
    consumer_idx = s->st21_dst_qc_idx;
    consumer_idx++;
    if (consumer_idx >= s->st21_dst_q_size) consumer_idx = 0;
    if (consumer_idx == s->st21_dst_qp_idx) {
      /* no buffer */
      pthread_mutex_lock(&s->st21_wake_mutex);
      if (!s->st21_app_thread_stop)
        pthread_cond_wait(&s->st21_wake_cond, &s->st21_wake_mutex);
      pthread_mutex_unlock(&s->st21_wake_mutex);
      continue;
    }

    dbg("%s(%d), dequeue idx %d\n", __func__, idx, consumer_idx);
    frame = s->st21_frames_dst_queue[consumer_idx];
    struct st_display* d = s->display;
    if (d) {  // copy to display frame
      st_memcpy(d->source_frame, frame, s->st21_frame_size);
      dbg("%s(%d), copied frame %p to display %p\n", __func__, idx, frame,
          d->source_frame);
      pthread_mutex_lock(&d->st_dispaly_wake_mutex);
      pthread_cond_signal(&d->st_dispaly_wake_cond);
      pthread_mutex_unlock(&d->st_dispaly_wake_mutex);
    } else {
      if (s->st21_dst_cursor + s->st21_frame_size > s->st21_dst_end)
        s->st21_dst_cursor = s->st21_dst_begin;
      dbg("%s(%d), dst %p src %p size %d\n", __func__, idx, s->st21_dst_cursor, frame,
          s->st21_frame_size);
      st_memcpy(s->st21_dst_cursor, frame, s->st21_frame_size);
      s->st21_dst_cursor += s->st21_frame_size;
    }

    st20_rx_put_framebuff(s->handle, frame);
    s->st21_dst_qc_idx = consumer_idx;
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_rx_video_handle_rtp(struct st_app_rx_video_session* s,
                                   struct st20_rfc4175_rtp_hdr* hdr) {
  int idx = s->idx;
  uint32_t tmstamp = ntohl(hdr->tmstamp);
  struct st20_rfc4175_extra_rtp_hdr* e_hdr = NULL;
  uint16_t row_number; /* 0 to 1079 for 1080p */
  uint16_t row_offset; /* [0, 480, 960, 1440] for 1080p */
  uint16_t row_length; /* 1200 for 1080p */
  uint8_t* frame;
  uint8_t* payload;

  dbg("%s(%d),tmstamp: 0x%x\n", __func__, idx, tmstamp);
  if (tmstamp != s->st21_last_tmstamp) {
    /* new frame received */
    s->st21_last_tmstamp = tmstamp;
    s->stat_frame_received++;

    s->st21_dst_cursor += s->st21_frame_size;
    if ((s->st21_dst_cursor + s->st21_frame_size) > s->st21_dst_end)
      s->st21_dst_cursor = s->st21_dst_begin;
  }

  if (s->st21_dst_fd < 0) return 0;

  frame = s->st21_dst_cursor;
  payload = (uint8_t*)hdr + sizeof(*hdr);
  row_number = ntohs(hdr->row_number);
  row_offset = ntohs(hdr->row_offset);
  row_length = ntohs(hdr->row_length);
  dbg("%s(%d), row: %d %d %d\n", __func__, idx, row_number, row_offset, row_length);
  if (row_offset & ST20_SRD_OFFSET_CONTINUATION) {
    /* additional Sample Row Data */
    row_offset &= ~ST20_SRD_OFFSET_CONTINUATION;
    e_hdr = (struct st20_rfc4175_extra_rtp_hdr*)payload;
    payload += sizeof(*e_hdr);
  }

  /* copy the payload to target frame */
  uint32_t offset =
      (row_number * s->width + row_offset) / s->st21_pg.coverage * s->st21_pg.size;
  if ((offset + row_length) > s->st21_frame_size) {
    err("%s(%d: invalid offset %u frame size %d\n", __func__, idx, offset,
        s->st21_frame_size);
    return -EIO;
  }
  st_memcpy(frame + offset, payload, row_length);
  if (e_hdr) {
    uint16_t row2_number = ntohs(e_hdr->row_number);
    uint16_t row2_offset = ntohs(e_hdr->row_offset);
    uint16_t row2_length = ntohs(e_hdr->row_length);

    dbg("%s(%d), row: %d %d %d\n", __func__, idx, row2_number, row2_offset, row2_length);
    uint32_t offset2 =
        (row2_number * s->width + row2_offset) / s->st21_pg.coverage * s->st21_pg.size;
    if ((offset2 + row2_length) > s->st21_frame_size) {
      err("%s(%d: invalid offset %u frame size %d for extra hdr\n", __func__, idx,
          offset2, s->st21_frame_size);
      return -EIO;
    }
    st_memcpy(frame + offset2, payload + row_length, row2_length);
  }

  return 0;
}

static void* app_rx_video_rtp_thread(void* arg) {
  struct st_app_rx_video_session* s = arg;
  int idx = s->idx;
  void* usrptr;
  uint16_t len;
  void* mbuf;
  struct st20_rfc4175_rtp_hdr* hdr;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st21_app_thread_stop) {
    mbuf = st20_rx_get_mbuf(s->handle, &usrptr, &len);
    if (!mbuf) {
      /* no buffer */
      pthread_mutex_lock(&s->st21_wake_mutex);
      if (!s->st21_app_thread_stop)
        pthread_cond_wait(&s->st21_wake_cond, &s->st21_wake_mutex);
      pthread_mutex_unlock(&s->st21_wake_mutex);
      continue;
    }

    /* get one packet */
    hdr = (struct st20_rfc4175_rtp_hdr*)usrptr;
    app_rx_video_handle_rtp(s, hdr);
    /* free to lib */
    st20_rx_put_mbuf(s->handle, mbuf);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_rx_video_close_source(struct st_app_rx_video_session* s) {
  if (s->st21_dst_fd >= 0) {
    munmap(s->st21_dst_begin, s->st21_dst_end - s->st21_dst_begin);
    close(s->st21_dst_fd);
    s->st21_dst_fd = -1;
  }

  return 0;
}

static int app_rx_video_open_source(struct st_app_rx_video_session* s) {
  int fd, ret, idx = s->idx;
  off_t f_size;

  /* user do not require fb save to file */
  if (s->st21_dst_fb_cnt <= 1) return 0;

  fd = open(s->st21_dst_url, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    err("%s(%d), open %s fail\n", __func__, idx, s->st21_dst_url);
    return -EIO;
  }

  f_size = s->st21_dst_fb_cnt * s->st21_frame_size;
  ret = ftruncate(fd, f_size);
  if (ret < 0) {
    err("%s(%d), ftruncate %s fail\n", __func__, idx, s->st21_dst_url);
    close(fd);
    return -EIO;
  }

  uint8_t* m = mmap(NULL, f_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    err("%s(%d), mmap %s fail\n", __func__, idx, s->st21_dst_url);
    close(fd);
    return -EIO;
  }

  s->st21_dst_begin = m;
  s->st21_dst_cursor = m;
  s->st21_dst_end = m + f_size;
  s->st21_dst_fd = fd;
  info("%s(%d), save %d framebuffers to file %s(%p,%" PRIu64 ")\n", __func__, idx,
       s->st21_dst_fb_cnt, s->st21_dst_url, m, f_size);

  return 0;
}

static int app_rx_video_init_frame_thread(struct st_app_rx_video_session* s) {
  int ret, idx = s->idx;

  /* user do not require fb save to file or display */
  if (s->st21_dst_fb_cnt <= 1 && s->display == NULL) return 0;

  ret = pthread_create(&s->st21_app_thread, NULL, app_rx_video_frame_thread, s);
  if (ret < 0) {
    err("%s(%d), st21_app_thread create fail %d\n", __func__, ret, idx);
    return -EIO;
  }

  return 0;
}

static int app_rx_video_init_rtp_thread(struct st_app_rx_video_session* s) {
  int ret, idx = s->idx;

  ret = pthread_create(&s->st21_app_thread, NULL, app_rx_video_rtp_thread, s);
  if (ret < 0) {
    err("%s(%d), st21_app_thread create fail %d\n", __func__, ret, idx);
    return -EIO;
  }

  return 0;
}

static int app_rx_video_frame_ready(void* priv, void* frame) {
  struct st_app_rx_video_session* s = priv;
  int ret;

  s->stat_frame_received++;

  if (s->st21_dst_fd < 0 && s->display == NULL) {
    /* free the queue directly as no read thread is running */
    st20_rx_put_framebuff(s->handle, frame);
    return 0;
  }

  pthread_mutex_lock(&s->st21_wake_mutex);
  pthread_cond_signal(&s->st21_wake_cond);
  pthread_mutex_unlock(&s->st21_wake_mutex);

  ret = app_rx_video_enqueue_frame(s, frame);
  if (ret < 0) {
    /* free the queue */
    st20_rx_put_framebuff(s->handle, frame);
    return ret;
  }

  return 0;
}

static int app_rx_video_rtp_ready(void* priv) {
  struct st_app_rx_video_session* s = priv;

  pthread_mutex_lock(&s->st21_wake_mutex);
  pthread_cond_signal(&s->st21_wake_cond);
  pthread_mutex_unlock(&s->st21_wake_mutex);

  return 0;
}

static int app_rx_video_uinit(struct st_app_rx_video_session* s) {
  int ret, idx = s->idx;

  s->st21_app_thread_stop = true;
  if (s->st21_app_thread) {
    /* wake up the thread */
    pthread_mutex_lock(&s->st21_wake_mutex);
    pthread_cond_signal(&s->st21_wake_cond);
    pthread_mutex_unlock(&s->st21_wake_mutex);
    info("%s(%d), wait app thread stop\n", __func__, idx);
    pthread_join(s->st21_app_thread, NULL);
  }

  pthread_mutex_destroy(&s->st21_wake_mutex);
  pthread_cond_destroy(&s->st21_wake_cond);

  if (s->handle) {
    ret = st20_rx_free(s->handle);
    if (ret < 0) err("%s(%d), st20_rx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }
  app_rx_video_close_source(s);
  if (s->st21_frames_dst_queue) {
    st_app_free(s->st21_frames_dst_queue);
    s->st21_frames_dst_queue = NULL;
  }

  st_app_dettach_display(s);

  return 0;
}

static int app_rx_video_init(struct st_app_context* ctx,
                             st_json_rx_video_session_t* video,
                             struct st_app_rx_video_session* s) {
  int idx = s->idx, ret;
  struct st20_rx_ops ops;
  char name[32];
  st20_rx_handle handle;

  snprintf(name, 32, "app_rx_video_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = video ? video->num_inf : ctx->para.num_ports;
  memcpy(ops.sip_addr[ST_PORT_P],
         video ? video->ip[ST_PORT_P] : ctx->rx_sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops.port[ST_PORT_P],
          video ? video->inf[ST_PORT_P]->name : ctx->para.port[ST_PORT_P],
          ST_PORT_MAX_LEN);
  ops.udp_port[ST_PORT_P] = video ? video->udp_port : (10000 + s->idx);
  if (ops.num_port > 1) {
    memcpy(ops.sip_addr[ST_PORT_R],
           video ? video->ip[ST_PORT_R] : ctx->rx_sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops.port[ST_PORT_R],
            video ? video->inf[ST_PORT_R]->name : ctx->para.port[ST_PORT_R],
            ST_PORT_MAX_LEN);
    ops.udp_port[ST_PORT_R] = video ? video->udp_port : (10000 + s->idx);
  }
  ops.pacing = ST21_PACING_NARROW;
  if (ctx->rx_video_rtp_ring_size > 0)
    ops.type = ST20_TYPE_RTP_LEVEL;
  else
    ops.type = video ? video->type : ST20_TYPE_FRAME_LEVEL;
  ops.width = video ? st_app_get_width(video->video_format) : 1920;
  ops.height = video ? st_app_get_height(video->video_format) : 1080;
  ops.fps = video ? st_app_get_fps(video->video_format) : ST_FPS_P59_94;
  ops.fmt = video ? video->pg_format : ST20_FMT_YUV_422_10BIT;
  ops.notify_frame_ready = app_rx_video_frame_ready;
  ops.notify_rtp_ready = app_rx_video_rtp_ready;
  ops.framebuff_cnt = s->framebuff_cnt;
  ops.rtp_ring_size = ctx->rx_video_rtp_ring_size;
  if (!ops.rtp_ring_size) ops.rtp_ring_size = 1024;

  pthread_mutex_init(&s->st21_wake_mutex, NULL);
  pthread_cond_init(&s->st21_wake_cond, NULL);

  uint32_t soc = 0, b = 0, d = 0, f = 0;
  sscanf(ctx->para.port[ST_PORT_P], "%x:%x:%x.%x", &soc, &b, &d, &f);
  snprintf(s->st21_dst_url, ST_APP_URL_MAX_LEN, "st_app%d_%d_%d_%02x_%02x_%02x-%02x.yuv",
           idx, ops.width, ops.height, soc, b, d, f);
  ret = st20_get_pgroup(ops.fmt, &s->st21_pg);
  if (ret < 0) return ret;
  s->st21_frame_size = ops.width * ops.height * s->st21_pg.size / s->st21_pg.coverage;
  s->width = ops.width;
  s->height = ops.height;

  s->st21_dst_q_size = s->st21_dst_fb_cnt ? s->st21_dst_fb_cnt : 4;
  s->st21_frames_dst_queue = st_app_zmalloc(sizeof(void*) * s->st21_dst_q_size);
  if (!s->st21_frames_dst_queue) return -ENOMEM;
  s->st21_dst_qp_idx = 0;
  s->st21_dst_qc_idx = s->st21_dst_q_size - 1;

  if (ctx->has_sdl && ((video && video->display) || ctx->display)) {
    ret = st_app_attach_display(s);
    if (ret < 0) {
      err("%s(%d), st_app_attach_display fail %d\n", __func__, idx, ret);
      app_rx_video_uinit(s);
      return -EIO;
    }
  }

  ret = app_rx_video_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_rx_video_open_source fail %d\n", __func__, idx, ret);
    app_rx_video_uinit(s);
    return -EIO;
  }

  handle = st20_rx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st20_rx_create fail\n", __func__, idx);
    app_rx_video_uinit(s);
    return -EIO;
  }
  s->handle = handle;

  if (ops.type == ST20_TYPE_FRAME_LEVEL) {
    ret = app_rx_video_init_frame_thread(s);
  } else if (ops.type == ST20_TYPE_RTP_LEVEL) {
    ret = app_rx_video_init_rtp_thread(s);
  } else {
    ret = -EINVAL;
  }
  if (ret < 0) {
    err("%s(%d), app_rx_video_init_thread fail %d, type %d\n", __func__, idx, ret,
        ops.type);
    app_rx_video_uinit(s);
    return -EIO;
  }

  s->stat_frame_received = 0;
  s->stat_last_time = st_app_get_monotonic_time();

  return 0;
}

static int app_rx_video_stat(struct st_app_rx_video_session* s) {
  int idx = s->idx;
  uint64_t cur_time_ns = st_app_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  double framerate = s->stat_frame_received / time_sec;

  info("%s(%d), fps %f, %d frame received\n", __func__, idx, framerate,
       s->stat_frame_received);
  s->stat_frame_received = 0;
  s->stat_last_time = cur_time_ns;

  return 0;
}

int st_app_rx_video_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_rx_video_session* s;

  for (i = 0; i < ctx->rx_video_session_cnt; i++) {
    s = &ctx->rx_video_sessions[i];
    s->idx = i;
    s->framebuff_cnt = 6;
    s->st21_dst_fb_cnt = ctx->rx_video_file_frames;
    s->st21_dst_fd = -1;

    ret = app_rx_video_init(ctx, ctx->json_ctx ? &ctx->json_ctx->rx_video[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_rx_video_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_rx_video_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_video_session* s;

  for (i = 0; i < ctx->rx_video_session_cnt; i++) {
    s = &ctx->rx_video_sessions[i];
    app_rx_video_uinit(s);
  }

  return 0;
}

int st_app_rx_video_sessions_stat(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_video_session* s;

  for (i = 0; i < ctx->rx_video_session_cnt; i++) {
    s = &ctx->rx_video_sessions[i];
    app_rx_video_stat(s);
  }

  return 0;
}
