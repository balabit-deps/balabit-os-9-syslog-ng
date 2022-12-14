/*
 * Copyright (c) 2015 BalaBit
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "groupingby.h"
#include "correlation.h"
#include "correlation-context.h"
#include "synthetic-message.h"
#include "messages.h"
#include "str-utils.h"
#include "scratch-buffers.h"
#include "filter/filter-expr.h"
#include "timeutils/cache.h"
#include "timeutils/misc.h"
#include <iv.h>

typedef struct _GroupingBy
{
  StatefulParser super;
  GMutex lock;
  struct iv_timer tick;
  TimerWheel *timer_wheel;
  GTimeVal last_tick;
  CorrelationState *correlation;
  LogTemplate *key_template;
  LogTemplate *sort_key_template;
  gint timeout;
  CorrelationScope scope;
  SyntheticMessage *synthetic_message;
  FilterExprNode *trigger_condition_expr;
  FilterExprNode *where_condition_expr;
  FilterExprNode *having_condition_expr;
} GroupingBy;

typedef struct
{
  CorrelationState *correlation;
  TimerWheel *timer_wheel;
} GroupingByPersistData;

static NVHandle context_id_handle = 0;


#define EXPECTED_NUMBER_OF_MESSAGES_EMITTED 32
typedef struct _GPMessageEmitter
{
  gpointer emitted_messages[EXPECTED_NUMBER_OF_MESSAGES_EMITTED];
  GPtrArray *emitted_messages_overflow;
  gint num_emitted_messages;
} GPMessageEmitter;


/* This function is called to populate the emitted_messages array in
 * msg_emitter.  It only manipulates per-thread data structure so it does
 * not require locks but does not mind them being locked either.  */
static void
_emit_message(GPMessageEmitter *msg_emitter, LogMessage *msg)
{
  if (msg_emitter->num_emitted_messages < EXPECTED_NUMBER_OF_MESSAGES_EMITTED)
    {
      msg_emitter->emitted_messages[msg_emitter->num_emitted_messages++] = log_msg_ref(msg);
      return;
    }

  if (!msg_emitter->emitted_messages_overflow)
    msg_emitter->emitted_messages_overflow = g_ptr_array_new();

  g_ptr_array_add(msg_emitter->emitted_messages_overflow, log_msg_ref(msg));
}

static void
_send_emitted_message_array(GroupingBy *self, gpointer *values, gsize len)
{
  for (gint i = 0; i < len; i++)
    {
      LogMessage *msg = values[i];
      stateful_parser_emit_synthetic(&self->super, msg);
      log_msg_unref(msg);
    }
}

/* This function is called to flush the accumulated list of messages that
 * are generated during processing.  We must not hold any locks within
 * GroupingBy when doing this, as it will cause log_pipe_queue() calls to
 * subsequent elements in the message pipeline, which in turn may recurse
 * into PatternDB.  This works as msg_emitter itself is per-thread
 * (actually an auto variable on the stack), and this is called without
 * locks held at the end of a process() invocation. */
static void
_flush_emitted_messages(GroupingBy *self, GPMessageEmitter *msg_emitter)
{
  _send_emitted_message_array(self, msg_emitter->emitted_messages, msg_emitter->num_emitted_messages);
  msg_emitter->num_emitted_messages = 0;

  if (!msg_emitter->emitted_messages_overflow)
    return;

  _send_emitted_message_array(self, msg_emitter->emitted_messages_overflow->pdata,
                              msg_emitter->emitted_messages_overflow->len);

  g_ptr_array_free(msg_emitter->emitted_messages_overflow, TRUE);
  msg_emitter->emitted_messages_overflow = NULL;

}

static void
_free_persist_data(GroupingByPersistData *self)
{
  correlation_state_free(self->correlation);
  timer_wheel_free(self->timer_wheel);
  g_free(self);
}

void
grouping_by_set_key_template(LogParser *s, LogTemplate *key_template)
{
  GroupingBy *self = (GroupingBy *) s;

  log_template_unref(self->key_template);
  self->key_template = log_template_ref(key_template);
}

void
grouping_by_set_sort_key_template(LogParser *s, LogTemplate *sort_key)
{
  GroupingBy *self = (GroupingBy *) s;

  log_template_unref(self->sort_key_template);
  self->sort_key_template = log_template_ref(sort_key);
}

void
grouping_by_set_scope(LogParser *s, CorrelationScope scope)
{
  GroupingBy *self = (GroupingBy *) s;

  self->scope = scope;
}

void
grouping_by_set_timeout(LogParser *s, gint timeout)
{
  GroupingBy *self = (GroupingBy *) s;

  self->timeout = timeout;
}

void
grouping_by_set_trigger_condition(LogParser *s, FilterExprNode *filter_expr)
{
  GroupingBy *self = (GroupingBy *) s;

  self->trigger_condition_expr = filter_expr;
}

void
grouping_by_set_where_condition(LogParser *s, FilterExprNode *filter_expr)
{
  GroupingBy *self = (GroupingBy *) s;

  self->where_condition_expr = filter_expr;
}

void
grouping_by_set_having_condition(LogParser *s, FilterExprNode *filter_expr)
{
  GroupingBy *self = (GroupingBy *) s;

  self->having_condition_expr = filter_expr;
}

void
grouping_by_set_synthetic_message(LogParser *s, SyntheticMessage *message)
{
  GroupingBy *self = (GroupingBy *) s;

  if (self->synthetic_message)
    synthetic_message_free(self->synthetic_message);
  self->synthetic_message = message;
}

/* NOTE: lock should be acquired for writing before calling this function. */
void
grouping_by_set_time(GroupingBy *self, const UnixTime *ls, GPMessageEmitter *msg_emitter)
{
  GTimeVal now;

  /* clamp the current time between the timestamp of the current message
   * (low limit) and the current system time (high limit).  This ensures
   * that incorrect clocks do not skew the current time know by the
   * correlation engine too much. */

  cached_g_current_time(&now);
  self->last_tick = now;

  if (ls->ut_sec < now.tv_sec)
    now.tv_sec = ls->ut_sec;

  timer_wheel_set_time(self->timer_wheel, now.tv_sec, msg_emitter);
  msg_debug("Advancing grouping-by() current time because of an incoming message",
            evt_tag_long("utc", timer_wheel_get_time(self->timer_wheel)),
            log_pipe_location_tag(&self->super.super.super));
}

/*
 * This function can be called any time when pattern-db is not processing
 * messages, but we expect the correlation timer to move forward.  It
 * doesn't need to be called absolutely regularly as it'll use the current
 * system time to determine how much time has passed since the last
 * invocation.  See the timing comment at pattern_db_process() for more
 * information.
 */
void
_grouping_by_timer_tick(GroupingBy *self)
{
  GTimeVal now;
  glong diff;

  GPMessageEmitter msg_emitter = {0};

  g_mutex_lock(&self->lock);
  cached_g_current_time(&now);
  diff = g_time_val_diff(&now, &self->last_tick);

  if (diff > 1e6)
    {
      glong diff_sec = (glong)(diff / 1e6);

      timer_wheel_set_time(self->timer_wheel, timer_wheel_get_time(self->timer_wheel) + diff_sec, &msg_emitter);
      msg_debug("Advancing grouping-by() current time because of timer tick",
                evt_tag_long("utc", timer_wheel_get_time(self->timer_wheel)),
                log_pipe_location_tag(&self->super.super.super));
      /* update last_tick, take the fraction of the seconds not calculated into this update into account */

      self->last_tick = now;
      g_time_val_add(&self->last_tick, - (glong)(diff - diff_sec * 1e6));
    }
  else if (diff < 0)
    {
      /* time moving backwards, this can only happen if the computer's time
       * is changed.  We don't update patterndb's idea of the time now, wait
       * another tick instead to update that instead.
       */
      self->last_tick = now;
    }
  g_mutex_unlock(&self->lock);
  _flush_emitted_messages(self, &msg_emitter);
}

static void
grouping_by_timer_tick(gpointer s)
{
  GroupingBy *self = (GroupingBy *) s;

  _grouping_by_timer_tick(self);
  iv_validate_now();
  self->tick.expires = iv_now;
  self->tick.expires.tv_sec++;
  iv_timer_register(&self->tick);
}

static gboolean
_evaluate_filter(FilterExprNode *expr, CorrelationContext *context)
{
  return filter_expr_eval_with_context(expr,
                                       (LogMessage **) context->messages->pdata,
                                       context->messages->len, &DEFAULT_TEMPLATE_EVAL_OPTIONS);
}

static gboolean
_evaluate_having(GroupingBy *self, CorrelationContext *context)
{
  if (!self->having_condition_expr)
    return TRUE;

  return _evaluate_filter(self->having_condition_expr, context);
}

static gboolean
_evaluate_trigger(GroupingBy *self, CorrelationContext *context)
{
  if (!self->trigger_condition_expr)
    return FALSE;

  return _evaluate_filter(self->trigger_condition_expr, context);
}

static LogMessage *
grouping_by_generate_synthetic_msg(GroupingBy *self, CorrelationContext *context)
{
  LogMessage *msg = NULL;

  if (!_evaluate_having(self, context))
    {
      msg_debug("groupingby() dropping context, because having() is FALSE",
                evt_tag_str("key", context->key.session_id),
                log_pipe_location_tag(&self->super.super.super));
      return NULL;
    }

  msg = synthetic_message_generate_with_context(self->synthetic_message, context);

  return msg;
}

static LogMessage *
grouping_by_update_context_and_generate_msg(GroupingBy *self, CorrelationContext *context)
{
  if (self->sort_key_template)
    correlation_context_sort(context, self->sort_key_template);

  LogMessage *msg = grouping_by_generate_synthetic_msg(self, context);

  g_hash_table_remove(self->correlation->state, &context->key);

  /* correlation_context_free is automatically called when returning from
     this function by the timerwheel code as a destroy notify
     callback. */

  return msg;
}

static void
grouping_by_expire_entry(TimerWheel *wheel, guint64 now, gpointer user_data, gpointer caller_context)
{
  CorrelationContext *context = user_data;
  GPMessageEmitter *msg_emitter = caller_context;
  GroupingBy *self = (GroupingBy *) timer_wheel_get_associated_data(wheel);

  msg_debug("Expiring grouping-by() correlation context",
            evt_tag_long("utc", timer_wheel_get_time(wheel)),
            evt_tag_str("context-id", context->key.session_id),
            log_pipe_location_tag(&self->super.super.super));

  LogMessage *msg = grouping_by_update_context_and_generate_msg(self, context);
  if (msg)
    {
      _emit_message(msg_emitter, msg);
      log_msg_unref(msg);
    }
}


gchar *
grouping_by_format_persist_name(LogParser *s)
{
  static gchar persist_name[512];
  GroupingBy *self = (GroupingBy *)s;

  g_snprintf(persist_name, sizeof(persist_name), "grouping-by(%s)", self->key_template->template);
  return persist_name;
}

static CorrelationContext *
_lookup_or_create_context(GroupingBy *self, LogMessage *msg)
{
  CorrelationContext *context;
  CorrelationKey key;
  GString *buffer = scratch_buffers_alloc();

  log_template_format(self->key_template, msg, &DEFAULT_TEMPLATE_EVAL_OPTIONS, buffer);
  log_msg_set_value(msg, context_id_handle, buffer->str, -1);

  correlation_key_init(&key, self->scope, msg, buffer->str);
  context = g_hash_table_lookup(self->correlation->state, &key);
  if (!context)
    {
      msg_debug("Correlation context lookup failure, starting a new context",
                evt_tag_str("key", key.session_id),
                evt_tag_int("timeout", self->timeout),
                evt_tag_int("expiration", timer_wheel_get_time(self->timer_wheel) + self->timeout),
                log_pipe_location_tag(&self->super.super.super));

      context = correlation_context_new(&key);
      g_hash_table_insert(self->correlation->state, &context->key, context);
      g_string_steal(buffer);
    }
  else
    {
      msg_debug("Correlation context lookup successful",
                evt_tag_str("key", key.session_id),
                evt_tag_int("timeout", self->timeout),
                evt_tag_int("expiration", timer_wheel_get_time(self->timer_wheel) + self->timeout),
                evt_tag_int("num_messages", context->messages->len),
                log_pipe_location_tag(&self->super.super.super));
    }

  return context;
}

static gboolean
_perform_groupby(GroupingBy *self, LogMessage *msg)
{
  GPMessageEmitter msg_emitter = {0};

  g_mutex_lock(&self->lock);
  grouping_by_set_time(self, &msg->timestamps[LM_TS_STAMP], &msg_emitter);

  CorrelationContext *context = _lookup_or_create_context(self, msg);
  g_ptr_array_add(context->messages, log_msg_ref(msg));

  if (_evaluate_trigger(self, context))
    {
      msg_verbose("Correlation trigger() met, closing state",
                  evt_tag_str("key", context->key.session_id),
                  evt_tag_int("timeout", self->timeout),
                  evt_tag_int("num_messages", context->messages->len),
                  log_pipe_location_tag(&self->super.super.super));

      /* close down state */
      if (context->timer)
        timer_wheel_del_timer(self->timer_wheel, context->timer);

      LogMessage *genmsg = grouping_by_update_context_and_generate_msg(self, context);

      g_mutex_unlock(&self->lock);
      _flush_emitted_messages(self, &msg_emitter);

      if (genmsg)
        {
          stateful_parser_emit_synthetic(&self->super, genmsg);
          log_msg_unref(genmsg);
        }

      log_msg_write_protect(msg);

      return TRUE;
    }
  else
    {

      if (context->timer)
        {
          timer_wheel_mod_timer(self->timer_wheel, context->timer, self->timeout);
        }
      else
        {
          context->timer = timer_wheel_add_timer(self->timer_wheel, self->timeout, grouping_by_expire_entry,
                                                 correlation_context_ref(context), (GDestroyNotify) correlation_context_unref);
        }
    }

  log_msg_write_protect(msg);

  g_mutex_unlock(&self->lock);
  _flush_emitted_messages(self, &msg_emitter);

  return TRUE;
}

static gboolean
_evaluate_where(GroupingBy *self, LogMessage **pmsg, const LogPathOptions *path_options)
{
  if (!self->where_condition_expr)
    return TRUE;

  return filter_expr_eval_root(self->where_condition_expr, pmsg, path_options);
}

static gboolean
grouping_by_process(LogParser *s, LogMessage **pmsg, const LogPathOptions *path_options, const char *input,
                    gsize input_len)
{
  GroupingBy *self = (GroupingBy *) s;

  if (_evaluate_where(self, pmsg, path_options))
    return _perform_groupby(self, log_msg_make_writable(pmsg, path_options));
  return TRUE;
}

static void
_load_correlation_state(GroupingBy *self, GlobalConfig *cfg)
{
  GroupingByPersistData *persist_data = cfg_persist_config_fetch(cfg,
                                        grouping_by_format_persist_name(&self->super.super));
  if (persist_data)
    {
      self->correlation = persist_data->correlation;
      self->timer_wheel = persist_data->timer_wheel;
      timer_wheel_set_associated_data(self->timer_wheel, log_pipe_ref((LogPipe *)self), (GDestroyNotify)log_pipe_unref);
    }
  else
    {
      self->correlation = correlation_state_new();
      self->timer_wheel = timer_wheel_new();
      timer_wheel_set_associated_data(self->timer_wheel, log_pipe_ref((LogPipe *)self), (GDestroyNotify)log_pipe_unref);
    }
  g_free(persist_data);
}

static gboolean
grouping_by_init(LogPipe *s)
{
  GroupingBy *self = (GroupingBy *) s;
  GlobalConfig *cfg = log_pipe_get_config(s);

  if (!self->synthetic_message)
    {
      msg_error("The aggregate() option for grouping-by() is mandatory",
                log_pipe_location_tag(s));
      return FALSE;
    }
  if (self->timeout < 1)
    {
      msg_error("timeout() needs to be specified explicitly and must be greater than 0 in the grouping-by() parser",
                log_pipe_location_tag(s));
      return FALSE;
    }
  if (!self->key_template)
    {
      msg_error("The key() option is mandatory for the grouping-by() parser",
                log_pipe_location_tag(s));
      return FALSE;
    }

  _load_correlation_state(self, cfg);

  iv_validate_now();
  IV_TIMER_INIT(&self->tick);
  self->tick.cookie = self;
  self->tick.handler = grouping_by_timer_tick;
  self->tick.expires = iv_now;
  self->tick.expires.tv_sec++;
  self->tick.expires.tv_nsec = 0;
  iv_timer_register(&self->tick);

  if (self->trigger_condition_expr && !filter_expr_init(self->trigger_condition_expr, cfg))
    return FALSE;
  if (self->where_condition_expr && !filter_expr_init(self->where_condition_expr, cfg))
    return FALSE;
  if (self->having_condition_expr && !filter_expr_init(self->having_condition_expr, cfg))
    return FALSE;

  return stateful_parser_init_method(s);
}

static void
_store_data_in_persist(GroupingBy *self, GlobalConfig *cfg)
{
  GroupingByPersistData *persist_data = g_new0(GroupingByPersistData, 1);
  persist_data->correlation = self->correlation;
  persist_data->timer_wheel = self->timer_wheel;

  cfg_persist_config_add(cfg, grouping_by_format_persist_name(&self->super.super), persist_data,
                         (GDestroyNotify) _free_persist_data, FALSE);
  self->correlation = NULL;
  self->timer_wheel = NULL;
}

static gboolean
grouping_by_deinit(LogPipe *s)
{
  GroupingBy *self = (GroupingBy *) s;
  GlobalConfig *cfg = log_pipe_get_config(s);

  if (iv_timer_registered(&self->tick))
    {
      iv_timer_unregister(&self->tick);
    }

  _store_data_in_persist(self, cfg);

  return stateful_parser_deinit_method(s);
}

static LogPipe *
grouping_by_clone(LogPipe *s)
{
  LogParser *cloned;
  GroupingBy *self = (GroupingBy *) s;

  /* FIXME: share state between clones! */
  cloned = grouping_by_new(s->cfg);
  grouping_by_set_key_template(cloned, self->key_template);
  grouping_by_set_timeout(cloned, self->timeout);
  return &cloned->super;
}

static void
grouping_by_free(LogPipe *s)
{
  GroupingBy *self = (GroupingBy *) s;

  g_mutex_clear(&self->lock);
  log_template_unref(self->key_template);
  log_template_unref(self->sort_key_template);
  if (self->synthetic_message)
    synthetic_message_free(self->synthetic_message);
  stateful_parser_free_method(s);

  filter_expr_unref(self->trigger_condition_expr);
  filter_expr_unref(self->where_condition_expr);
  filter_expr_unref(self->having_condition_expr);
}

LogParser *
grouping_by_new(GlobalConfig *cfg)
{
  GroupingBy *self = g_new0(GroupingBy, 1);

  stateful_parser_init_instance(&self->super, cfg);
  self->super.super.super.free_fn = grouping_by_free;
  self->super.super.super.init = grouping_by_init;
  self->super.super.super.deinit = grouping_by_deinit;
  self->super.super.super.clone = grouping_by_clone;
  self->super.super.process = grouping_by_process;
  g_mutex_init(&self->lock);
  self->scope = RCS_GLOBAL;
  cached_g_current_time(&self->last_tick);
  self->timeout = -1;
  return &self->super.super;
}

void
grouping_by_global_init(void)
{
  context_id_handle = log_msg_get_value_handle(".classifier.context_id");
}
