#include <debug.h>
#include <string.h>
#include <malloc.h>
#include <board.h>
#include <stdlib.h>
#include <boot_device.h>
#include <lib/atagparse.h>
#include <lib/cmdline.h>
#include "atags.h"

#include <libfdt.h>
#include <dev_tree.h>

typedef struct {
    uint64_t start;
    uint64_t size;
} meminfo_t;

typedef struct {
    uint32_t version;
    uint32_t chipset;
    uint32_t platform;
    uint32_t subtype;
    uint32_t revNum;
    uint32_t pmic_model[4];
} efidroid_fdtinfo_t;

typedef struct {
    struct list_node node;

    const char* name;
    uint32_t value;
} qcid_item_t;

typedef struct {
    uint32_t pmic_model;
    uint32_t pmic_minor;
    uint32_t pmic_major;
} qcpmicinfo_t;

typedef struct {
    // platform_id (x)
    uint32_t msm_id;
    uint32_t foundry_id;

    // variant_id (y)
    uint32_t platform_hw;
    uint32_t platform_major;
    uint32_t platform_minor;
    //uint32_t platform_subtype;

    // soc_rev (z)
    uint32_t soc_rev;

    // platform_subtype (y')
    uint32_t subtype;
    uint32_t ddr;
    uint32_t panel;
    uint32_t bootdev;

    // pmic_rev
    qcpmicinfo_t pmic_rev[4];
} qchwinfo_t;

// lk boot args
extern uint32_t lk_boot_args[4];

// atags backup
static void*  tags_copy = NULL;
static size_t tags_size = 0;

// parsed data: common
static qchwinfo_t* hwinfo_tags = NULL;
static qchwinfo_t* hwinfo_lk = NULL;
static char* command_line = NULL;
static struct list_node cmdline_list;
static lkargs_uefi_bootmode uefi_bootmode = LKARGS_UEFI_BM_NORMAL;
static meminfo_t* meminfo = NULL;
static size_t meminfo_count = 0;
static struct list_node qciditem_list;

static void qciditem_add(const char* name, uint32_t value)
{
    qcid_item_t* item = malloc(sizeof(qcid_item_t));
    ASSERT(item);

    item->name = strdup(name);
    item->value = value;

    list_add_tail(&qciditem_list, &item->node);
}

int qciditem_get(const char* name, uint32_t* datap)
{
    qcid_item_t *entry;
    list_for_every_entry(&qciditem_list, entry, qcid_item_t, node) {
        if (!strcmp(entry->name, name)) {
            *datap = entry->value;
            return 0;
        }
    }

    return -1;
}

uint32_t qciditem_get_zero(const char* name)
{
    uint32_t data = 0;
    qciditem_get(name, &data);
    return data;
}


const char* lkargs_get_command_line(void)
{
    return command_line;
}

struct list_node* lkargs_get_command_line_list(void)
{
    return &cmdline_list;
}

const char* lkargs_get_panel_name(const char* key)
{
    const char* value = cmdline_get(&cmdline_list, key);
    if (!value) return NULL;

    const char* name;
    const char* pch=strrchr(value,':');
    if (!pch) name = pch;
    else name = pch+1;

    return name;
}

lkargs_uefi_bootmode lkargs_get_uefi_bootmode(void)
{
    return uefi_bootmode;
}

void* lkargs_get_tags_backup(void)
{
    return tags_copy;
}
size_t lkargs_get_tags_backup_size(void)
{
    return tags_size;
}

// backup functions
static int save_atags(const struct tag *tags)
{
    const struct tag *t = tags;
    for (; t->hdr.size; t = tag_next(t));
    t++;
    tags_size = ((uint32_t)t)-((uint32_t)tags);

    tags_copy = malloc(tags_size);
    if (!tags_copy) {
        dprintf(CRITICAL, "Error saving atags!\n");
        return -1;
    }

    memcpy(tags_copy, tags, tags_size);
    return 0;
}

int atags_check_header(void* tags)
{
    struct tag *atags = (struct tag *)tags;
    return atags->hdr.tag!=ATAG_CORE;
}

static int save_fdt(void* fdt)
{
    tags_size = fdt_totalsize(fdt);
    tags_copy = malloc(tags_size);
    if (!tags_copy) {
        dprintf(CRITICAL, "Error saving fdt!\n");
        return -1;
    }

    memcpy(tags_copy, fdt, tags_size);
    return 0;
}

// parse ATAGS
static int parse_atag_core(const struct tag *tag)
{
    return 0;
}

static void add_meminfo(uint64_t start, uint64_t size)
{
    meminfo = realloc(meminfo, (++meminfo_count)*sizeof(*meminfo));
    ASSERT(meminfo);

    meminfo[meminfo_count-1].start = start;
    meminfo[meminfo_count-1].size = size;
}

static int parse_atag_mem32(const struct tag *tag)
{
    dprintf(INFO, "0x%08x-0x%08x\n", tag->u.mem.start, tag->u.mem.start+tag->u.mem.size);

    add_meminfo((uint64_t)tag->u.mem.start, (uint64_t)tag->u.mem.size);

    return 0;
}

static int parse_atag_cmdline(const struct tag *tag)
{
    command_line = malloc(COMMAND_LINE_SIZE+1);
    if (!tags_copy) {
        dprintf(CRITICAL, "Error allocating cmdline memory!\n");
        return -1;
    }

    strlcpy(command_line, tag->u.cmdline.cmdline, COMMAND_LINE_SIZE);

    return 0;
}

static struct tagtable tagtable[] = {
    {ATAG_CORE, parse_atag_core},
    {ATAG_MEM, parse_atag_mem32},
    {ATAG_CMDLINE, parse_atag_cmdline},
};

static int parse_atag(const struct tag *tag)
{
    struct tagtable *t;
    struct tagtable *t_end = tagtable+ARRAY_SIZE(tagtable);

    for (t = tagtable; t < t_end; t++)
        if (tag->hdr.tag == t->tag) {
            t->parse(tag);
            break;
        }

    return t < t_end;
}

static struct tagtable* get_tagtable_entry(const struct tag *tag)
{
    struct tagtable *t;
    struct tagtable *t_end = tagtable+ARRAY_SIZE(tagtable);

    for (t = tagtable; t < t_end; t++) {
        if (tag->hdr.tag == t->tag) {
            return t;
        }
    }

    return NULL;
}

static void parse_atags(const struct tag *t)
{
    for (; t->hdr.size; t = tag_next(t))
        if (!parse_atag(t))
            dprintf(INFO, "Ignoring unrecognised tag 0x%08x\n",
                    t->hdr.tag);
}

void* lkargs_atag_insert_unknown(void* tags)
{
    struct tag *tag = (struct tag *)tags;
    const struct tag *t;

    if (!tags_copy || atags_check_header(tags_copy))
        return tag;

    for (t=tags_copy; t->hdr.size; t=tag_next(t)) {
        if (!get_tagtable_entry(t)) {
            tag = tag_next(tag);
            memcpy(tag, t, t->hdr.size*sizeof(uint32_t));
        }
    }

    return tag;
}

static unsigned *target_mem_atag_create(unsigned *ptr, uint32_t size, uint32_t addr)
{
    *ptr++ = 4;
    *ptr++ = ATAG_MEM;
    *ptr++ = size;
    *ptr++ = addr;

    return ptr;
}

unsigned *lkargs_gen_meminfo_atags(unsigned *ptr)
{
    uint8_t i = 0;

    for (i = 0; i < meminfo_count; i++) {
        ptr = target_mem_atag_create(ptr,
                                     (uint32_t)meminfo[i].size,
                                     (uint32_t)meminfo[i].start);
    }

    return ptr;
}

void* lkargs_get_mmap_callback(void* pdata, platform_mmap_cb_t cb)
{
    uint32_t i;

    ASSERT(meminfo);

    for (i=0; i<meminfo_count; i++) {
        pdata = cb(pdata, meminfo[i].start, meminfo[i].size, false);
    }

    return pdata;
}

bool lkargs_has_meminfo(void)
{
    return !!meminfo;
}

// parse FDT
static int fdt_get_cell_sizes(void* fdt, uint32_t* out_addr_cell_size, uint32_t* out_size_cell_size)
{
    int rc;
    int len;
    const uint32_t *valp;
    uint32_t offset;
    uint32_t addr_cell_size = 0;
    uint32_t size_cell_size = 0;

    // get root node offset
    rc = fdt_path_offset(fdt, "/");
    if (rc<0) return -1;
    offset = rc;

    // find the #address-cells size
    valp = fdt_getprop(fdt, offset, "#address-cells", &len);
    if (len<=0 || !valp) {
        if (len == -FDT_ERR_NOTFOUND)
            addr_cell_size = 2;
        else return -1;
    } else {
        addr_cell_size = fdt32_to_cpu(*valp);
    }

    // find the #size-cells size
    valp = fdt_getprop(fdt, offset, "#size-cells", &len);
    if (len<=0 || !valp) {
        if (len == -FDT_ERR_NOTFOUND)
            size_cell_size = 2;
        else return -1;
    } else {
        size_cell_size = fdt32_to_cpu(*valp);
    }

    *out_addr_cell_size = addr_cell_size;
    *out_size_cell_size = size_cell_size;

    return 0;
}

static void print_qchwinfo(const char* prefix, qchwinfo_t* hwinfo)
{
    dprintf(INFO, "%splat=%u/%u variant=%u/%u/%u socrev=%x subtype=%u/%u/%u/%u pmic_rev=<%u/%u/%u> <%u/%u/%u> <%u/%u/%u> <%u/%u/%u>\n",
            prefix?:"",
            hwinfo->msm_id, hwinfo->foundry_id,
            hwinfo->platform_hw, hwinfo->platform_major, hwinfo->platform_minor,
            hwinfo->soc_rev,
            hwinfo->subtype, hwinfo->ddr, hwinfo->panel, hwinfo->bootdev,
            hwinfo->pmic_rev[0].pmic_model, hwinfo->pmic_rev[0].pmic_minor, hwinfo->pmic_rev[0].pmic_major,
            hwinfo->pmic_rev[1].pmic_model, hwinfo->pmic_rev[1].pmic_minor, hwinfo->pmic_rev[1].pmic_major,
            hwinfo->pmic_rev[2].pmic_model, hwinfo->pmic_rev[2].pmic_minor, hwinfo->pmic_rev[2].pmic_major,
            hwinfo->pmic_rev[3].pmic_model, hwinfo->pmic_rev[3].pmic_minor, hwinfo->pmic_rev[3].pmic_major
           );
}

static int parse_fdt(void* fdt)
{
    int ret = 0;
    uint32_t offset;
    int len;
    uint32_t i;

    // get memory node
    ret = fdt_path_offset(fdt, "/memory");
    if (ret < 0) {
        dprintf(CRITICAL, "Could not find memory node.\n");
    } else {
        offset = ret;

        uint32_t addr_cell_size = 1;
        uint32_t size_cell_size = 1;
        fdt_get_cell_sizes(fdt, &addr_cell_size, &size_cell_size);
        if (addr_cell_size>2 || size_cell_size>2) {
            dprintf(CRITICAL, "unsupported cell sizes\n");
            goto next;
        }

        // get reg node
        const uint32_t* reg = fdt_getprop(fdt, offset, "reg", &len);
        if (!reg) {
            dprintf(CRITICAL, "Could not find reg node.\n");
        } else {
            uint32_t regpos = 0;
            while (regpos<len/sizeof(uint32_t)) {
                uint64_t base = fdt32_to_cpu(reg[regpos++]);
                if (addr_cell_size==2) {
                    base = base<<32;
                    base = fdt32_to_cpu(reg[regpos++]);
                }

                uint64_t size = fdt32_to_cpu(reg[regpos++]);
                if (size_cell_size==2) {
                    size = size<<32;
                    size = fdt32_to_cpu(reg[regpos++]);
                }

                dprintf(INFO, "0x%016llx-0x%016llx\n", base, base+size);
                if (base>0xffffffff || size>0xffffffff) {
                    dprintf(CRITICAL, "address range exceeds 32bit address space\n");
                } else {
                    add_meminfo(base, size);
                }
            }
        }
    }

next:
    // get chosen node
    ret = fdt_path_offset(fdt, "/chosen");
    if (ret < 0) {
        dprintf(CRITICAL, "Could not find chosen node.\n");
        return ret;
    } else {
        offset = ret;

        // get bootargs
        const char* bootargs = (const char *)fdt_getprop(fdt, offset, "bootargs", &len);
        if (bootargs && len>0) {
            command_line = malloc(len);
            memcpy(command_line, bootargs, len);
        }
    }

    // get root node
    ret = fdt_path_offset(fdt, "/");
    if (ret < 0) {
        dprintf(CRITICAL, "Could not find root node.\n");
    }
    offset = ret;

    // get socinfo property
    int len_socinfo;
    const struct fdt_property* prop_socinfo = fdt_get_property(fdt, offset, "efidroid-soc-info", &len_socinfo);
    if (!prop_socinfo) {
        dprintf(CRITICAL, "Could not find efidroid-soc-info.\n");
    } else {
        // read info from fdt
        const efidroid_fdtinfo_t* socinfo = (const efidroid_fdtinfo_t*)prop_socinfo->data;
        uint32_t platform_id = fdt32_to_cpu(socinfo->chipset);
        uint32_t variant_id = fdt32_to_cpu(socinfo->platform);
        uint32_t hw_subtype = fdt32_to_cpu(socinfo->subtype);
        uint32_t soc_rev = fdt32_to_cpu(socinfo->revNum);
        uint32_t pmic_model[4] = {
            fdt32_to_cpu(socinfo->pmic_model[0]),
            fdt32_to_cpu(socinfo->pmic_model[1]),
            fdt32_to_cpu(socinfo->pmic_model[2]),
            fdt32_to_cpu(socinfo->pmic_model[3]),
        };

        // if subtype is 0, we have to use the subtype id from the variant_id
        if (hw_subtype==0) {
            hw_subtype = (variant_id&0xff000000)>>24;
        }

        // build hwinfo_tags
        hwinfo_tags = calloc(1, sizeof(qchwinfo_t));
        ASSERT(hwinfo_tags);
        hwinfo_tags->msm_id = platform_id&0x0000ffff;
        hwinfo_tags->foundry_id = (platform_id&0x00ff0000)>>16;
        hwinfo_tags->platform_hw = variant_id&0x000000ff;
        hwinfo_tags->platform_minor = (variant_id&0x0000ff00)>>8;
        hwinfo_tags->platform_major = (variant_id&0x00ff0000)>>16;
        hwinfo_tags->platform_minor = (soc_rev&0xff);
        hwinfo_tags->platform_major = (soc_rev>>16)&0xff;
        hwinfo_tags->soc_rev = soc_rev;
        hwinfo_tags->subtype = hw_subtype&0x000000ff;
        hwinfo_tags->ddr = (hw_subtype&0x700)>>8;
        hwinfo_tags->panel = (hw_subtype&0x1800)>>11;
        hwinfo_tags->bootdev = (hw_subtype&0xf0000)>>16;
        for (i=0; i<4; i++) {
            hwinfo_tags->pmic_rev[i].pmic_model = (pmic_model[i]&0x000000ff);
            hwinfo_tags->pmic_rev[i].pmic_minor = (pmic_model[i]&0x0000ff00)>>8;
            hwinfo_tags->pmic_rev[i].pmic_major = (pmic_model[i]&0x00ff0000)>>16;
        }
    }

    return 0;
}

uint32_t lkargs_gen_meminfo_fdt(void *fdt, uint32_t memory_node_offset)
{
    unsigned int i;
    int ret = 0;

    for (i = 0 ; i < meminfo_count; i++) {
        ret = dev_tree_add_mem_info(fdt,
                                    memory_node_offset,
                                    meminfo[i].start,
                                    meminfo[i].size);

        if (ret) {
            dprintf(CRITICAL, "Failed to add memory info\n");
            goto out;
        }
    }

out:
    return ret;
}

static int lkargs_fdt_insert_properties(void *fdtdst, int offsetdst, const void* fdtsrc, int offsetsrc)
{
    int len;
    int offset;
    int ret;

    offset = offsetsrc;
    for (offset = fdt_first_property_offset(fdtsrc, offset);
            (offset >= 0);
            (offset = fdt_next_property_offset(fdtsrc, offset))) {
        const struct fdt_property *prop;

        if (!(prop = fdt_get_property_by_offset(fdtsrc, offset, &len))) {
            offset = -FDT_ERR_INTERNAL;
            break;
        }

        const char* name = fdt_string(fdtsrc, fdt32_to_cpu(prop->nameoff));
        dprintf(SPEW, "PROP: %s\n", name);

        // blacklist our nodes
        if (!strcmp(name, "bootargs"))
            continue;
        if (!strcmp(name, "linux,initrd-start"))
            continue;
        if (!strcmp(name, "linux,initrd-end"))
            continue;

        // set prop
        ret = fdt_setprop(fdtdst, offsetdst, name, prop->data, len);
        if (ret) {
            dprintf(CRITICAL, "can't set prop: %s\n", fdt_strerror(ret));
            continue;
        }
    }

    return 0;
}

static int lkargs_fdt_insert_nodes(void *fdt, int target_offset)
{
    int depth;
    int ret = 0;
    uint32_t source_offset_chosen;
    uint32_t target_offset_node;
    int offset;

    // get chosen node in source
    ret = fdt_path_offset(tags_copy, "/chosen");
    if (ret < 0) {
        dprintf(CRITICAL, "Could not find chosen node.\n");
        return ret;
    }
    source_offset_chosen = ret;

    offset = source_offset_chosen;
    for (depth = 0; (offset >= 0) && (depth >= 0);
            offset = fdt_next_node(tags_copy, offset, &depth)) {
        const char *name = fdt_get_name(tags_copy, offset, NULL);
        dprintf(SPEW, "NODE: %s\n", name);

        // get/create node
        if (!strcmp(name, "chosen")) {
            ret = target_offset;
        } else {
            ret = fdt_subnode_offset(fdt, target_offset, name);
            if (ret < 0) {
                dprintf(SPEW, "creating node %s.\n", name);
                ret = fdt_add_subnode(fdt, target_offset, name);
                if (ret < 0) {
                    dprintf(CRITICAL, "can't create node %s: %s\n", name, fdt_strerror(ret));
                    continue;
                }
            }
        }
        target_offset_node = ret;

        // insert all properties
        lkargs_fdt_insert_properties(fdt, target_offset_node, tags_copy, offset);
    }

    return 0;
}

int lkargs_insert_chosen(void* fdt)
{
    int ret = 0;
    uint32_t target_offset_chosen;

    if (!tags_copy || fdt_check_header(tags_copy))
        return 0;

    // get chosen node
    ret = fdt_path_offset(fdt, "/chosen");
    if (ret < 0) {
        dprintf(CRITICAL, "Could not find chosen node.\n");
        return ret;
    }
    target_offset_chosen = ret;

    // insert all nodes
    return lkargs_fdt_insert_nodes(fdt, target_offset_chosen);
}

void atag_parse(void)
{
    uint32_t i;

    dprintf(INFO, "bootargs: 0x%x 0x%x 0x%x 0x%x\n",
            lk_boot_args[0],
            lk_boot_args[1],
            lk_boot_args[2],
            lk_boot_args[3]
           );

    // init
    cmdline_init(&cmdline_list);
    list_initialize(&qciditem_list);

    void* tags = (void*)lk_boot_args[2];

    // fdt
    if (!fdt_check_header(tags)) {
        save_fdt(tags);
        parse_fdt(tags);
    }

    // atags
    else if (!atags_check_header(tags)) {
        // machine type
        uint32_t machinetype = lk_boot_args[1];
        dprintf(INFO, "machinetype: %u\n", machinetype);

        qciditem_add("qcom,machtype", machinetype);
        save_atags(tags);
        parse_atags(tags);
    }

    // unknown
    else {
        dprintf(CRITICAL, "Invalid atags!\n");
        return;
    }

    // parse cmdline
    dprintf(INFO, "cmdline=[%s]\n", command_line);
    cmdline_addall(&cmdline_list, command_line, true);

    // get bootmode
    const char* bootmode = cmdline_get(&cmdline_list, "uefi.bootmode");
    if (bootmode) {
        dprintf(INFO, "uefi.bootmode = [%s]\n", bootmode);

        if (!strcmp(bootmode, "recovery"))
            uefi_bootmode = LKARGS_UEFI_BM_RECOVERY;

        cmdline_remove(&cmdline_list, "uefi.bootmode");
    }

    // build and print hwinfo_lk
#ifndef PLATFORM_MSM7X27A
    {
        uint32_t platform_id = board_platform_id();
        uint32_t foundry_id = (platform_id&0x00ff0000)>>16;
        foundry_id = MAX(foundry_id, board_foundry_id());
        uint32_t soc_rev = board_soc_version();

        // allocate
        hwinfo_lk = calloc(1, sizeof(qchwinfo_t));
        ASSERT(hwinfo_lk);

        hwinfo_lk->msm_id = platform_id&0x0000ffff;
        hwinfo_lk->foundry_id = foundry_id;
        hwinfo_lk->platform_hw = board_hardware_id();
        hwinfo_lk->platform_minor = (soc_rev&0xff);
        hwinfo_lk->platform_major = (soc_rev>>16)&0xff;
        hwinfo_lk->soc_rev = soc_rev;
        hwinfo_lk->subtype = board_hardware_subtype();
        hwinfo_lk->ddr = board_get_ddr_subtype();
        hwinfo_lk->panel = platform_detect_panel();
        hwinfo_lk->bootdev = platform_get_boot_dev();
        for (i=0; i<4; i++) {
            uint32_t pmic_model = board_pmic_target(i);
            hwinfo_lk->pmic_rev[i].pmic_model = (pmic_model&0x000000ff);
            hwinfo_lk->pmic_rev[i].pmic_minor = (pmic_model&0x0000ff00)>>8;
            hwinfo_lk->pmic_rev[i].pmic_major = (pmic_model&0x00ff0000)>>16;
        }

        print_qchwinfo("[LK]   ", hwinfo_lk);
    }

    // build info based on hwinfo_lk
    uint32_t platform_id = hwinfo_lk->msm_id | (hwinfo_lk->foundry_id<<16); // EQ
    uint32_t platform_hw = hwinfo_lk->platform_hw; // EQ
    uint32_t subtype = hwinfo_lk->subtype; // EQ
    uint32_t platform_subtype = (hwinfo_lk->subtype) | (hwinfo_lk->ddr << 8) | (hwinfo_lk->panel << 11) | (hwinfo_lk->bootdev << 16); // EQ
    uint32_t soc_rev = hwinfo_lk->soc_rev; // LE
    uint32_t variant_id_platform_hw = hwinfo_lk->platform_hw; // LE
    uint32_t variant_id_platform_minor = hwinfo_lk->platform_minor; // LE
    uint32_t variant_id_platform_major = hwinfo_lk->platform_major; // LE
    uint32_t variant_id_subtype = hwinfo_lk->subtype; // LE
    uint32_t pmicrev1 = (hwinfo_lk->pmic_rev[0].pmic_model) | (hwinfo_lk->pmic_rev[0].pmic_minor << 8) | (hwinfo_lk->pmic_rev[0].pmic_major << 16); // LE
    uint32_t pmicrev2 = (hwinfo_lk->pmic_rev[1].pmic_model) | (hwinfo_lk->pmic_rev[1].pmic_minor << 8) | (hwinfo_lk->pmic_rev[1].pmic_major << 16); // LE
    uint32_t pmicrev3 = (hwinfo_lk->pmic_rev[2].pmic_model) | (hwinfo_lk->pmic_rev[2].pmic_minor << 8) | (hwinfo_lk->pmic_rev[2].pmic_major << 16); // LE
    uint32_t pmicrev4 = (hwinfo_lk->pmic_rev[3].pmic_model) | (hwinfo_lk->pmic_rev[3].pmic_minor << 8) | (hwinfo_lk->pmic_rev[3].pmic_major << 16); // LE
    uint32_t foundry_id = hwinfo_lk->foundry_id; // EQ

    // improve info using hwinfo_tags
    if (hwinfo_tags) {
        // print hwinfo
        print_qchwinfo("[TAGS] ", hwinfo_tags);

        // use exact-match values from tags
        platform_id = hwinfo_tags->msm_id | (hwinfo_tags->foundry_id<<16);
        platform_hw = hwinfo_tags->platform_hw;
        subtype = hwinfo_tags->subtype;
        platform_subtype = (hwinfo_tags->subtype) | (hwinfo_tags->ddr << 8) | (hwinfo_tags->panel << 11) | (hwinfo_tags->bootdev << 16);
        foundry_id = hwinfo_tags->foundry_id;

        // use LE values from tags if they are bigger
        if (hwinfo_tags->soc_rev > hwinfo_lk->soc_rev)
            soc_rev = hwinfo_tags->soc_rev;
        // variant_id
        if (hwinfo_tags->platform_hw > variant_id_platform_hw)
            variant_id_platform_hw = hwinfo_tags->platform_hw;
        if (hwinfo_tags->platform_minor > variant_id_platform_minor)
            variant_id_platform_minor = hwinfo_tags->platform_minor;
        if (hwinfo_tags->platform_major > variant_id_platform_major)
            variant_id_platform_major = hwinfo_tags->platform_major;
        if (hwinfo_tags->subtype > variant_id_subtype)
            variant_id_subtype = hwinfo_tags->subtype;

        // always use pmicrev from LK until we tested this
    }

    uint32_t variant_id = (variant_id_platform_hw) | (variant_id_platform_minor << 8) | (variant_id_platform_major << 16) | (variant_id_subtype << 24);

    qciditem_add("qcom,platform_id", platform_id); // libboot_qcdt_platform_id
    qciditem_add("qcom,platform_hw", platform_hw); // libboot_qcdt_hardware_id
    qciditem_add("qcom,subtype", subtype); // libboot_qcdt_hardware_subtype
    qciditem_add("qcom,platform_subtype", platform_subtype); // libboot_qcdt_get_hlos_subtype
    qciditem_add("qcom,soc_rev", soc_rev); // libboot_qcdt_soc_version
    qciditem_add("qcom,variant_id", variant_id); // libboot_qcdt_target_id
    qciditem_add("qcom,pmic_rev1", pmicrev1); // libboot_qcdt_pmic_target
    qciditem_add("qcom,pmic_rev2", pmicrev2); // libboot_qcdt_pmic_target
    qciditem_add("qcom,pmic_rev3", pmicrev3); // libboot_qcdt_pmic_target
    qciditem_add("qcom,pmic_rev4", pmicrev4); // libboot_qcdt_pmic_target
    qciditem_add("qcom,foundry_id", foundry_id); // libboot_qcdt_foundry_id
#endif
}
