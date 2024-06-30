/*
 *      Minix 16bit format binary image loader and exec support routines.
 *
 *      5th Jan 1999    Alistair Riddoch (ajr@ecs.soton.ac.uk)
 *                      Added shared lib support consisting of a sys_dlload
 *                      Which loads dll text into new code segment, and dll
 *                      data into processes data segment.
 *                      This required support for the mh.unused field in the
 *                      bin header to contain the size of the dlls data, which
 *                      is used in sys_exec to offset loading of the processes
 *                      data at run time, new MINIX_DLLID library format, and
 *                      new MINIX_S_SPLITID binary format.
 *      21th Jan 2000   Alistair Riddoch (ajr@ecs.soton.ac.uk)
 *                      Rethink of binary format leads me to think that the
 *                      shared library route is not worth pursuing with the
 *                      implemented so far. Removed hacks from exec()
 *                      related to this, and instead add support for
 *                      binaries which have the stack below the data
 *                      segment. Binaries in this form have a large minix
 *                      format header (0x30 bytes rather than 0x20) which
 *                      contain a field which is reffered to in the a.out.h
 *                      file as "data relocation base". This is taken as the
 *                      address within the data segment where the program
 *                      expects its data segment to begin. Below this we
 *                      then put the stack. This requires adding support
 *                      for allocating blocks of memory which do not start
 *                      at the bottom of the segment. See arch/i86/mm/malloc.c
 *                      for details.
 */

#include <linuxmt/config.h>
#include <linuxmt/errno.h>
#include <linuxmt/fcntl.h>
#include <linuxmt/stat.h>
#include <linuxmt/string.h>
#include <linuxmt/sched.h>
#include <linuxmt/kernel.h>
#include <linuxmt/signal.h>
#include <linuxmt/time.h>
#include <linuxmt/mm.h>
#include <linuxmt/minix.h>
#include <linuxmt/os2.h>
#include <linuxmt/init.h>
#include <linuxmt/debug.h>
#include <linuxmt/memory.h>

#include <arch/segment.h>

/* for relocation debugging change to printk */
#define debug_reloc     debug

#ifndef __GNUC__
/* FIXME: evaluates some operands twice */
#   define add_overflow(a, b, res) \
        (*(res) = (a) + (b), *(res) < (b))
#   define bytes_to_paras(bytes) \
        ((segext_t)(((unsigned long)(bytes) + 15) >> 4))
#else
#   define add_overflow __builtin_add_overflow
#   define bytes_to_paras(bytes) __extension__({ \
        segext_t __w; \
        asm("addw $15, %0; rcrw %0" \
              : "=&r" (__w) \
              : "0" (bytes) \
              : "cc"); \
        __w >> 3; })
#endif

static void finalize_exec(struct inode *inode, segment_s *seg_code, segment_s *seg_data,
    word_t entry);

#ifdef CONFIG_EXEC_MMODEL
/*
 * Read relocations for a particular segment and apply them
 * Only IA-16 segment relocations are accepted
 */
static int relocate(seg_t place_base, unsigned long rsize, segment_s *seg_code,
               segment_s *seg_data, struct inode *inode, struct file *filp, size_t tseg)
{
    int retval = 0;
    seg_t save_ds = current->t_regs.ds;
    struct minix_reloc reloc;   //FIXME too large
    word_t val;

    if ((int)rsize % sizeof(struct minix_reloc))
        return -EINVAL;
    current->t_regs.ds = kernel_ds;
    debug_reloc("EXEC: applying %04lx bytes of relocations to segment %x\n",
           (unsigned long)rsize, place_base);
    while (rsize >= sizeof(struct minix_reloc)) {
        retval = filp->f_op->read(inode, filp, (char *)&reloc, sizeof(reloc));
        if (retval != (int)sizeof(reloc))
            goto error;
        switch (reloc.r_type) {
        case R_SEGWORD:
            switch (reloc.r_symndx) {
            case S_TEXT:
                val = seg_code->base; break;
            case S_FTEXT:
                val = seg_code->base + bytes_to_paras(tseg); break;
            case S_DATA:
                val = seg_data->base; break;
            default:
                debug_reloc("EXEC: bad relocation symbol index 0x%x\n", reloc.r_symndx);
                goto error;
            }
            debug_reloc("EXEC: reloc %d,%d,%04x:%04x %04x -> %04x\n",
                reloc.r_type, reloc.r_symndx, place_base, (word_t)reloc.r_vaddr,
                peekw((word_t)reloc.r_vaddr, place_base), val);
            pokew((word_t)reloc.r_vaddr, place_base, val);
            break;
        default:
            debug_reloc("EXEC: bad relocation type 0x%x\n", reloc.r_type);
            goto error;
        }
        rsize -= sizeof(struct minix_reloc);
    }
    current->t_regs.ds = save_ds;
    return 0;
  error:
    debug_reloc("EXEC: error in relocations\n");
    current->t_regs.ds = save_ds;
    if (retval >= 0)
        retval = -EINVAL;
    return retval;
}
#endif

#ifdef CONFIG_EXEC_OS2
#define debug_os2   printk

static struct dos_exec_hdr doshdr;
static struct os2_exec_hdr os2hdr;
static struct ne_segment os2segs[NEMAXSEGS];
static size_t seg_paras[NEMAXSEGS];
static segment_s *seg_mem[NEMAXSEGS];
static struct ne_reloc_num reloc_num;
static struct ne_reloc reloc;

static int execve_os2(struct inode *inode, struct file *filp, char *sptr, size_t slen)
{
    int retval, seg, n;
    size_t s, heap;
    struct ne_segment *segp;
    segment_s *seg_code, *seg_data;
    //TODO: eliminate excess stack vars
    seg_t ds = current->t_regs.ds;

    /* read MZ header, then OS/2 header */
    current->t_regs.ds = kernel_ds;
    filp->f_pos = 0;
    retval = filp->f_op->read(inode, filp, (char *)&doshdr, sizeof(doshdr));
    if (retval != sizeof(doshdr)) goto errout;

    filp->f_pos = doshdr.ext_hdr_offset;
    retval = filp->f_op->read(inode, filp, (char *)&os2hdr, sizeof(os2hdr));
    if (retval != sizeof(os2hdr)) goto errout;
    if (os2hdr.magic != NEMAGIC || os2hdr.target_os != NETARGET_OS2 ||
            (os2hdr.program_flags & 0x1F) != NEPRG_FLG_MULTIPLEDATA ||
            !os2hdr.auto_data_segment || os2hdr.auto_data_segment > os2hdr.num_segments ||
            os2hdr.num_modules || os2hdr.num_movable_entries ||
            os2hdr.num_resource_entries) {
        debug_os2("EXEC: Unsupported OS/2 format\n");
        goto errout;
    }

    printk("Segments: %d\n", os2hdr.num_segments);
    printk("Auto data segment: %d\n", os2hdr.auto_data_segment);
    printk("Heap: %u\n", os2hdr.heap_size);
    printk("Stack: %u\n", os2hdr.stack_size);
    if (!os2hdr.heap_size)
        os2hdr.heap_size = INIT_HEAP;
    if (!os2hdr.stack_size)
        os2hdr.stack_size = INIT_STACK;

    if (os2hdr.num_segments > NEMAXSEGS) {
        printk("EXEC: %d segments, exceeds %d max\n", os2hdr.num_segments, NEMAXSEGS);
        retval = -E2BIG;
        goto errout;
    }

    /* read segment table and allocate memory for segments */
    filp->f_pos = os2hdr.segment_table_offset + doshdr.ext_hdr_offset;
    retval = filp->f_op->read(inode, filp, (char *)os2segs,
        os2hdr.num_segments * sizeof(struct ne_segment));
    if (retval != os2hdr.num_segments * sizeof(struct ne_segment)) goto errout;
    retval = -ENOMEM;
    s = 0;  /* for compiler */
    seg_code = seg_data = 0;
    for(seg = 0; seg < os2hdr.num_segments; seg++) {
        segp = &os2segs[seg];
        s = ((segp->min_alloc + 15) & ~15) >> 4;
        if (!s) s = 0x1000;         /* 0 = 64K */
        seg_paras[seg] = s;
        if (seg+1 == os2hdr.auto_data_segment) {
            if ((heap = os2hdr.heap_size) == 0xffff)
                heap = 1;
            s += ((os2hdr.stack_size+slen+15) >> 4) + ((heap + 15) >> 4);
            if (s >= 0x1000) {
                retval = -E2BIG;
                goto errout2;
            }
            if (os2hdr.heap_size == 0xffff) {
                heap = (0x0FFF - s) << 4;
                s = 0x0FFF;
                printk("Auto max heap: %u\n", heap);
            }
            seg_paras[seg] = s;
        }
        printk("Segment %d: offset %04x size %5u/%6lu flags %04x minalloc %u\n",
            seg+1, segp->offset << os2hdr.file_alignment_shift_count, segp->size,
            (unsigned long)seg_paras[seg]<<4, segp->flags, segp->min_alloc);

        if (!(seg_mem[seg] = seg_alloc(s, (segp->flags & NESEG_DATA)?
                SEG_FLAG_DSEG: SEG_FLAG_CSEG)))
            goto errout2;
        if (seg+1 == os2hdr.reg_cs)
            seg_code = seg_mem[seg];
        if (seg+1 == os2hdr.auto_data_segment)
            seg_data = seg_mem[seg];

        /* clear BSS */
        if (segp->min_alloc != segp->size) {
            printk("Clear bss at %04x:%04x for %u bytes\n",
                seg_mem[seg]->base, segp->size, segp->min_alloc - segp->size);
            fmemsetb((char *)segp->size, seg_mem[seg]->base, 0,
                segp->min_alloc - segp->size);
        } else if (segp->min_alloc == 0) {
            printk("Clear bss at %04x:%04x for 64K bytes\n",    //TODO: test or remove
                seg_mem[seg]->base, segp->size);
            fmemsetb((char *)0, seg_mem[seg]->base, 0, 0x8000);
            fmemsetb((char *)0x8000, seg_mem[seg]->base, 0, 0x8000);
        }
    }
    if (!seg_code || !seg_data) {
        printk("Missing code and data segments\n");
        goto errout2;
    }

    /* read in segments and perform relocations */
    for(seg = 0; seg < os2hdr.num_segments; seg++) {
        segp = &os2segs[seg];
        if (!segp->size) {                                      //FIXME 64k read
            printk("EXEC: can't read 64K segment!\n");
            goto errout2;
        }
        filp->f_pos = (unsigned long)segp->offset << os2hdr.file_alignment_shift_count;
        current->t_regs.ds = seg_mem[seg]->base;
        printk("Reading seg %d %04x:0000 %u bytes offset %04lx\n", seg+1,
            current->t_regs.ds, segp->size,
            filp->f_pos);
        retval = filp->f_op->read(inode, filp, 0, segp->size);
        if (retval != segp->size) goto errout2;

        current->t_regs.ds = kernel_ds;
        if (segp->flags & NESEG_RELOCINFO) {
            retval = filp->f_op->read(inode, filp, (char *)&reloc_num, sizeof(reloc_num));
            if (retval != sizeof(reloc_num)) goto errout2;
            printk("%d relocation records\n", reloc_num.num_records);

            for (n = 0; n < reloc_num.num_records; n++) {
                retval = filp->f_op->read(inode, filp, (char *)&reloc, sizeof(reloc));
                if (retval != sizeof(reloc)) goto errout2;
                printk("Reloc %d: type %d flags %d, "
                    "source chain %04x (segment %d offset %04x)\n",
                    n+1, reloc.src_type, reloc.flags, reloc.src_chain, reloc.segment,
                    reloc.offset);

                if ((reloc.flags & NEFIXFLG_TARGET_MASK) != NEFIXFLG_INTERNALREF ||
                    (reloc.src_type != NEFIXSRC_SEGMENT &&
                     reloc.src_type != NEFIXSRC_FARADDR)) {
                        printk("Unsupported fixup type %d flags %d\n", reloc.src_type,
                            reloc.flags);
                            goto errout2;
                }
                heap = reloc.flags & NEFIXFLG_ADDITIVE;
                switch (reloc.src_type) {
                case NEFIXSRC_SEGMENT:
                    printk("pokew %04x:%04x <- seg %d %04x\n",
                        seg_mem[seg]->base, reloc.src_chain,
                        reloc.segment, seg_mem[reloc.segment-1]->base);
                    pokew(reloc.src_chain, seg_mem[seg]->base,
                        seg_mem[reloc.segment-1]->base);
                    break;
                case NEFIXSRC_FARADDR:
                    if (heap) {     /* additive */
                        printk("pokew %04x:%04x += offset %04x\n",
                            seg_mem[seg]->base, reloc.src_chain, reloc.offset);
                        word_t prev = peekw(reloc.src_chain, seg_mem[seg]->base);
                        pokew(reloc.src_chain, seg_mem[seg]->base, prev+reloc.offset);
                    } else {
                        printk("pokew %04x:%04x = offset %04x\n",
                            seg_mem[seg]->base, reloc.src_chain, reloc.offset);
                        pokew(reloc.src_chain, seg_mem[seg]->base, reloc.offset);
                    }
                    printk("pokew %04x:%04x <- seg %d %04x\n",
                        seg_mem[seg]->base, reloc.src_chain+2,
                        reloc.segment, seg_mem[reloc.segment-1]->base);
                    pokew(reloc.src_chain+2, seg_mem[seg]->base,
                        seg_mem[reloc.segment-1]->base);
                    break;
                }
            }
        }
    }

    /* From this point, exec() will surely succeed */

    /* set data/stack limits and copy argc/argv */
    current->t_endseg = s << 4;;
    current->t_enddata = os2segs[os2hdr.auto_data_segment-1].min_alloc;
    current->t_begstack = (current->t_endseg - slen) & ~1;
    printk("t_endseg %04x, t_begstack %04x\n", current->t_endseg, current->t_begstack);
    current->t_minstack = os2hdr.stack_size;
    if (seg_data != seg_mem[os2hdr.auto_data_segment-1]) panic("BAD");
    printk("fmemcpy %04x:%04x <- %d\n", seg_mem[os2hdr.auto_data_segment-1]->base,
        current->t_begstack, slen);
    fmemcpyb((char *)current->t_begstack, seg_mem[os2hdr.auto_data_segment-1]->base,
        sptr, ds, slen);

    current->t_regs.ds = seg_data->base;
    finalize_exec(inode, seg_code, seg_data, os2hdr.reg_ip);

    retval = 0;
    goto normal_out;

errout2:
    for (seg = 0; seg < NEMAXSEGS; seg++) {
        if (seg_mem[seg])
            seg_put(seg_mem[seg]);
        seg_mem[seg] = 0;
    }
errout:
    if (retval >= 0)
        retval = -EINVAL;
    current->t_regs.ds = ds;
normal_out:
    close_filp(inode, filp);
    if (retval)
        iput(inode);
    return retval;
}
#endif

int sys_execve(const char *filename, char *sptr, size_t slen)
{
    register __ptask currentp;
    struct inode *inode;
    struct file *filp;
    int retval;
    seg_t ds;
    seg_t base_data = 0;
    segment_s * seg_code;
    segment_s * seg_data;
    size_t len, min_len, heap, stack = 0;
    size_t bytes;
    segext_t paras;
    ASYNCIO_REENTRANT struct minix_exec_hdr mh;         /* 32 bytes */
#ifdef CONFIG_EXEC_MMODEL
    ASYNCIO_REENTRANT struct elks_supl_hdr esuph;       /* 24 bytes */
    int need_reloc_code = 1;
#endif

    /* Open the image */
    debug_file("EXEC(%P): '%t' env %d\n", filename, slen);

    retval = open_namei(filename, 0, 0, &inode, NULL);
    debug("EXEC: open returned %d\n", -retval);
    if (retval) goto error_exec1;

    /* Get a reading file handle */
    if ((retval = open_filp(O_RDONLY, inode, &filp))) goto error_exec2;
    if (!(filp->f_op) || !(filp->f_op->read)) goto error_exec2_5;

    /* Read the header */
    ds = current->t_regs.ds;
    current->t_regs.ds = kernel_ds;
    retval = filp->f_op->read(inode, filp, (char *) &mh, sizeof(mh));

    /* Sanity check it */
    if (retval != sizeof(mh)) goto error_exec3;

#ifdef CONFIG_EXEC_OS2
    if (((struct dos_exec_hdr *)&mh)->magic == MZMAGIC) {
        current->t_regs.ds = ds;
        return execve_os2(inode, filp, sptr, slen);
    }
#endif

    if ((mh.type != MINIX_SPLITID_AHISTORICAL && mh.type != MINIX_SPLITID) ||
        (size_t)mh.tseg == 0) {
        debug("EXEC: bad header, result %d\n", retval);
        goto error_exec3;
    }

    /* Look for the binary in memory */
    seg_code = 0;
    currentp = &task[0];
    do {
        if ((currentp->state <= TASK_STOPPED) && (currentp->t_inode == inode)) {
            debug("EXEC found copy\n");
            seg_code = currentp->mm.seg_code;
            break;
        }
    } while (++currentp < &task[max_tasks]);
    currentp = current;

    min_len = (size_t)mh.dseg;
    if (add_overflow(min_len, (size_t)mh.bseg, &min_len)) {
        retval = -EINVAL;
        goto error_exec3;
    }

#ifdef CONFIG_EXEC_MMODEL
    memset(&esuph, 0, sizeof(esuph));
#endif

    switch (mh.hlen) {
    case EXEC_MINIX_HDR_SIZE:
        break;
#ifdef CONFIG_EXEC_MMODEL
    case EXEC_RELOC_HDR_SIZE:
    case EXEC_FARTEXT_HDR_SIZE:
        /* BIG HEADER */
        retval = filp->f_op->read(inode, filp, (char *) &esuph,
                                  mh.hlen - EXEC_MINIX_HDR_SIZE);
        if (retval != mh.hlen - EXEC_MINIX_HDR_SIZE) {
            debug("EXEC: Bad secondary header, result %u\n", retval);
            goto error_exec3;
        }
        debug_reloc("EXEC: text rels %d, data rels %d, "
               "ftext rels %d, text/data base %x,%x\n",
               (int)esuph.msh_trsize/sizeof(struct minix_reloc),
               (int)esuph.msh_drsize/sizeof(struct minix_reloc),
               (int)esuph.esh_ftrsize/sizeof(struct minix_reloc),
               (int)esuph.msh_tbase, (int)esuph.msh_dbase);
#ifndef CONFIG_EXEC_COMPRESS
        if (esuph.esh_compr_tseg || esuph.esh_compr_ftseg || esuph.esh_compr_dseg)
            goto error_exec3;
#endif
        retval = -EINVAL;
        if (esuph.msh_tbase != 0)
            goto error_exec3;
        base_data = esuph.msh_dbase;
#ifdef CONFIG_EXEC_LOW_STACK
        if (base_data & 0xf)
            goto error_exec3;
        if (base_data != 0)
            debug("EXEC: New type executable stack = %x\n", base_data);

        if (add_overflow(min_len, base_data, &min_len)) /* adds stack size*/
            goto error_exec3;
#else
        if (base_data != 0)
            goto error_exec3;
#endif
        break;
#endif /* CONFIG_EXEC_MMODEL*/
    default:
        goto error_exec3;
    }

    /*
     * mh.version == 1: chmem is size of heap, 0 means use default heap
     * mh.version == 0: old ld86 used chmem as size of data+bss+heap+stack
     */
    switch (mh.version) {
    default:
        goto error_exec3;
    case 1:
        len = min_len;
#ifdef CONFIG_EXEC_LOW_STACK
        if (!base_data)
#endif
        {
            stack = mh.minstack? mh.minstack: INIT_STACK;
            if (add_overflow(len, stack, &len)) {       /* add stack */
                retval = -EFBIG;
                goto error_exec3;
            }
            if (add_overflow(len, slen, &len)) {        /* add argv, envp */
                retval = -E2BIG;
                goto error_exec3;
            }
        }
        heap = mh.chmem? mh.chmem: INIT_HEAP;
        if (heap >= 0xFFF0) {                           /* max heap specified*/
            if (len < 0xFFF0)                           /* len could be near overflow from above*/
                len = 0xFFF0;
        } else {
            if (add_overflow(len, heap, &len)) {        /* add heap */
                retval = -EFBIG;
                goto error_exec3;
            }
        }
        debug("EXEC: stack %u heap %u env %u total %u\n", stack, heap, slen, len);
        break;
    case 0:
        len = mh.chmem;
        if (len) {
            if (len <= min_len) {                       /* check bad chmem*/
                retval = -EINVAL;
                goto error_exec3;
            }
            stack = 0;                                  /* no protected stack space*/
            heap = len - min_len;
            if (heap < slen) {                          /* check space for environment*/
                retval = -E2BIG;
                goto error_exec3;
            }
        } else {
            stack = INIT_STACK;
            len = min_len;
#ifdef CONFIG_EXEC_LOW_STACK
            if (base_data) {
                if (add_overflow(len, INIT_HEAP, &len)) {
                    retval = -EFBIG;
                    goto error_exec3;
                }
            } else
#endif
            {
                if (add_overflow(len, INIT_HEAP + INIT_STACK, &len)) {
                    retval = -EFBIG;
                    goto error_exec3;
                }
            }
            if (add_overflow(len, slen, &len)) {
                retval = -E2BIG;
                goto error_exec3;
            }
        }
        debug("EXEC v0: stack %u heap %u env %u total %u\n", stack, len-min_len-stack-slen, slen, len);
    }

    /* Round data segment length up to a paragraph boundary
       (If the length overflows at this point, blame argv and envp...) */
    if (add_overflow(len, 15, &len)) {
        retval = -E2BIG;
        goto error_exec3;
    }
    len &= ~(size_t)15;

    debug("EXEC: Malloc time\n");

    /*
     *      Looks good. Get the memory we need
     */
    if (!seg_code) {
        bytes = (size_t)mh.tseg;
        paras = bytes_to_paras(bytes);
        retval = -ENOMEM;
#ifdef CONFIG_EXEC_COMPRESS
        if (esuph.esh_compr_tseg || esuph.esh_compr_ftseg) {
            if (esuph.esh_compr_tseg)
                bytes = esuph.esh_compr_tseg;
            paras += 1;         /* add 16 bytes for safety offset */
        }
#endif
#ifdef CONFIG_EXEC_MMODEL
        paras += bytes_to_paras((size_t)esuph.esh_ftseg);
#endif
        debug_reloc("EXEC: allocating %04x paras (%04x bytes) for text segment(s)\n", paras,
            bytes);
        seg_code = seg_alloc(paras, SEG_FLAG_CSEG);
        if (!seg_code) goto error_exec3;
        currentp->t_regs.ds = seg_code->base;  // segment used by read()
        retval = filp->f_op->read(inode, filp, 0, bytes);
        if (retval != bytes) {
            debug("EXEC(tseg read): bad result %u, expected %u\n", retval, bytes);
            goto error_exec4;
        }
#ifdef CONFIG_EXEC_COMPRESS
        retval = -ENOEXEC;
        if (esuph.esh_compr_tseg &&
            decompress(0, seg_code->base, (size_t)mh.tseg, bytes, 16) != (size_t)mh.tseg)
                goto error_exec4;
#endif
#ifdef CONFIG_EXEC_MMODEL
        bytes = esuph.esh_ftseg;
        if (bytes) {
#ifdef CONFIG_EXEC_COMPRESS
            if (esuph.esh_compr_ftseg)
                bytes = esuph.esh_compr_ftseg;
#endif
            currentp->t_regs.ds = seg_code->base + bytes_to_paras((size_t)mh.tseg);
            retval = filp->f_op->read(inode, filp, 0, bytes);
            if (retval != bytes) {
                debug("EXEC(ftseg read): bad result %u, expected %u\n", retval, bytes);
                goto error_exec4;
            }
#ifdef CONFIG_EXEC_COMPRESS
            retval = -ENOEXEC;
            if (esuph.esh_compr_ftseg &&
                decompress(0, seg_code->base + bytes_to_paras((size_t)mh.tseg),
                    (size_t)esuph.esh_ftseg, bytes, 16) != (size_t)esuph.esh_ftseg)
                        goto error_exec4;
#endif
        }
#endif
    } else {
        seg_get (seg_code);
#ifdef CONFIG_EXEC_MMODEL
        filp->f_pos += esuph.esh_compr_tseg? esuph.esh_compr_tseg: (size_t)mh.tseg;
        filp->f_pos += esuph.esh_compr_ftseg? esuph.esh_compr_ftseg: (size_t)esuph.esh_ftseg;
        need_reloc_code = 0;
#else
        filp->f_pos += (size_t)mh.tseg;
#endif
    }

    paras = len >> 4;
    retval = -ENOMEM;
    debug_reloc("EXEC: allocating %04x paras (%04x bytes) for data segment\n", paras, len);
    seg_data = seg_alloc (paras, SEG_FLAG_DSEG);
    if (!seg_data) goto error_exec4;
    debug("EXEC: Malloc succeeded - cs=%x ds=%x\n", seg_code->base, seg_data->base);

    bytes = (size_t)mh.dseg;
#ifdef CONFIG_EXEC_COMPRESS
    if (esuph.esh_compr_dseg) {
            bytes = esuph.esh_compr_dseg;
            //if (mh.bss < 16 && !stack && !heap)
                //paras += 1;           /* add 16 bytes for safety offset */
    }
#endif
    currentp->t_regs.ds = seg_data->base;  // segment used by read()
    retval = filp->f_op->read(inode, filp, (char *)base_data, bytes);
    if (retval != bytes) {
        debug("EXEC(dseg read): bad result %d, expected %u\n", retval, bytes);
        goto error_exec5;
    }
#ifdef CONFIG_EXEC_COMPRESS
        if (esuph.esh_compr_dseg &&
            decompress(0, seg_data->base, (size_t)mh.dseg, bytes, 16) != (size_t)mh.dseg)
                goto error_exec5;
#endif

#ifdef CONFIG_EXEC_MMODEL
    if (need_reloc_code) {
        /* Read and apply text segment relocations */
        retval = relocate(seg_code->base, esuph.msh_trsize, seg_code, seg_data,
                          inode, filp, mh.tseg);
        if (retval != 0)
            goto error_exec5;
        /* Read and apply far text segment relocations */
        retval = relocate(seg_code->base + bytes_to_paras((size_t)mh.tseg),
                          esuph.esh_ftrsize, seg_code, seg_data,
                          inode, filp, mh.tseg);
        if (retval != 0)
            goto error_exec5;
    } else {
        /* If reusing existing text segments, no need to re-relocate */
        filp->f_pos += esuph.msh_trsize;
        filp->f_pos += esuph.esh_ftrsize;
    }
    /* Read and apply data relocations */
    retval = relocate(seg_data->base, esuph.msh_drsize, seg_code, seg_data,
                      inode, filp, mh.tseg);
    if (retval != 0)
        goto error_exec5;
#endif

    /* From this point, exec() will surely succeed */

    /* Wipe the BSS */
    fmemsetb((char *)(size_t)mh.dseg + base_data, seg_data->base, 0, (size_t)mh.bseg);

    currentp->t_endseg = (__pptr)len;   /* Needed for sys_brk() */
    currentp->t_enddata = (__pptr) ((size_t)mh.dseg + (size_t)mh.bseg + base_data);

    /* Copy the command line and environment */
#ifdef CONFIG_EXEC_LOW_STACK
    currentp->t_begstack = (base_data   /* Just above the top of stack */
        ? (__pptr)base_data
        : currentp->t_endseg) - slen;
#else
    currentp->t_begstack = currentp->t_endseg - slen;
#endif
    currentp->t_begstack &= ~1;         /* force even stack pointer and argv/envp*/
    fmemcpyb((char *)currentp->t_begstack, seg_data->base, sptr, ds, slen);
    currentp->t_minstack = stack;

    finalize_exec(inode, seg_code, seg_data, (word_t)mh.entry);

    /*      Done.
     *      This will return onto the new user stack and to cs:entry of
     *      the user process.
     */
    retval = 0;
    goto normal_out;

  error_exec5:
    seg_put (seg_data);

  error_exec4:
    seg_put (seg_code);

  error_exec3:
    current->t_regs.ds = ds;
  error_exec2_5:
    if (retval >= 0)
        retval = -ENOEXEC;
  normal_out:
    close_filp(inode, filp);

    if (retval)
  error_exec2:
        iput(inode);
  error_exec1:
    debug("EXEC(%P): return %d\n", retval);
    return retval;
}

static void finalize_exec(struct inode *inode, segment_s *seg_code, segment_s *seg_data,
    word_t entry)
{
    __ptask currentp = current;
    int i, n, v;

    /* From this point, the old code and data segments are not needed anymore */

    /* Flush the old binary out.  */
    if (currentp->mm.seg_code) seg_put(currentp->mm.seg_code);
    if (currentp->mm.seg_data) seg_put(currentp->mm.seg_data);

    currentp->mm.seg_code = seg_code;
    currentp->t_xregs.cs = seg_code->base;
    printk("ENTRY %04x:%04x\n", seg_code->base, entry);

    currentp->mm.seg_data = seg_data;
    currentp->t_regs.ss = currentp->t_regs.es = seg_data->base;
    currentp->t_regs.sp = (__u16)currentp->t_begstack;

    currentp->t_endbrk =  currentp->t_enddata;
    if ((int)currentp->t_endbrk & 1)    /* help libc malloc with even break address */
        currentp->t_endbrk++;

    {
        i = 0;

        /* argv and envp are two NULL-terminated arrays of pointers, located
         * right after argc.  This fixes them up so that the loaded program
         * gets the right strings. */

        n = 0;              /* Start by skipping argc */
        do {
            i += sizeof(__u16);
            if ((v = get_ustack(currentp, i)) != 0)
                put_ustack(currentp, i, (currentp->t_begstack + v));
            else n++;       /* increments for each array traversed */
        } while (n < 2);

        /* Clear signal handlers */
        i = 0;
        do {
            currentp->sig.action[i].sa_dispose = SIGDISP_DFL;
        } while (++i < NSIG);
        currentp->sig.handler = (__kern_sighandler_t)NULL;

        /* Close required files */
        i = 0;
        do {
            if (FD_ISSET(i, &currentp->files.close_on_exec))
                sys_close(i);
        } while (++i < NR_OPEN);
    }

    iput(currentp->t_inode);
    currentp->t_inode = inode;

    /* this could be a good place to set the effective user identifier
     * in case the suid bit of the executable had been set */

    /* can I trust the following fields?  */
    if (inode->i_mode & S_ISUID)
        currentp->euid = inode->i_uid;
    if (inode->i_mode & S_ISGID)
        currentp->egid = inode->i_gid;

    /* arrange our return to be to CS:entry */
    arch_setup_user_stack(currentp, entry);

#if UNUSED      /* used only for vfork()*/
    wake_up(&currentp->p_parent->child_wait);
#endif
}
