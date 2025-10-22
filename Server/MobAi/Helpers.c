// Copyright (C) 2024 Paul Johnson
// Copyright (C) 2024-2025 Maxim Nesterov

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <Server/MobAi/Ai.h>

#include <math.h>

#include <Server/EntityDetection.h>
#include <Server/Simulation.h>

static uint8_t is_close_enough_to_parent(struct rr_simulation *simulation,
                                         EntityIdx seeker, EntityIdx target,
                                         void *captures)
{
    struct rr_component_physical *parent_physical = captures;
    struct rr_component_physical *physical =
        rr_simulation_get_physical(simulation, target);
    return ((physical->x - parent_physical->x) *
                    (physical->x - parent_physical->x) +
                (physical->y - parent_physical->y) *
                    (physical->y - parent_physical->y) <
            1000 * 1000);
}

uint8_t has_new_target(struct rr_component_ai *ai,
                       struct rr_simulation *simulation)
{
    if (ai->target_entity == RR_NULL_ENTITY ||
        !rr_simulation_entity_alive(simulation, ai->target_entity))
    {
        struct rr_component_relations *relations =
            rr_simulation_get_relations(simulation, ai->parent_id);
        EntityIdx target_id;
        if (relations->team == rr_simulation_team_id_mobs)
            target_id = rr_simulation_choose_nearby_enemy(
                simulation, ai->parent_id, ai->aggro_range, NULL,
                high_zone_filter);
        else
            target_id = rr_simulation_find_nearest_enemy(
                simulation, ai->parent_id, ai->aggro_range,
                relations->nest == RR_NULL_ENTITY
                    ? rr_simulation_get_physical(simulation, relations->owner)
                    : rr_simulation_get_physical(simulation, relations->nest),
                is_close_enough_to_parent);
        ai->target_entity =
            rr_simulation_get_entity_hash(simulation, target_id);
    }
    if (ai->target_entity != RR_NULL_ENTITY &&
        rr_simulation_entity_alive(simulation, ai->target_entity))
    {
        if (ai->ai_state == rr_ai_state_idle ||
            ai->ai_state == rr_ai_state_idle_moving)
        {
            ai->ticks_until_next_action = 25;
            return 1;
        }
    }
    else if (!(ai->ai_state == rr_ai_state_idle ||
               ai->ai_state == rr_ai_state_idle_moving))
    {
        ai->target_entity = RR_NULL_ENTITY;
        ai->ai_state = rr_ai_state_idle;
        ai->ticks_until_next_action = rand() % 25 + 25;
    }
    return 0;
}

uint8_t ai_is_passive(struct rr_component_ai *ai)
{
    return ai->ai_state == rr_ai_state_idle ||
           ai->ai_state == rr_ai_state_idle_moving;
}

uint8_t should_aggro(struct rr_simulation *simulation,
                     struct rr_component_ai *ai)
{
    if (ai->ai_type == rr_ai_type_neutral)
        return rr_simulation_entity_alive(simulation, ai->target_entity) &&
               ai_is_passive(ai);
    if (ai->ai_type == rr_ai_type_aggro)
        return has_new_target(ai, simulation);
    return 0;
}

struct rr_vector predict(struct rr_vector delta, struct rr_vector velocity,
                         float speed)
{
    float distance = rr_vector_get_magnitude(&delta);
    if (speed != 0)
        rr_vector_scale(&velocity, distance / speed);
    else
        rr_vector_set(&velocity, 0, 0);
    rr_vector_add(&delta, &velocity);
    return delta;
}

void tick_idle(EntityIdx entity, struct rr_simulation *simulation)
{
    struct rr_component_ai *ai = rr_simulation_get_ai(simulation, entity);
    struct rr_component_physical *physical =
        rr_simulation_get_physical(simulation, entity);

    if (ai->ticks_until_next_action == 0)
    {
        ai->ticks_until_next_action = rand() % 33 + 25;
        ai->ai_state = rr_ai_state_idle_moving;
        rr_component_physical_set_angle(
            physical, physical->angle + (rr_frand() - 0.5) * M_PI);
        physical->bearing_angle = physical->angle;
    }
}

void tick_idle_move_default(EntityIdx entity, struct rr_simulation *simulation)
{
    struct rr_component_ai *ai = rr_simulation_get_ai(simulation, entity);
    struct rr_component_physical *physical =
        rr_simulation_get_physical(simulation, entity);
    if (ai->ticks_until_next_action == 0)
    {
        ai->ticks_until_next_action = 12 + rr_frand() * 37;
        ai->ai_state = rr_ai_state_idle;
    }
    struct rr_vector accel;
    rr_vector_from_polar(&accel, 1.0f, physical->angle);
    rr_vector_add(&physical->acceleration, &accel);
}

void tick_idle_move_sinusoid(EntityIdx entity, struct rr_simulation *simulation,
                             float speed)
{
    struct rr_component_ai *ai = rr_simulation_get_ai(simulation, entity);
    struct rr_component_physical *physical =
        rr_simulation_get_physical(simulation, entity);
    float add = sin(ai->ticks_until_next_action * 0.2) * 0.75;
    struct rr_vector accel;
    rr_component_physical_set_angle(physical, physical->bearing_angle + add);
    rr_vector_from_polar(&accel, speed, physical->angle);
    rr_vector_add(&physical->acceleration, &accel);
}

uint8_t tick_summon_return_to_owner(EntityIdx entity,
                                    struct rr_simulation *simulation)
{
    struct rr_component_ai *ai = rr_simulation_get_ai(simulation, entity);
    struct rr_component_mob *mob = rr_simulation_get_mob(simulation, entity);
    struct rr_component_physical *physical =
        rr_simulation_get_physical(simulation, entity);
    struct rr_component_relations *relations =
        rr_simulation_get_relations(simulation, entity);
    if (!rr_simulation_entity_alive(simulation, relations->owner) ||
        is_dead_flower(simulation, relations->owner))
    {
        rr_simulation_request_entity_deletion(simulation, entity);
        return 1;
    }
    struct rr_component_physical *parent_physical =
        relations->nest == RR_NULL_ENTITY
            ? rr_simulation_get_physical(simulation, relations->owner)
            : rr_simulation_get_physical(simulation, relations->nest);
    struct rr_vector delta = {parent_physical->x - physical->x,
                              parent_physical->y - physical->y};
    if (rr_vector_magnitude_cmp(&delta, 5000) == 1)
    {
        rr_simulation_request_entity_deletion(simulation, entity);
        return 1;
    }
    if (ai->ai_type <= rr_ai_type_passive || physical->stun_ticks > 0)
        return 0;
    if (ai->ai_state == rr_ai_state_returning_to_owner &&
        rr_vector_magnitude_cmp(&delta, 250 + physical->radius) == 1)
    {
        struct rr_vector accel = delta;
        rr_vector_set_magnitude(&accel, RR_PLAYER_SPEED * 1.2);
        rr_vector_add(&physical->acceleration, &accel);
        rr_component_physical_set_angle(physical, rr_vector_theta(&accel));
        ai->target_entity = RR_NULL_ENTITY;
        return 1;
    }
    else if (rr_vector_magnitude_cmp(&delta, 1000 + physical->radius) == 1)
    {
        ai->ai_state = rr_ai_state_returning_to_owner;
        struct rr_vector accel = delta;
        rr_vector_set_magnitude(&accel, RR_PLAYER_SPEED * 1.2);
        rr_vector_add(&physical->acceleration, &accel);
        rr_component_physical_set_angle(physical, rr_vector_theta(&accel));
        ai->target_entity = RR_NULL_ENTITY;
        return 1;
    }
    else if (ai->ai_state == rr_ai_state_returning_to_owner)
    {
        ai->ai_state = rr_ai_state_idle;
        ai->ticks_until_next_action = rand() % 25 + 25;
        return 0;
    }
    return 0;
}

void tick_return_to_higher_zone(EntityIdx entity,
                                struct rr_simulation *simulation)
{
    struct rr_component_ai *ai = rr_simulation_get_ai(simulation, entity);
    struct rr_component_physical *physical =
        rr_simulation_get_physical(simulation, entity);
    if (ai->ai_state == rr_ai_state_returning_to_higher_zone)
    {
        struct rr_vector delta = {ai->return_pos.x - physical->x,
                                  ai->return_pos.y - physical->y};
        if (rr_vector_magnitude_cmp(&delta, physical->radius) == 1)
        {
            struct rr_vector accel = delta;
            rr_vector_set_magnitude(&accel, RR_PLAYER_SPEED * 1.2);
            rr_vector_add(&physical->acceleration, &accel);
            rr_component_physical_set_angle(physical, rr_vector_theta(&accel));
            ai->target_entity = RR_NULL_ENTITY;
        }
        else
        {
            ai->ai_state = rr_ai_state_idle;
            ai->ticks_until_next_action = rand() % 25 + 25;
        }
        return;
    }
    struct rr_component_mob *mob = rr_simulation_get_mob(simulation, entity);
    if (mob->rarity < rr_rarity_id_ultimate)
        return;
    struct rr_component_arena *arena =
        rr_simulation_get_arena(simulation, physical->arena);
    int32_t grid_x = rr_fclamp(physical->x / arena->maze->grid_size,
                               0, arena->maze->maze_dim - 1);
    int32_t grid_y = rr_fclamp(physical->y / arena->maze->grid_size,
                               0, arena->maze->maze_dim - 1);
    struct rr_maze_grid *grid =
        rr_component_arena_get_grid(arena, grid_x, grid_y);
    if (grid->difficulty >= 48 || grid->value == 0 || (grid->value & 8))
        return;
    for (int8_t i = -1; i <= 1; ++i)
        for (int8_t j = -1; j <= 1; ++j)
        {
            int32_t x = (grid_x / 2 + i) * 2 + 1;
            int32_t y = (grid_y / 2 + j) * 2 + 1;
            if (x < 0 || x >= arena->maze->maze_dim ||
                y < 0 || y >= arena->maze->maze_dim)
                continue;
            grid = rr_component_arena_get_grid(arena, x, y);
            if (grid->difficulty < 48)
                continue;
            rr_vector_set(&ai->return_pos, x * arena->maze->grid_size,
                          y * arena->maze->grid_size);
            ai->ai_state = rr_ai_state_returning_to_higher_zone;
            return;
        }
}
