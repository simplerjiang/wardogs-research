                                                                                  
                                                                                  
                                                                               
                                             
#include <cmath>
#include <string>
#include <vector>

#include "ci.h"
#include "common.h"
#include "draw_list.h"
#include "esp_projection.h"
#include "esp_scene.h"
#include "translate.h"

namespace {

constexpr u64 kVaBase = 0xFFFFF80000000000ull;
constexpr u64 kPml4Pa = 0x1000;
constexpr u64 kPdptPa = 0x2000;
constexpr u64 kPdPa = 0x3000;
constexpr u64 kPtPa = 0x4000;
constexpr u64 kImagePa = 0x200000;                                                 

struct Flat {
    std::vector<u8> mem;

    explicit Flat(size_t bytes) : mem(bytes, 0) {}

    u64* Table(u64 pa) { return reinterpret_cast<u64*>(mem.data() + pa); }

    void SetEntry(u64 pa, u64 index, u64 value) { Table(pa)[index] = value; }
};

}              

int RunSelfTest(Logger& log) {
    int failures = 0;
    auto check = [&](const char* name, bool ok, const std::string& detail = std::string()) {
        if (!ok) ++failures;
        log.Event(ok ? "selftest_step_ok" : "selftest_step_failed",
                  std::string("\"step\":") + Json(name) + (detail.empty() ? "" : ",\"detail\":" + Json(detail)));
    };

                                                                                
    Flat flat((size_t)(kImagePa + (2ull << 20)));
    const u64 idx4 = (kVaBase >> 39) & 0x1FF;
    const u64 idx3 = (kVaBase >> 30) & 0x1FF;
    const u64 idx2 = (kVaBase >> 21) & 0x1FF;
    flat.SetEntry(kPml4Pa, idx4, kPdptPa | 3);
    flat.SetEntry(kPdptPa, idx3, kPdPa | 3);
    flat.SetEntry(kPdPa, idx2, kImagePa | 0x83);                      

                                                                           
    for (u64 i = 0; i < 8; ++i) {
        u64 pa = 0x20000 + i * 0x1000;
        flat.SetEntry(kPml4Pa, 256 + 16 + i, pa | 3);
    }
                                                                          
    const u64 va4k = kVaBase + 0x200000 + 0x3000;
    flat.SetEntry(kPdPa, (va4k >> 21) & 0x1FF, kPtPa | 3);
    flat.SetEntry(kPtPa, (va4k >> 12) & 0x1FF, 0x50000 | 3);

    Translator t;
    std::string error;
    check("translator_init_memory", t.InitMemory(&flat.mem), error);
    t.SetCr3ForTest(kPml4Pa);

    check("pml4_heuristic_accepts_table", Translator::LooksLikePml4(flat.mem.data() + kPml4Pa, flat.mem.size()));
    u8 junk[0x1000];
    std::memset(junk, 0xAB, sizeof(junk));
    check("pml4_heuristic_rejects_junk", !Translator::LooksLikePml4(junk, flat.mem.size()));

    u64 pa_large = t.VaToPa(kVaBase + 0x12345);
    check("walk_2mb_page", pa_large == kImagePa + 0x12345, Hex(pa_large));
    u64 pa_4k = t.VaToPa(va4k + 0xAB);
    check("walk_4kb_page", pa_4k == 0x50000 + 0xAB, Hex(pa_4k));
    check("walk_unmapped_is_zero", t.VaToPa(kVaBase + (3ull << 21)) == 0);

    u8 page[0x1000];
    std::memcpy(flat.mem.data() + 0x50000, "hello-wardog", 12);
    bool read_ok = t.ReadVa(va4k, page, 12, &error) && std::memcmp(page, "hello-wardog", 12) == 0;
    check("read_va", read_ok, error);
    bool write_ok = t.WriteVa(va4k + 1, "E", 1, &error) &&
                    std::memcmp(flat.mem.data() + 0x50000, "hEllo-wardog", 12) == 0;
    check("write_va", write_ok, error);

                                                                               
    wchar_t win[MAX_PATH]{};
    GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring ci_path = std::wstring(win) + L"\\System32\\ci.dll";
    PeFile disk_ci;
    if (!disk_ci.Load(ci_path)) {
        log.Event("selftest_note", "\"detail\":\"ci.dll not readable; skipping image tests\"");
    } else {
        std::memcpy(flat.mem.data() + kImagePa, disk_ci.bytes.data(), disk_ci.bytes.size());

        CiImage image;
        bool parsed = ci::ReadImage(t, kVaBase, (u32)disk_ci.bytes.size(), &image, &error);
        check("ci_read_image", parsed, error);
        check("ci_timestamp_matches_disk", parsed && image.timestamp == disk_ci.timestamp);

                                                                             
                                                                                  
                                                           
        const PeSection* writable = nullptr;
        const PeSection* executable = nullptr;
        for (const auto& s : image.sections) {
            if (!writable && (s.characteristics & 0x80000000) && s.vsize > 0x200) writable = &s;
            if (!executable && (s.characteristics & 0x20000000) && s.vsize > 0x100) executable = &s;
        }
        check("ci_sections_identified", writable && executable);
        if (writable && executable) {
            const u32 options = 0x0000C001;
            const u32 target_rva = writable->va + 0x80;
            const u64 target_va = kVaBase + target_rva;
            ci::WriteDword(t, target_va, options, &error);

            const u64 patch_site = kVaBase + executable->va + 0x40;
            u8 mov[6] = {0x8B, 0x05, 0, 0, 0, 0};                         
            std::int32_t disp = (std::int32_t)(target_va - (patch_site + 6));
            std::memcpy(mov + 2, &disp, 4);
            t.WriteVa(patch_site, mov, 6, &error);

            u32 back = 0;
            check("ci_read_dword", ci::ReadDword(t, target_va, &back, &error) && back == options, error);

            std::vector<CiCandidate> found;
            bool ok = ci::FindValueCandidates(t, image, options, &found, &error);
            check("ci_find_candidates", ok && !found.empty(), error);
            if (ok && !found.empty()) {
                bool hit = false;
                for (const auto& c : found) {
                    if (c.rva == target_rva && c.xrefs >= 1) hit = true;
                }
                check("ci_candidate_ranked_with_xref", hit,
                      found.empty() ? "" : ("rva=" + Hex(found[0].rva, 8) + " xrefs=" + std::to_string(found[0].xrefs)));
            }
                            
            ci::WriteDword(t, target_va, options ^ 0xFFFFFFFFu, &error);
            u32 dirty = 0;
            ci::ReadDword(t, target_va, &dirty, &error);
            ci::WriteDword(t, target_va, options, &error);
            u32 fixed = 0;
            ci::ReadDword(t, target_va, &fixed, &error);
            check("ci_restore_roundtrip", dirty == (options ^ 0xFFFFFFFFu) && fixed == options);
        }
    }
    {
        Camera camera;
        camera.location = Vec3{0.0f, 0.0f, 100.0f};
        camera.pitch = 0.0f;
        camera.yaw = 0.0f;
        camera.roll = 0.0f;
        camera.fov = 90.0f;
        camera.width = 1920.0f;
        camera.height = 1080.0f;
        camera.fov_horizontal = true;

        const Vec3 feet{900.0f, 0.0f, 0.0f};
        const Vec3 head{900.0f, 0.0f, 180.0f};
        ProjectionStyle style;
        const ProjectionResult box = ProjectBox(camera, head, feet, style);
        const bool box_ok = box.status == BoxStatus::kTwoPoints &&
                            std::fabs(box.box.Width() - 88.32f) < 0.05f &&
                            std::fabs(box.box.Height() - 192.0f) < 0.05f &&
                            std::fabs(box.box.left - 915.84f) < 0.05f &&
                            std::fabs(box.box.top - 454.67f) < 0.05f;
        check("esp_project_reference_box", box_ok,
              "w=" + std::to_string(box.box.Width()) + " h=" + std::to_string(box.box.Height()) +
                  " left=" + std::to_string(box.box.left) + " top=" + std::to_string(box.box.top));

        const bool ratio_ok = std::fabs(box.box.Width() - 0.46f * box.box.Height()) < 0.02f;
        check("esp_width_ratio_0_46", ratio_ok);

        ProjectionStyle scaled = style;
        scaled.scale = 1.3f;
        const ProjectionResult box_130 = ProjectBox(camera, head, feet, scaled);
        const bool scale_ok = std::fabs(box_130.box.Width() - box.box.Width() * 1.3f) < 0.05f &&
                              std::fabs(box_130.box.Height() - box.box.Height() * 1.3f) < 0.05f;
        check("esp_scale_130", scale_ok, "w130=" + std::to_string(box_130.box.Width()));

        const Vec3 far_head{200000.0f, 0.0f, 180.0f};
        const Vec3 far_feet{200000.0f, 0.0f, 0.0f};
        const ProjectionResult far_box = ProjectBox(camera, far_head, far_feet, style);
        const bool clamp_ok = far_box.status != BoxStatus::kSkipped &&
                              std::fabs(far_box.box.Height() - 3.0f) < 0.001f &&
                              std::fabs(far_box.box.Width() - 2.0f) < 0.001f;
        check("esp_minimum_size_clamp", clamp_ok,
              "w=" + std::to_string(far_box.box.Width()) + " h=" + std::to_string(far_box.box.Height()));

        const Vec3 behind_head{-900.0f, 0.0f, 180.0f};
        const Vec3 behind_feet{-900.0f, 0.0f, 0.0f};
        check("esp_behind_camera_skipped", ProjectBox(camera, behind_head, behind_feet, style).status ==
                                               BoxStatus::kSkipped);

        std::vector<EspTarget> targets;
        EspTarget first;
        first.feet = feet;
        targets.push_back(first);
        EspTarget second;
        second.feet = Vec3{1400.0f, 350.0f, 0.0f};
        targets.push_back(second);

        DrawList list;
        SceneStats stats;
        EspSettings settings;
        BuildScene(camera, targets, settings, &list, &stats);
        check("esp_scene_two_boxes", stats.drawn == 2 && list.quads.size() == 8,
              "drawn=" + std::to_string(stats.drawn) + " quads=" + std::to_string(list.quads.size()));

        EspSettings corner_settings;
        corner_settings.corners = true;
        DrawList corner_list;
        SceneStats corner_stats;
        BuildScene(camera, targets, corner_settings, &corner_list, &corner_stats);
        check("esp_corner_brackets", corner_list.quads.size() == 16,
              "quads=" + std::to_string(corner_list.quads.size()));

        EspSettings limited;
        limited.max_boxes = 1;
        DrawList limited_list;
        SceneStats limited_stats;
        BuildScene(camera, targets, limited, &limited_list, &limited_stats);
        check("esp_box_budget", limited_stats.drawn == 1 && limited_stats.limited == 1);
    }

    log.Event(failures == 0 ? "selftest_complete_ok" : "selftest_complete_failed",
              "\"failures\":" + Logger::Num((u64)failures));
    return failures == 0 ? 0 : 20;
}

