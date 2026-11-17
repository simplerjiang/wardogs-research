#include "reference_projection.h"

#include <cmath>

namespace reconstruction::reference {
namespace {
constexpr double radians = 0.017453292519943295;

// 066c polynomial and nearest-pi reduction, with constants read from the sample.
double sine(double angle) {
    const double turns = std::floor(angle * 0.3183098861837907 + 0.5);
    double reduced = angle - turns * 3.141592653589793;
    if (std::fmod(turns, 2.0) != 0) reduced = -reduced;
    const double square = reduced * reduced;
    double polynomial = 2.7557319223985893e-6 - square * 2.5052108385441717e-8;
    polynomial = polynomial * square - 0.0001984126984126984;
    polynomial = polynomial * square + 0.008333333333333333;
    polynomial = polynomial * square - 0.16666666666666666;
    polynomial = polynomial * square + 1;
    return polynomial * reduced;
}
double cosine(double angle) { return sine(angle + 1.5707963267948966); }
bool finite_point(const Position& p) { return scalar(p[0]) && scalar(p[1]) && scalar(p[2]); }
double distance(const Position& a, const Position& b) {
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return distance_root((y * y + x * x) + z * z);
}
double horizontal(const Position& a, const Position& b) {
    const double x = a[0] - b[0], y = a[1] - b[1];
    return distance_root(y * y + x * x);
}
bool present(std::uint32_t mask, int index) {
    return index >= 0 && index <= 20 && ((mask >> index) & 1) != 0;
}

struct Skeleton {
    const Camera& camera;
    Viewport viewport;
    const std::array<Position, 21>& points;
    std::uint32_t mask;
    std::vector<ScreenSegment> lines;

    void draw(const Position& a, const Position& b) {
        ScreenSegment line;
        if (project_segment(camera, viewport, a, b, line)) lines.push_back(line);
    }
    void chain(std::initializer_list<int> indices, double maximum) {
        int previous = -1;
        for (int index : indices) {
            if (!present(mask, index)) continue; // c85c does not break the chain.
            if (previous >= 0 && distance(points[previous], points[index]) <= maximum)
                draw(points[previous], points[index]);
            previous = index; // Even a rejected edge advances the chain.
        }
    }
};

// 1e70 / 9d08 classify the branch; they do not assign ownership to source points.
bool bent_pair(const Skeleton& s, int a, int b) {
    if (!present(s.mask, 1) || !present(s.mask, a) || !present(s.mask, b)) return false;
    const auto& pelvis = s.points[1];
    const auto& endpoint = s.points[b];
    const double dx = endpoint[0] - pelvis[0], dy = endpoint[1] - pelvis[1];
    const double planar_squared = dy * dy + dx * dx;
    const double length = distance(s.points[a], endpoint);
    return finite_point(pelvis) && finite_point(endpoint) &&
           distance(pelvis, s.points[a]) <= 36 &&
           length >= 15 && length <= 80 &&
           planar_squared >= 400 && planar_squared <= 8100 &&
           endpoint[2] - pelvis[2] >= -30 && endpoint[2] - pelvis[2] <= 35;
}
bool alternate_profile(const Skeleton& s, bool flag20) {
    return flag20 || ((s.mask & 0x42) == 0x42 &&
           s.points[6][2] - s.points[1][2] >= 40 &&
           bent_pair(s, 15, 16) && bent_pair(s, 18, 19));
}

// 5378 gates strict-profile leg chains.
bool strict_leg(const Skeleton& s, int a, int b, int c, bool large, bool alternate) {
    const std::uint32_t required = 0x42u | (1u << a) | (1u << b) | (1u << c);
    if ((s.mask & required) != required) return false;
    for (int i : {1, 6, a, b, c}) if (!finite_point(s.points[i])) return false;
    const auto& pelvis = s.points[1];
    const auto& foot = s.points[c];
    const double first = distance(s.points[a], s.points[b]);
    const double second = distance(s.points[b], foot);
    if (distance(pelvis, s.points[a]) > 36 ||
        first < 6 || first > 80 || second < 6 || second > 80) return false;
    const double total = distance(pelvis, foot);
    if (large) return total <= 170;
    const double planar = horizontal(foot, pelvis), dz = foot[2] - pelvis[2];
    if (alternate) return dz >= -130 && dz <= 35 && planar <= 145 &&
                          total >= 15 && total <= 150;
    if (s.points[6][2] - pelvis[2] < 40)
        return planar >= 28 && planar <= 130 && dz >= -35 && dz <= 20 && total >= 28;
    return dz <= -40 && planar <= 60 && total >= 45 && total <= 140;
}

// ffa8 extends the last segment; these points are synthetic display repair.
Position extend_foot(const Position& start, const Position& end) {
    Position delta{end[0]-start[0], end[1]-start[1], end[2]-start[2]};
    double length = distance(start, end);
    if (length < 6) return {end[0], end[1], end[2] - 44};
    for (auto& n : delta) n /= length;
    if (delta[2] > -0.2) {
        delta[2] += -0.9;
        length = distance_root((delta[1]*delta[1] + delta[0]*delta[0]) + delta[2]*delta[2]);
        if (length > 0.0001) for (auto& n : delta) n /= length;
    }
    return {end[0]+delta[0]*46, end[1]+delta[1]*46, end[2]+delta[2]*46};
}
struct Leg {
    int attempted = 0; // d0f4 counts accepted world pairs, not submitted pixels.
    Position anchor{1e12,0,0}, knee{1e12,0,0}, foot{1e12,0,0};
};
Leg repaired_leg(Skeleton& s, int anchor, int hip, int knee, int foot) {
    bool a = present(s.mask, anchor), b = present(s.mask, hip);
    bool c = present(s.mask, knee), d = present(s.mask, foot);
    Position pa = a ? s.points[anchor] : Position{};
    Position pb = b ? s.points[hip] : Position{};
    Position pc = c ? s.points[knee] : Position{};
    Position pd = d ? s.points[foot] : Position{};
    if (a && b && (distance(pa,pb) > 72 || distance(pa,pb) < 2)) b = false;
    else if (b && c && distance(pb,pc) > 78) c = false;
    if (a && d && distance(pa,pd) > 115) d = false;

    if (c && d && distance(pc,pd) < 8) d = false;
    if (c && !d) {
        if (b) { pd = extend_foot(pb,pc); d = true; }
        else if (a) { pd = extend_foot(pa,pc); d = true; }
    }
    if (a && d) {
        const double dz = pd[2] - pa[2], planar = horizontal(pd,pa);
        bool acceptable;
        if (!present(s.mask, 6) || s.points[6][2] - pa[2] >= 40)
            acceptable = dz <= -35 && planar <= 70;
        else acceptable = planar >= 20 && planar <= 130 && dz <= 25;
        if (!acceptable) b = c = d = false;
    }

    Leg out;
    auto attempt = [&](const Position& x, const Position& y) {
        if (distance(x,y) < 3) return false;
        s.draw(x,y);
        ++out.attempted;
        return true;
    };
    if (a && b) attempt(pa,pb);
    if (!(b && c && attempt(pb,pc)) && a && c) attempt(pa,pc);
    if (!(c && d && attempt(pc,pd)) && b && d) attempt(pb,pd);
    if (b) out.anchor = pb;
    else if (a) out.anchor = pa;
    if (c) out.knee = pc;
    if (d) out.foot = pd;
    return out;
}

// 115b8: reflection in a supplied plane; short normal falls back to XY inversion.
Position mirror(const Position& center, const Position& normal, const Position& input) {
    const Position delta{input[0]-center[0],input[1]-center[1],input[2]-center[2]};
    const double length = distance_root((normal[0]*normal[0] + normal[1]*normal[1]) + normal[2]*normal[2]);
    if (length < 8) return {center[0]-delta[0],center[1]-delta[1],input[2]};
    const Position unit{normal[0]/length,normal[1]/length,normal[2]/length};
    const double dot = ((unit[1]*delta[1] + unit[0]*delta[0]) + unit[2]*delta[2]) * 2;
    return {input[0]-dot*unit[0],input[1]-dot*unit[1],input[2]-dot*unit[2]};
}
} // namespace

bool project(const Camera& camera, Viewport viewport, const Position& point, ScreenPoint& out) {
    if (!camera.valid || viewport.width < 2 || viewport.height < 2 || !finite_point(point))
        return false;
    const auto& r = camera.rotation_degrees;
    const double sp = sine(r[0]*radians), cp = cosine(r[0]*radians);
    const double sy = sine(r[1]*radians), cy = cosine(r[1]*radians);
    const double sr = sine(r[2]*radians), cr = cosine(r[2]*radians);
    const Position forward{cy*cp,sy*cp,sp};
    const Position right{(sr*sp)*cy-cr*sy,(sr*sp)*sy+cr*cy,-sr*cp};
    const Position up{-((cr*sp)*cy+sr*sy),sr*cy-(cr*sp)*sy,cr*cp};
    const Position delta{point[0]-camera.location[0],point[1]-camera.location[1],point[2]-camera.location[2]};
    auto dot = [&](const Position& axis) {
        return (delta[1]*axis[1] + delta[0]*axis[0]) + delta[2]*axis[2];
    };
    const double depth = dot(forward);
    if (depth < 1) return false; // Near plane is accepted at exactly 1.
    const double half_fov = (double(camera.fov_degrees)*0.5)*radians;
    const double sn = sine(half_fov), cs = cosine(half_fov);
    if (!(sn > 0.0001) || !(cs > 0)) return false;
    const double width = viewport.width, height = viewport.height, aspect = width/height;
    double focal = (cs/sn)*(width*0.5);
    if (aspect < 1.7 || aspect > 1.85) {
        double correction = 16.0/9.0;
        const double hint = camera.aspect_hint;
        if (hint > 1.3 && hint < 2.4 &&
            (hint > aspect ? hint/aspect : aspect/hint) > 1.02) correction = hint;
        focal *= correction/aspect;
    }
    out.x = float((dot(right)/depth)*focal + width*0.5);
    out.y = float(height*0.5 - (dot(up)/depth)*focal);
    return scalar(double(out.x)) && scalar(double(out.y));
}

bool project_segment(const Camera& camera, Viewport viewport, const Position& a,
                     const Position& b, ScreenSegment& out) {
    const double length = distance(a,b);
    if (!(length >= 3 && length <= 96)) return false;
    if (camera.valid) {
        const double da = distance(camera.location,a), db = distance(camera.location,b);
        const double low = da <= db ? da : db, high = da <= db ? db : da;
        if (low < 85 && low + 60 < high) return false;
    }
    if (!project(camera,viewport,a,out.from) || !project(camera,viewport,b,out.to)) return false;
    const float dx = out.to.x-out.from.x, dy = out.to.y-out.from.y;
    return dy*dy + dx*dx <= 129600.0f;
}

std::vector<ScreenSegment> project_skeleton(const Camera& camera, Viewport viewport,
    const std::array<Position, 21>& points, std::uint32_t mask, bool largeProfile, bool profile20) {
    Skeleton s{camera,viewport,points,mask,{}};
    const int arm = present(mask,4) ? 4 : present(mask,5) ? 5 : -1;
    const int pelvis = present(mask,1) ? 1 : present(mask,0) ? 0 : -1;
    s.chain({1,2,3,4,5,6},46);
    s.chain({arm,7,8,9,10},62);
    s.chain({arm,11,12,13,14},62);
    const bool alternate = alternate_profile(s,profile20);
    if (largeProfile || alternate) {
        if (strict_leg(s,15,16,17,largeProfile,alternate)) s.chain({pelvis,15,16,17},80);
        if (strict_leg(s,18,19,20,largeProfile,alternate)) s.chain({pelvis,18,19,20},80);
    } else {
        const Leg left = repaired_leg(s,pelvis,15,16,17);
        const Leg right = repaired_leg(s,pelvis,18,19,20);
        if ((left.attempted > 0) != (right.attempted > 0) && present(mask,1)) {
            Position normal{};
            if ((mask & 0x880) == 0x880)
                for (int i=0;i<3;++i) normal[i] = points[11][i]-points[7][i];
            const auto& good = left.attempted > 0 ? left : right;
            const auto a = mirror(points[1],normal,good.anchor);
            const auto b = mirror(points[1],normal,good.knee);
            const auto c = mirror(points[1],normal,good.foot);
            if (a[0] < 1e11 && b[0] < 1e11) s.draw(a,b);
            if (b[0] < 1e11 && c[0] < 1e11) s.draw(b,c);
            else if (a[0] < 1e11 && c[0] < 1e11) s.draw(a,c);
        }
    }
    return s.lines;
}
} // namespace reconstruction::reference
