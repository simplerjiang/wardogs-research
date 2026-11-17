#include "reference_projection.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using namespace reconstruction::reference;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool close(float a, float b, float epsilon = 0.003f) { return std::fabs(a-b) <= epsilon; }
std::array<Position,21> fixture() {
    return {{
        {500,0,85},{500,0,90},{500,0,105},{500,0,120},{500,0,135},
        {500,0,150},{500,0,170},{500,-18,135},{500,-35,120},{500,-45,105},
        {500,-50,100},{500,18,135},{500,35,120},{500,45,105},{500,50,100},
        {500,-10,85},{500,-10,45},{500,-10,5},{500,10,85},{500,10,45},{500,10,5}
    }};
}
void projection() {
    Camera camera;
    const Viewport wide{1920,1080};
    ScreenPoint point{-99,-99};
    require(project(camera,wide,{500,0,0},point) && close(point.x,960) && close(point.y,540),
            "Forward center projection failed");
    require(project(camera,wide,{500,100,50},point) && close(point.x,1152) && close(point.y,444),
            "Reference right/up axes differ");
    require(project(camera,wide,{500,1000,0},point) && point.x > wide.width,
            "49dc must not clip offscreen points");
    require(!project(camera,wide,{-500,0,0},point), "Behind-camera point accepted");
    require(!project(camera,wide,{0.99,0,0},point), "Below-near point accepted");
    require(project(camera,wide,{1.01,0,0},point), "Above-near point rejected");
    // At exact depth=1 the COMISD/JA boundary accepts; world X=1 is not exact
    // depth=1 because the reference's cos(0) polynomial is slightly below 1.
    camera.rotation_degrees = {0,90,0};
    require(project(camera,wide,{0,500,0},point) && close(point.x,960), "Yaw basis failed");
    camera.rotation_degrees = {0,0,90};
    require(project(camera,wide,{500,100,0},point) && point.y < 540 && close(point.x,960),
            "Roll basis failed");
    camera = {};
    require(project(camera,{1000,1000},{500,100,0},point) && close(point.x,677.7778f),
            "Nonwide default aspect correction missing");
    camera.aspect_hint = 1.4f;
    require(project(camera,{1000,1000},{500,100,0},point) && close(point.x,640),
            "Explicit differing aspect hint not used");
    camera.aspect_hint = 2.4f; // float 2.4 is above the strict upper bound.
    require(project(camera,{1000,1000},{500,100,0},point) && close(point.x,677.7778f),
            "Invalid hint must retain the 16:9 correction");
    camera.fov_degrees = 0;
    require(!project(camera,wide,{500,0,0},point), "Zero FOV accepted");
    camera = {};
    camera.valid = false;
    require(!project(camera,wide,{500,0,0},point), "Invalid snapshot accepted");
    camera.valid = true;
    require(!project(camera,{1.9f,1080},{500,0,0},point), "Small viewport accepted");
    require(!project(camera,wide,{1e10,0,0},point), "Scalar limit accepted");
    require(!project(camera,wide,{500,std::numeric_limits<double>::quiet_NaN(),0},point),
            "NaN world coordinate accepted");
    std::puts("PASS reference projection axes, FOV/aspect, no viewport clip, near domain");
}
void segments() {
    Camera camera;
    Viewport view{1920,1080};
    ScreenSegment line;
    require(!project_segment(camera,view,{500,0,0},{502.9,0,0},line), "World length below 3 accepted");
    require(project_segment(camera,view,{500,0,0},{503,0,0},line), "World length 3 rejected");
    require(project_segment(camera,view,{500,0,0},{596,0,0},line), "World length 96 rejected");
    require(!project_segment(camera,view,{500,0,0},{596.1,0,0},line), "World length above 96 accepted");
    require(!project_segment(camera,view,{50,0,0},{120,0,0},line), "Near-camera disparity accepted");
    require(project_segment(camera,view,{50,0,0},{110,0,0},line), "Disparity at 60 rejected");
    require(project_segment(camera,view,{250,0,0},{250,90,0},line), "Screen span below 360 rejected");
    require(!project_segment(camera,view,{240,0,0},{240,95,0},line), "Screen span above 360 accepted");
    std::puts("PASS world/screen segment and near-camera disparity filters");
}
void skeletons() {
    const Camera camera;
    const Viewport view{1920,1080};
    auto p = fixture();
    constexpr std::uint32_t all = 0x1fffff;
    require(project_skeleton(camera,view,p,all).size() == 19, "Complete ordinary skeleton differs");
    require(project_skeleton(camera,view,p,0).empty(), "Missing pose produced lines");
    // The c85c walk bridges missing joint 2: 1->3, not two broken edges.
    const auto missing = project_skeleton(camera,view,p,all & ~(1u<<2));
    require(missing.size() == 18, "Missing-joint bridge count differs");
    ScreenPoint first, third;
    require(project(camera,view,p[1],first) && project(camera,view,p[3],third), "Fixture projection failed");
    require(close(missing[0].from.y,first.y) && close(missing[0].to.y,third.y),
            "Missing joint did not connect the next valid neighbor");
    require(project_skeleton(camera,view,p,all,true).size() == 19, "Strict large-profile legs differ");
    require(project_skeleton(camera,view,p,all,false,true).size() == 19, "Forced alternate legs differ");
    require(project_skeleton(camera,view,p,all & ~((1u<<17)|(1u<<20))).size() == 19,
            "Missing foot extrapolation failed");
    const auto one_side = project_skeleton(camera,view,p,all & ~(7u<<18));
    require(one_side.size() == 18, "Single-leg mirror repair differs");
    // Long rejected edges still update c85c's previous index.
    p[2] = {500,300,105};
    const auto outlier = project_skeleton(camera,view,p,all,true);
    require(outlier.size() == 17, "Rejected chain edge did not advance previous index");
    // A rejected strict leg remains absent; no ordinary-leg repair in this branch.
    p = fixture();
    p[17] = {500,-10,500};
    require(project_skeleton(camera,view,p,all,true).size() == 16, "Strict rejected leg was repaired");
    std::puts("PASS body/arm chains, strict legs, missing foot, mirror, invalid-neighbor behavior");
}
}
int main() {
    try { projection(); segments(); skeletons(); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what()); return 1; }
}
