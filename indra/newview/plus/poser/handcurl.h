// Charis: finger curl / spread / thumb sliders <-> joint rotations for the poser's hand tab (research/315).
// Pure functions, no viewer types, so handcurl_test.cpp can be built on its own.
//
// All joint values here are the poser's "raw" bone-frame Euler angles in radians, the very numbers the hand
// preset files hold (FSPoserAnimator::loadJointRotation takes them with SWAP_NOTHING / NEGATE_NOTHING). In that
// frame the two hands are mirror images: right = (-x, y, -z) of left (checked against every shipped preset).
//
// A finger's curl is a path through the nine numbers of its three joints: straight from the open hand (Flat.xml, curl 0)
// to Tight_Grip.xml (curl 0.8) and straight on to the full fist (curl 1): every joint moves in x, y and z at once, as
// the presets do (the outer fingers turn toward the palm's middle as they close). The curl value of a finger is where its
// joints lie on that path; a finger whose joints are far from the path was made joint by joint and is "custom". Moving
// a slider moves the joints along the path by the difference, so what was made by hand off the path stays. The spread of
// a finger is the sideways angle of its knuckle (z of joint 1) away from the path; closing shrinks it (the fingers come
// together in a fist), opening does not bring it back.
// The thumb has the wrap, swing and arch of Thumb1 (three directions, orthogonal on the positive side of the swing: Flat -> Grip.xml, Okay.xml across it, Thumbs_Up.xml across both; the swing also goes the other way, a turn about z) and the bend (Thumb2 and 3: Flat ->
// a bend of Thumb2 and 3 as measured in the poser, see THUMB_BENT in handcurl.cpp).
#pragma once

namespace plus
{
namespace handcurl
{

enum class Finger
{
    Index  = 0,
    Middle = 1,
    Ring   = 2,
    Pinky  = 3,
};
constexpr int FINGER_COUNT = 4;

/// A raw joint rotation (x, y, z radians), as in the preset files.
struct Raw
{
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

/// A whole hand: 5 digits x 3 joints (digit 0 = thumb, 1..4 = index..pinky); the joint values are the truth.
constexpr int HAND_DIGITS = 5;
struct HandRaw
{
    Raw joint[HAND_DIGITS][3];  // [digit][0..2] = joint 1..3 (e.g. mHandIndex1..3)
};
/// Digit slot of a finger (the thumb is slot 0).
inline int slotOf(Finger f) { return static_cast<int>(f) + 1; }

/// A finger whose joints lie farther than this (radians, in any one axis) from its curl path is "custom" (hand-made).
constexpr float CUSTOM_RESIDUAL_RAD = 0.40f;
/// A fitted value outside 0..1 by more than this also counts as custom (a bent-back finger, a thumb past its end).
constexpr float CUSTOM_MARGIN = 0.15f;
/// No slider moves the y of a joint past this (radians, either sign). The poser reads a joint back as Euler angles whose y lies
/// within +-pi/2 (LLQuaternion::getEulerAngles): a y written past that comes back as another triple (x and z turned by pi), and
/// the value of the slider jumps. The part of a move that would pass the limit is cut off (it can only be a hand-made offset: the
/// paths themselves stay inside); a joint that already lies past it is not pushed further, and not pulled in either.
constexpr float JOINT_Y_LIMIT_RAD = 1.50f;

/// The left hand's open and closed joint values of a finger (joint 0..2), for tests and for reading the model.
Raw fingerOpenJoint(Finger finger, int joint);   // Flat.xml
Raw fingerTightGripJoint(Finger finger, int joint);  // Tight_Grip.xml (curl 0.8)
Raw fingerClosedJoint(Finger finger, int joint);  // curl 1: the full fist

/// Curl 0 (open hand) .. 1 (closed hand) as read from the joints. custom (optional) says the finger was made by hand.
float getFingerCurl(const HandRaw& hand, bool rightHand, Finger finger, bool* custom = nullptr);
/// Moves the joints along the curl path to curl c (clamped to 0..1). What lies off the path stays (but for a y held to
/// JOINT_Y_LIMIT_RAD). The spread shrinks as the finger closes (in proportion to 1 - curl) so a closed finger is the shipped
/// fist; opening does not bring it back.
void setFingerCurl(HandRaw& hand, bool rightHand, Finger finger, float c);

/// Spread -1 (squeezed together) .. 0 (as the curl path has it, the flat hand when open) .. +1 (fanned out). custom
/// (optional) says the knuckle lies outside that range by more than CUSTOM_MARGIN (a hand-made finger: the slider shows the
/// end of its range, and its first move brings the knuckle into the range).
float getFingerSpread(const HandRaw& hand, bool rightHand, Finger finger, bool* custom = nullptr);
void setFingerSpread(HandRaw& hand, bool rightHand, Finger finger, float s);
/// The sideways angle (radians, the left hand's z, away from the curl path) of a spread value, and back.
/// Fanning turns the index toward smaller z and the other fingers toward larger z (a positive z turns a finger toward -x).
float spreadToAngle(Finger finger, float s);
float angleToSpread(Finger finger, float angle);

/// The thumb's four sliders, 0..1 (the swing -1..1). Wrap: the thumb folded over the palm toward the little finger (Thumb1 Flat -> Grip.xml). Swing:
/// the thumb turned toward the palm to face the fingers (Thumb1's part of Okay.xml that is not wrap; negative: the thumb spread away from the hand in the plane of the palm). Arch: the thumb arched back toward the back of the hand (the part of Thumbs_Up.xml's Thumb1 that is neither). Bend: Thumb2 and 3. custom as for the fingers (a coordinate of
/// Thumb1 out of range marks the slider of that coordinate). Wrap, swing and arch move Thumb1 only along their own
/// direction; Thumb2 and 3 keep their bend.
float getThumbWrap(const HandRaw& hand, bool rightHand, bool* custom = nullptr);
void setThumbWrap(HandRaw& hand, bool rightHand, float a);
float getThumbSwing(const HandRaw& hand, bool rightHand, bool* custom = nullptr);
void setThumbSwing(HandRaw& hand, bool rightHand, float w);
float getThumbArch(const HandRaw& hand, bool rightHand, bool* custom = nullptr);
void setThumbArch(HandRaw& hand, bool rightHand, float x);
float getThumbBend(const HandRaw& hand, bool rightHand, bool* custom = nullptr);
void setThumbBend(HandRaw& hand, bool rightHand, float b);

/// Whole-hand bars, relative: all four fingers move by delta along their paths (each clamped to 0..1; a finger already
/// past the end is not pushed), so differences set one by one survive. The shown value is the mean of the four.
float meanFingerCurl(const HandRaw& hand, bool rightHand);
float meanFingerSpread(const HandRaw& hand, bool rightHand);
void shiftAllFingerCurl(HandRaw& hand, bool rightHand, float delta);
void shiftAllFingerSpread(HandRaw& hand, bool rightHand, float delta);

/// The hand presets made from the model (Open_Hand, Peace, Rock, Gun), as slider values: the curl and spread of each
/// finger, the thumb's wrap, swing, arch and bend. makeNamedHand gives the left hand's 15 joints; the right hand is
/// mirrorHand of it. handcurl_genpresets_test.cpp writes them as preset files and checks the shipped files against them.
enum class NamedHand
{
    OpenHand,
    Peace,
    Rock,
    Gun,
};
constexpr int NAMED_HAND_COUNT = 4;
const char* namedHandFileName(NamedHand which);  // "Open_Hand" ...
/// The version number of the hand's preset file. The user's copy of a shipped preset is replaced only by a file with a higher
/// version (FSFloaterPoser::createUserPoseDirectoryIfNeeded): it is raised whenever makeNamedHand gives that hand other joints.
int     namedHandFileVersion(NamedHand which);
/// The version of a preset file the user saves ("Save hand"): above every shipped file's, so that an update never replaces a
/// user's file (handcurl.cpp asserts it for the hands made here; tools/hand_presets.py checks every shipped file).
constexpr int USER_HAND_PRESET_FILE_VERSION = 100;
HandRaw makeNamedHand(NamedHand which);

/// The same pose on the other hand (a copy that looks alike, not the same raw numbers).
Raw mirrorRaw(const Raw& r);
HandRaw mirrorHand(const HandRaw& hand);

/// Clamp helper shared by the UI (also safe for NaN: gives lo).
float clampf(float v, float lo, float hi);

}  // namespace handcurl
}  // namespace plus
