/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2021-11-25     liukang      first version
 * 2024-8-9       V1Kin9       implement the rtt-MAL for risc-v
 */

#include <riscv_mal.h>
#include <board.h>

#define DBG_TAG    "mal.riscv"
#ifdef RT_MAL_USING_LOG
#define DBG_LVL    DBG_LOG
#else 
#define DBG_LVL    DBG_INFO
#endif
#include <rtdbg.h>

#define RT_RISCV_MAX_PMP_REGIONS  __PMP_ENTRY_NUM
_Static_assert(RT_RISCV_MAX_PMP_REGIONS >= RT_MPU_REGIONS_NUMBER, "The RT_MPU_REGIONS_NUMBER is out of HW limit");

static rt_uint32_t pmp_align_min(rt_uint32_t len)
{
    rt_uint32_t region_size;
    rt_uint32_t value = PMP_SHIFT;

    for (region_size = RT_MPU_ALIGN_SMALLEST_SIZE; value < __RISCV_XLEN; region_size <<= 1)
    {
        if( len <= region_size)
        {
            break;
        }
        else
        {
            value++;
        }
    }

    return value;
}

static void pmp_clear_region(rt_uint8_t region_idx)
{
    pmp_config pmp_attr = {0};
    (void)__set_PMPENTRYx(region_idx, &pmp_attr);
}


/* Enable PMP */
static void pmp_enable(void)
{
    rt_uint8_t region_idx;
    pmp_config pmp_attr;

    for (region_idx = 0; region_idx < RT_RISCV_MAX_PMP_REGIONS; region_idx++)
    {
        (void)__get_PMPENTRYx(region_idx, &pmp_attr);
        //The serial-N CPU not support PMP_A_TOR, so that it will use PMP_A_NAPOT instead
        pmp_attr.protection |= PMP_A_NAPOT;
    }
}

/* Disable PMP */
static void pmp_disable(void)
{
    rt_uint32_t region_idx;
    pmp_config pmp_attr;

    for (region_idx = 0; region_idx < RT_RISCV_MAX_PMP_REGIONS; region_idx++)
    {
        (void)__get_PMPENTRYx(region_idx, &pmp_attr);
        pmp_attr.protection &= ~PMP_A;
    }
}

static rt_err_t check_addr_and_size(uint8_t region_idx, rt_uint32_t addr, rt_uint32_t size)
{
    rt_err_t ret = -RT_ERROR;
    if (region_idx <= RT_RISCV_MAX_PMP_REGIONS)
    {
        //The min size of PMP region is 4 byte and the addr and size should be aligned to 2^N
        if ((size >= 4U) && (0U == (size & (size - 1U))))
        {
            if (0U == addr % size)
            {
                ret = RT_EOK;
            }
        }
    }
    return ret;
}

/* Set the attribute for PMP */
static rt_err_t pmp_set_attr(rt_uint8_t region_idx, rt_uint32_t attribute, rt_uint32_t addr, rt_uint32_t size)
{
    //The RV PMP only support PMP_R PMP_W PMP_X
    rt_uint32_t execute;
    rt_uint32_t access;
    rt_err_t ret = RT_EOK;
    pmp_config pmp_attr = {0};
    if (RT_EOK == check_addr_and_size(region_idx, addr, size))
    {
        access = (attribute >> REGION_PERMISSION_Pos) & 0x7UL;
        execute = (attribute >> REGION_EXECUTE_Pos) & 0x1UL;
        if (RT_MPU_REGION_RO == access)
        {
            pmp_attr.protection|= PMP_R;
        }
        else if (RT_MPU_REGION_RW == access)
        {
             pmp_attr.protection|= (PMP_R | PMP_W);
        }
        else
        {
            ret = -RT_EINVAL;
        }
        if (RT_EOK == ret)
        {
            if (RT_MPU_REGION_EXECUTE_ENABLE == execute)
            {
                pmp_attr.protection |= PMP_X;
            }

            pmp_attr.base_addr = addr;
            pmp_attr.order = __CTZ(size);
        
            __set_PMPENTRYx(region_idx, &pmp_attr);
        }
    }
    return ret;
}

static void pmp_get_attr(unsigned int protection, rt_uint32_t *access, rt_uint32_t *execute)
{
    if (1U == (protection >> 2) & 0x1UL)
    {
        *execute = RT_MPU_REGION_EXECUTE_ENABLE;
    }
    else
    {
        *execute = RT_MPU_REGION_EXECUTE_DISABLE;
    }

    if (1U == (protection & 0x1UL))
    {
        *access = RT_MPU_REGION_RO;
        if (1U == (protection >> 1) & 0x1UL)
        {
            *access = RT_MPU_REGION_RW;
        }
    }
    else
    {
        *access = RT_MPU_REGION_NO_ACCESS;
    }
}

static void pmp_get_region_config(void *arg)
{
    rt_uint8_t region_idx;
    rt_uint32_t access = RT_MPU_REGION_NO_ACCESS;
    rt_uint32_t execute = RT_MPU_REGION_EXECUTE_DISABLE;
    struct rt_mal_region *tables = (struct rt_mal_region *)arg;
    pmp_config pmp_attr;

    for (region_idx = 0U; region_idx < RT_MPU_REGIONS_NUMBER; region_idx++)
    {
        __get_PMPENTRYx(region_idx, &pmp_attr);
        tables[region_idx].addr = pmp_attr.base_addr;
        tables[region_idx].size = 2U << pmp_attr.order;
        pmp_get_attr(pmp_attr.protection, &access, &execute);
        tables[region_idx].attribute = rt_mpu_region_attribute(access,
                                                               execute,
                                                               0U,
                                                               0U,
                                                               0U,
                                                               0U);

    }
}



static rt_err_t pmp_get_info(rt_thread_t thread, rt_uint32_t type, void *arg)
{
    rt_err_t result = RT_EOK;
    if (thread == RT_NULL || arg == RT_NULL)
    {
        return -RT_ERROR;
    }
    
    switch (type)
    {
        case GET_MPU_REGIONS_NUMBER:
            *(int *)arg = thread->setting.index; 
        break;
        
        case GET_MPU_REGIONS_CONFGIG:
            pmp_get_region_config(arg);
        default:
            LOG_W("not support type: %d", type);
            result = -RT_ERROR;
        break;
    }

    return result;
}

static void pmp_general_region_table_switch(rt_thread_t thread)
{
    rt_uint8_t region_idx = 0;
    rt_uint8_t region;
    rt_uint32_t align_size;
    rt_uint32_t align_addr;
    pmp_config pmp_attr;
#ifdef RT_MPU_USING_THREAD_STACK_PROTECT
    LOG_D("thread: %s, stack_addr: %p, size: %d", thread->name, thread->stack_addr, RT_MPU_THREAD_PROTECT_SIZE);
    memset(&pmp_attr, 0U, sizeof(pmp_config));
    align_size = pmp_align_min(RT_MPU_THREAD_PROTECT_SIZE);
    align_addr = (rt_uint32_t)RT_ALIGN_DOWN((rt_uint32_t)thread->stack_addr,  1 << (align_size + 1));
    pmp_attr.protection = PMP_R;
    pmp_attr.order = align_size;
    pmp_attr.base_addr = align_addr;
    __set_PMPENTRYx(RT_MPU_THREAD_STACK_REGION, &pmp_attr);
#else
    pmp_clear_region(RT_MPU_THREAD_STACK_REGION);
#endif

    for (region_idx = 0; region_idx < thread->setting.index; region_idx++)
    {
        region = region_idx + RT_MPU_FIRST_CONFIGURABLE_REGION;
        align_size = pmp_align_min(thread->setting.tables[region_idx].size);
        align_addr = (rt_uint32_t)RT_ALIGN_DOWN((rt_uint32_t)thread->setting.tables[region_idx].addr,  1 << (align_size + 1));
        if(RT_EOK != pmp_set_attr(region, thread->setting.tables[region_idx].attribute, align_addr, align_size))
        {
            LOG_E("pmp_set_attr error with index: %d", thread->setting.index);
        }
    }

    for (; region_idx < RT_MPU_NUM_CONFIGURABLE_REGION; region_idx++)
    {
        pmp_clear_region(RT_MPU_FIRST_CONFIGURABLE_REGION + region_idx);
    }
}

static void pmp_protect_region_table_switch(rt_thread_t thread, rt_uint8_t pmp_protect_area_num, 
                                             struct mpu_protect_regions* pmp_protect_areas)
{
    rt_uint8_t region_idx = 0;
    rt_uint8_t region;
    rt_uint32_t align_size;
    rt_uint32_t align_addr;
    rt_uint32_t unprotect_attribute;
    pmp_config pmp_attr;
    for (region_idx = 0; region_idx < pmp_protect_area_num; region_idx++)
    {
        region = region_idx + RT_MPU_FIRST_PROTECT_AREA_REGION;
        align_size = pmp_align_min(pmp_protect_areas[region_idx].tables.size);
        align_addr = (rt_uint32_t)RT_ALIGN_DOWN((rt_uint32_t)pmp_protect_areas[region_idx].tables.addr, 1 << (align_size + 1));

        /* thread can access this region */
        if (pmp_protect_areas[region_idx].thread == thread)
        {
            if(RT_EOK != pmp_set_attr(region, pmp_protect_areas[region_idx].tables.attribute, align_addr, align_size))
            {
                LOG_E("pmp_set_attr error with index: %d", thread->setting.index);
            }
        }
        /* thread can't access this region */
        else
        {
            unprotect_attribute = (pmp_protect_areas[region_idx].tables.attribute & (~REGION_PERMISSION_Msk)) | ((RT_MPU_REGION_NO_ACCESS << REGION_PERMISSION_Pos) & REGION_PERMISSION_Msk);
            if(RT_EOK != pmp_set_attr(region, unprotect_attribute, align_addr, align_size))
            {
                LOG_E("pmp_set_attr error with index: %d", thread->setting.index);
            }
        }
    }

    for (; region_idx < pmp_protect_area_num; region_idx++)
    {
        pmp_clear_region(region_idx + RT_MPU_FIRST_PROTECT_AREA_REGION);
    }
}

static void pmp_switch_table(rt_thread_t thread, rt_uint8_t mpu_protect_area_num, 
                                    struct mpu_protect_regions* mpu_protect_areas)
{
    RT_ASSERT(thread != RT_NULL);

    pmp_general_region_table_switch(thread);

#ifdef RT_MPU_PROTECT_AREA_REGIONS
    pmp_protect_region_table_switch(thread, mpu_protect_area_num, mpu_protect_areas);
#endif

}

static rt_err_t pmp_init(struct rt_mal_region *tables)
{
    rt_uint8_t region_idx = 0;
    rt_uint32_t align_size;
    rt_uint32_t align_addr;
    rt_err_t ret = -RT_ERROR;
    for (region_idx = 0; region_idx < RT_MPU_HW_USED_REGIONS; region_idx++)
    {
        if (tables[region_idx].size > 0U)
        {
            align_size = pmp_align_min(tables[region_idx].size);
            align_addr = (rt_uint32_t)RT_ALIGN_DOWN((rt_uint32_t)tables[region_idx].addr, 1 << (align_size + 1));
            
            ret = pmp_set_attr(region_idx, tables[region_idx].addr, align_addr, align_size);
            if (RT_EOK != ret)
            {
                LOG_E("pmp_set_attr error with index: %d", region_idx);
                break;
            }
        }
    }
    if (RT_EOK == ret)
    {
        LOG_I("mpu init success.");
    }
    return ret;
}

static struct rt_mpu_ops pmp_ops =
{
    .init         = pmp_init,
    .switch_table = pmp_switch_table,
    .get_info     = pmp_get_info
};

static int pmp_register(void)
{
    rt_err_t result = RT_EOK;

    result = rt_mpu_ops_register(&pmp_ops);
    if (result != RT_EOK)
    {
        LOG_E("riscv mal ops register failed");
    }

    return result;
}
INIT_BOARD_EXPORT(pmp_register);
