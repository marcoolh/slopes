#ifndef SIM_H
#define SIM_H

#include <stdint.h>

/* ---- capacities (fixed pools, no malloc) ---- */
#define MAX_NODES  64
#define MAX_EDGES  128
#define MAX_NPCS   512
#define MAX_CONN   8     /* outgoing edges per node */
#define QUEUE_MAX  64    /* waiting skiers per lift */
#define NUM_LEVELS 4     /* green, blue, red, black */

/* ---- tuning (map units per second) ---- */
#define SLOPE_SPEED            6.0f
#define LIFT_SPEED             4.0f
#define LIFT_DISPATCH_INTERVAL 2.0f   /* seconds between departures */
#define NPC_PATIENCE           60.0f  /* seconds waiting before giving up */

typedef enum {
    NODE_JUNCTION,
    NODE_SPAWN,
    NODE_RESTAURANT,
    NODE_VIEW
} NodeType;

typedef enum {
    EDGE_SLOPE,   /* goes downhill, uses difficulty */
    EDGE_LIFT     /* goes uphill, uses seats and queue */
} EdgeKind;

typedef enum {
    NPC_FREE = 0, /* unused pool slot (must stay 0 so memset clears it) */
    NPC_AT_NODE,  /* standing on a node, choosing where to go next */
    NPC_QUEUED,   /* waiting in a lift queue */
    NPC_ON_LIFT,
    NPC_ON_SLOPE
} NpcState;

typedef struct {
    float x, y;
    int altitude;
    NodeType type;
    int connections[MAX_CONN];  /* ids of OUTGOING edges */
    int n_connections;
} Node;

typedef struct {
    EdgeKind kind;
    int from, to;               /* node ids */
    float length;               /* computed once in world_add_* */
    int difficulty;             /* slopes: 0..3 */
    int seats;                  /* lifts: skiers per departure */
    int queue[QUEUE_MAX];       /* lifts: npc ids, front at index 0 */
    int queue_count;
    float dispatch_timer;
} Edge;

typedef struct {
    NpcState state;
    int level;       /* 0..3: hardest slope difficulty it can ski */
    int target;      /* node id */
    int node;        /* valid when NPC_AT_NODE */
    int edge;        /* valid when QUEUED / ON_LIFT / ON_SLOPE */
    float progress;  /* 0..1 along the current edge */
    float wait_time; /* seconds spent waiting since last movement */
} Npc;

typedef struct {
    Node nodes[MAX_NODES];
    Edge edges[MAX_EDGES];
    Npc  npcs[MAX_NPCS];
    int n_nodes, n_edges, n_active;
    int arrived;     /* skiers that reached their target */
    int lost;        /* skiers that gave up waiting */
    uint32_t rng;
} World;

/* ---- building the map (all return an id, or -1 on failure) ---- */
void world_init(World *w, uint32_t seed);
int  world_add_node(World *w, float x, float y, int altitude, NodeType type);
int  world_add_slope(World *w, int from, int to, int difficulty); /* from must be higher than to */
int  world_add_lift(World *w, int from, int to, int seats);       /* to must be higher than from */

/* ---- npcs ---- */
int  world_spawn_npc(World *w, int node, int level, int target);
int  world_spawn_random(World *w);   /* random spawn node, level and target */

/* ---- simulation ---- */
void world_tick(World *w, float dt);

#endif