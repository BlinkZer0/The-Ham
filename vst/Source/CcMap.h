#pragma once
// ============================================================================
//  Drawbar CC maps for hardware drawbar controllers. A mapped CC moves the drawbar
//  of the manual's ACTIVE set (A when its A# key is down, otherwise B) as a parameter
//  change, so the UI and the host see it.
//
//  Maps (source: Apple's EVB3 manual, "MIDI controller assignments" table, which lists
//  the controller modes of the common hardware; see docs/PLAN.md):
//    B4D           upper 16'..1' = CC 12-20, lower = CC 21-29   (Native Instruments B4D)
//    Nord Electro  upper 16'..1' = CC 16-24, lower = CC 70-78
//    VK / CX-3     one set 16'..1' = CC 70-78 (Roland VK, Korg CX-3) -> upper
//  Hammond SK and Crumar maps are not included until their CC tables can be cited.
//  Pedal drawbars have no standard CC; use MIDI learn (phase 2).
//  A CC value of 0..127 maps to drawbar 0..8 continuously (the engine takes fractions).
// ============================================================================

namespace ham
{
struct CcTarget { int manual = -1; int foot = -1; };

inline CcTarget drawbarForCC (int map, int cc)
{
    auto inRange = [cc] (int lo) { return cc >= lo && cc < lo + 9; };
    switch (map)
    {
        case 1: if (inRange (12)) return { 0, cc - 12 };
                if (inRange (21)) return { 1, cc - 21 };
                break;
        case 2: if (inRange (16)) return { 0, cc - 16 };
                if (inRange (70)) return { 1, cc - 70 };
                break;
        case 3: if (inRange (70)) return { 0, cc - 70 };
                break;
        default: break;
    }
    return {};
}
} // namespace ham
