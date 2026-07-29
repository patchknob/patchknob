//----------------------------------------------------------------------------
//
//  This file is part of PatchKnob (Windows port).
//
//  PatchKnob is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//-----------------------------------------------------------------------------
//
//  ui/patchbay/patch_view_model.h
//
//  The VIEW-MODEL for the modular node-editor / patchbay canvas.
//
//  This is the ONLY thing the canvas (ui/patchbay/patch_canvas.h) knows about
//  the audio engine: a flat, engine-free description of what to draw and edit.
//  The real engine graph (PatchKnob::engine::patch::PatchGraph, built in parallel)
//  is deliberately NOT referenced here so the UI and the DSP graph stay
//  decoupled -- a coordinator (src/patchbayapp.cpp) translates between the two.
//
//  Concepts:
//      * A Node is a box with a name and two ordered lists of Ports
//        (inputs on the left, outputs on the right).  Its x/y are pure VIEW
//        state (where the box sits on the canvas) -- the engine does not care.
//      * A Port has a kind (Audio / Midi) and a stable id, unique within its
//        node.  Audio ports draw FILLED, Midi ports draw HOLLOW.
//      * A Connection joins one OUT port (from) to one IN port (to).  Only
//        like-kind ports may be joined (audio->audio, midi->midi); the canvas
//        enforces this before it ever emits a connect request.
//
//  Everything is plain data (no UI toolkit, no engine headers) so it is trivial to
//  build, copy, and snapshot from either side of the seam.
//
//-----------------------------------------------------------------------------

#ifndef PATCHKNOB_UI_PATCHBAY_PATCH_VIEW_MODEL_H
#define PATCHKNOB_UI_PATCHBAY_PATCH_VIEW_MODEL_H

#include <string>
#include <vector>

namespace PatchKnob {
namespace patchbay {

//! Stable node handle.  0 is reserved to mean "invalid / none".
typedef unsigned int NodeId;

//! Port handle, unique WITHIN a single node (not globally).
typedef unsigned int PortId;

//! Port signal type.  Audio ports render filled; Midi ports render hollow.
enum class PortKind { Audio, Midi };

//! Which side of a node a port lives on.
enum class PortDir { In, Out };

//! One connector on a node.
struct Port
{
    PortId       id;      //!< unique within the owning node
    PortKind     kind;    //!< Audio (filled) or Midi (hollow)
    std::string  name;    //!< short label, e.g. "in L/R", "midi in", "out"

    Port() : id( 0 ), kind( PortKind::Audio ) {}
    Port( PortId a_id, PortKind a_kind, const std::string& a_name )
        : id( a_id ), kind( a_kind ), name( a_name ) {}
};

//! A module box.  `inPorts` draw down the left edge, `outPorts` down the right.
struct Node
{
    NodeId             id;        //!< stable handle (assigned by the canvas if 0)
    std::string        name;      //!< title bar text
    std::string        category;  //!< informational grouping ("Instrument", ...)
    double             x;         //!< top-left X in canvas coords (VIEW state)
    double             y;         //!< top-left Y in canvas coords (VIEW state)
    std::vector<Port>  inPorts;
    std::vector<Port>  outPorts;

    Node() : id( 0 ), x( 0.0 ), y( 0.0 ) {}
};

//! Points at one specific port on one specific node.
struct PortRef
{
    NodeId node;
    PortId port;

    PortRef() : node( 0 ), port( 0 ) {}
    PortRef( NodeId a_node, PortId a_port ) : node( a_node ), port( a_port ) {}
};

//! A directed edge: `from` is always an OUT port, `to` an IN port.
struct Connection
{
    PortRef from;   //!< source, an OUT port
    PortRef to;     //!< sink, an IN port

    Connection() {}
    Connection( const PortRef& a_from, const PortRef& a_to )
        : from( a_from ), to( a_to ) {}
};

inline bool operator==( const PortRef& a, const PortRef& b )
{
    return a.node == b.node && a.port == b.port;
}

inline bool same_connection( const Connection& a, const Connection& b )
{
    return a.from == b.from && a.to == b.to;
}

} // namespace patchbay
} // namespace PatchKnob

#endif // PATCHKNOB_UI_PATCHBAY_PATCH_VIEW_MODEL_H
