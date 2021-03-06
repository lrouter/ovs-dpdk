#include <config.h>
#include "dpif-netdev.h"
#include "dpif-netdev-private.h"
#include "dpif-netdev-offload.h"

#include <net/if.h>
#include "dp-packet.h"
#include "dpif.h"
#include "flow.h"
#include "openvswitch/match.h"
#include "netdev.h"
#include "netdev-provider.h"
#include "netdev-vport.h"
#include "netdev-dpdk.h"
#include "netdev-offload.h"
#include "netdev-vport-private.h"
#include "odp-util.h"
#include "openvswitch/vlog.h"
#include "unixctl.h"

VLOG_DEFINE_THIS_MODULE(dpif_netdev_offload);

static void *
dp_netdev_flow_offload_main(void *data);

static struct dp_flow_offload g_dp_flow_offload;
static struct ovsthread_once offload_thread_once
      = OVSTHREAD_ONCE_INITIALIZER;
static void
dp_netdev_dump_vtp_hw_flows(struct unixctl_conn *conn, int argc OVS_UNUSED,
                            const char *argv[], void *aux OVS_UNUSED);

struct dp_flow_offload *
dp_netdev_offload_new(void)
{
    if (ovsthread_once_start(&offload_thread_once)) {

        unixctl_command_register("offload/dump-vtp", "name",
                1, 1, dp_netdev_dump_vtp_hw_flows,
                NULL);

        struct dp_flow_offload *dp_flow_offload = &g_dp_flow_offload;
        ovs_mutex_init(&dp_flow_offload->mutex);
        ovs_list_init(&dp_flow_offload->list);
        xpthread_cond_init(&dp_flow_offload->cond, NULL);
        dp_flow_offload->exit = false;
        dp_flow_offload->req = true;
        dp_flow_offload->thread = \
                                  ovs_thread_create("hw_offload",
                                          dp_netdev_flow_offload_main, dp_flow_offload);
        ovsthread_once_done(&offload_thread_once);
    }
    return &g_dp_flow_offload;
}

void
dp_netdev_wait_offload_done(struct dp_flow_offload *offload)
{
    bool process;
    bool target;

    do {
        atomic_read_explicit(&offload->process, &process, memory_order_acquire);
        if (process == false) {
            /* not processing, check if no flows */
            ovs_mutex_lock(&offload->mutex);
            if (!ovs_list_is_empty(&offload->list)) {
                xpthread_cond_signal(&offload->cond);
                target = true;
            } else {
                target = false;
            }
            ovs_mutex_unlock(&offload->mutex);
        } else {
            /* processing */
            target = false;
        }
    } while (process != target);
}

void
dp_netdev_join_offload_thread(struct dp_flow_offload *offload)
{
    ovs_mutex_lock(&offload->mutex);
    atomic_store_explicit(&offload->exit, true, memory_order_release);
    xpthread_cond_signal(&offload->cond);
    ovs_mutex_unlock(&offload->mutex);
    xpthread_join(offload->thread, NULL);
    offload->thread = (pthread_t)0;
}

void
dp_netdev_offload_restart(struct dp_flow_offload *offload)
{
    offload->exit = false;
    offload->thread = \
        ovs_thread_create("hw_offload",
                          dp_netdev_flow_offload_main, offload);
}

struct tnl_offload_aux *
tnl_offload_aux_new(void)
{
    struct tnl_offload_aux *aux = xzalloc(sizeof(*aux));
    hmap_init(&aux->ingress_flows);
    hmap_init(&aux->tnl_pop_flows);
    ovs_rwlock_init(&aux->rwlock);
    return aux;
}

static void
ingress_flow_flush(struct tnl_offload_aux *aux);
static void
tnlflow_flush(struct tnl_offload_aux *aux);

void
tnl_offload_aux_free(void *offload_aux) {
    struct tnl_offload_aux *aux = offload_aux;
    ingress_flow_flush(aux);
    tnlflow_flush(aux);
    hmap_destroy(&aux->ingress_flows);
    hmap_destroy(&aux->tnl_pop_flows);
    ovs_rwlock_destroy(&aux->rwlock);
}

static struct dp_flow_offload_item *
dp_netdev_alloc_flow_offload(const struct dpif_class * const dpif_class,
                             struct dp_netdev_flow *flow,
                             struct dp_netdev_actions *old_act,
                             int op)
{
    struct dp_flow_offload_item *offload = NULL;

    if (!dp_netdev_flow_ref(flow)) {
        return NULL;
    }
    offload = xzalloc(sizeof(*offload));
    *CONST_CAST(const struct dpif_class **, &offload->class) = dpif_class;
    offload->flow = flow;
    offload->op = op;
    /* if get actions here, the actions might be 
     * freed during the queueing.
     */
    offload->dp_act = NULL; 
    if (old_act)
        offload->old_dp_act = \
                              dp_netdev_actions_create(\
                                      old_act->actions, \
                                      old_act->size);
    return offload;
}

static void
dp_netdev_free_flow_offload(struct dp_flow_offload_item *offload)
{
    dp_netdev_flow_unref(offload->flow);

    dp_netdev_actions_free(offload->old_dp_act);
    free(offload);
}

static void
dp_netdev_append_flow_offload(struct dp_flow_offload *dp_flow_offload,
                              struct dp_flow_offload_item *offload)
{
    ovs_list_push_back(&dp_flow_offload->list, &offload->node);
    if (!dp_flow_offload->process) {
        xpthread_cond_signal(&dp_flow_offload->cond);
    }
}

static void
tnl_pop_flow_get_ufid(struct ingress_flow *inflow,\
                      struct tnl_pop_flow *tnlflow, \
                      ovs_u128 *ufid)
{
    ufid->u64.hi = inflow->flow->mega_ufid.u64.hi ^ tnlflow->flow->mega_ufid.u64.hi;
    ufid->u64.lo = inflow->flow->mega_ufid.u64.lo ^ tnlflow->flow->mega_ufid.u64.lo;
}

static const struct nlattr *
dp_netdev_action_get(const struct nlattr *actions,
                     size_t act_len, enum ovs_action_attr type)
{
    const struct nlattr *a;
    unsigned int left;

    NL_ATTR_FOR_EACH_UNSAFE (a, left, actions, act_len) {
        int _type = nl_attr_type(a);
        if ((enum ovs_action_attr)_type == type) {
            return a;
        }
    }
    return NULL;
}

static struct ingress_flow *
ingress_flow_find(struct dp_netdev_flow *flow,\
                  struct tnl_offload_aux *aux, \
                  bool *found)
{
    *found = false;

    struct ingress_flow *inflow = NULL;

    ovs_rwlock_rdlock(&aux->rwlock);
    HMAP_FOR_EACH_WITH_HASH(inflow, node,
            dp_netdev_flow_hash(&flow->mega_ufid), &aux->ingress_flows) {
        if (ovs_u128_equals(inflow->flow->mega_ufid, flow->mega_ufid)) {
            *found = true;
            break;
        }
    }
    ovs_rwlock_unlock(&aux->rwlock);
    return inflow;
}

static void
ingress_flow_free(struct ingress_flow *inflow)
{
    dp_netdev_flow_unref(inflow->flow);
    netdev_close(inflow->ingress_netdev);
    free(inflow);
}

static void
ingress_flow_del(struct ingress_flow *inflow,\
                 struct tnl_offload_aux *aux)
{
    ovs_rwlock_wrlock(&aux->rwlock);
    hmap_remove(&aux->ingress_flows, &inflow->node);
    ovs_rwlock_unlock(&aux->rwlock);

    ingress_flow_free(inflow);
}

static struct ingress_flow *
ingress_flow_new(struct dp_netdev_flow *flow,
                 struct netdev *inport,
                 uint32_t action_flags)
{
    struct ingress_flow *inflow;

    inflow = xzalloc(sizeof(struct ingress_flow));
    inflow->ingress_netdev = inport;
    inflow->flow = flow;
    inflow->action_flags = action_flags;

    netdev_ref(inflow->ingress_netdev);
    dp_netdev_flow_ref(flow);
    return inflow;
}

static void
ingress_flow_insert(struct tnl_offload_aux *aux,
                    struct ingress_flow *inflow)
{
    ovs_rwlock_wrlock(&aux->rwlock);
    hmap_insert(&aux->ingress_flows, &inflow->node, \
                dp_netdev_flow_hash(&inflow->flow->mega_ufid)); 
    ovs_rwlock_unlock(&aux->rwlock);
}

static void
ingress_flow_flush(struct tnl_offload_aux *aux)
{
    struct ingress_flow *inflow;
    ovs_rwlock_wrlock(&aux->rwlock);
    HMAP_FOR_EACH_POP(inflow, node, &aux->ingress_flows) {
        ingress_flow_free(inflow);
    }
    ovs_rwlock_unlock(&aux->rwlock);
}

static int 
tnl_pop_flow_op_put(struct ingress_flow *inflow,\
                    struct tnl_pop_flow *tnlflow,\
                    struct dp_netdev_actions *act, \
                    struct offload_info *info)
{
    int ret;

    /* set inner info */
    struct match tnl_m; 
    miniflow_expand(&tnlflow->flow->cr.flow.mf, &tnl_m.flow);
    miniflow_expand(&tnlflow->flow->cr.mask->mf, &tnl_m.wc.masks);
    memset(&tnl_m.tun_md, 0, sizeof tnl_m.tun_md);

    /* set outter info */
    struct flow in_flow;
    miniflow_expand(&inflow->flow->cr.flow.mf, &in_flow);
    info->tp_dst_port = in_flow.tp_dst; 
    memcpy(&info->tun_dl_dst, &in_flow.dl_dst, sizeof(struct eth_addr));
    info->tun_dst = in_flow.nw_dst;

    ovs_u128 mega_ufid;
    tnl_pop_flow_get_ufid(inflow, tnlflow, &mega_ufid);
    info->action_flags |= tnlflow->action_flags;
    info->action_flags |= inflow->action_flags;

    ret = netdev_flow_put(inflow->ingress_netdev, &tnl_m,
            act->actions, act->size, &mega_ufid, info,
            NULL);
    return ret;
}

static int
tnl_pop_flow_op_del(struct ingress_flow *inflow, \
                    struct tnl_pop_flow *tnlflow)
{
    int ret;
    ovs_u128 mega_ufid;
    tnl_pop_flow_get_ufid(inflow, tnlflow, &mega_ufid);
    ret = netdev_flow_del(inflow->ingress_netdev, \
                            &mega_ufid, NULL);
    return ret;
}

static int
tnl_pop_flow_op_stat(struct ingress_flow *inflow, \
                     struct tnl_pop_flow *tnlflow, \
                     struct dpif_flow_stats *stats)
{
    int ret;
    ovs_u128 mega_ufid;
    tnl_pop_flow_get_ufid(inflow, tnlflow, &mega_ufid);
    ret = netdev_flow_get(inflow->ingress_netdev, NULL, NULL, \
                          &mega_ufid, stats, NULL, NULL);
    return ret;
}

static void
tnlflow_free(struct tnl_pop_flow *tnlflow)
{
    dp_netdev_flow_unref(tnlflow->flow);
    free(tnlflow);
}

static void
tnlflow_del(struct tnl_pop_flow *tnlflow, \
                 struct tnl_offload_aux *aux)
{
    ovs_rwlock_wrlock(&aux->rwlock);
    hmap_remove(&aux->tnl_pop_flows, &tnlflow->node);
    ovs_rwlock_unlock(&aux->rwlock);

    tnlflow_free(tnlflow);
}

static void
tnlflow_flush(struct tnl_offload_aux *aux)
{
    struct tnl_pop_flow *tnlflow;
    ovs_rwlock_wrlock(&aux->rwlock);
    HMAP_FOR_EACH_POP(tnlflow, node, &aux->tnl_pop_flows) {
        tnlflow_free(tnlflow);
    }
    ovs_rwlock_unlock(&aux->rwlock);
}

static void
__ingress_flow_op_flush(struct ingress_flow *inflow, \
                        struct tnl_offload_aux *aux)
{
    struct tnl_pop_flow *tnlflow;
    HMAP_FOR_EACH(tnlflow, node, &aux->tnl_pop_flows) {
        tnl_pop_flow_op_del(inflow, tnlflow);
    }
}

static void
ingress_flow_op_flush(struct ingress_flow *inflow, \
                      struct tnl_offload_aux *aux)
{
    ovs_rwlock_rdlock(&aux->rwlock);
    __ingress_flow_op_flush(inflow, aux); 
    ovs_rwlock_unlock(&aux->rwlock);
}
static void
__tnlflow_op_flush(struct tnl_pop_flow *tnlflow, \
                    struct tnl_offload_aux *aux)
{
    struct ingress_flow *inflow;
    HMAP_FOR_EACH(inflow, node, &aux->ingress_flows) {
        tnl_pop_flow_op_del(inflow, tnlflow);
    }
}

static void
tnlflow_op_flush(struct tnl_pop_flow *tnlflow, \
                 struct tnl_offload_aux *aux)
{
    ovs_rwlock_rdlock(&aux->rwlock);
    __tnlflow_op_flush(tnlflow, aux); 
    ovs_rwlock_unlock(&aux->rwlock);
}

static int
try_offload_tnl_pop(struct ingress_flow *inflow,\
                    struct tnl_offload_aux *aux,\
                    struct offload_info *info)
{
    struct tnl_pop_flow *tnlflow;
    int ret;
    int hint = 0;
    bool need_rollback = false;

    ovs_rwlock_wrlock(&aux->rwlock);
    HMAP_FOR_EACH(tnlflow, node, &aux->tnl_pop_flows) {
        tnlflow->status = OFFLOAD_NONE;
    }

    HMAP_FOR_EACH(tnlflow, node, &aux->tnl_pop_flows) {
        ret = tnl_pop_flow_op_put(inflow, tnlflow, \
                                    dp_netdev_flow_get_actions(tnlflow->flow), \
                                    info);
        if (ret == -1) {
            need_rollback = true;
            tnlflow->status = OFFLOAD_FAILED;
        } else {
            tnlflow->status = OFFLOAD_FULL;
            tnlflow->ref ++;
        }
    }

    if (need_rollback) {
        struct tnl_pop_flow *next;
        HMAP_FOR_EACH_SAFE(tnlflow, next, node, &aux->tnl_pop_flows) {
            if (tnlflow->status == OFFLOAD_FAILED) {
                if (tnlflow->ref == 0) {
                    atomic_store_explicit(&tnlflow->flow->status, OFFLOAD_FAILED,\
                                            memory_order_release); 
                    hmap_remove(&aux->tnl_pop_flows, &tnlflow->node);
                    tnlflow_free(tnlflow);
                } else {
                    /* this is weird as this inflow insert failed,
                     * however it is associated with another ingress-flow
                     * which means that it has been successfully put before.
                     */
                    VLOG_ERR("inflow merges tnlflow failed, but ref != 0\n");
                    hint = -1;
                }
            } else {
                tnl_pop_flow_op_del(inflow, tnlflow);
            }
        }
    }
    ovs_rwlock_unlock(&aux->rwlock);

    return hint;
}

static struct tnl_pop_flow*
tnlflow_find(struct dp_netdev_flow *flow, \
             struct tnl_offload_aux *aux, \
             bool *found)
{
    struct tnl_pop_flow *tnlflow;
    *found = false;

    ovs_rwlock_rdlock(&aux->rwlock);
    HMAP_FOR_EACH_WITH_HASH(tnlflow, node, \
                            dp_netdev_flow_hash(&flow->mega_ufid), \
                            &aux->tnl_pop_flows) {
        if (ovs_u128_equals(tnlflow->flow->mega_ufid, flow->mega_ufid)) {
            *found = true;
            break;
        }
    }
    ovs_rwlock_unlock(&aux->rwlock);
    return tnlflow;
}

static struct tnl_pop_flow*
tnlflow_new(struct dp_netdev_flow *flow, uint32_t action_flags)
{
    struct tnl_pop_flow *tnlflow;
    tnlflow = xzalloc(sizeof(struct tnl_pop_flow));
    tnlflow->flow = flow;
    tnlflow->action_flags = action_flags;
    dp_netdev_flow_ref(flow);
    return tnlflow;
}

static void
tnlflow_insert(struct tnl_offload_aux *aux, \
               struct tnl_pop_flow *tnlflow)
{
    ovs_rwlock_wrlock(&aux->rwlock);
    hmap_insert(&aux->tnl_pop_flows, \
            &tnlflow->node, \
            dp_netdev_flow_hash(&tnlflow->flow->mega_ufid));
    ovs_rwlock_unlock(&aux->rwlock);
}

static struct netdev *
try_ingress(struct dp_netdev_actions *act, \
            const struct dpif_class * const dpif_class)
{
    const struct nlattr *tnl_pop = dp_netdev_action_get(act->actions,\
            act->size, OVS_ACTION_ATTR_TUNNEL_POP);

    if (!tnl_pop) {
        return NULL;
    }

    odp_port_t portno = nl_attr_get_odp_port(tnl_pop);
    struct netdev *tnl_dev = netdev_ports_get(portno, dpif_class);
    if (!tnl_dev) {
        return NULL;
    }
    return tnl_dev;
}

static int 
del_ingress(struct dp_netdev_flow *flow,\
            struct netdev *tnl_dev)
{
    struct netdev_vport *vport = netdev_vport_cast(tnl_dev);
    struct tnl_offload_aux *aux  = vport->offload_aux;
    // get old act and get vport 
    bool found;
    struct ingress_flow *inflow = ingress_flow_find(flow, aux, &found);
    /* del all tnl_pop flows */
    /* there could be multiple flow from different PMDs */
    if (found && inflow->flow == flow) {
        ingress_flow_op_flush(inflow, aux);
        atomic_store_explicit(&inflow->flow->status, OFFLOAD_NONE, \
                        memory_order_release);
        ingress_flow_del(inflow, aux);
        netdev_close(tnl_dev);
        return 0;
    }
    netdev_close(tnl_dev);
    return -1;
}

static int
try_del_ingress(struct dp_netdev_flow *flow, \
                struct dp_netdev_actions *act, \
                const struct dpif_class * const dpif_class)
{
    struct netdev *tnl_dev;
    tnl_dev = try_ingress(act, dpif_class);
    if (!tnl_dev)
        return -1;
    return del_ingress(flow, tnl_dev);
}

static bool
try_tnlflow(struct dp_netdev_flow *flow, \
            struct netdev *inport)
{
    if(!flow_tnl_dst_is_set(&flow->flow.tunnel)) {
        return false;
    }
    
    if (!netdev_vport_is_vport_class(netdev_get_class(inport))) {
        return false;
    }

    if (!netdev_get_tunnel_config(inport)) {
        return false;
    }

    struct netdev_vport *vport = netdev_vport_cast(inport);

    if (!vport->offload_aux) {
        return false;
    }
    return true;
}

static int 
try_del_tnlflow(struct dp_netdev_flow *flow, \
                struct netdev *inport)
{
    bool is_tnlflow = try_tnlflow(flow, inport);
    if (!is_tnlflow) {
        return -1;
    }

    struct tnl_pop_flow *tnlflow;
    bool found; 
    struct netdev_vport * vport = netdev_vport_cast(inport);
    struct tnl_offload_aux * aux = vport->offload_aux;
    tnlflow = tnlflow_find(flow, aux, &found);
    if (found && tnlflow->flow == flow) {
       tnlflow_op_flush(tnlflow, aux); 
       atomic_store_explicit(&tnlflow->flow->status, OFFLOAD_NONE, \
                            memory_order_release);
       tnlflow_del(tnlflow, aux);
       return 0;
    }
    return -1;
}

static int
dp_netdev_flow_offload_del(struct dp_flow_offload_item *offload)
{
    struct netdev *netdev;
    struct dp_netdev_flow *flow = offload->flow;
    odp_port_t in_port = flow->flow.in_port.odp_port;
    const struct dpif_class *const dpif_class = offload->class;
    int ret = -1;

    netdev = netdev_ports_get(in_port, offload->class);
    if (!netdev) {
        /* this should never happens, if any netdev is 
         * removed, all hw flows should be removed first!
         * The only possible is that this flow has never
         * been offloaded before.
         */
        VLOG_ERR("try to del a flow that does not has a valid inport!\n");
        atomic_store_explicit(&flow->status, OFFLOAD_NONE, \
            memory_order_release);
        return -1;
    }

    ret = try_del_ingress(flow, offload->dp_act, dpif_class);
    if (!ret) {
        goto exit;
    }
    ret = try_del_tnlflow(flow, netdev);
    if (!ret) {
        goto exit;
    }
    ret = netdev_flow_del(netdev, &flow->mega_ufid, NULL);
    /* ignore ret */
    atomic_store_explicit(&flow->status, OFFLOAD_NONE, \
            memory_order_release);
exit:
    netdev_close(netdev);
    if (ret) {
        return -1;
    } else {
        dp_netdev_flow_unref(flow);
    }
    return 0;
}

static enum offload_status
dp_netdev_try_offload_tnl_pop(struct dp_netdev_flow *flow,\
                              struct netdev *inport, \
                              struct dp_flow_offload_item *offload, \
                              struct offload_info *info)
{
    struct ingress_flow *inflow;
    struct tnl_pop_flow *tnlflow;

    bool is_tnlflow = try_tnlflow(flow, inport);
    if (!is_tnlflow) {
        return OFFLOAD_NONE;
    }

    int ret = 0;
    bool need_rollback = false;

    struct dp_netdev_actions *act = offload->dp_act;

    bool found;
    struct netdev_vport *vport = netdev_vport_cast(inport);
    struct tnl_offload_aux * aux = vport->offload_aux;

    /* if it's add, find will fail, new one.
     * if it's mod, find might also fail, this is because
     * the previous insertion might fail.
     * let's try to insert anyway.
     */
    tnlflow = tnlflow_find(flow, aux, &found);
    if (!found)
        tnlflow = tnlflow_new(flow, info->action_flags);
    else if (tnlflow->flow != flow) {
        /* if exist, check if the flow is coming from
         * a different PMDs
         */
        return OFFLOAD_FAILED;
    }

    ovs_rwlock_rdlock(&aux->rwlock);
    HMAP_FOR_EACH(inflow, node, &aux->ingress_flows) {
        inflow->status = OFFLOAD_NONE;
    }

    HMAP_FOR_EACH(inflow, node, &aux->ingress_flows) {
        ret = tnl_pop_flow_op_put(inflow, tnlflow, act, info);
        if (ret == -1) {
            need_rollback = true;
            break;
        } else {
            tnlflow->ref ++;
            inflow->status = OFFLOAD_FULL;
        }
    }

    if (need_rollback) {
        HMAP_FOR_EACH(inflow, node, &aux->ingress_flows) {
            if (inflow->status == OFFLOAD_FULL) {
                tnlflow->ref --;
                tnl_pop_flow_op_del(inflow, tnlflow);
            }
        }
    }
    ovs_rwlock_unlock(&aux->rwlock);

    if (!found) {
        if (!need_rollback) {
            tnlflow_insert(aux, tnlflow);
        } else {
            tnlflow_free(tnlflow);
        }
        return need_rollback ? OFFLOAD_FAILED : OFFLOAD_FULL;
    }

    /* mod */
    if (need_rollback) {
        tnlflow_del(tnlflow, aux);
    }
    return need_rollback ? OFFLOAD_FAILED : OFFLOAD_FULL;
}

static bool
ingress_flow_validate(struct ingress_flow *inflow, \
                        struct offload_info *info)
{
    struct match m;
    miniflow_expand(&inflow->flow->cr.flow.mf, &m.flow);
    miniflow_expand(&inflow->flow->cr.mask->mf, &m.wc.masks);
    memset(&m.tun_md, 0, sizeof m.tun_md);

    info->mark_set = 1;
    int ret = netdev_flow_put(inflow->ingress_netdev, &m,
            NULL, 0, &inflow->flow->mega_ufid, info,
            NULL);
    info->mark_set = 0;
    if(ret)
        return false;
    netdev_flow_del(inflow->ingress_netdev, &inflow->flow->mega_ufid, NULL);
    return true;
}

static enum offload_status
dp_netdev_try_offload_ingress_add(struct dp_netdev_flow *flow,\
                                  struct netdev *inport,\
                                  struct dp_flow_offload_item *offload, \
                                  struct offload_info *info)
{
    struct dp_netdev_actions *act = offload->dp_act;
    const struct nlattr *tnl_pop = dp_netdev_action_get(act->actions,\
                                    act->size, OVS_ACTION_ATTR_TUNNEL_POP);
    if (!tnl_pop) {
        return OFFLOAD_NONE;
    }

    odp_port_t portno = nl_attr_get_odp_port(tnl_pop);
    struct netdev *tnl_dev = netdev_ports_get(portno, info->dpif_class);
    if (!tnl_dev) {
        return OFFLOAD_NONE;
    }
    struct netdev_vport *vport = netdev_vport_cast(tnl_dev);
    struct tnl_offload_aux *aux = vport->offload_aux;
    struct ingress_flow *inflow;
    bool found;
    inflow = ingress_flow_find(flow, aux, &found);
    /* multiple pmd thread has the same flow in
     * pmd thread flow table
     */
    if (!found) {
        inflow = ingress_flow_new(flow, inport, info->action_flags);
        bool valid;
        valid = ingress_flow_validate(inflow, info);
        if (!valid) {
            ingress_flow_free(inflow);
            netdev_close(tnl_dev);
            return OFFLOAD_FAILED;
        }
    } else {
        /* let the same flow in another PMD just fail */
        netdev_close(tnl_dev);
        return OFFLOAD_FAILED;
    }

    int ret = try_offload_tnl_pop(inflow, aux, info);
    if (ret) {
        ingress_flow_free(inflow);
        netdev_close(tnl_dev);
        return OFFLOAD_FAILED;
    }
    ingress_flow_insert(aux, inflow);
    netdev_close(tnl_dev);
    return OFFLOAD_FULL;
}

static char *get_act_str(struct ds *ds, struct dp_netdev_actions *act)
{
    format_odp_actions(ds, act->actions, act->size, NULL);
    return ds_cstr(ds);
}

static void dp_netdev_show_mod_act(struct dp_netdev_actions *act)
{
    struct ds ds;
    ds_init(&ds);
    VLOG_INFO("mod actions to:%s\n",  get_act_str(&ds, act));
    ds_destroy(&ds);
}

static enum offload_status
dp_netdev_try_offload_ingress(struct dp_netdev_flow *flow,\
                              const struct dpif_class *const dpif_class, \
                              struct netdev *inport,\
                              struct dp_flow_offload_item *offload, \
                              struct offload_info *info)
{
    if (offload->op == DP_NETDEV_FLOW_OFFLOAD_OP_ADD)
        return dp_netdev_try_offload_ingress_add(flow, inport, offload, info);

    if (offload->op == DP_NETDEV_FLOW_OFFLOAD_OP_MOD) {
        struct dp_netdev_actions *act = offload->old_dp_act;
        struct netdev *tnl_dev = try_ingress(act, dpif_class);
        if (!tnl_dev) {
            return OFFLOAD_NONE;
        }
        VLOG_INFO("MOD an ingress flow on port %d\n",
                flow->flow.in_port.odp_port);
        dp_netdev_show_mod_act(dp_netdev_flow_get_actions(flow));
        del_ingress(flow, tnl_dev);
        return OFFLOAD_NONE;
    }
    return OFFLOAD_NONE;
}

static int
dp_netdev_normal_offload(struct dp_netdev_flow *flow, \
                         struct netdev *netdev, \
                         struct dp_flow_offload_item *offload, \
                         struct offload_info *info)
{
    struct match m; 
    miniflow_expand(&flow->cr.flow.mf, &m.flow);
    miniflow_expand(&flow->cr.mask->mf, &m.wc.masks);
    memset(&m.tun_md, 0, sizeof m.tun_md);
    int ret;
    struct dp_netdev_actions *act = offload->dp_act;
    info->version = flow->version;

    ret = netdev_flow_put(netdev, &m, act->actions, \
            act->size, &flow->mega_ufid, info, NULL);
    return ret;
}

static bool
is_port_tap(odp_port_t portno, const struct dpif_class *const class)
{
    struct netdev *dev = netdev_ports_get(portno, class);
    if (!dev) {
        return true;
    }
    netdev_close(dev);
    return false;
}

enum {
    ACTION_OUTPUT = 1<<0,
};

static unsigned
check_clone_actions(const struct nlattr *clone_act, \
                    const size_t act_size, \
                    const struct dpif_class *const class, \
                    bool *check_ret)
{
    const struct nlattr *a;
    unsigned int left;
    *check_ret = false;
    unsigned flag = 0;

    NL_ATTR_FOR_EACH_UNSAFE (a, left, clone_act, act_size) {
        int _type = nl_attr_type(a);
        if (_type == OVS_ACTION_ATTR_OUTPUT) {
            /* "internal dev, tap dev, not offload */
            odp_port_t portno = nl_attr_get_odp_port(a);
            if (is_port_tap(portno, class)) {
                return flag;
            } else {
                /* has fate action */
                *check_ret = true;
            }
            flag |= ACTION_OUTPUT;
        }
    }
    return flag;
}

static bool
offload_check_action(struct netdev *inport,\
                     struct dp_netdev_actions *act, \
                     struct offload_info *info)
{
    const struct nlattr *a;
    unsigned int left;
    bool offloadable = false;
    unsigned flag = 0;

    if (!strcmp(netdev_get_type(inport), "vxlan")) {
        info->vxlan_decap = 1;
    }

    NL_ATTR_FOR_EACH_UNSAFE (a, left, act->actions, act->size) {
        int _type = nl_attr_type(a);
        if (_type == OVS_ACTION_ATTR_OUTPUT) {
            flag |= ACTION_OUTPUT;
            odp_port_t portno = nl_attr_get_odp_port(a);
            /* tap dev not offload */
            if (is_port_tap(portno, info->dpif_class))
                return false; 
            else {
                offloadable = true;
            }
        } else if (_type == OVS_ACTION_ATTR_CLONE) {
            if (left <= NLA_ALIGN(a->nla_len)) {
                const struct nlattr *clone_actions = nl_attr_get(a);
                size_t clone_actions_len = nl_attr_get_size(a);
                flag |= check_clone_actions(clone_actions, clone_actions_len,
                        info->dpif_class, &offloadable);
            } else {
                /* does not clone */
                return false;
            }
        } else if (_type == OVS_ACTION_ATTR_TUNNEL_POP) {
            flag |= ACTION_OUTPUT;
            odp_port_t portno = nl_attr_get_odp_port(a);
            struct netdev *tnl_dev = netdev_ports_get(portno, info->dpif_class);
            if (!strcmp(netdev_get_type(tnl_dev), "vxlan")) {
                info->vxlan_decap = 1;
            }
            netdev_close(tnl_dev);
            offloadable = true;
        } else if (_type == OVS_ACTION_ATTR_PUSH_VLAN) {
            info->vlan_push = 1;
            offloadable = true;
        }
    }

    if (act->size == 0 || !(flag & ACTION_OUTPUT)) {
        info->drop = 1;
        /* drop action */
        offloadable = true;
    }
    return offloadable;
}

static int
dp_netdev_try_offload(struct dp_flow_offload_item *offload)
{
    struct dp_netdev_flow *flow = offload->flow;
    odp_port_t in_port = flow->flow.in_port.odp_port;
    struct offload_info info;
    
    memset(&info, 0, sizeof(info));

    info.odp_support = &dp_netdev_support;
    const struct dpif_class *const dpif_class = offload->class;
    *CONST_CAST(const struct dpif_class **, &info.dpif_class) = dpif_class;
    enum offload_status old_status = flow_offload_status(flow);

    if (flow->dead) {
        return -1;
    }

    struct netdev *netdev = netdev_ports_get(in_port, dpif_class);
    if (!netdev) {
        return -1;
    }

    int ret = 0;
    if (!offload_check_action(netdev, offload->dp_act, &info)) {
        netdev_close(netdev);
        if (offload->op == DP_NETDEV_FLOW_OFFLOAD_OP_ADD || \
                    !offload_status_offloaded(old_status)) {
            atomic_store_explicit(&flow->status, OFFLOAD_FAILED, memory_order_release);
            return -1;
        } else {
            /* mod, we have to del it, since it mod to something hw
             * not accept*/
            offload->op = DP_NETDEV_FLOW_OFFLOAD_OP_DEL;
            ret = dp_netdev_flow_offload_del(offload);
            atomic_store_explicit(&flow->status, OFFLOAD_FAILED, memory_order_release);
            return -1;
        }
    }

    enum offload_status status;
    status = dp_netdev_try_offload_ingress(flow, dpif_class, netdev, offload, &info);
    if (status != OFFLOAD_NONE) {
        atomic_store_explicit(&flow->status, status, memory_order_release);
        goto exit;
    }

    status = dp_netdev_try_offload_tnl_pop(flow, netdev, offload, &info);
    if (status != OFFLOAD_NONE) {
        atomic_store_explicit(&flow->status, status, memory_order_release);
        goto exit;
    }

    ret = dp_netdev_normal_offload(flow, netdev, offload, &info);
    if (!ret) {
        status = info.actions_offloaded ? OFFLOAD_FULL : OFFLOAD_MASK;
    } else {
        status = OFFLOAD_FAILED;
    }
    atomic_store_explicit(&flow->status, status, memory_order_release);

exit:
    if (!offload_status_offloaded(old_status) &&\
            offload_status_offloaded(status)) {
        dp_netdev_flow_ref(flow);
    }
    netdev_close(netdev);
    return ret;
}
 
/*
 * There are two flow offload operations here: addition and modification.
 *
 * For flow addition, this function does:
 * - allocate a new flow mark id
 * - perform hardware flow offload
 * - associate the flow mark with flow and mega flow
 *
 * For flow modification, both flow mark and the associations are still
 * valid, thus only item 2 needed.
 */
static int
dp_netdev_flow_offload_put(struct dp_flow_offload_item *offload)
{
    return dp_netdev_try_offload(offload);
}

static void *
dp_netdev_flow_offload_main(void *data)
{
    struct dp_flow_offload_item *offload;
    struct dp_flow_offload *dp_flow_offload = (struct dp_flow_offload*)data;
    struct ovs_list *list;
    const char *op;
    int ret;
    bool exit;

    for (;;) {
        ovs_mutex_lock(&dp_flow_offload->mutex);
check_again:
        atomic_read_explicit(&dp_flow_offload->exit, \
                             &exit, \
                             memory_order_acquire);
        if (exit) {
            ovs_mutex_unlock(&dp_flow_offload->mutex);
            break;
        }

        if (!ovs_list_is_empty(&dp_flow_offload->list)) {
            list = ovs_list_pop_front(&dp_flow_offload->list);
            offload = CONTAINER_OF(list, struct dp_flow_offload_item, node);
        } else {
            dp_flow_offload->process = false;
            ovsrcu_quiesce_start();
            ovs_mutex_cond_wait(&dp_flow_offload->cond,
                                &dp_flow_offload->mutex);
            ovsrcu_quiesce_end();
            goto check_again;
        }

        dp_flow_offload->process = true;
        ovs_mutex_unlock(&dp_flow_offload->mutex);

        /*  get actions here, act will not be freed as 
         *  this is not in a grace period.
         */
        offload->dp_act = dp_netdev_flow_get_actions(offload->flow);
        switch (offload->op) {
        case DP_NETDEV_FLOW_OFFLOAD_OP_ADD:
            op = "add";
            ret = dp_netdev_flow_offload_put(offload);
            break;
        case DP_NETDEV_FLOW_OFFLOAD_OP_MOD:
            op = "mod";
            ret = dp_netdev_flow_offload_put(offload);
            break;
        case DP_NETDEV_FLOW_OFFLOAD_OP_DEL:
            op = "delete";
            ret = dp_netdev_flow_offload_del(offload);
            break;
        default:
            OVS_NOT_REACHED();
        }

        VLOG_DBG("%s to %s netdev flow\n",
                 ret == 0 ? "succeed" : "failed", op);

        dp_netdev_free_flow_offload(offload);
    }

    ovs_mutex_lock(&dp_flow_offload->mutex);
    for (;!ovs_list_is_empty(&dp_flow_offload->list);) {
        list = ovs_list_pop_front(&dp_flow_offload->list);
        offload = CONTAINER_OF(list, struct dp_flow_offload_item, node);
        /* in case IN_PROGRESS flow coming here, change it to NONE */
        atomic_store_explicit(&offload->flow->status, OFFLOAD_NONE, memory_order_release);
        dp_netdev_free_flow_offload(offload);
    }
    ovs_mutex_unlock(&dp_flow_offload->mutex);
    VLOG_INFO("hw_offload exit\n");

    return NULL;
}

void
queue_netdev_flow_del(struct dp_flow_offload *dp_flow_offload, \
                      const struct dpif_class *const dpif_class,\
                      struct dp_netdev_flow *flow)
{
    struct dp_flow_offload_item *offload;

    ovs_mutex_lock(&dp_flow_offload->mutex);
    if (!flow_offload_in_progress(flow)) {
        offload = dp_netdev_alloc_flow_offload(dpif_class, flow, NULL,
                DP_NETDEV_FLOW_OFFLOAD_OP_DEL);
        if (offload) {
            flow->status |= OFFLOAD_IN_PROGRESS;
            dp_netdev_append_flow_offload(dp_flow_offload, offload);
        }
    }
    ovs_mutex_unlock(&dp_flow_offload->mutex);
}

void
queue_netdev_flow_put(struct dp_flow_offload *dp_flow_offload,\
                      const struct dpif_class * const dpif_class, \
                      struct dp_netdev_flow *flow, \
                      struct dp_netdev_actions *old_act, \
                      int op)
{
    struct dp_flow_offload_item *offload;

    if (!netdev_is_flow_api_enabled()) {
        return;
    }

    if (!dp_flow_offload->req) {
        return;
    }

    ovs_mutex_lock(&dp_flow_offload->mutex);
    if (!flow_offload_in_progress(flow)) {
        offload = dp_netdev_alloc_flow_offload(dpif_class, flow, old_act, op);
        if (offload) {
            flow->status |= OFFLOAD_IN_PROGRESS;
            dp_netdev_append_flow_offload(dp_flow_offload, offload);
        }
    }
    ovs_mutex_unlock(&dp_flow_offload->mutex);
}

static int
try_ingress_stats(struct dp_netdev_flow *flow, \
                  struct dp_netdev_actions *act, \
                  const struct dpif_class const * dpif_class, \
                  long long now, \
                  struct dpif_flow_stats *stats)
{
    struct netdev *tnl_dev = \
                        try_ingress(act, dpif_class);
    if (!tnl_dev) {
        return -1;
    }
    struct netdev_vport *vport = netdev_vport_cast(tnl_dev);  
    struct tnl_offload_aux *aux = vport->offload_aux;

    bool found;
    struct ingress_flow *inflow = \
                        ingress_flow_find(flow, aux, &found);
    if (found) {
        struct dpif_flow_stats _stats;
        struct tnl_pop_flow *tnlflow;
        int ret;
        _stats.used = now / 1000; 

        ovs_rwlock_rdlock(&aux->rwlock);
        HMAP_FOR_EACH(tnlflow, node, &aux->tnl_pop_flows) {
            _stats.n_packets = 0;
            _stats.n_bytes = 0;
            ret = tnl_pop_flow_op_stat(inflow, tnlflow, &_stats);
            if (!ret) {
                stats->n_packets += _stats.n_packets;
                stats->n_bytes += _stats.n_bytes;
            }
        }
        ovs_rwlock_unlock(&aux->rwlock);
        netdev_close(tnl_dev);
        return 0;
    }
    netdev_close(tnl_dev);
    return -1;
}

static int
try_tnlflow_stats(struct dp_netdev_flow *flow, \
                  struct netdev *inport, \
                  long long now, \
                  struct dpif_flow_stats *stats)
{
    bool is_tnlflow = try_tnlflow(flow, inport);
    if (!is_tnlflow)
        return -1;

    bool found;
    struct netdev_vport *vport = netdev_vport_cast(inport);
    struct tnl_offload_aux *aux = vport->offload_aux;
    struct tnl_pop_flow *tnlflow = \
                        tnlflow_find(flow, aux, &found);
    if (found) {
        struct dpif_flow_stats _stats;
        _stats.used = now / 1000; 
        int ret;
        struct ingress_flow *inflow;
        memset(stats, 0, sizeof(*stats));

        ovs_rwlock_rdlock(&aux->rwlock);
        HMAP_FOR_EACH(inflow, node, &aux->ingress_flows) {
            _stats.n_bytes = 0;
            _stats.n_packets = 0;
            ret = tnl_pop_flow_op_stat(inflow, tnlflow, &_stats);
            if (!ret) {
                stats->n_packets += _stats.n_packets;
                stats->n_bytes += _stats.n_bytes;
            }
        }
        ovs_rwlock_unlock(&aux->rwlock);
        return 0;
    }
    return -1;
}

int
dpif_netdev_offload_used(struct dp_netdev_flow *netdev_flow, \
                         const struct dpif_class const *dpif_class, \
                         long long now)
{
    int ret;
    struct netdev *port;
    struct dpif_flow_stats stats = {0};

    odp_port_t in_port = netdev_flow->flow.in_port.odp_port;

    port = netdev_ports_get(in_port, dpif_class);
    if (!port) {
        return -1;
    }
    /* get offloaded stats */
    ret = try_ingress_stats(netdev_flow, \
                        dp_netdev_flow_get_actions(netdev_flow),\
                        dpif_class, \
                        now, \
                        &stats);
    if (!ret) {
        goto exit;
    }

    ret = try_tnlflow_stats(netdev_flow, \
                            port, \
                            now,\
                            &stats);
    if (!ret) {
        goto exit;
    }

    ret = netdev_flow_get(port, NULL, NULL, &netdev_flow->mega_ufid, &stats, NULL, NULL);
exit:
    netdev_close(port);
    if (ret) {
        return -1;
    }

    if (stats.n_packets) {
        atomic_store_relaxed(&netdev_flow->stats.used, now / 1000);
        non_atomic_ullong_add(&netdev_flow->stats.packet_count, stats.n_packets);
        non_atomic_ullong_add(&netdev_flow->stats.byte_count, stats.n_bytes);
    }

    return 0;
}

static void
dp_netdev_dump_vtp_hw_flows(struct unixctl_conn *conn, int argc OVS_UNUSED,
                            const char *argv[], void *_aux OVS_UNUSED)
{
    struct ds reply = DS_EMPTY_INITIALIZER;

    struct netdev *netdev = netdev_from_name(argv[1]);
    if (netdev == NULL) {
        unixctl_command_reply_error(conn,
                                    "netdev not found");
        return;
    }
    
    if (!netdev_vport_is_vport_class(netdev_get_class(netdev))) {
        unixctl_command_reply_error(conn,
                                    "netdev not a vport");
        netdev_close(netdev);
        return; 
    }

    struct netdev_vport *vport = netdev_vport_cast(netdev);

    if (!vport->offload_aux) {
        unixctl_command_reply(conn, "");
        netdev_close(netdev);
        return;
    }

    struct tnl_offload_aux *aux = vport->offload_aux;

    ovs_rwlock_rdlock(&aux->rwlock);
    ds_put_cstr(&reply, "INGRESS flow:\n");
    struct ingress_flow *inflow;
    HMAP_FOR_EACH (inflow, node, &aux->ingress_flows) {
        odp_format_ufid(&inflow->flow->mega_ufid, &reply); 
        ds_put_format(&reply, ", netdev:%s\n", netdev_get_name(inflow->ingress_netdev));
    }

    ds_put_cstr(&reply, "TNL_POP flow:\n");
    struct tnl_pop_flow *tnlflow;
    HMAP_FOR_EACH (tnlflow, node, &aux->tnl_pop_flows) {
        odp_format_ufid(&tnlflow->flow->mega_ufid, &reply); 
        ds_put_format(&reply, ", ref:%d\n", tnlflow->ref);
    }

    ovs_u128 mega_ufid;
    ds_put_cstr(&reply, "MERGED flow:\n");
    HMAP_FOR_EACH (inflow, node, &aux->ingress_flows) {
        HMAP_FOR_EACH (tnlflow, node, &aux->tnl_pop_flows) {
            tnl_pop_flow_get_ufid(inflow, tnlflow, &mega_ufid);
            odp_format_ufid(&mega_ufid, &reply);
            ds_put_char(&reply, '\n');
        }
    }
    ovs_rwlock_unlock(&aux->rwlock);

    unixctl_command_reply(conn, ds_cstr(&reply));
    ds_destroy(&reply);
    netdev_close(netdev);
}

bool
dp_netdev_offload_pause(struct dp_flow_offload *offload)
{
    if (offload->req == true) {
        atomic_store_explicit(&offload->req, false, memory_order_seq_cst);
        dp_netdev_wait_offload_done(offload);
        return true;
    }
    return false;
}

void
dp_netdev_offload_resume(struct dp_flow_offload *offload, bool prev)
{
    atomic_store_explicit(&offload->req, prev, memory_order_seq_cst);
}
