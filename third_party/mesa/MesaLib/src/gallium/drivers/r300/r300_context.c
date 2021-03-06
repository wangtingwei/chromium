/*
 * Copyright 2008 Corbin Simpson <MostAwesomeDude@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE. */

#include "draw/draw_context.h"

#include "util/u_memory.h"
#include "util/u_sampler.h"
#include "util/u_simple_list.h"
#include "util/u_upload_mgr.h"

#include "r300_cb.h"
#include "r300_context.h"
#include "r300_emit.h"
#include "r300_hyperz.h"
#include "r300_screen.h"
#include "r300_screen_buffer.h"
#include "r300_winsys.h"

#include <inttypes.h>

static void r300_update_num_contexts(struct r300_screen *r300screen,
                                     int diff)
{
    if (diff > 0) {
        p_atomic_inc(&r300screen->num_contexts);

        if (r300screen->num_contexts > 1)
            util_mempool_set_thread_safety(&r300screen->pool_buffers,
                                           UTIL_MEMPOOL_MULTITHREADED);
    } else {
        p_atomic_dec(&r300screen->num_contexts);

        if (r300screen->num_contexts <= 1)
            util_mempool_set_thread_safety(&r300screen->pool_buffers,
                                           UTIL_MEMPOOL_SINGLETHREADED);
    }
}

static void r300_release_referenced_objects(struct r300_context *r300)
{
    struct pipe_framebuffer_state *fb =
            (struct pipe_framebuffer_state*)r300->fb_state.state;
    struct r300_textures_state *textures =
            (struct r300_textures_state*)r300->textures_state.state;
    struct r300_query *query, *temp;
    unsigned i;

    /* Framebuffer state. */
    util_unreference_framebuffer_state(fb);

    /* Textures. */
    for (i = 0; i < textures->sampler_view_count; i++)
        pipe_sampler_view_reference(
                (struct pipe_sampler_view**)&textures->sampler_views[i], NULL);

    /* The special dummy texture for texkill. */
    if (r300->texkill_sampler) {
        pipe_sampler_view_reference(
                (struct pipe_sampler_view**)&r300->texkill_sampler,
                NULL);
    }

    /* The SWTCL VBO. */
    pipe_resource_reference(&r300->vbo, NULL);

    /* Vertex buffers. */
    for (i = 0; i < r300->vertex_buffer_count; i++) {
        pipe_resource_reference(&r300->vertex_buffer[i].buffer, NULL);
    }

    /* If there are any queries pending or not destroyed, remove them now. */
    foreach_s(query, temp, &r300->query_list) {
        remove_from_list(query);
        FREE(query);
    }
}

static void r300_destroy_context(struct pipe_context* context)
{
    struct r300_context* r300 = r300_context(context);
    struct r300_atom *atom;

    if (r300->blitter)
        util_blitter_destroy(r300->blitter);
    if (r300->draw)
        draw_destroy(r300->draw);

    /* Print stats, if enabled. */
    if (SCREEN_DBG_ON(r300->screen, DBG_STATS)) {
        fprintf(stderr, "r300: Stats for context %p:\n", r300);
        fprintf(stderr, "    : Flushes: %" PRIu64 "\n", r300->flush_counter);
        foreach(atom, &r300->atom_list) {
            fprintf(stderr, "    : %s: %" PRIu64 " emits\n",
                atom->name, atom->counter);
        }
    }

    if (r300->upload_vb)
        u_upload_destroy(r300->upload_vb);
    if (r300->upload_ib)
        u_upload_destroy(r300->upload_ib);

    if (r300->tran.translate_cache)
        translate_cache_destroy(r300->tran.translate_cache);

    /* XXX: This function assumes r300->query_list was initialized */
    r300_release_referenced_objects(r300);

    if (r300->zmask_mm)
        r300_hyperz_destroy_mm(r300);

    if (r300->cs)
        r300->rws->cs_destroy(r300->cs);

    /* XXX: No way to tell if this was initialized or not? */
    util_mempool_destroy(&r300->pool_transfers);

    r300_update_num_contexts(r300->screen, -1);

    /* Free the structs allocated in r300_setup_atoms() */
    if (r300->aa_state.state) {
        FREE(r300->aa_state.state);
        FREE(r300->blend_color_state.state);
        FREE(r300->clip_state.state);
        FREE(r300->fb_state.state);
        FREE(r300->gpu_flush.state);
        FREE(r300->hyperz_state.state);
        FREE(r300->invariant_state.state);
        FREE(r300->rs_block_state.state);
        FREE(r300->scissor_state.state);
        FREE(r300->textures_state.state);
        FREE(r300->vap_invariant_state.state);
        FREE(r300->viewport_state.state);
        FREE(r300->ztop_state.state);
        FREE(r300->fs_constants.state);
        FREE(r300->vs_constants.state);
        if (!r300->screen->caps.has_tcl) {
            FREE(r300->vertex_stream_state.state);
        }
    }
    FREE(r300);
}

void r300_flush_cb(void *data)
{
    struct r300_context* const cs_context_copy = data;

    cs_context_copy->context.flush(&cs_context_copy->context, 0, NULL);
}

#define R300_INIT_ATOM(atomname, atomsize) \
 do { \
    r300->atomname.name = #atomname; \
    r300->atomname.state = NULL; \
    r300->atomname.size = atomsize; \
    r300->atomname.emit = r300_emit_##atomname; \
    r300->atomname.dirty = FALSE; \
    insert_at_tail(&r300->atom_list, &r300->atomname); \
 } while (0)

static void r300_setup_atoms(struct r300_context* r300)
{
    boolean is_rv350 = r300->screen->caps.is_rv350;
    boolean is_r500 = r300->screen->caps.is_r500;
    boolean has_tcl = r300->screen->caps.has_tcl;
    boolean drm_2_3_0 = r300->rws->get_value(r300->rws, R300_VID_DRM_2_3_0);
    boolean drm_2_6_0 = r300->rws->get_value(r300->rws, R300_VID_DRM_2_6_0);
    boolean has_hyperz = r300->rws->get_value(r300->rws, R300_CAN_HYPERZ);
    boolean has_hiz_ram = r300->screen->caps.hiz_ram > 0;

    /* Create the actual atom list.
     *
     * Each atom is examined and emitted in the order it appears here, which
     * can affect performance and conformance if not handled with care.
     *
     * Some atoms never change size, others change every emit - those have
     * the size of 0 here.
     *
     * NOTE: The framebuffer state is split into these atoms:
     * - gpu_flush          (unpipelined regs)
     * - aa_state           (unpipelined regs)
     * - fb_state           (unpipelined regs)
     * - hyperz_state       (unpipelined regs followed by pipelined ones)
     * - fb_state_pipelined (pipelined regs)
     * The motivation behind this is to be able to emit a strict
     * subset of the regs, and to have reasonable register ordering. */
    make_empty_list(&r300->atom_list);
    /* SC, GB (unpipelined), RB3D (unpipelined), ZB (unpipelined). */
    R300_INIT_ATOM(gpu_flush, 9);
    R300_INIT_ATOM(aa_state, 4);
    R300_INIT_ATOM(fb_state, 0);
    R300_INIT_ATOM(hyperz_state, is_r500 || (is_rv350 && drm_2_6_0) ? 10 : 8);
    /* ZB (unpipelined), SC. */
    R300_INIT_ATOM(ztop_state, 2);
    /* ZB, FG. */
    R300_INIT_ATOM(dsa_state, is_r500 ? 8 : 6);
    /* RB3D. */
    R300_INIT_ATOM(blend_state, 8);
    R300_INIT_ATOM(blend_color_state, is_r500 ? 3 : 2);
    /* SC. */
    R300_INIT_ATOM(scissor_state, 3);
    /* GB, FG, GA, SU, SC, RB3D. */
    R300_INIT_ATOM(invariant_state, 16 + (is_rv350 ? 4 : 0));
    /* VAP. */
    R300_INIT_ATOM(viewport_state, 9);
    R300_INIT_ATOM(pvs_flush, 2);
    R300_INIT_ATOM(vap_invariant_state, 9);
    R300_INIT_ATOM(vertex_stream_state, 0);
    R300_INIT_ATOM(vs_state, 0);
    R300_INIT_ATOM(vs_constants, 0);
    R300_INIT_ATOM(clip_state, has_tcl ? 5 + (6 * 4) : 2);
    /* VAP, RS, GA, GB, SU, SC. */
    R300_INIT_ATOM(rs_block_state, 0);
    R300_INIT_ATOM(rs_state, 0);
    /* SC, US. */
    R300_INIT_ATOM(fb_state_pipelined, 5 + (drm_2_3_0 ? 3 : 0));
    /* US. */
    R300_INIT_ATOM(fs, 0);
    R300_INIT_ATOM(fs_rc_constant_state, 0);
    R300_INIT_ATOM(fs_constants, 0);
    /* TX. */
    R300_INIT_ATOM(texture_cache_inval, 2);
    R300_INIT_ATOM(textures_state, 0);
    if (has_hyperz) {
        /* HiZ Clear */
        if (has_hiz_ram)
            R300_INIT_ATOM(hiz_clear, 0);
        /* zmask clear */
        R300_INIT_ATOM(zmask_clear, 0);
    }
    /* ZB (unpipelined), SU. */
    R300_INIT_ATOM(query_start, 4);

    /* Replace emission functions for r500. */
    if (is_r500) {
        r300->fs.emit = r500_emit_fs;
        r300->fs_rc_constant_state.emit = r500_emit_fs_rc_constant_state;
        r300->fs_constants.emit = r500_emit_fs_constants;
    }

    /* Some non-CSO atoms need explicit space to store the state locally. */
    r300->aa_state.state = CALLOC_STRUCT(r300_aa_state);
    r300->blend_color_state.state = CALLOC_STRUCT(r300_blend_color_state);
    r300->clip_state.state = CALLOC_STRUCT(r300_clip_state);
    r300->fb_state.state = CALLOC_STRUCT(pipe_framebuffer_state);
    r300->gpu_flush.state = CALLOC_STRUCT(pipe_framebuffer_state);
    r300->hyperz_state.state = CALLOC_STRUCT(r300_hyperz_state);
    r300->invariant_state.state = CALLOC_STRUCT(r300_invariant_state);
    r300->rs_block_state.state = CALLOC_STRUCT(r300_rs_block);
    r300->scissor_state.state = CALLOC_STRUCT(pipe_scissor_state);
    r300->textures_state.state = CALLOC_STRUCT(r300_textures_state);
    r300->vap_invariant_state.state = CALLOC_STRUCT(r300_vap_invariant_state);
    r300->viewport_state.state = CALLOC_STRUCT(r300_viewport_state);
    r300->ztop_state.state = CALLOC_STRUCT(r300_ztop_state);
    r300->fs_constants.state = CALLOC_STRUCT(r300_constant_buffer);
    r300->vs_constants.state = CALLOC_STRUCT(r300_constant_buffer);
    if (!r300->screen->caps.has_tcl) {
        r300->vertex_stream_state.state = CALLOC_STRUCT(r300_vertex_stream_state);
    }

    /* Some non-CSO atoms don't use the state pointer. */
    r300->fb_state_pipelined.allow_null_state = TRUE;
    r300->fs_rc_constant_state.allow_null_state = TRUE;
    r300->pvs_flush.allow_null_state = TRUE;
    r300->query_start.allow_null_state = TRUE;
    r300->texture_cache_inval.allow_null_state = TRUE;

    /* Some states must be marked as dirty here to properly set up
     * hardware in the first command stream. */
    r300->invariant_state.dirty = TRUE;
    r300->pvs_flush.dirty = TRUE;
    r300->vap_invariant_state.dirty = TRUE;
    r300->texture_cache_inval.dirty = TRUE;
    r300->textures_state.dirty = TRUE;
}

/* Not every state tracker calls every driver function before the first draw
 * call and we must initialize the command buffers somehow. */
static void r300_init_states(struct pipe_context *pipe)
{
    struct r300_context *r300 = r300_context(pipe);
    struct pipe_blend_color bc = {{0}};
    struct pipe_clip_state cs = {{{0}}};
    struct pipe_scissor_state ss = {0};
    struct r300_clip_state *clip =
            (struct r300_clip_state*)r300->clip_state.state;
    struct r300_gpu_flush *gpuflush =
            (struct r300_gpu_flush*)r300->gpu_flush.state;
    struct r300_vap_invariant_state *vap_invariant =
            (struct r300_vap_invariant_state*)r300->vap_invariant_state.state;
    struct r300_invariant_state *invariant =
            (struct r300_invariant_state*)r300->invariant_state.state;

    CB_LOCALS;

    pipe->set_blend_color(pipe, &bc);
    pipe->set_scissor_state(pipe, &ss);

    /* Initialize the clip state. */
    if (r300_context(pipe)->screen->caps.has_tcl) {
        pipe->set_clip_state(pipe, &cs);
    } else {
        BEGIN_CB(clip->cb, 2);
        OUT_CB_REG(R300_VAP_CLIP_CNTL, R300_CLIP_DISABLE);
        END_CB;
    }

    /* Initialize the GPU flush. */
    {
        BEGIN_CB(gpuflush->cb_flush_clean, 6);

        /* Flush and free renderbuffer caches. */
        OUT_CB_REG(R300_RB3D_DSTCACHE_CTLSTAT,
            R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS |
            R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D);
        OUT_CB_REG(R300_ZB_ZCACHE_CTLSTAT,
            R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
            R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);

        /* Wait until the GPU is idle.
         * This fixes random pixels sometimes appearing probably caused
         * by incomplete rendering. */
        OUT_CB_REG(RADEON_WAIT_UNTIL, RADEON_WAIT_3D_IDLECLEAN);
        END_CB;
    }

    /* Initialize the VAP invariant state. */
    {
        BEGIN_CB(vap_invariant->cb, 9);
        OUT_CB_REG(VAP_PVS_VTX_TIMEOUT_REG, 0xffff);
        OUT_CB_REG_SEQ(R300_VAP_GB_VERT_CLIP_ADJ, 4);
        OUT_CB_32F(1.0);
        OUT_CB_32F(1.0);
        OUT_CB_32F(1.0);
        OUT_CB_32F(1.0);
        OUT_CB_REG(R300_VAP_PSC_SGN_NORM_CNTL, R300_SGN_NORM_NO_ZERO);
        END_CB;
    }

    /* Initialize the invariant state. */
    {
        BEGIN_CB(invariant->cb, r300->invariant_state.size);
        OUT_CB_REG(R300_GB_SELECT, 0);
        OUT_CB_REG(R300_FG_FOG_BLEND, 0);
        OUT_CB_REG(R300_GA_ROUND_MODE, 1);
        OUT_CB_REG(R300_GA_OFFSET, 0);
        OUT_CB_REG(R300_SU_TEX_WRAP, 0);
        OUT_CB_REG(R300_SU_DEPTH_SCALE, 0x4B7FFFFF);
        OUT_CB_REG(R300_SU_DEPTH_OFFSET, 0);
        OUT_CB_REG(R300_SC_EDGERULE, 0x2DA49525);

        if (r300->screen->caps.is_rv350) {
            OUT_CB_REG(R500_RB3D_DISCARD_SRC_PIXEL_LTE_THRESHOLD, 0x01010101);
            OUT_CB_REG(R500_RB3D_DISCARD_SRC_PIXEL_GTE_THRESHOLD, 0xFEFEFEFE);
        }
        END_CB;
    }

    /* Initialize the hyperz state. */
    {
        struct r300_hyperz_state *hyperz =
            (struct r300_hyperz_state*)r300->hyperz_state.state;
        BEGIN_CB(&hyperz->cb_flush_begin, r300->hyperz_state.size);
        OUT_CB_REG(R300_ZB_ZCACHE_CTLSTAT,
                   R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE);
        OUT_CB_REG(R300_ZB_BW_CNTL, 0);
        OUT_CB_REG(R300_ZB_DEPTHCLEARVALUE, 0);
        OUT_CB_REG(R300_SC_HYPERZ, R300_SC_HYPERZ_ADJ_2);

        if (r300->screen->caps.is_r500 ||
            (r300->screen->caps.is_rv350 &&
             r300->rws->get_value(r300->rws, R300_VID_DRM_2_6_0))) {
            OUT_CB_REG(R300_GB_Z_PEQ_CONFIG, 0);
        }
        END_CB;
    }
}

struct pipe_context* r300_create_context(struct pipe_screen* screen,
                                         void *priv)
{
    struct r300_context* r300 = CALLOC_STRUCT(r300_context);
    struct r300_screen* r300screen = r300_screen(screen);
    struct r300_winsys_screen *rws = r300screen->rws;

    if (!r300)
        return NULL;

    r300_update_num_contexts(r300screen, 1);

    r300->rws = rws;
    r300->screen = r300screen;

    r300->context.winsys = (struct pipe_winsys*)rws;
    r300->context.screen = screen;
    r300->context.priv = priv;

    r300->context.destroy = r300_destroy_context;

    make_empty_list(&r300->query_list);

    util_mempool_create(&r300->pool_transfers,
                        sizeof(struct pipe_transfer), 64,
                        UTIL_MEMPOOL_SINGLETHREADED);

    r300->cs = rws->cs_create(rws);
    if (r300->cs == NULL)
        goto fail;

    if (!r300screen->caps.has_tcl) {
        /* Create a Draw. This is used for SW TCL. */
        r300->draw = draw_create(&r300->context);
        /* Enable our renderer. */
        draw_set_rasterize_stage(r300->draw, r300_draw_stage(r300));
        /* Disable converting points/lines to triangles. */
        draw_wide_line_threshold(r300->draw, 10000000.f);
        draw_wide_point_threshold(r300->draw, 10000000.f);
    }

    r300_setup_atoms(r300);

    r300_init_blit_functions(r300);
    r300_init_flush_functions(r300);
    r300_init_query_functions(r300);
    r300_init_state_functions(r300);
    r300_init_resource_functions(r300);

    r300->blitter = util_blitter_create(&r300->context);
    if (r300->blitter == NULL)
        goto fail;

    /* Render functions must be initialized after blitter. */
    r300_init_render_functions(r300);

    rws->cs_set_flush(r300->cs, r300_flush_cb, r300);

    /* setup hyper-z mm */
    if (r300->rws->get_value(r300->rws, R300_CAN_HYPERZ))
        if (!r300_hyperz_init_mm(r300))
            goto fail;

    r300->upload_ib = u_upload_create(&r300->context,
				      32 * 1024, 16,
				      PIPE_BIND_INDEX_BUFFER);

    if (r300->upload_ib == NULL)
        goto fail;

    r300->upload_vb = u_upload_create(&r300->context,
				      128 * 1024, 16,
				      PIPE_BIND_VERTEX_BUFFER);
    if (r300->upload_vb == NULL)
        goto fail;

    r300->tran.translate_cache = translate_cache_create();
    if (r300->tran.translate_cache == NULL)
        goto fail;

    r300_init_states(&r300->context);

    /* The KIL opcode needs the first texture unit to be enabled
     * on r3xx-r4xx. In order to calm down the CS checker, we bind this
     * dummy texture there. */
    if (!r300->screen->caps.is_r500) {
        struct pipe_resource *tex;
        struct pipe_resource rtempl = {{0}};
        struct pipe_sampler_view vtempl = {{0}};

        rtempl.target = PIPE_TEXTURE_2D;
        rtempl.format = PIPE_FORMAT_I8_UNORM;
        rtempl.bind = PIPE_BIND_SAMPLER_VIEW;
        rtempl.width0 = 1;
        rtempl.height0 = 1;
        rtempl.depth0 = 1;
        tex = screen->resource_create(screen, &rtempl);

        u_sampler_view_default_template(&vtempl, tex, tex->format);

        r300->texkill_sampler = (struct r300_sampler_view*)
            r300->context.create_sampler_view(&r300->context, tex, &vtempl);

        pipe_resource_reference(&tex, NULL);
    }

    return &r300->context;

 fail:
    r300_destroy_context(&r300->context);
    return NULL;
}

void r300_finish(struct r300_context *r300)
{
    struct pipe_framebuffer_state *fb;
    unsigned i;

    /* This is a preliminary implementation of glFinish.
     *
     * The ideal implementation should use something like EmitIrqLocked and
     * WaitIrq, or better, real fences.
     */
    if (r300->fb_state.state) {
        fb = r300->fb_state.state;

        for (i = 0; i < fb->nr_cbufs; i++) {
            if (fb->cbufs[i]->texture) {
                r300->rws->buffer_wait(r300->rws,
                    r300_texture(fb->cbufs[i]->texture)->buffer);
                return;
            }
        }
        if (fb->zsbuf && fb->zsbuf->texture) {
            r300->rws->buffer_wait(r300->rws,
                r300_texture(fb->zsbuf->texture)->buffer);
        }
    }
}
