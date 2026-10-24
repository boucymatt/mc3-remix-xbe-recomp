#pragma once
// The renderer's profile, fixed at boot: legacy is the minimal console path, enhanced the approved additions,
// hybrid the console path with features picked one by one. The approved speed-blur replacement is a shared baseline
// for all three. One backend serves every profile; a feature is a policy the backend asks about, never a second renderer.
#include <cstdint>
#include <string>

namespace gpu::render {

enum class Profile : uint8_t { Legacy, Enhanced, Hybrid };

enum Feature : uint32_t {
    kPost = 1u << 0,           // gamma/tonemap, FXAA, colour grade on the final image
    kMaterials = 1u << 1,      // sampling, specular, envmap rules on the translated shaders
    kContactShadow = 1u << 2,  // analytic shadow under the player's car
    kVehicleShadow = 1u << 3,  // shadow map of the player's car
    kSunShadow = 1u << 4,      // directional shadow of the city
    kReflections = 1u << 5,    // the title's own reflections (car paint, glass) sharper and shaped like a clear coat
    kLightShadow = 1u << 6,    // shadows of the night's lights: the player's headlights first (MC3-0012)
    kMotionBlur = 1u << 7,     // calibrated LA-inspired depth/reprojection streak pass
    kAllFeatures = (1u << 8) - 1,
};

// features the backend has code for; a feature asked for before it lands is reported pending and stays off
inline constexpr uint32_t kImplemented = kPost | kContactShadow | kVehicleShadow | kSunShadow | kReflections | kLightShadow |
                                         kMotionBlur;
// features promoted into enhanced after their gates (MC3-0008: post and contact-shadow 18/10, vehicle-shadow and reflections 19/10;
// light-shadow, the headlight beams, 20/10; motion-blur 23/10 after the high-speed A/B). RENDER_PROFILES.md lists the
// scenes not tested yet. The motion blur is also the legacy baseline so all profiles avoid the 480p radial composite.
inline constexpr uint32_t kApproved = kPost | kContactShadow | kVehicleShadow | kReflections | kLightShadow | kMotionBlur;
// features hybrid has on top of the ones asked for (--mc3-render-disable takes them off): the beams replace the title's
// headlight cone, which only legacy keeps; the blur replacement is shared with every profile
inline constexpr uint32_t kHybridDefault = kLightShadow | kMotionBlur;

// the post pass's parameters, every one neutral by default: with fxaa off and these at 1 the pass changes nothing
struct Post {
    bool fxaa = true;
    bool split = false;      // left half untouched, for an A/B inside one image
    float exposure = 1;      // colour times this
    float gamma = 1;         // colour to the power 1/gamma
    float contrast = 1;      // around mid grey
    float saturation = 1;    // from its luma
    float shoulder = 0;      // 0 off; else the top of the range rolled off, 1 the most
    float threshold = 0.0625f;  // FXAA leaves pixels whose local contrast is below this alone
};

struct Blur {
    // A long shutter span that was acceptable at the Xbox's 480p becomes a
    // conspicuous radial smear on a 1080p output. Keep the default subtle;
    // the command-line/config setting can still restore the stronger look.
    float strength = 0.30f;   // fraction of the projected frame displacement used as shutter span
    float max_pixels = 10.0f; // cap in output render-target pixels
    int samples = 6;          // symmetric spatial samples along the motion vector
    bool split = false;       // left half remains the MC3 path for A/B
};

// the contact shadow's shape in the car's own frame (metres; x right, y up, z forward) and how it fades
struct Shadow {
    float strength = 0.60f;  // darkening at the centre on the contact plane
    float width = 1.25f;     // half widths of the ellipse, a little past the body so the edge shows round it
    float length = 2.65f;
    float ground = 0.0f;     // the contact plane in the car's frame: MC3 puts the origin there (calibrated 18/10, +-5 cm)
    float top = 0.22f;       // surfaces this far above the plane are the car, never shadowed
    float fade = 1.20f;      // a surface this far below the plane (a jump, a drop) gets no shadow
    float soft = 0.45f;      // the outer part of the ellipse that fades out
    float near_m = 35.0f;    // camera distance where it starts to fade, and where it is gone
    float far_m = 70.0f;
    int debug = 0;           // 1 paints height bands of 0.25 m inside a widened footprint
};

// the vehicle shadow: the player's car rendered from a directional light into a depth map, the ground near it
// compared against that map
struct VShadow {
    float strength = 0.5f;   // darkening of a fully shadowed receiver
    int size = 1024;         // the depth map's side, texels
    int game = 1;            // 1: the title's own key light (sun, dusk sun, moon) and its brightness; 0: elev/azim
    float lumref = 0.8f;     // a key light this bright gives the full strength; dimmer ones (moonlight) give less
    float floor = 0;         // the least of that scale: 0 lets a night without a key light have no shadow
    float elev = 55;         // the light's elevation above the horizon, degrees, when the title sets none
    float azim = 35;         // its direction round the vertical, degrees from +z towards +x
    float radius = 4.5f;     // metres round the car the map covers and the receivers are taken from
    float bias = 0.03f;      // constant bias, metres along the light
    float slope = 1.5f;      // slope bias, texels times the tangent of the angle to the light
    int pcf = 2;             // filter radius in texels: (2 pcf + 1)^2 taps
    float top = 0.30f;       // receivers more than this above the car's contact plane are not taken (the car itself)
    float near_m = 35, far_m = 70;  // camera distance where it fades out
    float boxw = 1.25f, boxl = 2.9f, boxt = 2.4f;  // the casters' box in the car's frame: half width, half length, top
    int debug = 0;           // 1 the mask unfaded, 2 also the depth map in the top left corner
};

// the sun shadow: one or two cascades over the view, from the title's environment light
struct Sun {
    float strength = 0.55f;  // darkening of a fully shadowed receiver under a full strength light
    int size = 2048;         // each cascade's depth map side, texels
    int cascades = 2;        // 1: the near cascade alone; 2: also a coarser one out to far
    float range = 60;        // metres from the camera the near cascade covers; its last fifth blends into the far one
    float far_m = 200;       // metres the far cascade reaches; its last fifth fades out
    float bias = 0.08f;      // constant bias, metres along the light
    float slope = 2.0f;      // slope bias, texels times the tangent of the angle to the light
    int pcf = 1;             // filter radius in texels: (2 pcf + 1)^2 taps
    int game = 1;            // 1: the title's environment light and its brightness; 0: elev/azim at full strength
    float lumref = 0.8f, floor = 0;  // as for the vehicle shadow
    float elev = 55, azim = 35;      // the light when the title sets none
    int debug = 0;           // 1 the mask unfaded, 2 also the maps in the top left corner, 3 the cascades as bands
};

// the reflections: the title renders its environment map (a 256 x 256 target the car paint and glass sample through the
// view space normal) and blends it in with a per vertex Fresnel term; this renders those maps sharper and shapes the blend
struct Refl {
    float scale = 2;       // the reflection targets (square, power of two, at most 512) at this times the render scale
    float strength = 1.3f; // times the title's reflection share
    float fresnel = 0.8f;  // that share raised to this: under 1 lifts the faces seen head on less than the edges
    float boost = 1.2f;    // brightness of what is reflected, for the lights in it
    int debug = 0;         // 1 the reflection share in grey, 2 the reflection alone
};

// the light shadows: the player's headlights as one spot light, from the car's matrix; the shadow takes the light out of
// the title's own headlight passes (mcSpotlight), so only what the headlights lit gets darker
struct Lights {
    float strength = 0.9f;  // the share of the headlight's light a shadowed pixel loses
    int size = 1024;        // the depth map's side
    float fov = 70;         // degrees, the spot's full angle
    float near_m = 0.3f, far_m = 50;
    float fwd = 2.0f;       // metres ahead of the car's origin (the contact plane's centre)
    float height = 0.65f;   // metres above it
    float pitch = -4;       // degrees, down is negative
    float bias = 0.06f;     // metres, plus slope times the distance
    float slope = 0.015f;
    int pcf = 1;
    int debug = 0;          // 1 the headlight passes in red where shadowed, green where lit; 3 the lamps' shadows black
    // the street lamps: placed by the glows MC3 draws at their heads, kept once seen at one place on lamp_hits frames
    int lamps = 0;               // the nearest this many cast (0 none, at most 4); off by default: MCLA's lamps cast none
    float lamp_range = 45;       // metres from the camera a lamp may be to cast
    float lamp_radius = 14;      // metres its light reaches; the shadow fades out towards it
    float lamp_strength = 0.5f;  // the share of the baked light a point in its shadow loses at the lamp's foot
    float lamp_fov = 140;        // degrees, the downward map's angle
    int lamp_size = 1024;        // each lamp's map side
    float lamp_far = 25;
    float lamp_min_h = 2.5f, lamp_max_h = 14;  // a glow this far above the player's car is a lamp head
    float lamp_drop = 0.3f;      // the light sits this far under the glow
    int lamp_hits = 6;
    float lamp_bias = 0.08f, lamp_slope = 0.01f;
    // the headlight beams, after MCLA: the cars nearest the camera light the road ahead, and what is in the way shades it.
    // A car is any the title draws with its car paint program
    int cars = 4;                // beams (0 none, at most 4)
    float beam_fwd = 2.4f;       // metres ahead of the car's origin: clear of the front bumper
    float beam_height = 0.7f;    // metres above it
    float beam_strength = 2.0f;  // the light, multiplied into what is lit
    float beam_range = 32;       // metres
    float beam_fov = 90;         // degrees, the cone
    float beam_inner = 0.45f;    // the share of the cone at full light
    float beam_pitch = -10;      // degrees, down is negative: the pool starts near the bumper
    float beam_r = 1.0f, beam_g = 0.86f, beam_b = 0.66f;
    int beam_size = 1024;        // each beam's map side
    float beam_bias = 0.06f, beam_slope = 0.01f;
    int beam_debug = 0;          // 1 the beams' shadows red, their light green
    int beam_cone = 0;           // 1 keeps the title's own headlight cone beside the beams (0: the beams replace it)
};

struct Config {
    Profile profile = Profile::Legacy;
    const char* profile_from = "default";  // default, settings or command line
    uint32_t requested = 0;  // asked for with --mc3-render-feature or render_features
    uint32_t disabled = 0;   // --mc3-render-disable
    uint32_t ignored = 0;    // asked for but outside the profile (legacy takes none, enhanced only its package)
    uint32_t pending = 0;    // in the profile but not implemented yet
    uint32_t effective = 0;  // what the backend does
    int gpu_index = -1;      // -1: first adapter by high performance preference
    const char* gpu_from = "default";
    bool timing = false;     // per frame CPU and GPU time with percentiles
    std::string capture_ab;  // tag on F3 captures, which also turns timing on
    Post post;               // --mc3-post=key=value,... or render_post in the settings file
    Blur blur;               // --mc3-blur=key=value,... or render_blur in the settings file
    Shadow shadow;           // --mc3-shadow=key=value,... or render_shadow in the settings file
    VShadow vshadow;         // --mc3-vshadow=key=value,... or render_vshadow in the settings file
    Sun sun;                 // --mc3-sun=key=value,... or render_sun in the settings file
    Refl refl;               // --mc3-refl=key=value,... or render_refl in the settings file
    Lights lights;           // --mc3-lights=key=value,... or render_lights in the settings file
    std::string errors;      // values that did not parse, logged at boot
    uint64_t hash = 0;       // over profile, effective features and adapter choice
};

// parsed on first use from the command line over the settings file; immutable afterwards
const Config& Get();
inline bool On(Feature f) { return (Get().effective & f) != 0; }

const char* ProfileName(Profile p);
std::string FeatureNames(uint32_t mask);  // "post,contact-shadow" or "none"
std::string PostNames(const Post& p);     // "fxaa=1 exposure=1.000 ..."
std::string BlurNames(const Blur& b);
std::string ShadowNames(const Shadow& s);
std::string VShadowNames(const VShadow& s);
std::string SunNames(const Sun& s);
std::string ReflNames(const Refl& s);
std::string LightsNames(const Lights& s);
// the features that stream the scene's positions out for their casters
inline bool Streams() { return On(kVehicleShadow) || On(kSunShadow) || On(kLightShadow); }

// True after the original MC3 speed-blur tune was safely replaced in the guest
// archive read. The LA-inspired pass stays off if the sidecar could not apply.
bool MotionBlurTuneReady();
void MarkMotionBlurTuneReady();

// what the backend learns at Init, for the boot log and render_effective_config.txt
struct Adapter {
    int index = -1;
    std::string name;
    uint64_t luid = 0;
    uint64_t vram_mb = 0;
    uint32_t vendor = 0, device = 0, subsys = 0, revision = 0;
    std::string driver;  // UMD version a.b.c.d
};
struct Output {
    uint32_t guest_w = 0, guest_h = 0, out_w = 0, out_h = 0;
    float scale = 1;
    bool vsync = false, tearing = false;
    uint32_t fps_cap = 0;
};

// identity for a persistent cache: a file whose header differs from this was made elsewhere and is ignored
std::string CacheIdentity(const Adapter& a);

void LogConfig();                     // MC3RENDER profile line and the errors, before the device
void LogAdapter(const Adapter& a);    // MC3RENDER adapter_index line
void LogOutput(const Output& o);      // MC3RENDER guest/output and cache lines
void WriteEffectiveConfig();          // logs/render_effective_config.txt, after LogOutput

}  // namespace gpu::render
