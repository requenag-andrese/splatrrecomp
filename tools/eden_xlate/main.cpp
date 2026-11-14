// sr-recomp, translator study (branch translator-study): eden-xlate, a drop-in for sr-xlate (tools/sr_xlate/Program.cs)
// built on the shader recompiler of PokeDinner, an Eden fork (https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork,
// GPL-3.0-or-later; cloned shallow to ../vendor/pokedinner, never copied here). NVN2 (Ampere, sm_86) code is lowered to
// Maxwell instructions (src/shader_recompiler/frontend/ampere/lower_to_maxwell.h) and translated by yuzu's Maxwell
// frontend and SPIR-V back end, as Eden's own video_core does (shader_environment.cpp LowerNativeCode,
// vk_pipeline_cache.cpp CreateGraphicsPipeline).
//
// Same protocol and outputs as sr-xlate, so runtime/core/gfx_shader.c uses it unchanged (SR_XLATE=<this exe>):
//   eden-xlate --serve              one request per stdin line {"job": {...}, "out": "<dir>"}; one reply line per request,
//                                   "ok <key>" or "fail <key> <message>".
//   eden-xlate <jobs.json> <outdir> batch mode: a JSON array of jobs; prints a summary.
//   Output: <out>/<key>.spv and <out>/<key>.res.json in sr-xlate's layout (bindings in set 0 from 1, binding 0 the
//   support buffer the runtime declares and these shaders never read; constant buffers by slot; storage buffers by the
//   constant buffer word that holds their address; textures and images by their c2 handle word).
//
// What it does not translate goes to sr-xlate (a "sr-xlate --serve" child, from EDEN_XLATE_FALLBACK or beside this exe
// or one folder up), so a run always has every shader: compute jobs (NVN2 compute reads its c[0] in the layout
// Ryujinx's Ampere frontend remaps, nvn2_cs, which yuzu's does not), jobs asking for the DLSS motion output
// ("mvprev", translator patch 0015, Ryujinx only), and any program the lowering or the translation rejects.
// EDEN_XLATE_FALLBACK=0: no child; those jobs fail. EDEN_XLATE_LOG=1: a stderr line per job.
// The runtime's Ryujinx-specific fix-up of gradient samples must be off for these modules: SR_TXD_TRANSPOSE=0.
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/glsl/emit_glsl.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Am = Shader::Ampere;

// ---- a small JSON reader (the jobs the runtime writes: objects, arrays, strings, numbers, booleans)
struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;
    const Json* get(const std::string& k) const {
        for (auto& [key, v] : o)
            if (key == k) return &v;
        return nullptr;
    }
    std::string str(const std::string& k, const std::string& d = {}) const {
        const Json* v = get(k);
        return v && v->kind == Str ? v->s : d;
    }
    double num(const std::string& k, double d = 0) const {
        const Json* v = get(k);
        return v && v->kind == Num ? v->n : d;
    }
    bool flag(const std::string& k) const {
        const Json* v = get(k);
        return v && v->kind == Bool && v->b;
    }
};

struct JsonParser {
    const char *p, *e;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    bool lit(const char* w) {
        size_t n = std::strlen(w);
        if ((size_t)(e - p) < n || std::memcmp(p, w, n)) return false;
        p += n;
        return true;
    }
    std::string string() {
        std::string out;
        if (p >= e || *p != '"') throw std::runtime_error("JSON: string expected");
        for (p++; p < e && *p != '"'; p++) {
            if (*p != '\\') { out += *p; continue; }
            if (++p >= e) break;
            switch (*p) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (e - p < 5) throw std::runtime_error("JSON: bad escape");
                unsigned c = std::stoul(std::string(p + 1, p + 5), nullptr, 16);
                p += 4;
                if (c < 0x80) out += (char)c;
                else if (c < 0x800) out += (char)(0xc0 | c >> 6), out += (char)(0x80 | (c & 0x3f));
                else out += (char)(0xe0 | c >> 12), out += (char)(0x80 | ((c >> 6) & 0x3f)), out += (char)(0x80 | (c & 0x3f));
                break;
            }
            default: out += *p;
            }
        }
        if (p >= e) throw std::runtime_error("JSON: unterminated string");
        p++;
        return out;
    }
    Json value() {
        ws();
        Json v;
        if (p >= e) throw std::runtime_error("JSON: value expected");
        if (*p == '{') {
            v.kind = Json::Obj;
            p++, ws();
            if (p < e && *p == '}') return p++, v;
            for (;;) {
                ws();
                std::string k = string();
                ws();
                if (p >= e || *p++ != ':') throw std::runtime_error("JSON: ':' expected");
                v.o.emplace_back(std::move(k), value());
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; return v; }
                throw std::runtime_error("JSON: ',' or '}' expected");
            }
        }
        if (*p == '[') {
            v.kind = Json::Arr;
            p++, ws();
            if (p < e && *p == ']') return p++, v;
            for (;;) {
                v.a.push_back(value());
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; return v; }
                throw std::runtime_error("JSON: ',' or ']' expected");
            }
        }
        if (*p == '"') return v.kind = Json::Str, v.s = string(), v;
        if (lit("true")) return v.kind = Json::Bool, v.b = true, v;
        if (lit("false")) return v.kind = Json::Bool, v;
        if (lit("null")) return v;
        char* end;
        v.n = std::strtod(p, &end);
        if (end == p) throw std::runtime_error("JSON: unexpected character");
        p = end, v.kind = Json::Num;
        return v;
    }
};

static Json parse_json(const std::string& text) {
    JsonParser jp{text.data(), text.data() + text.size()};
    return jp.value();
}

static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

// ---- the answers of a job, as sr-xlate reads them
// c2 handle tables (sr-xlate Accessor.TexSlot; runtime/core/gfx_shader.c handle_slot): combined textures from 0x20
// (32 slots), images from 0x120 (9 slots), separate textures from 0x168; 8 bytes per slot.
static int tex_slot(u32 cbuf, u32 offset, bool* image) {
    *image = false;
    if (cbuf != 2 || offset % 8) return -1;
    if (offset >= 0x168 && offset < 0x568) return (int)(offset - 0x168) / 8;
    if (offset >= 0x120 && offset < 0x168) return *image = true, (int)(offset - 0x120) / 8;
    if (offset >= 0x20 && offset < 0x120) return (int)(offset - 0x20) / 8;
    return -1;
}

static Shader::TextureType tex_type(std::string t) {
    if (t.size() > 6 && t.compare(t.size() - 6, 6, "_depth") == 0) t.resize(t.size() - 6);
    if (t == "1d") return Shader::TextureType::Color1D;
    if (t == "1darray") return Shader::TextureType::ColorArray1D;
    if (t == "2darray" || t == "2dmsarray") return Shader::TextureType::ColorArray2D;
    if (t == "3d") return Shader::TextureType::Color3D;
    if (t == "cube") return Shader::TextureType::ColorCube;
    if (t == "cubearray") return Shader::TextureType::ColorArrayCube;
    if (t == "buffer") return Shader::TextureType::Buffer;
    return Shader::TextureType::Color2D;
}

static Shader::AttributeType attr_type(const std::string& s) {
    if (s == "sint") return Shader::AttributeType::SignedInt;
    if (s == "uint") return Shader::AttributeType::UnsignedInt;
    return Shader::AttributeType::Float;
}

class JobEnv final : public Shader::Environment {
public:
    std::vector<u64> code;
    std::vector<u8> c1;
    const Json* tex = nullptr;
    const Json* img = nullptr;
    u32 texture_bound = 2, shared = 0, local = 0;
    std::array<u32, 3> wg{1, 1, 1};
    std::set<int> tex_queried, img_queried;
    std::vector<std::string> notes;

    JobEnv(Shader::Stage st, const Shader::ProgramHeader& header, u32 start) {
        stage = st, sph = header, start_address = start, is_proprietary_driver = true;
    }
    u64 ReadInstruction(u32 address) override {
        if (address / 8 >= code.size()) throw Shader::LogicError("read past the code at {:#x}", address);
        return code[address / 8];
    }
    u32 TranslateProgramAddress(u32 address) const override {
        if (auto t = Am::NativeProgramAddress(code, address)) return *t;
        throw Shader::RuntimeError("invalid native indirect branch address {:#x}", address);
    }
    // c1 is the program's own constants (folded at translation, as Eden does for native programs). Any other word read
    // here is a texture handle's (the texture pass asks for the handle to ask for its type): it reads as a token naming
    // the word, so ReadTextureType knows the slot.
    u32 ReadCbufValue(u32 index, u32 offset) override {
        if (index == 1) {
            u32 v = 0;
            if (offset + 4 <= c1.size()) std::memcpy(&v, c1.data() + offset, 4);
            else notes.push_back("c1 read past its end at +" + std::to_string(offset));
            return v;
        }
        tokens.push_back({index, offset});
        return (u32)tokens.size();
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        int slot;
        bool image;
        std::string t = answer(handle, &slot, &image, "type");
        return t.empty() ? Shader::TextureType::Color2D : tex_type(t);
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        int slot;
        bool image;
        std::string f = answer(handle, &slot, &image, "fmt");
        // the formats a typed image load needs; others read as RGBA8 (Eden then loads typeless)
        if (f == "R32Uint") return Shader::TexturePixelFormat::R32_UINT;
        if (f == "R32Sint") return Shader::TexturePixelFormat::R32_SINT;
        if (f == "R32Float") return Shader::TexturePixelFormat::R32_FLOAT;
        if (f == "R16G16B16A16Float") return Shader::TexturePixelFormat::R16G16B16A16_FLOAT;
        if (f == "R32G32B32A32Float") return Shader::TexturePixelFormat::R32G32B32A32_FLOAT;
        if (f == "R32G32B32A32Uint") return Shader::TexturePixelFormat::R32G32B32A32_UINT;
        if (f == "R11G11B10Float") return Shader::TexturePixelFormat::B10G11R11_FLOAT;
        if (f == "R10G10B10A2Unorm") return Shader::TexturePixelFormat::A2B10G10R10_UNORM;
        if (f == "R8Unorm") return Shader::TexturePixelFormat::R8_UNORM;
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        int slot;
        bool image;
        std::string f = answer(handle, &slot, &image, "fmt");
        return f.find("Uint") != std::string::npos || f.find("Sint") != std::string::npos;
    }
    u32 ReadViewportTransformState() override { return 1; }   // on: no render-area push constant (PositionPass)
    u32 TextureBoundBuffer() const override { return texture_bound; }
    u32 LocalMemorySize() const override { return local; }
    u32 SharedMemorySize() const override { return shared; }
    std::array<u32, 3> WorkgroupSize() const override { return wg; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}

private:
    std::vector<std::pair<u32, u32>> tokens;   // token n names tokens[n - 1] = {constant buffer, byte offset}

    std::string answer(u32 handle, int* slot, bool* image, const char* field) {
        *slot = -1, *image = false;
        if (handle == 0 || handle > tokens.size()) {
            notes.push_back("texture handle not from one constant buffer word: answered 2D");
            return {};
        }
        auto [cb, off] = tokens[handle - 1];
        *slot = tex_slot(cb, off, image);
        if (*slot < 0) {
            notes.push_back("texture handle at c" + std::to_string(cb) + "+" + std::to_string(off) + ": no slot, answered 2D");
            return {};
        }
        (*image ? img_queried : tex_queried).insert(*slot);
        const Json* first = *image ? img : tex;
        const Json* second = *image ? tex : img;
        for (const Json* t : {first, second}) {
            const Json* v = t ? t->get(std::to_string(*slot)) : nullptr;
            if (!v) continue;
            if (v->kind == Json::Str) return std::string(field) == "type" ? v->s : std::string{};
            if (v->kind == Json::Obj) return v->str(field);
        }
        return {};
    }
};

static Shader::Profile make_profile() {
    Shader::Profile p{};
    p.supported_spirv = 0x00010600;
    p.unified_descriptor_binding = true;
    p.support_descriptor_aliasing = true;
    p.support_int8 = p.support_uniform_and_storage_buffer_8bit = p.support_storage_buffer_8bit = true;
    p.support_int16 = p.support_uniform_and_storage_buffer_16bit = p.support_storage_buffer_16bit = true;
    p.support_int64 = true;
    p.support_float_controls = true;
    p.support_separate_denorm_behavior = p.support_separate_rounding_mode = true;
    p.support_fp16_denorm_preserve = p.support_fp32_denorm_preserve = true;
    p.support_fp16_denorm_flush = p.support_fp32_denorm_flush = true;
    p.support_fp16_signed_zero_nan_preserve = p.support_fp32_signed_zero_nan_preserve = true;
    p.support_fp64_signed_zero_nan_preserve = true;
    p.support_explicit_workgroup_layout = true;
    p.support_workgroup_layout_8bit_access = p.support_workgroup_layout_16bit_access = true;
    p.support_quad_shuffles = p.support_vote = p.support_shuffle_relative = true;
    p.supported_subgroup_stages = 0x3f;
    p.support_viewport_index_layer_non_geometry = true;
    p.support_typeless_image_loads = true;
    p.support_demote_to_helper_invocation = true;
    p.support_int64_atomics = p.support_shared_int64_atomics = true;
    p.support_derivative_control = true;
    p.support_native_ndc = true;   // depth clip control (the runtime enables it, as sr-xlate assumes)
    p.support_scaled_attributes = true;
    p.support_multi_viewport = true;
    p.support_sampled_image_array_nonuniform_indexing = true;
    p.support_storage_image_array_nonuniform_indexing = true;
    p.has_broken_fp16_float_controls = true;   // NVIDIA's driver, as Eden sets it
    p.max_subgroup_size = 32;
    p.min_ssbo_alignment = 16;
    p.max_user_clip_distances = 8;
    return p;
}

static Shader::HostTranslateInfo make_host(u32 ssbo_align) {
    Shader::HostTranslateInfo h{};
    h.min_ssbo_alignment = ssbo_align;
    h.max_per_stage_descriptor_sampled_images = 1u << 20;
    h.max_per_stage_resources = 1u << 20;
    h.max_descriptor_set_samplers = h.max_descriptor_set_uniform_buffers = 1u << 20;
    h.max_descriptor_set_uniform_buffers_dynamic = 15;
    h.max_descriptor_set_storage_buffers = 1u << 20;
    h.max_descriptor_set_storage_buffers_dynamic = 16;
    h.max_descriptor_set_sampled_images = h.max_descriptor_set_storage_images = 1u << 20;
    h.max_descriptor_set_input_attachements = 1u << 20;
    h.support_float64 = h.support_float16 = h.support_int64 = true;
    h.support_snorm_render_buffer = true;
    h.support_viewport_index_layer = true;
    h.support_conditional_barrier = true;
    h.ApplyDescriptorLimitPolicy();
    return h;
}

static bool read_file(const std::string& path, std::vector<u8>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

static bool write_file(const std::string& path, const void* data, size_t n) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f || !f.write((const char*)data, (std::streamsize)n)) return false;
    }
    return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// Ryujinx SamplerType, as the runtime reads "type": 1 1D, 2 buffer, 3 2D, 4 3D, 5 cube | 0x100 array, 0x200
// multisample, 0x400 shadow.
static int ryu_type(Shader::TextureType t, bool depth, bool ms) {
    int v = 3;
    switch (t) {
    case Shader::TextureType::Color1D: v = 1; break;
    case Shader::TextureType::ColorArray1D: v = 1 | 0x100; break;
    case Shader::TextureType::Color2D: case Shader::TextureType::Color2DRect: v = 3; break;
    case Shader::TextureType::ColorArray2D: v = 3 | 0x100; break;
    case Shader::TextureType::Color3D: v = 4; break;
    case Shader::TextureType::ColorCube: v = 5; break;
    case Shader::TextureType::ColorArrayCube: v = 5 | 0x100; break;
    case Shader::TextureType::Buffer: v = 2; break;
    }
    return v | (ms ? 0x200 : 0) | (depth ? 0x400 : 0);
}

// Translates one job into <out>/<key>.spv and .res.json. Empty on success, else why not.
static std::string translate(const Json& job, const std::string& out) {
    const std::string key = job.str("key"), stage_name = job.str("stage");
    if (stage_name == "cs") return "compute: left to sr-xlate";
    if (job.flag("mvprev")) return "motion output (patch 0015): left to sr-xlate";
    if (job.str("arch") != "sm86") return "not NVN2 (sm86) code";
    // EDEN_XLATE_STAGES=vs|ps|vs,ps and EDEN_XLATE_KEYS=<hex digits>: only those stages, only keys starting with one of
    // those digits (the rest to sr-xlate), to find which translations change a picture by halving
    static const char* stages = std::getenv("EDEN_XLATE_STAGES");
    static const char* keys = std::getenv("EDEN_XLATE_KEYS");
    if (stages && !std::strstr(stages, stage_name.c_str())) return "filtered (EDEN_XLATE_STAGES)";
    if (keys && *keys && (key.empty() || !std::strchr(keys, std::tolower((unsigned char)key[0]))))
        return "filtered (EDEN_XLATE_KEYS)";
    std::vector<u8> code;
    if (!read_file(job.str("code"), code)) return "cannot read the code file";
    if (code.size() < 0x90) return "code too short";
    const bool fragment = stage_name == "ps";
    // the native instructions after the 0x80-byte header, up to where the capture finds the program's end
    std::vector<Am::Instruction> native;
    Am::ProgramCaptureState cap;
    for (size_t at = 0x80, i = 0; at + 16 <= code.size() && i < Am::ProgramCaptureState::MaximumInstructions; at += 16, i++) {
        Am::Instruction in;
        std::memcpy(in.words.data(), &code[at], 16);
        native.push_back(in);
        auto r = cap.Observe(in, (u32)i);
        if (r == Am::ProgramCaptureState::Result::InvalidTarget) return "capture: a branch leaves the program";
        if (r == Am::ProgramCaptureState::Result::Complete) break;
    }
    Am::LoweredProgram lowered = Am::LowerToMaxwell(native, "");
    if (!lowered) return "lowering: " + std::string(lowered.error);

    Shader::ProgramHeader sph{};
    std::memcpy(&sph, code.data(), sizeof sph);
    JobEnv env(fragment ? Shader::Stage::Fragment : Shader::Stage::VertexB, sph, 0);
    std::string c1hex = job.str("c1");
    for (size_t i = 0; i + 1 < c1hex.size(); i += 2) env.c1.push_back((u8)std::stoul(c1hex.substr(i, 2), nullptr, 16));
    env.tex = job.get("tex"), env.img = job.get("img");
    if (lowered.required_texture_buffer != ~0U) env.texture_bound = lowered.required_texture_buffer;
    env.local = (u32)sph.LocalMemorySize() + sph.common3.shader_local_memory_crs_size;
    // the lowered code as Eden lays it out: the header, a zero scheduling word before every group of three instructions,
    // a zero, then the native address map (indirect branches)
    auto& lc = env.code;
    lc.resize(sizeof sph / 8);
    std::memcpy(lc.data(), &sph, sizeof sph);
    for (const u64 inst : lowered.instructions) {
        if ((lc.size() * 8) % 32 == 0) lc.push_back(0);
        lc.push_back(inst);
    }
    lc.push_back(0);
    Am::AppendNativeAddressMap(lc, lowered.source_instruction_offsets);

    static const Shader::Profile profile = make_profile();
    const u32 ssbo_align = job.get("sbuf_align") ? (u32)job.num("sbuf_align") : 4;
    const Shader::HostTranslateInfo host = make_host(ssbo_align);
    Shader::ObjectPool<Shader::IR::Inst> inst_pool{8192};
    Shader::ObjectPool<Shader::IR::Block> block_pool{32};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool{32};
    Shader::Maxwell::Flow::CFG cfg(env, flow_pool, (u32)sizeof(Shader::ProgramHeader), !fragment);
    Shader::IR::Program program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
    const Shader::Info& info = program.info;

    // what the runtime cannot bind: a handle computed per draw, a constant buffer array, separate sampler handles
    for (auto& t : info.texture_descriptors) {
        if (t.handle_program != ~0U) return "a texture handle computed per draw";
        if (t.has_secondary) return "separate texture and sampler handles";
    }
    for (auto& c : info.constant_buffer_descriptors)
        if (c.count != 1) return "a constant buffer array";
    // The lowering reads NVN2's bank 24 (the push constants, which the runtime serves as c[24]) from Maxwell's bank 17,
    // which this game never reads (no c17 in the 122,218 reflections of a played cache); res.json names it 24 again.
    // Where the reads look like a blit's, PokeDinner's BlitConstantsPass replaces them by the render area, a push
    // constant block the runtime does not provide: such programs are left to sr-xlate.
    if (info.uses_render_area || info.uses_rescaling_uniform) return "render area or rescaling push constants";

    Shader::RuntimeInfo rt{};
    rt.previous_stage_stores.mask.set();
    if (const Json* a = job.get("attr"))
        for (auto& [k, v] : a->o)
            if (int i = std::stoi(k); i >= 0 && i < 32 && v.kind == Json::Str) rt.generic_input_types[i] = attr_type(v.s);
    if (const Json* f = job.get("fout"))
        for (auto& [k, v] : f->o)
            if (int i = std::stoi(k); i >= 0 && i < 8 && v.kind == Json::Str) rt.color_output_types[i] = attr_type(v.s);
    rt.force_early_z = job.flag("early_fragment_tests");
    Shader::Maxwell::ConvertLegacyToGeneric(program, rt);
    Shader::Backend::Bindings bindings{};
    bindings.unified = 1;   // binding 0 is the runtime's support buffer
    if (std::getenv("EDEN_XLATE_GLSL")) {   // diagnostics: the same program as GLSL text too (<key>.glsl)
        Shader::Backend::Bindings gb{};
        gb.unified = 1;
        std::string glsl = Shader::Backend::GLSL::EmitGLSL(profile, rt, program, gb);
        write_file(out + "/" + key + ".glsl", glsl.data(), glsl.size());
    }
    std::vector<u32> spv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, program, bindings);
    // the runtime's pipeline layouts have no push constants: a module declaring any goes to sr-xlate
    for (size_t i = 5; i < spv.size();) {
        const u32 len = spv[i] >> 16, op = spv[i] & 0xffff;
        if (len == 0) return "malformed SPIR-V";
        if (op == 59 && len >= 4 && spv[i + 3] == 9) return "push constants in the module";   // OpVariable PushConstant
        i += len;
    }

    // res.json, the bindings in the order the SPIR-V back end gives them (spirv_emit_context.cpp: constant buffers,
    // storage buffers, texture buffers, image buffers, textures, images; one binding each, arrays as descriptor counts)
    std::ostringstream r;
    u32 b = 1;
    r << "{\n  \"key\": \"" << esc(key) << "\",\n  \"stage\": \"" << stage_name << "\",\n  \"translator\": \"eden\",\n";
    r << "  \"notes\": [";
    for (size_t i = 0; i < env.notes.size() && i < 16; i++) r << (i ? ", " : "") << '"' << esc(env.notes[i]) << '"';
    r << "],\n  \"cbufs\": [";
    for (size_t i = 0; i < info.constant_buffer_descriptors.size(); i++)
        r << (i ? ", " : "") << "{\"binding\": " << b++ << ", \"slot\": "
          << (info.constant_buffer_descriptors[i].index == 17 ? 24 : info.constant_buffer_descriptors[i].index) << "}";
    r << "],\n  \"sbufs\": [";
    for (size_t i = 0; i < info.storage_buffers_descriptors.size(); i++) {
        auto& s = info.storage_buffers_descriptors[i];
        r << (i ? ", " : "") << "{\"binding\": " << b << ", \"slot\": -1, \"cb_slot\": " << s.cbuf_index
          << ", \"cb_offset\": " << s.cbuf_offset / 4 << ", \"flags\": " << (s.is_written ? 1 : 0) << "}";
        b += s.count;
    }
    r << "],\n  \"textures\": [";
    bool first = true;
    auto handle = [](u32 cbuf, u32 offset, u32 count, int type, int flags, const char* fmt) {
        std::ostringstream o;
        o << "{\"binding\": %B, \"type\": " << type << ", \"format\": \"" << fmt << "\", \"cbuf_slot\": " << cbuf
          << ", \"handle\": " << offset / 4 << ", \"array\": " << count << ", \"separate\": false, \"flags\": " << flags << "}";
        return o.str();
    };
    auto emit = [&](std::ostringstream& o, std::string s, bool& f) {
        s.replace(s.find("%B"), 2, std::to_string(b++));
        o << (f ? "" : ", ") << s;
        f = false;
    };
    std::vector<std::string> images;
    for (auto& t : info.texture_buffer_descriptors) emit(r, handle(t.cbuf_index, t.cbuf_offset, t.count, 2, 0, ""), first);
    std::ostringstream im;
    bool ifirst = true;
    for (auto& t : info.image_buffer_descriptors)
        emit(im, handle(t.cbuf_index, t.cbuf_offset, t.count, 2, t.is_written ? 4 : 0, ""), ifirst);
    for (auto& t : info.texture_descriptors)
        emit(r, handle(t.cbuf_index, t.cbuf_offset, t.count, ryu_type(t.type, t.is_depth, t.is_multisample), 0, ""), first);
    for (auto& t : info.image_descriptors)
        emit(im, handle(t.cbuf_index, t.cbuf_offset, t.count, ryu_type(t.type, false, false), t.is_written ? 4 : 0, ""), ifirst);
    r << "],\n  \"images\": [" << im.str() << "],\n";
    r << "  \"uses_frag_coord\": false,\n  \"frag_out_map\": 0,\n  \"tex_queried\": [";
    first = true;
    for (int s : env.tex_queried) r << (first ? "" : ", ") << s, first = false;
    r << "],\n  \"img_queried\": [";
    first = true;
    for (int s : env.img_queried) r << (first ? "" : ", ") << s, first = false;
    r << "]\n}\n";

    if (!write_file(out + "/" + key + ".spv", spv.data(), spv.size() * 4)) return "cannot write the SPIR-V";
    std::string res = r.str();
    if (!write_file(out + "/" + key + ".res.json", res.data(), res.size())) return "cannot write the reflection";
    return {};
}

// ---- sr-xlate as a child, for what is left to it
struct Fallback {
    HANDLE in_w = nullptr, out_r = nullptr;
    PROCESS_INFORMATION pi{};
    bool tried = false, alive = false;
    std::string buf;

    static std::string find_exe() {
        if (const char* e = std::getenv("EDEN_XLATE_FALLBACK")) return e;
        char self[MAX_PATH];
        GetModuleFileNameA(nullptr, self, MAX_PATH);
        std::string dir = self;
        dir.resize(dir.find_last_of("\\/"));
        for (int up = 0; up < 2; up++) {
            std::string p = dir + "\\sr-xlate.exe";
            if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
            size_t s = dir.find_last_of("\\/");
            if (s == std::string::npos) break;
            dir.resize(s);
        }
        return {};
    }
    bool start() {
        if (tried) return alive;
        tried = true;
        std::string exe = find_exe();
        if (exe.empty() || exe == "0") return false;
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE in_r, out_w;
        if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)) return false;
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA si{sizeof si};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_r, si.hStdOutput = out_w, si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        std::string cmd = "\"" + exe + "\" --serve";
        alive = CreateProcessA(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                               nullptr, nullptr, &si, &pi);
        CloseHandle(in_r), CloseHandle(out_w);
        if (alive) {
            // the child dies with this process: the runtime stops its translators with TerminateProcess
            HANDLE job = CreateJobObjectA(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li), AssignProcessToJobObject(job, pi.hProcess);
            ResumeThread(pi.hThread);
        }
        if (!alive) std::fprintf(stderr, "eden-xlate: cannot start %s\n", exe.c_str());
        return alive;
    }
    // the request line forwarded, its reply line returned (empty when the child is gone)
    std::string ask(const std::string& line) {
        if (!start()) return {};
        std::string l = line + "\n";
        DWORD n;
        if (!WriteFile(in_w, l.data(), (DWORD)l.size(), &n, nullptr)) return alive = false, std::string{};
        for (;;) {
            size_t nl = buf.find('\n');
            if (nl != std::string::npos) {
                std::string r = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                while (!r.empty() && r.back() == '\r') r.pop_back();
                return r;
            }
            char tmp[4096];
            if (!ReadFile(out_r, tmp, sizeof tmp, &n, nullptr) || n == 0) return alive = false, std::string{};
            buf.append(tmp, n);
        }
    }
};

static std::string one_line(std::string s) {
    for (auto& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s;
}

int main(int argc, char** argv) {
    const bool log = std::getenv("EDEN_XLATE_LOG") != nullptr;
    const char* fb_env = std::getenv("EDEN_XLATE_FALLBACK");
    const bool use_fallback = !(fb_env && std::strcmp(fb_env, "0") == 0);
    Fallback fb;
    if (argc == 2 && std::strcmp(argv[1], "--serve") == 0) {
        std::string line;
        char chunk[65536];
        std::string pending;
        auto handle_line = [&](const std::string& l) {
            std::string key = "?", err;
            try {
                Json req = parse_json(l);
                const Json* job = req.get("job");
                if (!job) throw std::runtime_error("no job");
                key = job->str("key", "?");
                std::string out = req.str("out");
                CreateDirectoryA(out.c_str(), nullptr);
                auto t0 = std::chrono::steady_clock::now();
                try {
                    err = translate(*job, out);
                } catch (const std::exception& e) {
                    err = std::string("translate: ") + e.what();
                }
                if (log) {
                    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    std::fprintf(stderr, "eden-xlate: %s %s %.1f ms%s%s\n", key.c_str(), job->str("stage").c_str(), ms,
                                 err.empty() ? "" : ": ", one_line(err).c_str());
                }
            } catch (const std::exception& e) {
                err = std::string("bad request: ") + e.what();
            }
            if (err.empty()) return std::string("ok ") + key;
            if (use_fallback && key != "?") {
                std::string r = fb.ask(l);
                if (!r.empty()) return r;
            }
            return "fail " + key + " " + one_line(err);
        };
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), chunk, sizeof chunk, &n, nullptr) || n == 0) break;
            pending.append(chunk, n);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                while (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                std::string reply = handle_line(line) + "\n";
                DWORD w;
                WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), reply.data(), (DWORD)reply.size(), &w, nullptr);
            }
        }
        return 0;
    }
    if (argc != 3) {
        std::fprintf(stderr, "eden-xlate --serve | eden-xlate <jobs.json> <outdir>\n");
        return 2;
    }
    std::vector<u8> text;
    if (!read_file(argv[1], text)) return 2;
    Json jobs = parse_json(std::string(text.begin(), text.end()));
    CreateDirectoryA(argv[2], nullptr);
    int ok = 0, bad = 0;
    std::map<std::string, int> why;
    auto t0 = std::chrono::steady_clock::now();
    for (const Json& job : jobs.a) {
        std::string err;
        try {
            err = translate(job, argv[2]);
        } catch (const std::exception& e) {
            err = std::string("translate: ") + e.what();
        }
        if (err.empty()) ok++;
        else bad++, why[err.substr(0, 90)]++;
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("eden-xlate: %d translated, %d not (%.2f s)\n", ok, bad, s);
    for (auto& [k, n] : why) std::printf("  %5d  %s\n", n, k.c_str());
    return bad ? 1 : 0;
}
