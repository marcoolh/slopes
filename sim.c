#include "sim.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

/* speed multiplier per skill level (green, blue, red, black) */
static const float LEVEL_SPEED[NUM_LEVELS] = { 0.8f, 1.0f, 1.2f, 1.4f };

/* ------------------------------------------------------------------ */
/* RNG (xorshift32, deterministic and platform independent)           */
/* ------------------------------------------------------------------ */

static uint32_t rng_next(World *w)
{
    uint32_t x = w->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    w->rng = x;
    return x;
}

static int rng_range(World *w, int n)
{
    return (int)(rng_next(w) % (uint32_t)n);
}

/* ------------------------------------------------------------------ */
/* Building the map                                                    */
/* ------------------------------------------------------------------ */

void world_init(World *w, uint32_t seed)
{
    memset(w, 0, sizeof *w);
    w->rng = seed ? seed : 1;   /* xorshift must never be 0 */
}

int world_add_node(World *w, float x, float y, int altitude, NodeType type)
{
    if (w->n_nodes >= MAX_NODES) return -1;
    Node *n = &w->nodes[w->n_nodes];
    n->x = x;
    n->y = y;
    n->altitude = altitude;
    n->type = type;
    n->n_connections = 0;
    return w->n_nodes++;
}

static int add_edge(World *w, EdgeKind kind, int from, int to)
{
    if (from < 0 || from >= w->n_nodes) return -1;
    if (to < 0 || to >= w->n_nodes || to == from) return -1;
    if (w->n_edges >= MAX_EDGES) return -1;

    Node *a = &w->nodes[from];
    const Node *b = &w->nodes[to];
    if (a->n_connections >= MAX_CONN) return -1;

    float dz = (float)(b->altitude - a->altitude);
    if (kind == EDGE_SLOPE && dz >= 0) return -1;   /* slopes go down */
    if (kind == EDGE_LIFT  && dz <= 0) return -1;   /* lifts go up */

    float dx = b->x - a->x;
    float dy = b->y - a->y;

    Edge *e = &w->edges[w->n_edges];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->from = from;
    e->to = to;
    e->length = sqrtf(dx * dx + dy * dy + dz * dz);

    a->connections[a->n_connections++] = w->n_edges;
    return w->n_edges++;
}

int world_add_slope(World *w, int from, int to, int difficulty)
{
    if (difficulty < 0 || difficulty >= NUM_LEVELS) return -1;
    int id = add_edge(w, EDGE_SLOPE, from, to);
    if (id >= 0) w->edges[id].difficulty = difficulty;
    return id;
}

int world_add_lift(World *w, int from, int to, int seats)
{
    if (seats < 1) return -1;
    int id = add_edge(w, EDGE_LIFT, from, to);
    if (id >= 0) w->edges[id].seats = seats;
    return id;
}

/* ------------------------------------------------------------------ */
/* Pathfinding                                                         */
/* ------------------------------------------------------------------ */

/* Can an npc of this level use this edge? Skiers can take their own
   level or easier; everyone can ride lifts. */
static bool edge_allowed(const Edge *e, int level)
{
    return e->kind == EDGE_LIFT || e->difficulty <= level;
}

/* Travel time in seconds. Does not include queue waiting time yet:
   adding it here would make skiers avoid crowded lifts. */
static float edge_cost(const Edge *e, int level)
{
    float speed = (e->kind == EDGE_LIFT) ? LIFT_SPEED
                                         : SLOPE_SPEED * LEVEL_SPEED[level];
    return e->length / speed;
}

/* Dijkstra from 'from' to 'to'. Returns the id of the FIRST edge on the
   cheapest path, or -1 if the target is unreachable for this level.
   The graph is tiny, so a linear scan for the minimum is fine, and
   recomputing at every node means skiers adapt when the player builds. */
static int next_edge(const World *w, int from, int to, int level)
{
    float dist[MAX_NODES];
    int   prev_edge[MAX_NODES];
    bool  done[MAX_NODES];

    for (int i = 0; i < w->n_nodes; i++) {
        dist[i] = FLT_MAX;
        prev_edge[i] = -1;
        done[i] = false;
    }
    dist[from] = 0.0f;

    for (;;) {
        int u = -1;
        float best = FLT_MAX;
        for (int i = 0; i < w->n_nodes; i++) {
            if (!done[i] && dist[i] < best) {
                best = dist[i];
                u = i;
            }
        }
        if (u < 0 || u == to) break;
        done[u] = true;

        const Node *nu = &w->nodes[u];
        for (int k = 0; k < nu->n_connections; k++) {
            int eid = nu->connections[k];
            const Edge *e = &w->edges[eid];
            if (!edge_allowed(e, level)) continue;
            float nd = dist[u] + edge_cost(e, level);
            if (nd < dist[e->to]) {
                dist[e->to] = nd;
                prev_edge[e->to] = eid;
            }
        }
    }

    if (prev_edge[to] < 0) return -1;

    /* walk back from the target to find the first edge out of 'from' */
    int v = to;
    int first = -1;
    while (v != from) {
        first = prev_edge[v];
        v = w->edges[first].from;
    }
    return first;
}

/* ------------------------------------------------------------------ */
/* Lift queues (plain array: pop and remove are O(n), n <= QUEUE_MAX)  */
/* ------------------------------------------------------------------ */

static bool queue_push(Edge *e, int npc)
{
    if (e->queue_count >= QUEUE_MAX) return false;
    e->queue[e->queue_count++] = npc;
    return true;
}

static int queue_pop(Edge *e)
{
    int id = e->queue[0];
    e->queue_count--;
    memmove(e->queue, e->queue + 1, (size_t)e->queue_count * sizeof(int));
    return id;
}

static void queue_remove(Edge *e, int npc)
{
    for (int i = 0; i < e->queue_count; i++) {
        if (e->queue[i] == npc) {
            e->queue_count--;
            memmove(&e->queue[i], &e->queue[i + 1],
                    (size_t)(e->queue_count - i) * sizeof(int));
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* NPCs                                                                */
/* ------------------------------------------------------------------ */

int world_spawn_npc(World *w, int node, int level, int target)
{
    if (node < 0 || node >= w->n_nodes) return -1;
    if (target < 0 || target >= w->n_nodes) return -1;
    if (level < 0 || level >= NUM_LEVELS) return -1;

    for (int i = 0; i < MAX_NPCS; i++) {
        if (w->npcs[i].state == NPC_FREE) {
            Npc *n = &w->npcs[i];
            memset(n, 0, sizeof *n);
            n->state = NPC_AT_NODE;
            n->level = level;
            n->target = target;
            n->node = node;
            n->edge = -1;
            w->n_active++;
            return i;
        }
    }
    return -1;   /* pool full */
}

int world_spawn_random(World *w)
{
    int spawns[MAX_NODES], targets[MAX_NODES];
    int ns = 0, nt = 0;

    for (int i = 0; i < w->n_nodes; i++) {
        NodeType t = w->nodes[i].type;
        if (t == NODE_SPAWN) spawns[ns++] = i;
        else if (t == NODE_RESTAURANT || t == NODE_VIEW) targets[nt++] = i;
    }
    if (ns == 0 || nt == 0) return -1;

    /* separate statements on purpose: the evaluation order of function
       arguments is unspecified in C, which would break determinism */
    int spawn  = spawns[rng_range(w, ns)];
    int level  = rng_range(w, NUM_LEVELS);
    int target = targets[rng_range(w, nt)];
    return world_spawn_npc(w, spawn, level, target);
}

static void npc_free(World *w, int id)
{
    w->npcs[id].state = NPC_FREE;
    w->n_active--;
}

static void npc_give_up(World *w, int id)
{
    Npc *n = &w->npcs[id];
    if (n->state == NPC_QUEUED) queue_remove(&w->edges[n->edge], id);
    w->lost++;
    npc_free(w, id);
}

static void npc_step(World *w, int id, float dt)
{
    Npc *n = &w->npcs[id];

    switch (n->state) {
    case NPC_FREE:
        break;

    case NPC_AT_NODE: {
        if (n->node == n->target) {
            w->arrived++;
            npc_free(w, id);
            break;
        }

        int eid = next_edge(w, n->node, n->target, n->level);
        if (eid >= 0) {
            Edge *e = &w->edges[eid];
            if (e->kind == EDGE_SLOPE) {
                n->state = NPC_ON_SLOPE;
                n->edge = eid;
                n->progress = 0.0f;
                n->wait_time = 0.0f;
                break;
            }
            if (queue_push(e, id)) {
                n->state = NPC_QUEUED;   /* keeps its accumulated wait_time */
                n->edge = eid;
                break;
            }
            /* queue full: stay here and keep waiting */
        }

        /* no route, or queue full */
        n->wait_time += dt;
        if (n->wait_time > NPC_PATIENCE) npc_give_up(w, id);
        break;
    }

    case NPC_QUEUED:
        /* boarding is handled by the lift in world_tick */
        n->wait_time += dt;
        if (n->wait_time > NPC_PATIENCE) npc_give_up(w, id);
        break;

    case NPC_ON_LIFT:
    case NPC_ON_SLOPE: {
        const Edge *e = &w->edges[n->edge];
        float speed = (n->state == NPC_ON_LIFT)
                          ? LIFT_SPEED
                          : SLOPE_SPEED * LEVEL_SPEED[n->level];
        n->progress += speed * dt / e->length;
        if (n->progress >= 1.0f) {
            n->state = NPC_AT_NODE;
            n->node = e->to;
            n->edge = -1;
            n->progress = 0.0f;
        }
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Tick                                                                */
/* ------------------------------------------------------------------ */

static void lift_step(World *w, Edge *e, float dt)
{
    e->dispatch_timer += dt;
    if (e->dispatch_timer < LIFT_DISPATCH_INTERVAL) return;

    if (e->queue_count == 0) {
        /* ready to go: the next skier to arrive boards immediately */
        e->dispatch_timer = LIFT_DISPATCH_INTERVAL;
        return;
    }

    for (int s = 0; s < e->seats && e->queue_count > 0; s++) {
        Npc *n = &w->npcs[queue_pop(e)];
        n->state = NPC_ON_LIFT;
        n->progress = 0.0f;
        n->wait_time = 0.0f;
    }
    e->dispatch_timer = 0.0f;
}

void world_tick(World *w, float dt)
{
    for (int i = 0; i < w->n_edges; i++) {
        if (w->edges[i].kind == EDGE_LIFT) lift_step(w, &w->edges[i], dt);
    }
    for (int i = 0; i < MAX_NPCS; i++) {
        npc_step(w, i, dt);
    }
}