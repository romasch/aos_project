/**
 * \file
 * \brief Architecture specific dispatcher struct shared between kernel and user
 */

/*
 * Copyright (c) 2010, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#ifndef TARGET_ARM_BARRELFISH_KPI_DISPATCHER_SHARED_H
#define TARGET_ARM_BARRELFISH_KPI_DISPATCHER_SHARED_H

#include <barrelfish_kpi/dispatcher_shared.h>

///< Architecture specific kernel/user shared dispatcher struct
struct dispatcher_shared_arm {
    struct dispatcher_shared_generic d; ///< Generic portion

    lvaddr_t    got_base;           ///< Global Offset Table base

    union registers_arm save_area; ///< Register save area
};

static inline struct dispatcher_shared_arm*
get_dispatcher_shared_arm(dispatcher_handle_t handle)
{
    return (struct dispatcher_shared_arm*)handle;
}

#endif // TARGET_ARM_BARRELFISH_KPI_DISPATCHER_SHARED_H