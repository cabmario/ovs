/* Copyright (c) 2015, 2016 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "bitmap.h"
#include "byte-order.h"
#include "dirs.h"
#include "flow.h"
#include "hash.h"
#include "hindex.h"
#include "hmap.h"
#include "ofctrl.h"
#include "openflow/openflow.h"
#include "openvswitch/dynamic-string.h"
#include "openvswitch/list.h"
#include "openvswitch/match.h"
#include "openvswitch/ofp-actions.h"
#include "openvswitch/ofp-msgs.h"
#include "openvswitch/ofp-parse.h"
#include "openvswitch/ofp-print.h"
#include "openvswitch/ofp-util.h"
#include "openvswitch/ofpbuf.h"
#include "openvswitch/vlog.h"
#include "ovn-controller.h"
#include "ovn/lib/actions.h"
#include "physical.h"
#include "rconn.h"
#include "socket-util.h"
#include "util.h"
#include "vswitch-idl.h"

VLOG_DEFINE_THIS_MODULE(ofctrl);

/* An OpenFlow flow. */
struct ovn_flow {
    struct hmap_node match_hmap_node; /* For match based hashing. */
    struct hindex_node uuid_hindex_node; /* For uuid based hashing. */
    struct ovs_list list_node; /* For handling lists of flows. */

    /* Key. */
    uint8_t table_id;
    uint16_t priority;
    struct match match;

    /* Data. UUID is used for disambiguation. */
    struct uuid uuid;
    struct ofpact *ofpacts;
    size_t ofpacts_len;
};

static uint32_t ovn_flow_match_hash(const struct ovn_flow *);
static void ovn_flow_lookup(struct hmap *, const struct ovn_flow *target,
                            struct ovs_list *answers);
static char *ovn_flow_to_string(const struct ovn_flow *);
static void ovn_flow_log(const struct ovn_flow *, const char *action);
static void ovn_flow_destroy(struct ovn_flow *);

static ovs_be32 queue_msg(struct ofpbuf *);
static void queue_flow_mod(struct ofputil_flow_mod *);

/* OpenFlow connection to the switch. */
static struct rconn *swconn;

static void queue_group_mod(struct ofputil_group_mod *);

/* Last seen sequence number for 'swconn'.  When this differs from
 * rconn_get_connection_seqno(rconn), 'swconn' has reconnected. */
static unsigned int seqno;

/* Connection state machine. */
#define STATES                                  \
    STATE(S_NEW)                                \
    STATE(S_TLV_TABLE_REQUESTED)                \
    STATE(S_TLV_TABLE_MOD_SENT)                 \
    STATE(S_CLEAR_FLOWS)                        \
    STATE(S_UPDATE_FLOWS)
enum ofctrl_state {
#define STATE(NAME) NAME,
    STATES
#undef STATE
};

/* Current state. */
static enum ofctrl_state state;

/* Transaction IDs for messages in flight to the switch. */
static ovs_be32 xid, xid2;

/* Counter for in-flight OpenFlow messages on 'swconn'.  We only send a new
 * round of flow table modifications to the switch when the counter falls to
 * zero, to avoid unbounded buffering. */
static struct rconn_packet_counter *tx_counter;

/* Flow table of "struct ovn_flow"s, that holds the flow table currently
 * installed in the switch. */
static struct hmap installed_flows;

/* A reference to the group_table. */
static struct group_table *groups;

/* MFF_* field ID for our Geneve option.  In S_TLV_TABLE_MOD_SENT, this is
 * the option we requested (we don't know whether we obtained it yet).  In
 * S_CLEAR_FLOWS or S_UPDATE_FLOWS, this is really the option we have. */
static enum mf_field_id mff_ovn_geneve;

static void ovn_flow_table_destroy(void);

static void ovn_group_table_clear(struct group_table *group_table,
                                  bool existing);

static void ofctrl_recv(const struct ofp_header *, enum ofptype);

static struct hmap match_flow_table = HMAP_INITIALIZER(&match_flow_table);
static struct hindex uuid_flow_table = HINDEX_INITIALIZER(&uuid_flow_table);

void
ofctrl_init(void)
{
    swconn = rconn_create(5, 0, DSCP_DEFAULT, 1 << OFP13_VERSION);
    tx_counter = rconn_packet_counter_create();
    hmap_init(&installed_flows);
}

/* S_NEW, for a new connection.
 *
 * Sends NXT_TLV_TABLE_REQUEST and transitions to
 * S_TLV_TABLE_REQUESTED. */

static void
run_S_NEW(void)
{
    struct ofpbuf *buf = ofpraw_alloc(OFPRAW_NXT_TLV_TABLE_REQUEST,
                                      rconn_get_version(swconn), 0);
    xid = queue_msg(buf);
    state = S_TLV_TABLE_REQUESTED;
}

static void
recv_S_NEW(const struct ofp_header *oh OVS_UNUSED,
           enum ofptype type OVS_UNUSED)
{
    OVS_NOT_REACHED();
}

/* S_TLV_TABLE_REQUESTED, when NXT_TLV_TABLE_REQUEST has been sent
 * and we're waiting for a reply.
 *
 * If we receive an NXT_TLV_TABLE_REPLY:
 *
 *     - If it contains our tunnel metadata option, assign its field ID to
 *       mff_ovn_geneve and transition to S_CLEAR_FLOWS.
 *
 *     - Otherwise, if there is an unused tunnel metadata field ID, send
 *       NXT_TLV_TABLE_MOD and OFPT_BARRIER_REQUEST, and transition to
 *       S_TLV_TABLE_MOD_SENT.
 *
 *     - Otherwise, log an error, disable Geneve, and transition to
 *       S_CLEAR_FLOWS.
 *
 * If we receive an OFPT_ERROR:
 *
 *     - Log an error, disable Geneve, and transition to S_CLEAR_FLOWS. */

static void
run_S_TLV_TABLE_REQUESTED(void)
{
}

static void
recv_S_TLV_TABLE_REQUESTED(const struct ofp_header *oh, enum ofptype type)
{
    if (oh->xid != xid) {
        ofctrl_recv(oh, type);
    } else if (type == OFPTYPE_NXT_TLV_TABLE_REPLY) {
        struct ofputil_tlv_table_reply reply;
        enum ofperr error = ofputil_decode_tlv_table_reply(oh, &reply);
        if (error) {
            VLOG_ERR("failed to decode TLV table request (%s)",
                     ofperr_to_string(error));
            goto error;
        }

        const struct ofputil_tlv_map *map;
        uint64_t md_free = UINT64_MAX;
        BUILD_ASSERT(TUN_METADATA_NUM_OPTS == 64);

        LIST_FOR_EACH (map, list_node, &reply.mappings) {
            if (map->option_class == OVN_GENEVE_CLASS
                && map->option_type == OVN_GENEVE_TYPE
                && map->option_len == OVN_GENEVE_LEN) {
                if (map->index >= TUN_METADATA_NUM_OPTS) {
                    VLOG_ERR("desired Geneve tunnel option 0x%"PRIx16","
                             "%"PRIu8",%"PRIu8" already in use with "
                             "unsupported index %"PRIu16,
                             map->option_class, map->option_type,
                             map->option_len, map->index);
                    goto error;
                } else {
                    mff_ovn_geneve = MFF_TUN_METADATA0 + map->index;
                    state = S_CLEAR_FLOWS;
                    return;
                }
            }

            if (map->index < TUN_METADATA_NUM_OPTS) {
                md_free &= ~(UINT64_C(1) << map->index);
            }
        }

        VLOG_DBG("OVN Geneve option not found");
        if (!md_free) {
            VLOG_ERR("no Geneve options free for use by OVN");
            goto error;
        }

        unsigned int index = rightmost_1bit_idx(md_free);
        mff_ovn_geneve = MFF_TUN_METADATA0 + index;
        struct ofputil_tlv_map tm;
        tm.option_class = OVN_GENEVE_CLASS;
        tm.option_type = OVN_GENEVE_TYPE;
        tm.option_len = OVN_GENEVE_LEN;
        tm.index = index;

        struct ofputil_tlv_table_mod ttm;
        ttm.command = NXTTMC_ADD;
        ovs_list_init(&ttm.mappings);
        ovs_list_push_back(&ttm.mappings, &tm.list_node);

        xid = queue_msg(ofputil_encode_tlv_table_mod(OFP13_VERSION, &ttm));
        xid2 = queue_msg(ofputil_encode_barrier_request(OFP13_VERSION));
        state = S_TLV_TABLE_MOD_SENT;
    } else if (type == OFPTYPE_ERROR) {
        VLOG_ERR("switch refused to allocate Geneve option (%s)",
                 ofperr_to_string(ofperr_decode_msg(oh, NULL)));
        goto error;
    } else {
        char *s = ofp_to_string(oh, ntohs(oh->length), 1);
        VLOG_ERR("unexpected reply to TLV table request (%s)",
                 s);
        free(s);
        goto error;
    }
    return;

error:
    mff_ovn_geneve = 0;
    state = S_CLEAR_FLOWS;
}

/* S_TLV_TABLE_MOD_SENT, when NXT_TLV_TABLE_MOD and OFPT_BARRIER_REQUEST
 * have been sent and we're waiting for a reply to one or the other.
 *
 * If we receive an OFPT_ERROR:
 *
 *     - If the error is NXTTMFC_ALREADY_MAPPED or NXTTMFC_DUP_ENTRY, we
 *       raced with some other controller.  Transition to S_NEW.
 *
 *     - Otherwise, log an error, disable Geneve, and transition to
 *       S_CLEAR_FLOWS.
 *
 * If we receive OFPT_BARRIER_REPLY:
 *
 *     - Set the tunnel metadata field ID to the one that we requested.
 *       Transition to S_CLEAR_FLOWS.
 */

static void
run_S_TLV_TABLE_MOD_SENT(void)
{
}

static void
recv_S_TLV_TABLE_MOD_SENT(const struct ofp_header *oh, enum ofptype type)
{
    if (oh->xid != xid && oh->xid != xid2) {
        ofctrl_recv(oh, type);
    } else if (oh->xid == xid2 && type == OFPTYPE_BARRIER_REPLY) {
        state = S_CLEAR_FLOWS;
    } else if (oh->xid == xid && type == OFPTYPE_ERROR) {
        enum ofperr error = ofperr_decode_msg(oh, NULL);
        if (error == OFPERR_NXTTMFC_ALREADY_MAPPED ||
            error == OFPERR_NXTTMFC_DUP_ENTRY) {
            VLOG_INFO("raced with another controller adding "
                      "Geneve option (%s); trying again",
                      ofperr_to_string(error));
            state = S_NEW;
        } else {
            VLOG_ERR("error adding Geneve option (%s)",
                     ofperr_to_string(error));
            goto error;
        }
    } else {
        char *s = ofp_to_string(oh, ntohs(oh->length), 1);
        VLOG_ERR("unexpected reply to Geneve option allocation request (%s)",
                 s);
        free(s);
        goto error;
    }
    return;

error:
    state = S_CLEAR_FLOWS;
}

/* S_CLEAR_FLOWS, after we've established a Geneve metadata field ID and it's
 * time to set up some flows.
 *
 * Sends an OFPT_TABLE_MOD to clear all flows, then transitions to
 * S_UPDATE_FLOWS. */

static void
run_S_CLEAR_FLOWS(void)
{
    /* Send a flow_mod to delete all flows. */
    struct ofputil_flow_mod fm = {
        .match = MATCH_CATCHALL_INITIALIZER,
        .table_id = OFPTT_ALL,
        .command = OFPFC_DELETE,
    };
    queue_flow_mod(&fm);
    VLOG_DBG("clearing all flows");

    struct ofputil_group_mod gm;
    memset(&gm, 0, sizeof gm);
    gm.command = OFPGC11_DELETE;
    gm.group_id = OFPG_ALL;
    gm.command_bucket_id = OFPG15_BUCKET_ALL;
    ovs_list_init(&gm.buckets);
    queue_group_mod(&gm);
    ofputil_bucket_list_destroy(&gm.buckets);

    /* Clear installed_flows, to match the state of the switch. */
    ovn_flow_table_clear();

    /* Clear existing groups, to match the state of the switch. */
    if (groups) {
        ovn_group_table_clear(groups, true);
    }

    state = S_UPDATE_FLOWS;
}

static void
recv_S_CLEAR_FLOWS(const struct ofp_header *oh, enum ofptype type)
{
    ofctrl_recv(oh, type);
}

/* S_UPDATE_FLOWS, for maintaining the flow table over time.
 *
 * Compare the installed flows to the ones we want.  Send OFPT_FLOW_MOD as
 * necessary.
 *
 * This is a terminal state.  We only transition out of it if the connection
 * drops. */

static void
run_S_UPDATE_FLOWS(void)
{
    /* Nothing to do here.
     *
     * Being in this state enables ofctrl_put() to work, however. */
}

static void
recv_S_UPDATE_FLOWS(const struct ofp_header *oh, enum ofptype type)
{
    ofctrl_recv(oh, type);
}

/* Runs the OpenFlow state machine against 'br_int', which is local to the
 * hypervisor on which we are running.  Attempts to negotiate a Geneve option
 * field for class OVN_GENEVE_CLASS, type OVN_GENEVE_TYPE.  If successful,
 * returns the MFF_* field ID for the option, otherwise returns 0. */
enum mf_field_id
ofctrl_run(const struct ovsrec_bridge *br_int)
{
    if (br_int) {
        char *target;
        target = xasprintf("unix:%s/%s.mgmt", ovs_rundir(), br_int->name);
        if (strcmp(target, rconn_get_target(swconn))) {
            VLOG_INFO("%s: connecting to switch", target);
            rconn_connect(swconn, target, target);
        }
        free(target);
    } else {
        rconn_disconnect(swconn);
    }

    rconn_run(swconn);

    if (!rconn_is_connected(swconn)) {
        return 0;
    }
    if (seqno != rconn_get_connection_seqno(swconn)) {
        seqno = rconn_get_connection_seqno(swconn);
        state = S_NEW;
    }

    enum ofctrl_state old_state;
    do {
        old_state = state;
        switch (state) {
#define STATE(NAME) case NAME: run_##NAME(); break;
            STATES
#undef STATE
        default:
            OVS_NOT_REACHED();
        }
    } while (state != old_state);

    for (int i = 0; state == old_state && i < 50; i++) {
        struct ofpbuf *msg = rconn_recv(swconn);
        if (!msg) {
            break;
        }

        const struct ofp_header *oh = msg->data;
        enum ofptype type;
        enum ofperr error;

        error = ofptype_decode(&type, oh);
        if (!error) {
            switch (state) {
#define STATE(NAME) case NAME: recv_##NAME(oh, type); break;
                STATES
#undef STATE
            default:
                OVS_NOT_REACHED();
            }
        } else {
            char *s = ofp_to_string(oh, ntohs(oh->length), 1);
            VLOG_WARN("could not decode OpenFlow message (%s): %s",
                      ofperr_to_string(error), s);
            free(s);
        }

        ofpbuf_delete(msg);
    }

    return (state == S_CLEAR_FLOWS || state == S_UPDATE_FLOWS
            ? mff_ovn_geneve : 0);
}

void
ofctrl_wait(void)
{
    rconn_run_wait(swconn);
    rconn_recv_wait(swconn);
}

void
ofctrl_destroy(void)
{
    rconn_destroy(swconn);
    ovn_flow_table_destroy();
    rconn_packet_counter_destroy(tx_counter);
}

static ovs_be32
queue_msg(struct ofpbuf *msg)
{
    const struct ofp_header *oh = msg->data;
    ovs_be32 xid = oh->xid;
    rconn_send(swconn, msg, tx_counter);
    return xid;
}

static void
log_openflow_rl(struct vlog_rate_limit *rl, enum vlog_level level,
                const struct ofp_header *oh, const char *title)
{
    if (!vlog_should_drop(&this_module, level, rl)) {
        char *s = ofp_to_string(oh, ntohs(oh->length), 2);
        vlog(&this_module, level, "%s: %s", title, s);
        free(s);
    }
}

static void
ofctrl_recv(const struct ofp_header *oh, enum ofptype type)
{
    if (type == OFPTYPE_ECHO_REQUEST) {
        queue_msg(make_echo_reply(oh));
    } else if (type == OFPTYPE_ERROR) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(30, 300);
        log_openflow_rl(&rl, VLL_INFO, oh, "OpenFlow error");
    } else if (type != OFPTYPE_ECHO_REPLY &&
               type != OFPTYPE_BARRIER_REPLY &&
               type != OFPTYPE_PACKET_IN &&
               type != OFPTYPE_PORT_STATUS &&
               type != OFPTYPE_FLOW_REMOVED) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(30, 300);
        log_openflow_rl(&rl, VLL_DBG, oh, "OpenFlow packet ignored");
    }
}

/* Flow table interfaces to the rest of ovn-controller. */

static void
log_ovn_flow_rl(struct vlog_rate_limit *rl, enum vlog_level level,
                const struct ovn_flow *flow, const char *title)
{
    if (!vlog_should_drop(&this_module, level, rl)) {
        char *s = ovn_flow_to_string(flow);
        vlog(&this_module, level, "%s for parent "UUID_FMT": %s",
             title, UUID_ARGS(&flow->uuid), s);
        free(s);
    }
}

/* Adds a flow to the collection associated with 'uuid'.  The flow has the
 * specified 'match' and 'actions' to the OpenFlow table numbered 'table_id'
 * with the given 'priority'.  The caller retains ownership of 'match' and
 * 'actions'.
 *
 * Any number of flows may be associated with a given UUID.  The flows with a
 * given UUID must have a unique (table_id, priority, match) tuple.  A
 * duplicate within a generally indicates a bug in the ovn-controller code that
 * generated it, so this functions logs a warning.
 *
 * (table_id, priority, match) tuples should also be unique for flows with
 * different UUIDs, but it doesn't necessarily indicate a bug in
 * ovn-controller, for two reasons.  First, these duplicates could be caused by
 * logical flows generated by ovn-northd, which aren't ovn-controller's fault;
 * perhaps something should warn about these but the root cause is different.
 * Second, these duplicates might be transient, that is, they might go away
 * before the next call to ofctrl_run() if a call to ofctrl_remove_flows()
 * removes one or the other.
 *
 * This just assembles the desired flow tables in memory.  Nothing is actually
 * sent to the switch until a later call to ofctrl_run(). */
void
ofctrl_add_flow(uint8_t table_id, uint16_t priority,
                const struct match *match, const struct ofpbuf *actions,
                const struct uuid *uuid)
{
    /* Structure that uses table_id+priority+various things as hashes. */
    struct ovn_flow *f = xmalloc(sizeof *f);
    f->table_id = table_id;
    f->priority = priority;
    f->match = *match;
    f->ofpacts = xmemdup(actions->data, actions->size);
    f->ofpacts_len = actions->size;
    f->uuid = *uuid;
    f->match_hmap_node.hash = ovn_flow_match_hash(f);
    f->uuid_hindex_node.hash = uuid_hash(&f->uuid);

    /* Check to see if other flows exist with the same key (table_id priority,
     * match criteria) and uuid.  If so, discard this flow and log a
     * warning. */
    struct ovs_list existing;
    ovn_flow_lookup(&match_flow_table, f, &existing);
    struct ovn_flow *d;
    LIST_FOR_EACH (d, list_node, &existing) {
        if (uuid_equals(&f->uuid, &d->uuid)) {
            /* Duplicate flows with the same UUID indicate some kind of bug
             * (see the function-level comment), but we distinguish two
             * cases:
             *
             *     - If the actions for the duplicate flow are the same, then
             *       it's benign; it's hard to imagine how there could be a
             *       real problem.  Log at INFO level.
             *
             *     - If the actions are different, then one or the other set of
             *       actions must be wrong or (perhaps more likely) we've got a
             *       new set of actions replacing an old set but the caller
             *       neglected to use ofctrl_remove_flows() or
             *       ofctrl_set_flow() to do it properly.  Log at WARN level to
             *       get some attention.
             */
            if (ofpacts_equal(f->ofpacts, f->ofpacts_len,
                              d->ofpacts, d->ofpacts_len)) {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
                log_ovn_flow_rl(&rl, VLL_INFO, f, "duplicate flow");
            } else {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
                log_ovn_flow_rl(&rl, VLL_WARN, f,
                                "duplicate flow with modified action");

                /* It seems likely that the newer actions are the correct
                 * ones. */
                free(d->ofpacts);
                d->ofpacts = f->ofpacts;
                d->ofpacts_len = f->ofpacts_len;
                f->ofpacts = NULL;
            }
            ovn_flow_destroy(f);
            return;
        }
    }

    /* Otherwise, add the flow. */
    hmap_insert(&match_flow_table, &f->match_hmap_node,
                f->match_hmap_node.hash);
    hindex_insert(&uuid_flow_table, &f->uuid_hindex_node,
                f->uuid_hindex_node.hash);
}

/* Removes a bundles of flows from the flow table. */
void
ofctrl_remove_flows(const struct uuid *uuid)
{
    struct ovn_flow *f, *next;
    HINDEX_FOR_EACH_WITH_HASH_SAFE (f, next, uuid_hindex_node, uuid_hash(uuid),
                                    &uuid_flow_table) {
        if (uuid_equals(&f->uuid, uuid)) {
            hmap_remove(&match_flow_table, &f->match_hmap_node);
            hindex_remove(&uuid_flow_table, &f->uuid_hindex_node);
            ovn_flow_destroy(f);
        }
    }
}

/* Shortcut to remove all flows matching the supplied UUID and add this
 * flow. */
void
ofctrl_set_flow(uint8_t table_id, uint16_t priority,
                const struct match *match, const struct ofpbuf *actions,
                const struct uuid *uuid)
{
    ofctrl_remove_flows(uuid);
    ofctrl_add_flow(table_id, priority, match, actions, uuid);
}

/* ovn_flow. */

/* Duplicate an ovn_flow structure. */
struct ovn_flow *
ofctrl_dup_flow(struct ovn_flow *src)
{
    struct ovn_flow *dst = xmalloc(sizeof *dst);
    dst->table_id = src->table_id;
    dst->priority = src->priority;
    dst->match = src->match;
    dst->ofpacts = xmemdup(src->ofpacts, src->ofpacts_len);
    dst->ofpacts_len = src->ofpacts_len;
    dst->uuid = src->uuid;
    dst->match_hmap_node.hash = ovn_flow_match_hash(dst);
    dst->uuid_hindex_node.hash = uuid_hash(&src->uuid);
    return dst;
}

/* Returns a hash of the match key in 'f'. */
static uint32_t
ovn_flow_match_hash(const struct ovn_flow *f)
{
    return hash_2words((f->table_id << 16) | f->priority,
                       match_hash(&f->match, 0));
}

/* Compare two flows and return -1, 0, 1 based on whether a if less than,
 * equal to or greater than b. */
static int
ovn_flow_compare_flows(struct ovn_flow *a, struct ovn_flow *b)
{
    return uuid_compare_3way(&a->uuid, &b->uuid);
}

/* Given a list of ovn_flows, goes through the list and returns
 * a single flow, in a deterministic way. */
static struct ovn_flow *
ovn_flow_select_from_list(struct ovs_list *flows)
{
    struct ovn_flow *candidate;
    struct ovn_flow *answer = NULL;
    LIST_FOR_EACH (candidate, list_node, flows) {
        if (!answer || ovn_flow_compare_flows(candidate, answer) < 0) {
            answer = candidate;
        }
    }
    return answer;
}

/* Initializes and files in the supplied list with ovn_flows from 'flow_table'
 * whose key is identical to 'target''s key. */
static void
ovn_flow_lookup(struct hmap *flow_table, const struct ovn_flow *target,
                struct ovs_list *answer)
{
    struct ovn_flow *f;

    ovs_list_init(answer);
    HMAP_FOR_EACH_WITH_HASH (f, match_hmap_node, target->match_hmap_node.hash,
                             flow_table) {
        if (f->table_id == target->table_id
            && f->priority == target->priority
            && match_equal(&f->match, &target->match)) {
            ovs_list_push_back(answer, &f->list_node);
        }
    }
}

static char *
ovn_flow_to_string(const struct ovn_flow *f)
{
    struct ds s = DS_EMPTY_INITIALIZER;
    ds_put_format(&s, "table_id=%"PRIu8", ", f->table_id);
    ds_put_format(&s, "priority=%"PRIu16", ", f->priority);
    match_format(&f->match, &s, OFP_DEFAULT_PRIORITY);
    ds_put_cstr(&s, ", actions=");
    ofpacts_format(f->ofpacts, f->ofpacts_len, &s);
    return ds_steal_cstr(&s);
}

static void
ovn_flow_log(const struct ovn_flow *f, const char *action)
{
    if (VLOG_IS_DBG_ENABLED()) {
        char *s = ovn_flow_to_string(f);
        VLOG_DBG("%s flow: %s", action, s);
        free(s);
    }
}

static void
ovn_flow_destroy(struct ovn_flow *f)
{
    if (f) {
        free(f->ofpacts);
        free(f);
    }
}

/* Flow tables of struct ovn_flow. */

void
ovn_flow_table_clear(void)
{
    struct ovn_flow *f, *next;
    HMAP_FOR_EACH_SAFE (f, next, match_hmap_node, &match_flow_table) {
        hmap_remove(&match_flow_table, &f->match_hmap_node);
        hindex_remove(&uuid_flow_table, &f->uuid_hindex_node);
        ovn_flow_destroy(f);
    }
}

static void
ovn_flow_table_destroy(void)
{
    ovn_flow_table_clear();
    hmap_destroy(&match_flow_table);
    hindex_destroy(&uuid_flow_table);
}

/* Flow table update. */

static void
queue_flow_mod(struct ofputil_flow_mod *fm)
{
    fm->buffer_id = UINT32_MAX;
    fm->out_port = OFPP_ANY;
    fm->out_group = OFPG_ANY;
    queue_msg(ofputil_encode_flow_mod(fm, OFPUTIL_P_OF13_OXM));
}


/* group_table. */

/* Finds and returns a group_info in 'existing_groups' whose key is identical
 * to 'target''s key, or NULL if there is none. */
static struct group_info *
ovn_group_lookup(struct hmap *exisiting_groups,
                 const struct group_info *target)
{
    struct group_info *e;

    HMAP_FOR_EACH_WITH_HASH(e, hmap_node, target->hmap_node.hash,
                            exisiting_groups) {
        if (e->group_id == target->group_id) {
            return e;
        }
   }
    return NULL;
}

/* Clear either desired_groups or existing_groups in group_table. */
static void
ovn_group_table_clear(struct group_table *group_table, bool existing)
{
    struct group_info *g, *next;
    struct hmap *target_group = existing
                                ? &group_table->existing_groups
                                : &group_table->desired_groups;

    HMAP_FOR_EACH_SAFE (g, next, hmap_node, target_group) {
        hmap_remove(target_group, &g->hmap_node);
        bitmap_set0(group_table->group_ids, g->group_id);
        ds_destroy(&g->group);
        free(g);
    }
}

static void
queue_group_mod(struct ofputil_group_mod *gm)
{
    queue_msg(ofputil_encode_group_mod(OFP13_VERSION, gm));
}


/* Replaces the flow table on the switch, if possible, by the flows in added
 * with ofctrl_add_flow().
 *
 * Replaces the group table on the switch, if possible, by the groups in
 * 'group_table->desired_groups'. Regardless of whether the group table
 * is updated, this deletes all the groups from the
 * 'group_table->desired_groups' and frees them. (The hmap itself isn't
 * destroyed.)
 *
 * This should be called after ofctrl_run() within the main loop. */
void
ofctrl_put(struct group_table *group_table)
{
    if (!groups) {
        groups = group_table;
    }

    /* The flow table can be updated if the connection to the switch is up and
     * in the correct state and not backlogged with existing flow_mods.  (Our
     * criteria for being backlogged appear very conservative, but the socket
     * between ovn-controller and OVS provides some buffering.) */
    if (state != S_UPDATE_FLOWS
        || rconn_packet_counter_n_packets(tx_counter)) {
        ovn_group_table_clear(group_table, false);
        return;
    }

    /* Iterate through all the desired groups. If there are new ones,
     * add them to the switch. */
    struct group_info *desired;
    HMAP_FOR_EACH(desired, hmap_node, &group_table->desired_groups) {
        if (!ovn_group_lookup(&group_table->existing_groups, desired)) {
            /* Create and install new group. */
            struct ofputil_group_mod gm;
            enum ofputil_protocol usable_protocols;
            char *error;
            struct ds group_string = DS_EMPTY_INITIALIZER;
            ds_put_format(&group_string, "group_id=%u,%s",
                          desired->group_id, ds_cstr(&desired->group));

            error = parse_ofp_group_mod_str(&gm, OFPGC11_ADD,
                                            ds_cstr(&group_string),
                                            &usable_protocols);
            if (!error) {
                queue_group_mod(&gm);
            } else {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
                VLOG_ERR_RL(&rl, "new group %s %s", error,
                         ds_cstr(&group_string));
                free(error);
            }
            ds_destroy(&group_string);
            ofputil_bucket_list_destroy(&gm.buckets);
        }
    }

    /* Iterate through all of the installed flows.  If any of them are no
     * longer desired, delete them; if any of them should have different
     * actions, update them. */
    struct ovn_flow *i, *next;
    HMAP_FOR_EACH_SAFE (i, next, match_hmap_node, &installed_flows) {
        struct ovs_list matches;
        ovn_flow_lookup(&match_flow_table, i, &matches);
        if (ovs_list_is_empty(&matches)) {
            /* Installed flow is no longer desirable.  Delete it from the
             * switch and from installed_flows. */
            struct ofputil_flow_mod fm = {
                .match = i->match,
                .priority = i->priority,
                .table_id = i->table_id,
                .command = OFPFC_DELETE_STRICT,
            };
            queue_flow_mod(&fm);
            ovn_flow_log(i, "removing installed");

            hmap_remove(&installed_flows, &i->match_hmap_node);
            ovn_flow_destroy(i);
        } else {
            /* Since we still have desired flows that match this key,
             * select one and compare both its actions and uuid.
             * If the actions aren't the same, queue and update
             * action for the install flow.  If the uuid has changed
             * update that as well. */
            struct ovn_flow *d = ovn_flow_select_from_list(&matches);
            if (!uuid_equals(&i->uuid, &d->uuid)) {
                /* Update installed flow's UUID. */
                i->uuid = d->uuid;
            }
            if (!ofpacts_equal(i->ofpacts, i->ofpacts_len,
                               d->ofpacts, d->ofpacts_len)) {
                /* Update actions in installed flow. */
                struct ofputil_flow_mod fm = {
                    .match = i->match,
                    .priority = i->priority,
                    .table_id = i->table_id,
                    .ofpacts = d->ofpacts,
                    .ofpacts_len = d->ofpacts_len,
                    .command = OFPFC_MODIFY_STRICT,
                };
                queue_flow_mod(&fm);
                ovn_flow_log(i, "updating installed");

                /* Replace 'i''s actions by 'd''s. */
                free(i->ofpacts);
                i->ofpacts = xmemdup(d->ofpacts, d->ofpacts_len);
                i->ofpacts_len = d->ofpacts_len;
            }
        }
    }

    /* Iterate through the desired flows and add those that aren't found
     * in the installed flow table. */
    struct ovn_flow *c;
    HMAP_FOR_EACH (c, match_hmap_node, &match_flow_table) {
        struct ovs_list matches;
        ovn_flow_lookup(&installed_flows, c, &matches);
        if (ovs_list_is_empty(&matches)) {
            /* We have a key that isn't in the installed flows, so
             * look back into the desired flow list for all flows
             * that match this key, and select the one to be installed. */
            struct ovs_list candidates;
            ovn_flow_lookup(&match_flow_table, c, &candidates);
            struct ovn_flow *d = ovn_flow_select_from_list(&candidates);
            /* Send flow_mod to add flow. */
            struct ofputil_flow_mod fm = {
                .match = d->match,
                .priority = d->priority,
                .table_id = d->table_id,
                .ofpacts = d->ofpacts,
                .ofpacts_len = d->ofpacts_len,
                .command = OFPFC_ADD,
            };
            queue_flow_mod(&fm);
            ovn_flow_log(d, "adding installed");

            /* Copy 'd' from 'flow_table' to installed_flows. */
            struct ovn_flow *new_node = ofctrl_dup_flow(d);
            hmap_insert(&installed_flows, &new_node->match_hmap_node,
                        new_node->match_hmap_node.hash);
        }
    }

    /* Iterate through the installed groups from previous runs. If they
     * are not needed delete them. */
    struct group_info *installed, *next_group;
    HMAP_FOR_EACH_SAFE(installed, next_group, hmap_node,
                       &group_table->existing_groups) {
        if (!ovn_group_lookup(&group_table->desired_groups, installed)) {
            /* Delete the group. */
            struct ofputil_group_mod gm;
            enum ofputil_protocol usable_protocols;
            char *error;
            struct ds group_string = DS_EMPTY_INITIALIZER;
            ds_put_format(&group_string, "group_id=%u", installed->group_id);

            error = parse_ofp_group_mod_str(&gm, OFPGC11_DELETE,
                                            ds_cstr(&group_string),
                                            &usable_protocols);
            if (!error) {
                queue_group_mod(&gm);
            } else {
                static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
                VLOG_ERR_RL(&rl, "Error deleting group %d: %s",
                         installed->group_id, error);
                free(error);
            }
            ds_destroy(&group_string);
            ofputil_bucket_list_destroy(&gm.buckets);

            /* Remove 'installed' from 'group_table->existing_groups' */
            hmap_remove(&group_table->existing_groups, &installed->hmap_node);
            ds_destroy(&installed->group);

            /* Dealloc group_id. */
            bitmap_set0(group_table->group_ids, installed->group_id);
            free(installed);
        }
    }

    /* Move the contents of desired_groups to existing_groups. */
    HMAP_FOR_EACH_SAFE(desired, next_group, hmap_node,
                       &group_table->desired_groups) {
        hmap_remove(&group_table->desired_groups, &desired->hmap_node);
        if (!ovn_group_lookup(&group_table->existing_groups, desired)) {
            hmap_insert(&group_table->existing_groups, &desired->hmap_node,
                        desired->hmap_node.hash);
        } else {
            ds_destroy(&desired->group);
            free(desired);
        }
    }
}
