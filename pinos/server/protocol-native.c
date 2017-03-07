/* Pinos
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */


#include "spa/pod-iter.h"

#include "pinos/client/interfaces.h"
#include "pinos/server/resource.h"
#include "pinos/server/protocol-native.h"


typedef struct {
  SpaPODBuilder b;
  PinosConnection *connection;
} Builder;

static uint32_t
write_pod (SpaPODBuilder *b, uint32_t ref, const void *data, uint32_t size)
{
  if (ref == -1)
    ref = b->offset;

  if (b->size <= b->offset) {
    b->size = SPA_ROUND_UP_N (b->offset + size, 512);
    b->data = pinos_connection_begin_write (((Builder*)b)->connection, b->size);
  }
  memcpy (b->data + ref, data, size);
  return ref;
}

static void
core_marshal_info (void          *object,
                 PinosCoreInfo *info)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  uint32_t i, n_items;

  n_items = info->props ? info->props->n_items : 0;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, info->id,
        SPA_POD_TYPE_LONG, info->change_mask,
        SPA_POD_TYPE_STRING, info->user_name,
        SPA_POD_TYPE_STRING, info->host_name,
        SPA_POD_TYPE_STRING, info->version,
        SPA_POD_TYPE_STRING, info->name,
        SPA_POD_TYPE_INT, info->cookie,
        SPA_POD_TYPE_INT, n_items, 0);

  for (i = 0; i < n_items; i++) {
    spa_pod_builder_add (&b.b,
        SPA_POD_TYPE_STRING, info->props->items[i].key,
        SPA_POD_TYPE_STRING, info->props->items[i].value,
        0);
  }
  spa_pod_builder_add (&b.b, -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 0, b.b.offset);
}

static void
core_marshal_done (void          *object,
                 uint32_t       seq)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, seq,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 1, b.b.offset);
}

static void
core_marshal_error (void          *object,
                  uint32_t       id,
                  SpaResult      res,
                  const char     *error, ...)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  char buffer[128];
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  va_list ap;

  va_start (ap, error);
  vsnprintf (buffer, sizeof (buffer), error, ap);
  va_end (ap);

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, id,
        SPA_POD_TYPE_INT, res,
        SPA_POD_TYPE_STRING, buffer,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 2, b.b.offset);
}

static void
core_marshal_remove_id (void          *object,
                      uint32_t       id)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, id,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 3, b.b.offset);
}

static bool
core_demarshal_client_update (void  *object,
                              void  *data,
                              size_t size)
{
  PinosResource *resource = object;
  SpaDict props;
  SpaPODIter it;
  uint32_t i;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &props.n_items,
        0))
    return false;

  props.items = alloca (props.n_items * sizeof (SpaDictItem));
  for (i = 0; i < props.n_items; i++) {
    if (!spa_pod_iter_get (&it,
          SPA_POD_TYPE_STRING, &props.items[i].key,
          SPA_POD_TYPE_STRING, &props.items[i].value,
          0))
      return false;
  }
  pinos_core_do_client_update (resource, &props);
  return true;
}

static bool
core_demarshal_sync (void  *object,
                    void  *data,
                    size_t size)
{
  PinosResource *resource = object;
  SpaPODIter it;
  uint32_t seq;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &seq,
        0))
    return false;

  pinos_core_do_sync (resource, seq);
  return true;
}

static bool
core_demarshal_get_registry (void  *object,
                            void  *data,
                            size_t size)
{
  PinosResource *resource = object;
  SpaPODIter it;
  int32_t seq, new_id;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &seq,
        SPA_POD_TYPE_INT, &new_id,
        0))
    return false;

  pinos_core_do_get_registry (resource, seq, new_id);
  return true;
}

static bool
core_demarshal_create_node (void  *object,
                           void  *data,
                           size_t size)
{
  PinosResource *resource = object;
  SpaPODIter it;
  uint32_t seq, new_id, i;
  const char *factory_name, *name;
  SpaDict props;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &seq,
        SPA_POD_TYPE_STRING, &factory_name,
        SPA_POD_TYPE_STRING, &name,
        SPA_POD_TYPE_INT, &props.n_items,
        0))
    return false;

  props.items = alloca (props.n_items * sizeof (SpaDictItem));
  for (i = 0; i < props.n_items; i++) {
    if (!spa_pod_iter_get (&it,
          SPA_POD_TYPE_STRING, &props.items[i].key,
          SPA_POD_TYPE_STRING, &props.items[i].value,
          0))
      return false;
  }
  if (!spa_pod_iter_get (&it, SPA_POD_TYPE_INT, &new_id, 0))
    return false;

  pinos_core_do_create_node (resource,
                             seq,
                             factory_name,
                             name,
                             &props,
                             new_id);
  return true;
}

static bool
core_demarshal_create_client_node (void  *object,
                                  void  *data,
                                  size_t size)
{
  PinosResource *resource = object;
  SpaPODIter it;
  uint32_t seq, new_id, i;
  const char *name;
  SpaDict props;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &seq,
        SPA_POD_TYPE_STRING, &name,
        SPA_POD_TYPE_INT, &props.n_items,
        0))
    return false;

  props.items = alloca (props.n_items * sizeof (SpaDictItem));
  for (i = 0; i < props.n_items; i++) {
    if (!spa_pod_iter_get (&it,
          SPA_POD_TYPE_STRING, &props.items[i].key,
          SPA_POD_TYPE_STRING, &props.items[i].value,
          0))
      return false;
  }
  if (!spa_pod_iter_get (&it, SPA_POD_TYPE_INT, &new_id, 0))
    return false;

  pinos_core_do_create_client_node (resource,
                                    seq,
                                    name,
                                    &props,
                                    new_id);
  return true;
}

static void
registry_marshal_global (void          *object,
                       uint32_t       id,
                       const char    *type)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, id,
        SPA_POD_TYPE_STRING, type,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 0, b.b.offset);
}

static void
registry_marshal_global_remove (void          *object,
                              uint32_t       id)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, id,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 1, b.b.offset);
}

static bool
registry_demarshal_bind (void  *object,
                        void  *data,
                        size_t size)
{
  PinosResource *resource = object;
  SpaPODIter it;
  uint32_t id, new_id;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &id,
        SPA_POD_TYPE_INT, &new_id,
        0))
    return false;

  pinos_registry_do_bind (resource, id, new_id);
  return true;
}

static void
module_marshal_info (void            *object,
                     PinosModuleInfo *info)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  uint32_t i, n_items;

  n_items = info->props ? info->props->n_items : 0;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, info->id,
        SPA_POD_TYPE_LONG, info->change_mask,
        SPA_POD_TYPE_STRING, info->name,
        SPA_POD_TYPE_STRING, info->filename,
        SPA_POD_TYPE_STRING, info->args,
        SPA_POD_TYPE_INT, n_items, 0);

  for (i = 0; i < n_items; i++) {
    spa_pod_builder_add (&b.b,
        SPA_POD_TYPE_STRING, info->props->items[i].key,
        SPA_POD_TYPE_STRING, info->props->items[i].value,
        0);
  }
  spa_pod_builder_add (&b.b, -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 0, b.b.offset);
}

static void
node_marshal_done (void     *object,
                   uint32_t  seq)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, seq,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 0, b.b.offset);
}

static void
node_marshal_info (void          *object,
                   PinosNodeInfo *info)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  uint32_t i, n_items;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, info->id,
        SPA_POD_TYPE_LONG, info->change_mask,
        SPA_POD_TYPE_STRING, info->name,
        SPA_POD_TYPE_INT, info->max_inputs,
        SPA_POD_TYPE_INT, info->n_inputs,
        SPA_POD_TYPE_INT, info->n_input_formats, 0);

  for (i = 0; i < info->n_input_formats; i++)
    spa_pod_builder_add (&b.b, SPA_POD_TYPE_POD, info->input_formats[i], 0);

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_INT, info->max_outputs,
      SPA_POD_TYPE_INT, info->n_outputs,
      SPA_POD_TYPE_INT, info->n_output_formats, 0);

  for (i = 0; i < info->n_output_formats; i++)
    spa_pod_builder_add (&b.b, SPA_POD_TYPE_POD, info->output_formats[i], 0);

  n_items = info->props ? info->props->n_items : 0;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_INT, info->state,
      SPA_POD_TYPE_STRING, info->error,
      SPA_POD_TYPE_INT, n_items, 0);

  for (i = 0; i < n_items; i++) {
    spa_pod_builder_add (&b.b,
        SPA_POD_TYPE_STRING, info->props->items[i].key,
        SPA_POD_TYPE_STRING, info->props->items[i].value, 0);
  }
  spa_pod_builder_add (&b.b, -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 1, b.b.offset);
}

static void
client_marshal_info (void          *object,
                     PinosClientInfo *info)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  uint32_t i, n_items;

  n_items = info->props ? info->props->n_items : 0;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, info->id,
        SPA_POD_TYPE_LONG, info->change_mask,
        SPA_POD_TYPE_INT, n_items, 0);

  for (i = 0; i < n_items; i++) {
    spa_pod_builder_add (&b.b,
        SPA_POD_TYPE_STRING, info->props->items[i].key,
        SPA_POD_TYPE_STRING, info->props->items[i].value, 0);
  }
  spa_pod_builder_add (&b.b, -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 0, b.b.offset);
}

static void
client_node_marshal_done (void     *object,
                          uint32_t  seq,
                          int       datafd)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, seq,
        SPA_POD_TYPE_INT, pinos_connection_add_fd (connection, datafd),
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 0, b.b.offset);
}

static void
client_node_marshal_event (void               *object,
                           const SpaNodeEvent *event)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_BYTES, event, event->size,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 1, b.b.offset);
}

static void
client_node_marshal_add_port (void         *object,
                              uint32_t      seq,
                              SpaDirection  direction,
                              uint32_t      port_id)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, seq,
        SPA_POD_TYPE_INT, direction,
        SPA_POD_TYPE_INT, port_id,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 2, b.b.offset);
}

static void
client_node_marshal_remove_port (void         *object,
                                 uint32_t      seq,
                                 SpaDirection  direction,
                                 uint32_t      port_id)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, seq,
        SPA_POD_TYPE_INT, direction,
        SPA_POD_TYPE_INT, port_id,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 3, b.b.offset);
}

static void
client_node_marshal_set_format (void              *object,
                                uint32_t           seq,
                                SpaDirection       direction,
                                uint32_t           port_id,
                                SpaPortFormatFlags flags,
                                const SpaFormat   *format)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, seq,
        SPA_POD_TYPE_INT, direction,
        SPA_POD_TYPE_INT, port_id,
        SPA_POD_TYPE_INT, flags,
        SPA_POD_TYPE_INT, format ? 1 : 0, 0);
  if (format)
    spa_pod_builder_add (&b.b, SPA_POD_TYPE_POD, format, 0);
  spa_pod_builder_add (&b.b, -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 4, b.b.offset);
}

static void
client_node_marshal_set_property (void              *object,
                                  uint32_t           seq,
                                  uint32_t           id,
                                  uint32_t           size,
                                  const void        *value)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, seq,
        SPA_POD_TYPE_INT, id,
        SPA_POD_TYPE_BYTES, value, size,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 5, b.b.offset);
}

static void
client_node_marshal_add_mem (void              *object,
                             SpaDirection       direction,
                             uint32_t           port_id,
                             uint32_t           mem_id,
                             SpaDataType        type,
                             int                memfd,
                             uint32_t           flags,
                             uint32_t           offset,
                             uint32_t           size)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, direction,
        SPA_POD_TYPE_INT, port_id,
        SPA_POD_TYPE_INT, mem_id,
        SPA_POD_TYPE_INT, type,
        SPA_POD_TYPE_INT, pinos_connection_add_fd (connection, memfd),
        SPA_POD_TYPE_INT, flags,
        SPA_POD_TYPE_INT, offset,
        SPA_POD_TYPE_INT, size,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 6, b.b.offset);
}

static void
client_node_marshal_use_buffers (void                  *object,
                                 uint32_t               seq,
                                 SpaDirection           direction,
                                 uint32_t               port_id,
                                 uint32_t               n_buffers,
                                 PinosClientNodeBuffer *buffers)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;
  uint32_t i, j;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, seq,
        SPA_POD_TYPE_INT, direction,
        SPA_POD_TYPE_INT, port_id,
        SPA_POD_TYPE_INT, n_buffers, 0);

  for (i = 0; i < n_buffers; i++) {
    SpaBuffer *buf = buffers[i].buffer;

    spa_pod_builder_add (&b.b,
        SPA_POD_TYPE_INT, buffers[i].mem_id,
        SPA_POD_TYPE_INT, buffers[i].offset,
        SPA_POD_TYPE_INT, buffers[i].size,
        SPA_POD_TYPE_INT, buf->id,
        SPA_POD_TYPE_INT, buf->n_metas, 0);

    for (j = 0; j < buf->n_metas; j++) {
      SpaMeta *m = &buf->metas[j];
      spa_pod_builder_add (&b.b,
          SPA_POD_TYPE_INT, m->type,
          SPA_POD_TYPE_INT, m->size, 0);
    }
    spa_pod_builder_add (&b.b, SPA_POD_TYPE_INT, buf->n_datas, 0);
    for (j = 0; j < buf->n_datas; j++) {
      SpaData *d = &buf->datas[j];
      spa_pod_builder_add (&b.b,
          SPA_POD_TYPE_INT, d->type,
          SPA_POD_TYPE_INT, SPA_PTR_TO_UINT32 (d->data),
          SPA_POD_TYPE_INT, d->flags,
          SPA_POD_TYPE_INT, d->mapoffset,
          SPA_POD_TYPE_INT, d->maxsize, 0);
    }
  }
  spa_pod_builder_add (&b.b, -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 7, b.b.offset);
}

static void
client_node_marshal_node_command (void                 *object,
                                  uint32_t              seq,
                                  const SpaNodeCommand *command)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, seq,
        SPA_POD_TYPE_BYTES, command, command->size,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 8, b.b.offset);
}

static void
client_node_marshal_port_command (void                 *object,
                                  uint32_t              port_id,
                                  const SpaNodeCommand *command)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, port_id,
        SPA_POD_TYPE_BYTES, command, command->size,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 9, b.b.offset);
}

static void
client_node_marshal_transport (void              *object,
                               int                memfd,
                               uint32_t           offset,
                               uint32_t           size)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, pinos_connection_add_fd (connection, memfd),
        SPA_POD_TYPE_INT, offset,
        SPA_POD_TYPE_INT, size,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 10, b.b.offset);
}

static bool
client_node_demarshal_update (void  *object,
                              void  *data,
                              size_t size)
{
  PinosResource *resource = object;
  SpaPODIter it;
  uint32_t change_mask, max_input_ports, max_output_ports, have_props;
  const SpaProps *props = NULL;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &change_mask,
        SPA_POD_TYPE_INT, &max_input_ports,
        SPA_POD_TYPE_INT, &max_output_ports,
        SPA_POD_TYPE_INT, &have_props,
        0))
    return false;

  if (have_props && !spa_pod_iter_get (&it, SPA_POD_TYPE_OBJECT, &props, 0))
    return false;

  pinos_client_node_do_update (resource, change_mask, max_input_ports, max_output_ports, props);
  return true;
}

static bool
client_node_demarshal_port_update (void  *object,
                                   void  *data,
                                   size_t size)
{
  PinosResource *resource = object;
  SpaPODIter it;
  uint32_t i, t, direction, port_id, change_mask, n_possible_formats, sz;
  const SpaProps *props = NULL;
  const SpaFormat **possible_formats = NULL, *format = NULL;
  SpaPortInfo info, *infop = NULL;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it,
        SPA_POD_TYPE_INT, &direction,
        SPA_POD_TYPE_INT, &port_id,
        SPA_POD_TYPE_INT, &change_mask,
        SPA_POD_TYPE_INT, &n_possible_formats,
        0))
    return false;

  possible_formats = alloca (n_possible_formats * sizeof (SpaFormat*));
  for (i = 0; i < n_possible_formats; i++)
    if (!spa_pod_iter_get (&it,SPA_POD_TYPE_OBJECT, &possible_formats[i], 0))
      return false;

  if (!spa_pod_iter_get (&it, SPA_POD_TYPE_INT, &t, 0) ||
      (t && !spa_pod_iter_get (&it, SPA_POD_TYPE_OBJECT, &format, 0)))
    return false;

  if (!spa_pod_iter_get (&it, SPA_POD_TYPE_INT, &t, 0) ||
      (t && !spa_pod_iter_get (&it, SPA_POD_TYPE_OBJECT, &props, 0)))
    return false;

  if (!spa_pod_iter_get (&it, SPA_POD_TYPE_INT, &t, 0))
    return false;

  if (t) {
    SpaDict dict;
    infop = &info;

    if (!spa_pod_iter_get (&it,
          SPA_POD_TYPE_INT, &info.flags,
          SPA_POD_TYPE_LONG, &info.maxbuffering,
          SPA_POD_TYPE_LONG, &info.latency,
          SPA_POD_TYPE_INT, &info.n_params,
          0))
      return false;

    info.params = alloca (info.n_params * sizeof (SpaAllocParam *));
    for (i = 0; i < info.n_params; i++)
      if (!spa_pod_iter_get (&it, SPA_POD_TYPE_BYTES, &info.params[i], &sz, 0))
        return false;

    if (!spa_pod_iter_get (&it, SPA_POD_TYPE_INT, &dict.n_items, 0))
      return false;

    info.extra = &dict;
    dict.items = alloca (dict.n_items * sizeof (SpaDictItem));
    for (i = 0; i < dict.n_items; i++) {
      if (!spa_pod_iter_get (&it,
            SPA_POD_TYPE_STRING, &dict.items[i].key,
            SPA_POD_TYPE_STRING, &dict.items[i].value,
            0))
        return false;
    }
  }

  pinos_client_node_do_port_update (resource,
                                    direction,
                                    port_id,
                                    change_mask,
                                    n_possible_formats,
                                    possible_formats,
                                    format,
                                    props,
                                    infop);
  return true;
}

static bool
client_node_demarshal_state_change (void  *object,
                                    void  *data,
                                    size_t size)
{
  PinosResource *resource = object;
  SpaPODIter it;
  uint32_t state;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it, SPA_POD_TYPE_INT, &state, 0))
    return false;

  pinos_client_node_do_state_change (resource, state);
  return true;
}

static bool
client_node_demarshal_event (void   *object,
                            void   *data,
                            size_t  size)
{
  PinosResource *resource = object;
  SpaPODIter it;
  SpaNodeEvent *event;
  uint32_t sz;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it, SPA_POD_TYPE_BYTES, &event, &sz, 0))
    return false;

  pinos_client_node_do_event (resource, event);
  return true;
}

static bool
client_node_demarshal_destroy (void   *object,
                              void   *data,
                              size_t  size)
{
  PinosResource *resource = object;
  SpaPODIter it;
  uint32_t seq;

  if (!spa_pod_iter_struct (&it, data, size) ||
      !spa_pod_iter_get (&it, SPA_POD_TYPE_INT, &seq, 0))
    return false;

  pinos_client_node_do_destroy (resource, seq);
  return true;
}

static void
link_marshal_info (void          *object,
                   PinosLinkInfo *info)
{
  PinosResource *resource = object;
  PinosConnection *connection = resource->client->protocol_private;
  Builder b = { { NULL, 0, 0, NULL, write_pod }, connection };
  SpaPODFrame f;

  spa_pod_builder_add (&b.b,
      SPA_POD_TYPE_STRUCT, &f,
        SPA_POD_TYPE_INT, info->id,
        SPA_POD_TYPE_LONG, info->change_mask,
        SPA_POD_TYPE_LONG, info->output_node_id,
        SPA_POD_TYPE_LONG, info->output_port_id,
        SPA_POD_TYPE_LONG, info->input_node_id,
        SPA_POD_TYPE_LONG, info->input_port_id,
     -SPA_POD_TYPE_STRUCT, &f, 0);

  pinos_connection_end_write (connection, resource->id, 0, b.b.offset);
}

const PinosCoreEvent pinos_protocol_native_server_core_event = {
  &core_marshal_info,
  &core_marshal_done,
  &core_marshal_error,
  &core_marshal_remove_id
};

const PinosDemarshalFunc pinos_protocol_native_server_core_demarshal[] = {
  &core_demarshal_client_update,
  &core_demarshal_sync,
  &core_demarshal_get_registry,
  &core_demarshal_create_node,
  &core_demarshal_create_client_node
};

const PinosRegistryEvent pinos_protocol_native_server_registry_event = {
  &registry_marshal_global,
  &registry_marshal_global_remove,
};

const PinosDemarshalFunc pinos_protocol_native_server_registry_demarshal[] = {
  &registry_demarshal_bind,
};

const PinosModuleEvent pinos_protocol_native_server_module_event = {
  &module_marshal_info,
};

const PinosNodeEvent pinos_protocol_native_server_node_event = {
  &node_marshal_done,
  &node_marshal_info,
};

const PinosClientEvent pinos_protocol_native_server_client_event = {
  &client_marshal_info,
};

const PinosClientNodeEvent pinos_protocol_native_server_client_node_events = {
  &client_node_marshal_done,
  &client_node_marshal_event,
  &client_node_marshal_add_port,
  &client_node_marshal_remove_port,
  &client_node_marshal_set_format,
  &client_node_marshal_set_property,
  &client_node_marshal_add_mem,
  &client_node_marshal_use_buffers,
  &client_node_marshal_node_command,
  &client_node_marshal_port_command,
  &client_node_marshal_transport,
};

const PinosDemarshalFunc pinos_protocol_native_server_client_node_demarshal[] = {
  &client_node_demarshal_update,
  &client_node_demarshal_port_update,
  &client_node_demarshal_state_change,
  &client_node_demarshal_event,
  &client_node_demarshal_destroy,
};

const PinosLinkEvent pinos_protocol_native_server_link_event = {
  &link_marshal_info,
};