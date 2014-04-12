//     imgflo - Flowhub.io Image-processing runtime
//     (c) 2014 The Grid
//     imgflo may be freely distributed under the MIT license

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <glib.h>
#include <gegl.h>
#include <json-glib/json-glib.h>

// GOAL: get JSON serialization support upstream, support building meta-operations with it

// Convert between the lib/Foo used by NoFlo and lib:foo used by GEGL
gchar *
component2geglop(const gchar *name) {
    gchar *dup = g_strdup(name);
    gchar *sep = g_strstr_len(dup, -1, "/");
    if (sep) {
        *sep = ':';
    }
    g_ascii_strdown(dup, -1);
    return dup;
}

gchar *
geglop2component(const gchar *name) {
    gchar *dup = g_strdup(name);
    gchar *sep = g_strstr_len(dup, -1, ":");
    if (sep) {
        *sep = '/';
    }
    g_ascii_strdown(dup, -1);
    return dup;
}


typedef struct {
    GeglNode *top;
    GHashTable *node_map;
    GHashTable *processor_map;
} Graph;


Graph *
graph_new() {
    Graph *self = g_new(Graph, 1);
    self->top = gegl_node_new();
    self->node_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->processor_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    return self;
}

void
graph_free(Graph *self) {

    g_object_unref(self->top);
    g_hash_table_destroy(self->node_map);
    g_hash_table_destroy(self->processor_map);

    g_free(self);
}

void
graph_add_iip(Graph *self, const gchar *node, const gchar *port, GValue *value)
{
    g_return_if_fail(self);
    g_return_if_fail(node);
    g_return_if_fail(port);
    g_return_if_fail(value);

    const gchar *iip = G_VALUE_HOLDS_STRING(value) ? g_value_get_string(value) : "IIP";
    fprintf(stdout, "'%s' -> %s %s\n", iip, node, port);

    GeglNode *t = g_hash_table_lookup(self->node_map, node);

    GParamSpec *paramspec = gegl_node_find_property(t, port);
    if (paramspec) {
        GType value_type = G_VALUE_TYPE(value);
        GType target_type = G_PARAM_SPEC_VALUE_TYPE(paramspec);

        if (g_type_is_a(value_type, target_type)) {
            // No conversion needed
            gegl_node_set_property(t, port, value);
        } else {
            // TODO: attempt generic GValue transforms here?
            if (G_IS_PARAM_SPEC_DOUBLE(paramspec)) {
                gdouble d = g_ascii_strtod (iip, NULL);
                if (d != 0.0) {
                    gegl_node_set(t, port, d, NULL);
                }
            } else {
                fprintf(stderr, "target_type=%s value_type=%s\n",
                        g_type_name(target_type), g_type_name(value_type));
                fprintf(stderr, "Unable to convert value for property '%s' of node '%s'\n",
                        port, node);
            }
        }
    } else {
        fprintf(stderr, "Node '%s' has no property '%s'\n", node, port);
    }
}

void
graph_add_node(Graph *self, const gchar *name, const gchar *component)
{
    g_return_if_fail(self);
    g_return_if_fail(name);
    g_return_if_fail(component);

    if (g_strcmp0(component, "Processor") == 0) {
        Processor *proc = processor_new();
        g_hash_table_insert(self->processor_map, (gpointer)g_strdup(name), (gpointer)proc);
        return;
    }

    gchar *op = component2geglop(component);
    GeglNode *n = gegl_node_new_child(self->top, "operation", op, NULL);

    g_return_if_fail(n);

    fprintf(stdout, "%s(%s)\n", name, op);
    g_hash_table_insert(self->node_map, (gpointer)g_strdup(name), (gpointer)n);

    g_free(op);
}


void
graph_add_edge(Graph *self,
        const gchar *src, const gchar *srcport,
        const gchar *tgt, const gchar *tgtport)
{
    g_return_if_fail(self);
    g_return_if_fail(src);
    g_return_if_fail(tgt);
    g_return_if_fail(srcport);
    g_return_if_fail(tgtport);

    Processor *p = g_hash_table_lookup(self->processor_map, tgt);
    if (p) {
        GeglNode *s = g_hash_table_lookup(self->node_map, src);
        g_return_if_fail(s);
        processor_set_target(p, s);
        return;
    }

    GeglNode *t = g_hash_table_lookup(self->node_map, tgt);
    GeglNode *s = g_hash_table_lookup(self->node_map, src);

    g_return_if_fail(t);
    g_return_if_fail(s);

    fprintf(stdout, "%s %s -> %s %s\n", src, srcport,
                                        tgtport, tgt);
    gegl_node_connect_to(s, srcport, t, tgtport);
}

void
graph_load_json(Graph *self, JsonParser *parser) {

    JsonNode *rootnode = json_parser_get_root(parser);
    g_assert(JSON_NODE_HOLDS_OBJECT(rootnode));
    JsonObject *root = json_node_get_object(rootnode);

    // Processes
    JsonObject *processes = json_object_get_object_member(root, "processes");
    g_assert(processes);

    GList *process_names = json_object_get_members(processes);
    for (int i=0; i<g_list_length(process_names); i++) {
        const gchar *name = g_list_nth_data(process_names, i);
        JsonObject *proc = json_object_get_object_member(processes, name);
        const gchar *component = json_object_get_string_member(proc, "component");
        graph_add_node(self, name, component);
    }

    //g_free(process_names); crashes??

    // Connections
    JsonArray *connections = json_object_get_array_member(root, "connections");
    g_assert(connections);
    for (int i=0; i<json_array_get_length(connections); i++) {
        JsonObject *conn = json_array_get_object_element(connections, i);

        JsonObject *tgt = json_object_get_object_member(conn, "tgt");
        const gchar *tgt_proc = json_object_get_string_member(tgt, "process");
        const gchar *tgt_port = json_object_get_string_member(tgt, "port");

        JsonNode *srcnode = json_object_get_member(conn, "src");
        if (srcnode) {
            // Connection
            JsonObject *src = json_object_get_object_member(conn, "src");
            const gchar *src_proc = json_object_get_string_member(src, "process");
            const gchar *src_port = json_object_get_string_member(src, "port");

            graph_add_edge(self, src_proc, src_port, tgt_proc, tgt_port);
        } else {
            // IIP
            JsonNode *datanode = json_object_get_member(conn, "data");
            GValue value = G_VALUE_INIT;
            g_assert(JSON_NODE_HOLDS_VALUE(datanode));
            json_node_get_value(datanode, &value);

            graph_add_iip(self, tgt_proc, tgt_port, &value);
            g_value_unset(&value);
        }
    }
}

gboolean
graph_load_json_file(Graph *self, const gchar *path, GError **error) {

    JsonParser *parser = json_parser_new();

    gboolean success = json_parser_load_from_file(parser, path, error);
    if (success) {
        graph_load_json(self, parser);
    }

    g_object_unref(parser);
    return success;
}
