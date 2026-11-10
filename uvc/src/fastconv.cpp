#include "fastconv.h"

#include <string.h>

#include "layer/convolution.h"
#include "layer/convolutiondepthwise.h"

namespace {

using ncnn::Mat;
using ncnn::Option;

// The compiler orders a block's multiply-adds as it likes, and likes to put
// one sum's next to each other, which is the one thing the FPU cannot do
// fast. Passing a sum through an empty assembly statement pins its latest
// multiply-add before that statement; tools/maddgap.py checks the result.
#ifdef __mips__
#define KEEP(x) __asm__ volatile("" : "+f"(x))
#else
#define KEEP(x) (void)0
#endif

// Leaky ReLU (ReLU is slope 0) on 8 sums: the 8 products first, then the 8
// selects, so that no select waits for its own product.
#define ACT8(a, b, c, d, e, f, g, h)                                                               \
    do {                                                                                           \
        float ma = a * slope, mb = b * slope, mc = c * slope, md = d * slope;                      \
        float me = e * slope, mf = f * slope, mg = g * slope, mh = h * slope;                      \
        KEEP(ma); KEEP(mb); KEEP(mc); KEEP(md); KEEP(me); KEEP(mf); KEEP(mg); KEEP(mh);            \
        a = a > 0 ? a : ma, b = b > 0 ? b : mb, c = c > 0 ? c : mc, d = d > 0 ? d : md;            \
        e = e > 0 ? e : me, f = f > 0 ? f : mf, g = g > 0 ? g : mg, h = h > 0 ? h : mh;            \
    } while (0)

// 4 neighbouring output pixels of 4 output channels. in: the first input
// channel at the group's top left input pixel; wp: this channel block's
// weights as [input channel][tap][4 output channels].
template <int K, int S>
static inline void conv_group(const float *__restrict in, size_t cstep, int w, int inch,
                              const float *__restrict wp, const float *bias, float *__restrict o0,
                              float *__restrict o1, float *__restrict o2, float *__restrict o3,
                              int activate, float slope) {
    float a0 = bias[0], a1 = bias[1], a2 = bias[2], a3 = bias[3];
    float b0 = a0, b1 = a1, b2 = a2, b3 = a3;
    float c0 = a0, c1 = a1, c2 = a2, c3 = a3;
    float d0 = a0, d1 = a1, d2 = a2, d3 = a3;
    for (int c = 0; c < inch; c++) {
        const float *r = in + c * cstep;
        for (int ky = 0; ky < K; ky++, r += w) {
            for (int kx = 0; kx < K; kx++) {
                float p0 = r[kx], p1 = r[kx + S], p2 = r[kx + 2 * S], p3 = r[kx + 3 * S];
                float w0 = wp[0], w1 = wp[1], w2 = wp[2], w3 = wp[3];
                wp += 4;
                a0 += p0 * w0, a1 += p0 * w1, a2 += p0 * w2, a3 += p0 * w3;
                b0 += p1 * w0, b1 += p1 * w1, b2 += p1 * w2, b3 += p1 * w3;
                c0 += p2 * w0, c1 += p2 * w1, c2 += p2 * w2, c3 += p2 * w3;
                d0 += p3 * w0, d1 += p3 * w1, d2 += p3 * w2, d3 += p3 * w3;
            }
        }
    }
    if (activate) {
        ACT8(a0, a1, a2, a3, b0, b1, b2, b3);
        ACT8(c0, c1, c2, c3, d0, d1, d2, d3);
    }
    o0[0] = a0, o0[1] = b0, o0[2] = c0, o0[3] = d0;
    o1[0] = a1, o1[1] = b1, o1[2] = c1, o1[3] = d1;
    o2[0] = a2, o2[1] = b2, o2[2] = c2, o2[3] = d2;
    o3[0] = a3, o3[1] = b3, o3[2] = c3, o3[3] = d3;
}

// The whole layer: row by row, so that the input rows in use stay in the
// data cache while every block of output channels reads them.
template <int K, int S>
static void conv_run(const Mat &in, Mat &out, const float *packed, const float *bias, int activate,
                     float slope) {
    int w = in.w, inch = in.c, outw = out.w, outh = out.h, outch = out.c;
    size_t cstep = in.cstep, ocstep = out.cstep;
    const float *in0 = in;
    float *out0 = out;
    float spare[4]; // where a channel past the last one goes
    size_t block = (size_t)inch * K * K * 4;
    for (int y = 0; y < outh; y++) {
        const float *row = in0 + (size_t)y * S * w;
        for (int ob = 0; ob * 4 < outch; ob++) {
            float *o[4];
            for (int j = 0; j < 4; j++)
                o[j] = ob * 4 + j < outch ? out0 + (ob * 4 + j) * ocstep + (size_t)y * outw : NULL;
            for (int x = 0; x < outw; x += 4) {
                int gx = x + 4 > outw ? outw - 4 : x; // the last group overlaps the one before
                conv_group<K, S>(row + gx * S, cstep, w, inch, packed + ob * block, bias + ob * 4,
                                 o[0] + gx, o[1] ? o[1] + gx : spare, o[2] ? o[2] + gx : spare,
                                 o[3] ? o[3] + gx : spare, activate, slope);
            }
        }
    }
}

// 1x1 convolutions. Reading one pixel of every input channel touches as many
// cache lines as there are channels, and planes a multiple of the cache's
// way size apart evict each other, so a stretch of pixels is first copied
// to a small buffer as [group of 4 pixels][channel][4 pixels], which every
// block of output channels then reads front to back.
static inline void pw_group(const float *__restrict in, int inch, const float *__restrict wp,
                            const float *bias, float *__restrict o0, float *__restrict o1,
                            float *__restrict o2, float *__restrict o3, int activate, float slope) {
    float a0 = bias[0], a1 = bias[1], a2 = bias[2], a3 = bias[3];
    float b0 = a0, b1 = a1, b2 = a2, b3 = a3;
    float c0 = a0, c1 = a1, c2 = a2, c3 = a3;
    float d0 = a0, d1 = a1, d2 = a2, d3 = a3;
    for (int c = 0; c < inch; c++) {
        float p0 = in[0], p1 = in[1], p2 = in[2], p3 = in[3];
        float w0 = wp[0], w1 = wp[1], w2 = wp[2], w3 = wp[3];
        in += 4, wp += 4;
        a0 += p0 * w0, a1 += p0 * w1, a2 += p0 * w2, a3 += p0 * w3;
        b0 += p1 * w0, b1 += p1 * w1, b2 += p1 * w2, b3 += p1 * w3;
        c0 += p2 * w0, c1 += p2 * w1, c2 += p2 * w2, c3 += p2 * w3;
        d0 += p3 * w0, d1 += p3 * w1, d2 += p3 * w2, d3 += p3 * w3;
    }
    if (activate) {
        ACT8(a0, a1, a2, a3, b0, b1, b2, b3);
        ACT8(c0, c1, c2, c3, d0, d1, d2, d3);
    }
    o0[0] = a0, o0[1] = b0, o0[2] = c0, o0[3] = d0;
    o1[0] = a1, o1[1] = b1, o1[2] = c1, o1[3] = d1;
    o2[0] = a2, o2[1] = b2, o2[2] = c2, o2[3] = d2;
    o3[0] = a3, o3[1] = b3, o3[2] = c3, o3[3] = d3;
}

#define PW_BUFFER 2048 // floats: 8 KB of the 32 KB data cache

static int pw_run(const Mat &in, Mat &out, const float *packed, const float *bias, int activate,
                  float slope) {
    int inch = in.c, outch = out.c, n = in.w * in.h; // a channel's rows follow each other
    size_t cstep = in.cstep, ocstep = out.cstep;
    const float *in0 = in;
    float *out0 = out;
    float spare[4], buf[PW_BUFFER];
    int tile = PW_BUFFER / inch & ~3;
    tile = tile < 4 ? 4 : tile > 64 ? 64 : tile;
    Mat big; // more channels than the buffer has room for
    float *b = buf;
    if (tile * inch > PW_BUFFER) {
        big.create(tile * inch);
        if (big.empty())
            return -100;
        b = big;
    }
    for (int t = 0; t < n; t += tile) {
        // This stretch's groups of 4 pixels; where the pixels do not divide
        // by 4 the last group overlaps the one before (n is at least 4)
        int t0 = t, len = n - t < tile ? n - t : tile;
        if (len < 4)
            t0 = n - 4, len = 4;
        int groups = (len + 3) / 4;
        for (int c = 0; c < inch; c++) {
            const float *src = in0 + c * cstep + t0;
            float *dst = b + c * 4;
            for (int g = 0; g < groups; g++, dst += inch * 4) {
                int i = g * 4 > len - 4 ? len - 4 : g * 4;
                dst[0] = src[i], dst[1] = src[i + 1], dst[2] = src[i + 2], dst[3] = src[i + 3];
            }
        }
        for (int ob = 0; ob * 4 < outch; ob++) {
            float *o[4];
            for (int j = 0; j < 4; j++)
                o[j] = ob * 4 + j < outch ? out0 + (ob * 4 + j) * ocstep + t0 : NULL;
            const float *wp = packed + (size_t)ob * inch * 4;
            for (int g = 0; g < groups; g++) {
                int i = g * 4 > len - 4 ? len - 4 : g * 4;
                pw_group(b + (size_t)g * inch * 4, inch, wp, bias + ob * 4, o[0] + i, o[1] ? o[1] + i : spare,
                         o[2] ? o[2] + i : spare, o[3] ? o[3] + i : spare, activate, slope);
            }
        }
    }
    return 0;
}

// G neighbouring output pixels of one depthwise channel, a sum each, taking
// the kernel's taps in turn. rows: the K input rows, x: the group's first
// input column in them.
template <int K, int S, int G>
static inline void dw_group(const float *const *rows, int x, const float *__restrict k, float bias,
                            float *__restrict o, int activate, float slope) {
    float s[G];
    for (int j = 0; j < G; j++)
        s[j] = bias;
    for (int ky = 0; ky < K; ky++) {
        const float *__restrict r = rows[ky] + x;
        if (S == 1 && K == 3) {
            // Neighbouring pixels share their inputs: load a row's once
            float p[G + 2], w0 = k[ky * 3], w1 = k[ky * 3 + 1], w2 = k[ky * 3 + 2];
            for (int j = 0; j < G + 2; j++)
                p[j] = r[j];
            for (int j = 0; j < G; j++) {
                s[j] += p[j] * w0;
                KEEP(s[j]);
            }
            for (int j = 0; j < G; j++) {
                s[j] += p[j + 1] * w1;
                KEEP(s[j]);
            }
            for (int j = 0; j < G; j++) {
                s[j] += p[j + 2] * w2;
                KEEP(s[j]);
            }
            continue;
        }
        for (int kx = 0; kx < K; kx++) {
            float wv = k[ky * K + kx];
            for (int j = 0; j < G; j++) {
                s[j] += r[j * S + kx] * wv;
                KEEP(s[j]);
            }
        }
    }
    if (activate) {
        for (int j = 0; j < G; j += 4) {
            float m0 = s[j] * slope, m1 = s[j + 1] * slope, m2 = s[j + 2] * slope, m3 = s[j + 3] * slope;
            KEEP(m0); KEEP(m1); KEEP(m2); KEEP(m3);
            s[j] = s[j] > 0 ? s[j] : m0, s[j + 1] = s[j + 1] > 0 ? s[j + 1] : m1;
            s[j + 2] = s[j + 2] > 0 ? s[j + 2] : m2, s[j + 3] = s[j + 3] > 0 ? s[j + 3] : m3;
        }
    }
    for (int j = 0; j < G; j++)
        o[j] = s[j];
}

// The layer. Padding costs no copy of the input: the K rows in use are
// copied to row buffers with the border in place (and stay in the cache).
template <int K, int S>
static int dw_run(const Mat &in, Mat &out, const float *weights, const float *bias, int pad_left,
                  int pad_top, int activate, float slope, const Option &opt) {
    int w = in.w, h = in.h, outw = out.w, outh = out.h;
    int padded = w + (outw - 1) * S + K; // room for any right border
    Mat buf;
    int tag[K];
    buf.create(padded, K + 1, 4u, opt.workspace_allocator);
    if (buf.empty())
        return -100;
    buf.fill(0.f);
    const float *zero = buf.row(K);
    for (int g = 0; g < out.c; g++) {
        const float *plane = in.channel(g);
        float *o = out.channel(g);
        const float *k = weights + g * K * K;
        float b = bias ? bias[g] : 0;
        for (int i = 0; i < K; i++)
            tag[i] = -1;
        for (int y = 0; y < outh; y++, o += outw) {
            const float *rows[K];
            for (int ky = 0; ky < K; ky++) {
                int iy = y * S + ky - pad_top;
                if (iy < 0 || iy >= h) {
                    rows[ky] = zero;
                    continue;
                }
                int slot = iy % K;
                if (tag[slot] != iy) {
                    memcpy(buf.row(slot) + pad_left, plane + (size_t)iy * w, w * sizeof(float));
                    tag[slot] = iy;
                }
                rows[ky] = buf.row(slot);
            }
            int x = 0;
            // 8 sums at a time: the compiler keeps no more than that in
            // registers through the barriers
            for (; x + 8 <= outw; x += 8)
                dw_group<K, S, 8>(rows, x * S, k, b, o + x, activate, slope);
            if (x + 4 <= outw) {
                dw_group<K, S, 4>(rows, x * S, k, b, o + x, activate, slope);
                x += 4;
            }
            if (x < outw) // the last group overlaps the one before
                dw_group<K, S, 4>(rows, (outw - 4) * S, k, b, o + outw - 4, activate, slope);
        }
    }
    return 0;
}

class FastConvolution : public ncnn::Convolution {
public:
    virtual int create_pipeline(const Option &opt) {
        (void)opt;
        int k = kernel_w, taps = k * k;
        inch = num_output > 0 ? weight_data_size / (num_output * taps) : 0;
        fast = !dynamic_weight && !int8_scale_term && kernel_h == k && stride_h == stride_w &&
               dilation_w == 1 && dilation_h == 1 && activation_type <= 2 && inch > 0 &&
               ((k == 1 && stride_w == 1) || (k == 3 && stride_w <= 2));
        if (!fast)
            return 0;
        int blocks = (num_output + 3) / 4;
        packed.create(blocks * inch * taps * 4);
        bias4.create(blocks * 4);
        if (packed.empty() || bias4.empty())
            return -100;
        packed.fill(0.f);
        bias4.fill(0.f);
        const float *wd = weight_data;
        float *p = packed;
        for (int o = 0; o < num_output; o++)
            for (int c = 0; c < inch; c++)
                for (int t = 0; t < taps; t++)
                    p[(((size_t)(o / 4) * inch + c) * taps + t) * 4 + o % 4] = wd[((size_t)o * inch + c) * taps + t];
        if (bias_term)
            memcpy((float *)bias4, (const float *)bias_data, num_output * sizeof(float));
        return 0;
    }

    virtual int forward(const Mat &bottom, Mat &top, const Option &opt) const {
        if (!fast || bottom.dims != 3 || bottom.elempack != 1 || bottom.c != inch)
            return ncnn::Convolution::forward(bottom, top, opt);
        Mat in;
        make_padding(bottom, in, opt);
        if (in.empty())
            return -100;
        int outw = (in.w - kernel_w) / stride_w + 1, outh = (in.h - kernel_h) / stride_h + 1;
        if (kernel_w == 1 ? outw * outh < 4 : outw < 4)
            return ncnn::Convolution::forward(bottom, top, opt);
        top.create(outw, outh, num_output, 4u, opt.blob_allocator);
        if (top.empty())
            return -100;
        float slope = activation_type == 2 ? activation_params[0] : 0;
        if (kernel_w == 1)
            return pw_run(in, top, packed, bias4, activation_type, slope);
        if (stride_w == 1)
            conv_run<3, 1>(in, top, packed, bias4, activation_type, slope);
        else
            conv_run<3, 2>(in, top, packed, bias4, activation_type, slope);
        return 0;
    }

private:
    Mat packed, bias4;
    int inch;
    bool fast;
};

class FastDepthWise : public ncnn::ConvolutionDepthWise {
public:
    virtual int create_pipeline(const Option &opt) {
        (void)opt;
        int k = kernel_w;
        fast = !dynamic_weight && !int8_scale_term && kernel_h == k && stride_h == stride_w &&
               dilation_w == 1 && dilation_h == 1 && activation_type <= 2 && group == num_output &&
               weight_data_size == group * k * k && (k == 3 || k == 5) && stride_w <= 2 &&
               pad_left >= 0 && pad_right >= 0 && pad_top >= 0 && pad_bottom >= 0 && pad_value == 0;
        return 0;
    }

    virtual int forward(const Mat &bottom, Mat &top, const Option &opt) const {
        int outw = (bottom.w + pad_left + pad_right - kernel_w) / stride_w + 1;
        int outh = (bottom.h + pad_top + pad_bottom - kernel_h) / stride_h + 1;
        if (!fast || bottom.dims != 3 || bottom.elempack != 1 || bottom.c != group || outw < 4 || outh < 1)
            return ncnn::ConvolutionDepthWise::forward(bottom, top, opt);
        top.create(outw, outh, num_output, 4u, opt.blob_allocator);
        if (top.empty())
            return -100;
        float slope = activation_type == 2 ? activation_params[0] : 0;
        const float *b = bias_term ? (const float *)bias_data : NULL;
        if (kernel_w == 3 && stride_w == 1)
            return dw_run<3, 1>(bottom, top, weight_data, b, pad_left, pad_top, activation_type, slope, opt);
        if (kernel_w == 3)
            return dw_run<3, 2>(bottom, top, weight_data, b, pad_left, pad_top, activation_type, slope, opt);
        if (stride_w == 1)
            return dw_run<5, 1>(bottom, top, weight_data, b, pad_left, pad_top, activation_type, slope, opt);
        return dw_run<5, 2>(bottom, top, weight_data, b, pad_left, pad_top, activation_type, slope, opt);
    }

private:
    bool fast;
};

ncnn::Layer *create_conv(void *) { return new FastConvolution; }
ncnn::Layer *create_dw(void *) { return new FastDepthWise; }

} // namespace

void fastconv_register(ncnn::Net &net) {
    // Without SIMD there is nothing to gain from ncnn's 4-channel packing,
    // only conversions to and from it between layers
    net.opt.use_packing_layout = false;
    net.register_custom_layer("Convolution", create_conv);
    net.register_custom_layer("ConvolutionDepthWise", create_dw);
}
