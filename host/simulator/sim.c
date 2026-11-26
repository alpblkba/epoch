#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"

#define LINK_CAP 128u
#define TABLE_CAP 8192u

uint32_t epoch_test_checksum(const uint32_t *words, size_t n)
{
    uint32_t h = 2166136261u; /* FNV-1a over little-endian bytes, TEST ONLY */
    for (size_t i = 0; i < n; i++) {
        for (unsigned b = 0; b < 32u; b += 8u) {
            h ^= (words[i] >> b) & 0xFFu;
            h *= 16777619u;
        }
    }
    return h;
}

const struct epoch_ctrl_config epoch_test_ctrl_config = {
    { EPOCH_TEST_MAGIC_VERSION, epoch_test_checksum }, 200u, 1u, 1u, 0u
};

struct epoch_host_config epoch_test_host_config(enum epoch_host_policy policy)
{
    struct epoch_host_config c = { { EPOCH_TEST_MAGIC_VERSION, epoch_test_checksum }, policy, 1u, 100u, 100u, 100u, 20u };
    return c;
}

struct pending {
    uint32_t at;
    uint64_t seq;
    struct epoch_msg m;
};

struct link {
    struct pending q[LINK_CAP];
    unsigned n;
    uint32_t last_at;
    const char *name;
};

struct req {
    uint64_t session;
    uint32_t txid;
    enum epoch_outcome o;
    uint32_t counter;
    bool refined;
};

struct commit {
    uint64_t session;
    uint32_t txid;
    uint32_t counter;
};

struct sim {
    const struct epoch_sim_config *cfg;
    struct epoch_sim_result *res;
    uint32_t now;
    uint64_t net, crng, hrng, seq;
    struct link to_ctrl, to_host;
    struct epoch_retained ret;
    struct epoch_ctrl ctrl;
    struct epoch_ctrl_env cenv;
    struct epoch_host host;
    struct epoch_host_config hcfg;
    struct epoch_host_env henv;
    bool have_first, script_fired, reset_cold;
    uint64_t first_session;
    struct req reqs[TABLE_CAP];
    unsigned nreq;
    struct commit commits[TABLE_CAP];
    unsigned ncommit;
};

static uint64_t next(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15u); /* splitmix64 */
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9u;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBu;
    return z ^ (z >> 31);
}

static bool chance(uint64_t *state, uint32_t permille)
{
    return permille != 0u && next(state) % 1000u < permille;
}

static bool faults(const struct sim *s)
{
    return s->now < s->cfg->faults_until_ms;
}

static void mix(struct sim *s, uint64_t v)
{
    for (unsigned b = 0; b < 64u; b += 8u) {
        s->res->hash ^= (v >> b) & 0xFFu;
        s->res->hash *= 0x100000001B3u;
    }
}

static const struct commit *find_commit(const struct sim *s, uint64_t session, uint32_t txid)
{
    for (unsigned i = 0; i < s->ncommit; i++)
        if (s->commits[i].session == session && s->commits[i].txid == txid)
            return &s->commits[i];
    return NULL;
}

static struct req *find_req(struct sim *s, uint64_t session, uint32_t txid)
{
    for (unsigned i = s->nreq; i-- > 0;)
        if (s->reqs[i].session == session && s->reqs[i].txid == txid)
            return &s->reqs[i];
    if (s->nreq == TABLE_CAP) {
        s->res->table_overflow++;
        return NULL;
    }
    struct req *r = &s->reqs[s->nreq++];
    memset(r, 0, sizeof *r);
    r->session = session;
    r->txid = txid;
    return r;
}

static void link_send(struct sim *s, struct link *l, const struct epoch_msg *m)
{
    const struct epoch_sim_config *c = s->cfg;
    if (faults(s) && chance(&s->net, c->drop_permille)) {
        if (c->trace)
            fprintf(c->trace, "%6" PRIu32 " drop %s %s\n", s->now, l->name, epoch_msg_name(m->type));
        return;
    }
    unsigned copies = faults(s) && chance(&s->net, c->dup_permille) ? 2u : 1u;
    for (unsigned i = 0; i < copies; i++) {
        uint32_t at = s->now + c->delay_min_ms + (uint32_t)(next(&s->net) % ((uint64_t)c->delay_max_ms - c->delay_min_ms + 1u));
        if (at <= s->now)
            at = s->now + 1u;
        if (!c->reorder && at < l->last_at)
            at = l->last_at;
        l->last_at = at;
        if (l->n == LINK_CAP) {
            s->res->queue_overflow++;
            continue;
        }
        l->q[l->n].at = at;
        l->q[l->n].seq = s->seq++;
        l->q[l->n].m = *m;
        l->n++;
    }
}

static void ctrl_send(void *ctx, const struct epoch_msg *m)
{
    struct sim *s = ctx;
    link_send(s, &s->to_host, m);
}

static void host_send(void *ctx, const struct epoch_msg *m)
{
    struct sim *s = ctx;
    link_send(s, &s->to_ctrl, m);
}

static uint64_t ctrl_rand(void *ctx)
{
    return next(&((struct sim *)ctx)->crng);
}

static uint64_t host_rand(void *ctx)
{
    return next(&((struct sim *)ctx)->hrng);
}

static uint32_t new_era(void *ctx)
{
    uint32_t e = (uint32_t)next(&((struct sim *)ctx)->crng);
    return e != 0u ? e : 1u;
}

static void barrier(void *ctx)
{
    (void)ctx; /* the simulated stores are already in program order */
}

static bool random_reset(struct sim *s)
{
    if (!faults(s) || !chance(&s->net, s->cfg->reset_permille))
        return false;
    s->reset_cold = chance(&s->net, s->cfg->cold_permille);
    return true;
}

static bool checkpoint(void *ctx, enum epoch_checkpoint cp, uint32_t txid)
{
    struct sim *s = ctx;
    const struct epoch_sim_config *c = s->cfg;
    if (c->script && !s->script_fired && s->have_first && s->ctrl.session == s->first_session && cp == c->script_cp && txid == c->script_txid) {
        s->script_fired = true;
        s->reset_cold = c->script_cold;
        return true;
    }
    return random_reset(s);
}

/* Truth oracle: a record is committed when its final w0 store lands, whatever the controller does next. */
static bool after_store(void *ctx, const volatile uint32_t *slot, unsigned index)
{
    struct sim *s = ctx;
    if (index == EPOCH_STORE_COMMIT) {
        struct epoch_record r;
        epoch_record_decode(slot, &r);
        mix(s, r.session ^ ((uint64_t)r.txid << 32 | r.counter));
        if (r.txid == 0u) {
            if (!s->have_first) {
                s->have_first = true;
                s->first_session = r.session;
            }
        } else if (find_commit(s, r.session, r.txid)) {
            s->res->duplicate_commits++;
        } else if (s->ncommit == TABLE_CAP) {
            s->res->table_overflow++;
        } else {
            s->commits[s->ncommit].session = r.session;
            s->commits[s->ncommit].txid = r.txid;
            s->commits[s->ncommit].counter = r.counter;
            s->ncommit++;
        }
    }
    return random_reset(s);
}

static void outcome(void *ctx, uint64_t session, uint32_t txid, enum epoch_outcome o, uint32_t counter)
{
    struct sim *s = ctx;
    struct req *r = find_req(s, session, txid);
    if (s->cfg->trace)
        fprintf(s->cfg->trace, "%6" PRIu32 " outcome S=%016" PRIx64 " t=%" PRIu32 " %s c=%" PRIu32 "\n", s->now, session, txid, epoch_outcome_name(o), counter);
    mix(s, (uint64_t)s->now << 32 | (uint64_t)o << 24 | txid);
    if (r == NULL)
        return;
    if (r->o == EPOCH_OUTCOME_UNKNOWN && o != EPOCH_OUTCOME_UNKNOWN)
        r->refined = true;
    r->o = o;
    r->counter = counter;
    if (o == EPOCH_OUTCOME_COMPLETED) {
        const struct commit *k = find_commit(s, session, txid);
        if (k == NULL || k->counter != counter)
            s->res->false_completed++;
        if (!faults(s))
            s->res->completed_after_faults++;
    }
}

/* Applies the retained-state effect of a reset of the given kind and boots, repeating while a reset lands inside cold initialization. A warm reset keeps every retained word. */
static void reboot(struct sim *s, bool cold)
{
    for (;;) {
        s->res->resets++;
        if (cold) {
            s->res->cold_resets++;
            if (!chance(&s->net, s->cfg->cold_keeps_permille)) {
                s->ret.era = 0u;
                for (unsigned i = 0; i < 2u * EPOCH_JOURNAL_WORDS; i++)
                    s->ret.slots[i] = (uint32_t)next(&s->net);
            }
        } else if (chance(&s->net, s->cfg->corrupt_permille)) {
            unsigned k = (unsigned)(next(&s->net) % 2u);
            unsigned bit = (unsigned)(next(&s->net) % (32u * EPOCH_JOURNAL_WORDS));
            if (s->ret.slots[k * EPOCH_JOURNAL_WORDS] != 0u) {
                s->res->corruptions++;
                s->res->corruptions_newest += epoch_journal_newest(&epoch_test_ctrl_config.journal, s->ret.slots, s->ret.era) == (int)k;
                s->ret.slots[k * EPOCH_JOURNAL_WORDS + bit / 32u] ^= 1u << (bit % 32u);
            }
        }
        if (s->cfg->trace)
            fprintf(s->cfg->trace, "%6" PRIu32 " reset %s\n", s->now, cold ? "cold" : "warm");
        mix(s, (uint64_t)s->now << 1 | cold);
        int newest = s->ret.era != 0u ? epoch_journal_newest(&epoch_test_ctrl_config.journal, s->ret.slots, s->ret.era) : EPOCH_SLOT_NONE;
        if (epoch_ctrl_boot(&s->ctrl, &epoch_test_ctrl_config, &s->cenv, &s->ret, cold, cold ? 1u : 2u, s->now) == EPOCH_CTRL_OK)
            return;
        s->res->boot_resets++;
        if (newest >= 0)
            s->res->boot_resets_newest[newest]++;
        if (s->cfg->trace)
            fprintf(s->cfg->trace, "%6" PRIu32 " reset inside cold initialization\n", s->now);
        cold = s->reset_cold;
    }
}

static struct link *due(struct sim *s, unsigned *index)
{
    struct link *best = NULL;
    struct link *links[2] = { &s->to_ctrl, &s->to_host };
    for (unsigned k = 0; k < 2u; k++) {
        for (unsigned i = 0; i < links[k]->n; i++) {
            const struct pending *p = &links[k]->q[i];
            if (p->at > s->now)
                continue;
            if (best == NULL || p->at < best->q[*index].at || (p->at == best->q[*index].at && p->seq < best->q[*index].seq)) {
                best = links[k];
                *index = i;
            }
        }
    }
    return best;
}

static void deliver(struct sim *s)
{
    unsigned i = 0;
    struct link *l;
    while ((l = due(s, &i)) != NULL) {
        struct epoch_msg m = l->q[i].m;
        l->q[i] = l->q[--l->n];
        if (s->cfg->trace)
            fprintf(s->cfg->trace, "%6" PRIu32 " %s %s S=%016" PRIx64 " t=%" PRIu32 " c=%" PRIu32 "\n", s->now, l->name, epoch_msg_name(m.type), m.session, m.txid, m.counter);
        mix(s, (uint64_t)s->now << 40 | (uint64_t)(l == &s->to_ctrl) << 32 | (uint32_t)m.type);
        mix(s, m.session ^ m.boot_id ^ ((uint64_t)m.txid << 32 | m.counter));
        if (l == &s->to_ctrl) {
            if (epoch_ctrl_receive(&s->ctrl, &m, s->now) == EPOCH_CTRL_RESET)
                reboot(s, s->reset_cold);
        } else {
            epoch_host_receive(&s->host, &m, s->now);
        }
    }
}

static void tally(struct sim *s)
{
    struct epoch_sim_result *res = s->res;
    for (unsigned i = 0; i < s->nreq; i++) {
        const struct req *r = &s->reqs[i];
        bool committed = find_commit(s, r->session, r->txid) != NULL;
        res->refined += r->refined;
        switch (r->o) {
        case EPOCH_OUTCOME_COMPLETED: res->completed++; break;
        case EPOCH_OUTCOME_UNKNOWN: res->unknown++; break;
        case EPOCH_OUTCOME_REJECTED:
            res->rejected++;
            res->rejected_but_committed += committed;
            break;
        case EPOCH_OUTCOME_NOT_EXECUTED:
            res->not_executed++;
            res->false_not_executed += committed;
            break;
        default: res->abandoned++; break;
        }
        if (s->have_first && r->session == s->first_session && r->txid < EPOCH_SIM_TRACKED) {
            res->first_outcome[r->txid] = r->o;
            res->first_counter[r->txid] = r->counter;
        }
    }
    res->running_at_end = s->host.state == EPOCH_HOST_RUNNING;
}

void epoch_sim_default(struct epoch_sim_config *cfg, uint64_t seed)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->seed = seed;
    cfg->policy = EPOCH_POLICY_B3;
    cfg->duration_ms = 20000u;
    cfg->request_interval_ms = 25u;
    cfg->delay_min_ms = 1u;
    cfg->delay_max_ms = 10u;
    cfg->reorder = true;
    cfg->drop_permille = 50u;
    cfg->dup_permille = 30u;
    cfg->reset_permille = 3u;
    cfg->cold_permille = 200u;
    cfg->faults_until_ms = 12000u;
}

void epoch_sim_run(const struct epoch_sim_config *cfg, struct epoch_sim_result *res)
{
    struct sim *s = calloc(1, sizeof *s);
    if (s == NULL)
        abort();
    memset(res, 0, sizeof *res);
    res->hash = 0xCBF29CE484222325u;
    s->cfg = cfg;
    s->res = res;
    s->net = cfg->seed ^ 0x6E6574u;
    s->crng = cfg->seed ^ 0x637472u;
    s->hrng = cfg->seed ^ 0x686F73u;
    s->to_ctrl.name = "h->c";
    s->to_host.name = "c->h";
    s->cenv = (struct epoch_ctrl_env){ s, ctrl_rand, new_era, ctrl_send, barrier, checkpoint, after_store };
    s->hcfg = epoch_test_host_config(cfg->policy);
    s->henv = (struct epoch_host_env){ s, host_rand, host_send, outcome };

    if (epoch_ctrl_boot(&s->ctrl, &epoch_test_ctrl_config, &s->cenv, &s->ret, true, 1u, 0u) == EPOCH_CTRL_RESET) {
        s->res->boot_resets++;
        reboot(s, s->reset_cold);
    }
    epoch_host_init(&s->host, &s->hcfg, &s->henv);
    epoch_host_start(&s->host, 0u);
    for (s->now = 0; s->now < cfg->duration_ms; s->now++) {
        if (cfg->host_restart_at_ms != 0u && s->now == cfg->host_restart_at_ms) {
            if (cfg->trace)
                fprintf(cfg->trace, "%6" PRIu32 " host restart\n", s->now);
            epoch_host_init(&s->host, &s->hcfg, &s->henv);
            epoch_host_start(&s->host, s->now);
        }
        deliver(s);
        epoch_ctrl_tick(&s->ctrl, s->now);
        epoch_host_tick(&s->host, s->now);
        if (cfg->request_interval_ms != 0u && s->now % cfg->request_interval_ms == 0u) {
            uint32_t t = epoch_host_increment(&s->host, s->now);
            if (t != 0u) {
                res->issued++;
                find_req(s, s->host.session, t);
            }
        }
    }
    tally(s);
    free(s);
}

bool epoch_sim_violation(const struct epoch_sim_result *r)
{
    return r->false_completed || r->false_not_executed || r->duplicate_commits || r->rejected_but_committed || r->table_overflow || r->queue_overflow;
}
