#include "sim.h"

#include <stdio.h>

int main(void)
{
    static World w;   /* static: the world is big, keep it off the stack */
    world_init(&w, 12345);

    int base  = world_add_node(&w,   0,   0,   0, NODE_SPAWN);
    int top   = world_add_node(&w, 100,   0, 300, NODE_JUNCTION);
    int resto = world_add_node(&w,  60,  50, 150, NODE_RESTAURANT);
    int view  = world_add_node(&w, 120,  40, 250, NODE_VIEW);

    int lift = world_add_lift(&w, base, top, 4);
    world_add_slope(&w, top,   resto, 0);   /* green */
    world_add_slope(&w, resto, base,  0);   /* green */
    world_add_slope(&w, top,   view,  2);   /* red   */
    world_add_slope(&w, view,  base,  1);   /* blue  */
    world_add_slope(&w, top,   base,  3);   /* black */

    const float dt = 0.1f;
    const int ticks = 12000;                /* 20 simulated minutes */

    for (int t = 0; t <= ticks; t++) {
        if (t % 4 == 0) world_spawn_random(&w);   /* 2.5 skiers per second */
        world_tick(&w, dt);

        if (t % 600 == 0) {
            printf("t=%4.0fs active=%3d arrived=%4d lost=%4d lift_queue=%2d\n",
                   t * dt, w.n_active, w.arrived, w.lost,
                   w.edges[lift].queue_count);
        }
    }
    return 0;
}