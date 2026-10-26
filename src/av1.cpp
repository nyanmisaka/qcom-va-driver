// SPDX-License-Identifier: MIT
#include "internal.hpp"

namespace irisva {
namespace {
// VA provides parsed picture parameters and raw tiles, but Iris expects OBUs.
// The reference map below bridges VA surfaces and the firmware's eight slots.
constexpr VAStatus invalid = VA_STATUS_ERROR_INVALID_BUFFER;
constexpr unsigned key_frame = 0, inter_frame = 1, intra_only = 2, switch_frame = 3;
unsigned log2_ceil(unsigned n) {
    unsigned bits = 0;
    for (unsigned v = n - 1; v; v >>= 1)
        ++bits;
    return bits;
}
unsigned tile_log2(unsigned block, unsigned target) {
    unsigned bits = 0;
    while ((block << bits) < target)
        ++bits;
    return bits;
}
class Bits {
    std::vector<uint8_t> bytes_;
    unsigned position_ = 0;

  public:
    void bit(unsigned value) {
        if (!(position_ & 7))
            bytes_.push_back(0);
        bytes_.back() |= (value & 1) << (7 - (position_++ & 7));
    }
    void bits(uint32_t value, unsigned count) {
        check(count <= 32 && (count == 32 || value < (uint64_t(1) << count)),
              "AV1 bit field out of range", invalid);
        for (unsigned i = count; i; --i)
            bit(value >> (i - 1));
    }
    void signed_bits(int value, unsigned count) {
        check(count && count < 32 && value >= -(1 << (count - 1)) && value < (1 << (count - 1)),
              "AV1 signed field out of range", invalid);
        bits(uint32_t(value) & ((1u << count) - 1), count);
    }
    void ns(unsigned value, unsigned count) {
        check(count && value < count, "AV1 non-symmetric field out of range", invalid);
        unsigned width = log2_ceil(count), cutoff = (1u << width) - count;
        if (!width)
            return;
        if (value < cutoff)
            bits(value, width - 1);
        else
            bits(value + cutoff, width);
    }
    void subexp(unsigned value, unsigned count) {
        unsigned base = 0;
        for (unsigned i = 0;; ++i) {
            unsigned width = i ? i + 2 : 3, step = 1u << width;
            if (count <= base + 3 * step) {
                ns(value - base, count - base);
                return;
            }
            bool more = value >= base + step;
            bit(more);
            if (!more) {
                bits(value - base, width);
                return;
            }
            base += step;
        }
    }
    void align() {
        while (position_ & 7)
            bit(0);
    }
    std::vector<uint8_t> finish(bool trailing = false) {
        if (trailing)
            bit(1);
        align();
        return std::move(bytes_);
    }
};
void obu(std::vector<uint8_t> &out, unsigned type, const std::vector<uint8_t> &payload) {
    out.push_back(uint8_t(type << 3 | 2)); // OBU size field, no extension.
    size_t length = payload.size();
    do {
        uint8_t byte = length & 0x7f;
        length >>= 7;
        out.push_back(byte | (length ? 0x80 : 0));
    } while (length);
    out.insert(out.end(), payload.begin(), payload.end());
}
std::vector<uint8_t> sequence_header(const VADecPictureParameterBufferAV1 &p, unsigned width,
                                     unsigned height) {
    const auto &s = p.seq_info_fields.fields;
    Bits b;
    b.bits(0, 3); // Main profile; use the full header even for still pictures.
    b.bit(s.still_picture);
    b.bit(0);
    b.bit(0);     // No timing or decoder model: VA supplies neither.
    b.bit(0);     // initial_display_delay_present_flag
    b.bits(0, 5); // One operating point.
    b.bits(0, 12);
    unsigned level = width * height <= 2359296 ? 9 : width * height <= 8912896 ? 13 : 17;
    b.bits(level, 5); // Main tier, with a level appropriate for the configured dimensions.
    b.bit(0);
    unsigned width_bits = log2_ceil(width), height_bits = log2_ceil(height);
    b.bits(width_bits - 1, 4);
    b.bits(height_bits - 1, 4);
    b.bits(width - 1, width_bits);
    b.bits(height - 1, height_bits);
    b.bit(0); // Frame IDs are replaced by our VA-surface-to-reference mapping.
    b.bit(s.use_128x128_superblock);
    b.bit(s.enable_filter_intra);
    b.bit(s.enable_intra_edge_filter);
    b.bit(s.enable_interintra_compound);
    b.bit(s.enable_masked_compound);
    b.bit(1); // enable_warped_motion; selected per picture
    b.bit(s.enable_dual_filter);
    b.bit(s.enable_order_hint);
    if (s.enable_order_hint) {
        b.bit(s.enable_jnt_comp);
        b.bit(1); // enable_ref_frame_mvs; selected per picture
    }
    b.bit(1); // Screen content and integer motion selected per picture.
    b.bit(1);
    if (s.enable_order_hint)
        b.bits(p.order_hint_bits_minus_1, 3);
    b.bit(1); // enable_superres
    b.bit(s.enable_cdef);
    b.bit(1); // enable_restoration
    b.bit(p.bit_depth_idx == 1);
    b.bit(0);     // mono_chrome
    b.bit(1);     // color_description_present_flag
    b.bits(2, 8); // Primaries/transfer are carried by VA's output metadata.
    b.bits(2, 8);
    b.bits(p.matrix_coefficients, 8);
    b.bit(s.color_range);
    b.bits(0, 2); // chroma_sample_position is not available in the VA ABI.
    b.bit(1);     // separate_uv_delta_q
    b.bit(0);     // Film grain synthesis is not supported.
    return b.finish(true);
}
unsigned frame_width(const VADecPictureParameterBufferAV1 &p) {
    unsigned width = unsigned(p.frame_width_minus1) + 1;
    return p.pic_info_fields.bits.use_superres
               ? (width * 8 + p.superres_scale_denominator / 2) / p.superres_scale_denominator
               : width;
}
void tile_layout(Bits &b, const VADecPictureParameterBufferAV1 &p) {
    unsigned sb_size = p.seq_info_fields.fields.use_128x128_superblock ? 128 : 64;
    unsigned cols = (frame_width(p) + sb_size - 1) / sb_size;
    unsigned rows = (unsigned(p.frame_height_minus1) + sb_size) / sb_size;
    unsigned max_width = 4096 / sb_size, max_area = (4096 * 2304) / (sb_size * sb_size);
    unsigned min_cols_log2 = tile_log2(max_width, cols);
    unsigned min_tiles_log2 = std::max(min_cols_log2, tile_log2(max_area, cols * rows));
    unsigned cols_log2 = log2_ceil(p.tile_cols), rows_log2 = log2_ceil(p.tile_rows);
    b.bit(p.pic_info_fields.bits.uniform_tile_spacing_flag);
    if (p.pic_info_fields.bits.uniform_tile_spacing_flag) {
        auto uniform = [&](unsigned blocks, unsigned count, unsigned minimum) {
            unsigned maximum = log2_ceil(std::min(blocks, 64u));
            for (unsigned n = minimum; n <= maximum; ++n) {
                unsigned span = (blocks + (1u << n) - 1) >> n;
                if ((blocks + span - 1) / span != count)
                    continue;
                for (unsigned i = minimum; i < n; ++i)
                    b.bit(1);
                if (n < maximum)
                    b.bit(0);
                return n;
            }
            throw Error(invalid, "invalid AV1 uniform tile layout");
        };
        cols_log2 = uniform(cols, p.tile_cols, min_cols_log2);
        rows_log2 =
            uniform(rows, p.tile_rows, min_tiles_log2 > cols_log2 ? min_tiles_log2 - cols_log2 : 0);
    } else {
        unsigned widest = 0, remaining = cols;
        for (unsigned i = 0; i < p.tile_cols; ++i) {
            unsigned span = i + 1 == p.tile_cols ? remaining : p.width_in_sbs_minus_1[i] + 1;
            check(span && span <= remaining, "invalid AV1 tile width", invalid);
            b.ns(span - 1, std::min(remaining, max_width));
            widest = std::max(widest, span);
            remaining -= span;
        }
        check(!remaining, "incomplete AV1 tile columns", invalid);
        max_area = cols * rows;
        if (min_tiles_log2)
            max_area >>= min_tiles_log2 + 1;
        unsigned max_height = std::max(max_area / widest, 1u);
        remaining = rows;
        for (unsigned i = 0; i < p.tile_rows; ++i) {
            unsigned span = i + 1 == p.tile_rows ? remaining : p.height_in_sbs_minus_1[i] + 1;
            check(span && span <= remaining, "invalid AV1 tile height", invalid);
            b.ns(span - 1, std::min(remaining, max_height));
            remaining -= span;
        }
        check(!remaining, "incomplete AV1 tile rows", invalid);
    }
    if (cols_log2 || rows_log2) {
        b.bits(p.context_update_tile_id, cols_log2 + rows_log2);
        b.bits(3, 2); // Four-byte tile lengths in the reconstructed tile group.
    }
}
void quantization(Bits &b, const VADecPictureParameterBufferAV1 &p) {
    auto delta = [&](int value) {
        b.bit(value != 0);
        if (value)
            b.signed_bits(value, 7);
    };
    b.bits(p.base_qindex, 8);
    delta(p.y_dc_delta_q);
    bool separate = p.u_dc_delta_q != p.v_dc_delta_q || p.u_ac_delta_q != p.v_ac_delta_q;
    b.bit(separate);
    delta(p.u_dc_delta_q);
    delta(p.u_ac_delta_q);
    if (separate) {
        delta(p.v_dc_delta_q);
        delta(p.v_ac_delta_q);
    }
    const auto &q = p.qmatrix_fields.bits;
    b.bit(q.using_qmatrix);
    if (q.using_qmatrix) {
        b.bits(q.qm_y, 4);
        b.bits(q.qm_u, 4);
        b.bits(q.qm_v, 4);
    }
}
void segmentation(Bits &b, const VADecPictureParameterBufferAV1 &p, unsigned primary) {
    const auto &s = p.seg_info;
    const auto &flags = s.segment_info_fields.bits;
    b.bit(flags.enabled);
    if (!flags.enabled)
        return;
    if (primary != 7) {
        b.bit(flags.update_map);
        if (flags.update_map)
            b.bit(flags.temporal_update);
        b.bit(1); // VA supplies the resolved feature table, including inherited values.
    }
    constexpr unsigned widths[] = {8, 6, 6, 6, 6, 3, 0, 0};
    for (unsigned segment = 0; segment < 8; ++segment)
        for (unsigned feature = 0; feature < 8; ++feature) {
            bool enabled = s.feature_mask[segment] & (1u << feature);
            b.bit(enabled);
            if (!enabled)
                continue;
            int value = s.feature_data[segment][feature];
            check(value >= (feature < 5 ? -int((1u << widths[feature]) - 1) : 0) &&
                      value <= int((1u << widths[feature]) - 1),
                  "invalid AV1 segmentation feature", invalid);
            if (feature < 5)
                b.signed_bits(value, widths[feature] + 1);
            else
                b.bits(value, widths[feature]);
        }
}
bool coded_lossless(const VADecPictureParameterBufferAV1 &p) {
    if (p.y_dc_delta_q || p.u_dc_delta_q || p.u_ac_delta_q || p.v_dc_delta_q || p.v_ac_delta_q)
        return false;
    for (unsigned i = 0; i < 8; ++i) {
        int qindex = p.base_qindex;
        if (p.seg_info.segment_info_fields.bits.enabled && (p.seg_info.feature_mask[i] & 1))
            qindex += p.seg_info.feature_data[i][0];
        if (std::clamp(qindex, 0, 255))
            return false;
    }
    return true;
}
void filters(Bits &b, const VADecPictureParameterBufferAV1 &p, bool lossless) {
    if (!lossless && !p.pic_info_fields.bits.allow_intrabc) {
        b.bits(p.filter_level[0], 6);
        b.bits(p.filter_level[1], 6);
        if (p.filter_level[0] || p.filter_level[1]) {
            b.bits(p.filter_level_u, 6);
            b.bits(p.filter_level_v, 6);
        }
        b.bits(p.loop_filter_info_fields.bits.sharpness_level, 3);
        b.bit(p.loop_filter_info_fields.bits.mode_ref_delta_enabled);
        if (p.loop_filter_info_fields.bits.mode_ref_delta_enabled) {
            b.bit(1);
            for (int delta : p.ref_deltas) {
                b.bit(1);
                b.signed_bits(delta, 7);
            }
            for (int delta : p.mode_deltas) {
                b.bit(1);
                b.signed_bits(delta, 7);
            }
        }
        if (p.seq_info_fields.fields.enable_cdef) {
            b.bits(p.cdef_damping_minus_3, 2);
            b.bits(p.cdef_bits, 2);
            for (unsigned i = 0; i < (1u << p.cdef_bits); ++i) {
                b.bits(p.cdef_y_strengths[i], 6);
                b.bits(p.cdef_uv_strengths[i], 6);
            }
        }
    }
    if (!(lossless && !p.pic_info_fields.bits.use_superres) &&
        !p.pic_info_fields.bits.allow_intrabc) {
        const auto &r = p.loop_restoration_fields.bits;
        constexpr unsigned syntax_type[] = {0, 2, 3, 1};
        b.bits(syntax_type[r.yframe_restoration_type], 2);
        b.bits(syntax_type[r.cbframe_restoration_type], 2);
        b.bits(syntax_type[r.crframe_restoration_type], 2);
        if (r.yframe_restoration_type || r.cbframe_restoration_type || r.crframe_restoration_type) {
            check(r.lr_unit_shift <= 2, "invalid AV1 restoration unit", invalid);
            if (p.seq_info_fields.fields.use_128x128_superblock) {
                check(r.lr_unit_shift >= 1, "invalid AV1 128x128 restoration unit", invalid);
                b.bit(r.lr_unit_shift - 1);
            } else {
                b.bit(r.lr_unit_shift != 0);
                if (r.lr_unit_shift)
                    b.bit(r.lr_unit_shift == 2);
            }
            if (r.cbframe_restoration_type || r.crframe_restoration_type)
                b.bit(r.lr_uv_shift);
        }
    }
}
std::array<unsigned, 7> reference_slots(const VADecPictureParameterBufferAV1 &p,
                                        const Av1State &state) {
    std::array<unsigned, 7> slots{};
    for (unsigned i = 0; i < slots.size(); ++i) {
        check(p.ref_frame_idx[i] < 8, "invalid AV1 reference index", invalid);
        VASurfaceID id = p.ref_frame_map[p.ref_frame_idx[i]];
        check(id != VA_INVALID_SURFACE, "missing AV1 reference", invalid);
        auto slot = std::find_if(state.references.begin(), state.references.end(),
                                 [&](const Av1Reference &r) { return r.surface == id; });
        check(slot != state.references.end(), "AV1 reference is outside the decoder DPB", invalid);
        slots[i] = slot - state.references.begin();
    }
    return slots;
}
bool skip_mode_allowed(const VADecPictureParameterBufferAV1 &p, const Av1State &state,
                       const std::array<unsigned, 7> &slots) {
    if (!p.seq_info_fields.fields.enable_order_hint || !p.mode_control_fields.bits.reference_select)
        return false;
    unsigned bits = p.order_hint_bits_minus_1 + 1;
    auto distance = [bits](int a, int b) {
        unsigned delta = unsigned(a - b), sign = 1u << (bits - 1);
        return int(delta & (sign - 1)) - int(delta & sign);
    };
    int forward = -1, backward = -1;
    for (unsigned slot : slots) {
        int hint = state.references[slot].params.order_hint;
        int delta = distance(hint, p.order_hint);
        if (delta < 0 && (forward < 0 || distance(hint, forward) > 0))
            forward = hint;
        if (delta > 0 && (backward < 0 || distance(hint, backward) < 0))
            backward = hint;
    }
    if (forward < 0)
        return false;
    if (backward >= 0)
        return true;
    return std::any_of(slots.begin(), slots.end(), [&](unsigned slot) {
        return distance(state.references[slot].params.order_hint, forward) < 0;
    });
}
unsigned recenter(unsigned reference, unsigned value) {
    if (value > 2 * reference)
        return value;
    return value >= reference ? 2 * (value - reference) : 2 * (reference - value) - 1;
}
void global_motion(Bits &b, const VADecPictureParameterBufferAV1 &p,
                   const VADecPictureParameterBufferAV1 *primary) {
    for (unsigned ref = 0; ref < 7; ++ref) {
        const auto &motion = p.wm[ref];
        unsigned type = motion.wmtype;
        check(type <= VAAV1TransformationAffine, "invalid AV1 global motion type", invalid);
        b.bit(type != VAAV1TransformationIdentity);
        if (!type)
            continue;
        b.bit(type == VAAV1TransformationRotzoom);
        if (type != VAAV1TransformationRotzoom)
            b.bit(type == VAAV1TransformationTranslation);
        auto parameter = [&](unsigned index) {
            unsigned absolute_bits = 12, precision = 15;
            if (index < 2) {
                bool low_precision = !p.pic_info_fields.bits.allow_high_precision_mv;
                absolute_bits = type == VAAV1TransformationTranslation ? 9 - low_precision : 12;
                precision = type == VAAV1TransformationTranslation ? 3 - low_precision : 6;
            }
            int unit = 1 << (16 - precision), identity = index % 3 == 2 ? 65536 : 0;
            int64_t value = (int64_t(motion.wmmat[index]) - identity) / unit;
            int previous = primary ? primary->wm[ref].wmmat[index] : identity;
            // The predictor is arithmetically shifted, including negative values.
            int predictor = (previous >> (16 - precision)) - identity / unit;
            int limit = 1 << absolute_bits;
            check(motion.wmmat[index] % unit == 0 && value >= -limit && value <= limit &&
                      predictor >= -limit && predictor <= limit,
                  "invalid AV1 global motion coefficient", invalid);
            unsigned count = 2 * limit + 1, r = predictor + limit, v = value + limit;
            unsigned centered =
                2 * r <= count ? recenter(r, v) : recenter(count - 1 - r, count - 1 - v);
            b.subexp(centered, count);
        };
        if (type >= VAAV1TransformationRotzoom) {
            parameter(2);
            parameter(3);
            if (type == VAAV1TransformationAffine) {
                parameter(4);
                parameter(5);
            }
        }
        parameter(0);
        parameter(1);
    }
}
std::vector<uint8_t> frame(const Av1Picture &picture, const Av1State &state, unsigned width,
                           unsigned height, unsigned refresh, bool show, bool reset) {
    const auto &p = picture.params;
    const auto &f = p.pic_info_fields.bits;
    const auto &s = p.seq_info_fields.fields;
    const auto &mode = p.mode_control_fields.bits;
    // Hidden keys must not acquire the implicit all-slot refresh of a shown key.
    unsigned type = f.frame_type == key_frame && !reset ? intra_only : f.frame_type;
    bool intra = type == key_frame || type == intra_only;
    bool resilient = (type == key_frame && show) || type == switch_frame || f.error_resilient_mode;
    unsigned primary = intra || resilient ? 7 : p.primary_ref_frame;
    auto slots = intra ? std::array<unsigned, 7>{} : reference_slots(p, state);
    Bits b;
    b.bit(0); // show_existing_frame is resolved by the VA client using its surface.
    b.bits(type, 2);
    b.bit(show);
    if (!show)
        b.bit(0); // Reference installation does not request a displayable picture.
    if (type != switch_frame && !(type == key_frame && show))
        b.bit(resilient);
    b.bit(f.disable_cdf_update);
    b.bit(f.allow_screen_content_tools);
    if (f.allow_screen_content_tools)
        b.bit(f.force_integer_mv);
    if (type != switch_frame)
        b.bit(1); // Always write explicit dimensions.
    if (s.enable_order_hint)
        b.bits(p.order_hint, p.order_hint_bits_minus_1 + 1);
    if (!intra && !resilient)
        b.bits(primary, 3);
    if (type != switch_frame && !(type == key_frame && show))
        b.bits(refresh, 8);
    if ((!intra || refresh != 0xff) && resilient && s.enable_order_hint)
        for (const auto &ref : state.references)
            b.bits(ref.params.order_hint, p.order_hint_bits_minus_1 + 1);
    if (!intra) {
        if (s.enable_order_hint)
            b.bit(0); // Explicit reference mapping, no short signaling.
        for (unsigned slot : slots)
            b.bits(slot, 3);
        if (!resilient)
            for (unsigned i = 0; i < 7; ++i)
                b.bit(0); // Do not inherit dimensions from a reference.
    }
    b.bits(p.frame_width_minus1, log2_ceil(width));
    b.bits(p.frame_height_minus1, log2_ceil(height));
    b.bit(f.use_superres);
    if (f.use_superres)
        b.bits(p.superres_scale_denominator - 9, 3);
    b.bit(0); // Render size matches the reconstructed VA surface.
    if (intra) {
        if (f.allow_screen_content_tools && !f.use_superres)
            b.bit(f.allow_intrabc);
    } else {
        if (!f.force_integer_mv)
            b.bit(f.allow_high_precision_mv);
        b.bit(p.interp_filter == 4);
        if (p.interp_filter != 4)
            b.bits(p.interp_filter, 2);
        b.bit(f.is_motion_mode_switchable);
        if (!resilient && s.enable_order_hint)
            b.bit(f.use_ref_frame_mvs);
    }
    if (!f.disable_cdf_update)
        b.bit(f.disable_frame_end_update_cdf);
    tile_layout(b, p);
    quantization(b, p);
    segmentation(b, p, primary);
    if (p.base_qindex)
        b.bit(mode.delta_q_present_flag);
    if (mode.delta_q_present_flag) {
        b.bits(mode.log2_delta_q_res, 2);
        if (!f.allow_intrabc)
            b.bit(mode.delta_lf_present_flag);
        if (mode.delta_lf_present_flag) {
            b.bits(mode.log2_delta_lf_res, 2);
            b.bit(mode.delta_lf_multi);
        }
    }
    bool lossless = coded_lossless(p);
    filters(b, p, lossless);
    if (!lossless)
        b.bit(mode.tx_mode == 2);
    if (!intra)
        b.bit(mode.reference_select);
    bool skip_allowed = !intra && skip_mode_allowed(p, state, slots);
    check(skip_allowed || !mode.skip_mode_present, "invalid AV1 skip mode references", invalid);
    if (skip_allowed)
        b.bit(mode.skip_mode_present);
    if (!intra && !resilient)
        b.bit(f.allow_warped_motion);
    b.bit(mode.reduced_tx_set_used);
    if (!intra)
        global_motion(b, p, primary == 7 ? nullptr : &state.references[slots[primary]].params);
    b.align();
    if (p.tile_cols * p.tile_rows > 1) {
        b.bit(0); // One tile group containing every tile.
        b.align();
    }
    auto payload = b.finish();
    std::vector<const Av1Slice *> tiles(p.tile_cols * p.tile_rows);
    for (const auto &slice : picture.slices) {
        unsigned row = slice.params.tile_row, column = slice.params.tile_column;
        check(row < p.tile_rows && column < p.tile_cols && !tiles[row * p.tile_cols + column],
              "invalid or duplicate AV1 tile", invalid);
        tiles[row * p.tile_cols + column] = &slice;
    }
    for (unsigned i = 0; i < tiles.size(); ++i) {
        check(tiles[i] && !tiles[i]->bytes.empty() && tiles[i]->bytes.size() <= UINT32_MAX,
              "missing AV1 tile data", invalid);
        if (i + 1 < tiles.size()) {
            uint32_t length = tiles[i]->bytes.size() - 1;
            for (unsigned byte = 0; byte < 4; ++byte)
                payload.push_back(uint8_t(length >> (byte * 8)));
        }
        payload.insert(payload.end(), tiles[i]->bytes.begin(), tiles[i]->bytes.end());
    }
    return payload;
}
bool referenced(const VADecPictureParameterBufferAV1 &p, VASurfaceID id) {
    return id != VA_INVALID_SURFACE &&
           std::find(std::begin(p.ref_frame_map), std::end(p.ref_frame_map), id) !=
               std::end(p.ref_frame_map);
}
int free_slot(const Av1State &state, const VADecPictureParameterBufferAV1 &p) {
    for (unsigned i = 0; i < state.references.size(); ++i)
        if (!referenced(p, state.references[i].surface))
            return i;
    return -1;
}
void remember(Av1State &state, const Av1Picture &picture, unsigned refresh) {
    if (!refresh)
        return;
    bool assigned = false;
    for (unsigned i = 0; i < state.references.size(); ++i) {
        auto &ref = state.references[i];
        if (ref.surface == picture.params.current_frame)
            ref.surface = VA_INVALID_SURFACE;
        if (refresh & (1u << i)) {
            ref.params = picture.params;
            // Keep a single canonical slot when the key frame refreshes all eight.
            ref.surface = assigned ? VA_INVALID_SURFACE : picture.params.current_frame;
            assigned = true;
        }
    }
}
} // namespace

std::vector<uint8_t> av1_bitstream(const Av1Picture &picture, Av1State &state, unsigned width,
                                   unsigned height) {
    check(picture.has_params && !picture.slices.empty() && picture.pending_slices.empty(),
          "AV1 picture missing parameters or tiles", invalid);
    const auto &p = picture.params;
    const auto &f = p.pic_info_fields.bits;
    const auto &s = p.seq_info_fields.fields;
    check(p.profile == 0 && p.bit_depth_idx <= 1 && !s.mono_chrome && s.subsampling_x &&
              s.subsampling_y,
          "AV1 requires Main 8/10-bit 4:2:0", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    check(!f.large_scale_tile && !p.anchor_frames_num, "AV1 large scale tiles unsupported",
          VA_STATUS_ERROR_UNIMPLEMENTED);
    check(!p.film_grain_info.film_grain_info_fields.bits.apply_grain,
          "AV1 film grain synthesis unsupported", VA_STATUS_ERROR_UNIMPLEMENTED);
    check(p.current_frame != VA_INVALID_SURFACE && p.frame_width_minus1 < width &&
              p.frame_height_minus1 < height && width >= 128 && height >= 128 && width <= 8192 &&
              height <= 8192 && width * height <= 35651584 && (p.frame_width_minus1 & 1) &&
              (p.frame_height_minus1 & 1),
          "invalid AV1 picture dimensions", invalid);
    check(p.tile_cols && p.tile_cols <= 64 && p.tile_rows && p.tile_rows <= 64 &&
              picture.slices.size() == size_t(p.tile_cols) * p.tile_rows &&
              picture.slices.size() <= 512 &&
              p.context_update_tile_id < p.tile_cols * p.tile_rows && p.primary_ref_frame <= 7 &&
              (!s.enable_order_hint || p.order_hint_bits_minus_1 <= 7) && p.cdef_bits <= 3,
          "invalid AV1 picture fields", invalid);
    check(!f.use_superres ||
              (p.superres_scale_denominator >= 9 && p.superres_scale_denominator <= 16),
          "invalid AV1 super-resolution denominator", invalid);
    bool intra = f.frame_type == key_frame || f.frame_type == intra_only;
    check((!f.allow_intrabc || (intra && f.allow_screen_content_tools && !f.use_superres)) &&
              (!f.force_integer_mv || intra || f.allow_screen_content_tools) &&
              (!p.mode_control_fields.bits.delta_q_present_flag || p.base_qindex) &&
              (!p.mode_control_fields.bits.delta_lf_present_flag ||
               (p.mode_control_fields.bits.delta_q_present_flag && !f.allow_intrabc)),
          "inconsistent AV1 coding tools", invalid);
    auto sequence = sequence_header(p, width, height);
    bool new_sequence = state.sequence != sequence;
    check(!new_sequence || f.frame_type == key_frame, "AV1 sequence must start with a key frame",
          invalid);
    if (new_sequence) {
        state = Av1State{};
        state.sequence = sequence;
    }
    bool reset = f.frame_type == switch_frame ||
                 (f.frame_type == key_frame && (f.show_frame || new_sequence));
    std::vector<uint8_t> out;
    obu(out, 2, {}); // One complete temporal unit per V4L2 OUTPUT buffer.
    if (new_sequence)
        obu(out, 1, sequence);

    // VA omits refresh_frame_flags. When all eight references are still live,
    // decode without replacing any of them. The next VA reference map tells us
    // which slot can be retired. Replay only that uncommitted picture as NOSHOW
    // before decoding the new picture, retaining its entropy and motion state.
    if (state.pending_reference && !reset &&
        referenced(p, state.pending_reference->params.current_frame)) {
        int slot = free_slot(state, p);
        check(slot >= 0, "AV1 reference map exceeds eight frames", invalid);
        unsigned refresh = 1u << slot;
        obu(out, 6, frame(*state.pending_reference, state, width, height, refresh, false, false));
        remember(state, *state.pending_reference, refresh);
        trace("AV1 install reference surface=%u slot=%d",
              state.pending_reference->params.current_frame, slot);
    }
    state.pending_reference.reset();
    int slot = free_slot(state, p);
    unsigned refresh = reset ? 0xff : slot >= 0 ? 1u << slot : 0;
    obu(out, 6, frame(picture, state, width, height, refresh, true, reset));
    remember(state, picture, refresh);
    if (!refresh)
        state.pending_reference = std::make_shared<Av1Picture>(picture);
    trace("AV1 frame surface=%u type=%u original-show=%u refresh=%02x tiles=%ux%u bytes=%zu",
          p.current_frame, f.frame_type, f.show_frame, refresh, p.tile_cols, p.tile_rows,
          out.size());
    return out;
}
} // namespace irisva
